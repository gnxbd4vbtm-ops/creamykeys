#include "mechvibes.hpp"
#include <algorithm>
#include <archive.h>
#include <archive_entry.h>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <linux/input-event-codes.h>
#include <map>
#include <memory>
#include <nlohmann/json.hpp>
#include <regex>
#include <set>
#include <sndfile.h>
#include <sstream>
#include <stdexcept>
#include <unistd.h>

namespace ck
{
namespace fs = std::filesystem;
using Json = nlohmann::json;
constexpr size_t decoded_limit = 32 * 1024 * 1024;

static fs::path relative_path(std::string name)
{
	std::replace(name.begin(), name.end(), '\\', '/');
	if (name.starts_with("#/"))
		name.erase(0, 2);
	fs::path p(name);
	if (p.empty() || p.is_absolute() || name.find(':') != name.npos || name.find('\0') != name.npos)
		throw std::runtime_error("Unsafe pack path");
	for (auto &part : p)
		if (part == "..")
			throw std::runtime_error("Unsafe pack path: parent traversal");
	p = p.lexically_normal();
	if (p.empty() || p == ".")
		throw std::runtime_error("Empty pack path");
	return p;
}

static Json read_json(const fs::path &path)
{
	if (fs::file_size(path) > 1024 * 1024)
		throw std::runtime_error("Pack JSON exceeds 1 MiB");
	std::ifstream file(path);
	if (!file)
		throw std::runtime_error("Cannot read pack config");
	return Json::parse(file, nullptr, true, true);
}

unsigned mechvibes_key(const std::string &name)
{
	// Mechvibes legacy packs use libuiohook scan codes, not Linux extended codes.
	if (!name.empty() && name.find_first_not_of("0123456789") == name.npos)
	{
		unsigned long code = std::stoul(name);
		if (code >= 1 && code <= 83)
			return unsigned(code);
		if (code == 86 || code == 87 || code == 88)
			return unsigned(code);
		if (code >= 91 && code <= 93)
			return KEY_F13 + unsigned(code - 91);
		if (code >= 99 && code <= 107)
			return KEY_F16 + unsigned(code - 99);
		if ((code & 0xff00) == 0xee00)
			return unsigned(code & 0xff); // numpad, NumLock off
		static const std::map<unsigned, unsigned> extended = {
			{0x0e0d, KEY_KPEQUAL}, {0x0e1c, KEY_KPENTER}, {0x0e1d, KEY_RIGHTCTRL}, {0x0e35, KEY_KPSLASH}, {0x0e37, KEY_SYSRQ}, {0x0e38, KEY_RIGHTALT}, {0x0e45, KEY_PAUSE}, {0x0e47, KEY_HOME}, {0x0e49, KEY_PAGEUP}, {0x0e4f, KEY_END}, {0x0e51, KEY_PAGEDOWN}, {0x0e52, KEY_INSERT}, {0x0e53, KEY_DELETE}, {0x0e5b, KEY_LEFTMETA}, {0x0e5c, KEY_RIGHTMETA}, {0x0e5d, KEY_COMPOSE}, {0xe048, KEY_UP}, {0xe04b, KEY_LEFT}, {0xe04d, KEY_RIGHT}, {0xe050, KEY_DOWN}, {0xe05e, KEY_POWER}, {0xe05f, KEY_SLEEP}, {0xe063, KEY_WAKEUP}, {0xe022, KEY_PLAYPAUSE}, {0xe024, KEY_STOPCD}, {0xe010, KEY_PREVIOUSSONG}, {0xe019, KEY_NEXTSONG}, {0xe02c, KEY_EJECTCD}, {0xe020, KEY_MUTE}, {0xe030, KEY_VOLUMEUP}, {0xe02e, KEY_VOLUMEDOWN}, {0xe06c, KEY_MAIL}, {0xe021, KEY_CALC}, {0xe032, KEY_HOMEPAGE}, {0xe06a, KEY_BACK}, {0xe069, KEY_FORWARD}, {0xe065, KEY_SEARCH}, {0xe066, KEY_BOOKMARKS}};
		auto it = extended.find(unsigned(code));
		return it == extended.end() ? 0 : it->second;
	}
	// MechvibesDX uses KeyboardEvent.code names.
	if (name.size() == 4 && name.starts_with("Key") && name[3] >= 'A' && name[3] <= 'Z')
	{
		static constexpr unsigned letters[] = {KEY_A, KEY_B, KEY_C, KEY_D, KEY_E, KEY_F, KEY_G, KEY_H, KEY_I, KEY_J, KEY_K, KEY_L, KEY_M, KEY_N, KEY_O, KEY_P, KEY_Q, KEY_R, KEY_S, KEY_T, KEY_U, KEY_V, KEY_W, KEY_X, KEY_Y, KEY_Z};
		return letters[name[3] - 'A'];
	}
	if (name.size() == 6 && name.starts_with("Digit") && name[5] >= '0' && name[5] <= '9')
		return name[5] == '0' ? KEY_0 : KEY_1 + name[5] - '1';
	if (name.size() == 7 && name.starts_with("Numpad") && name[6] >= '0' && name[6] <= '9')
	{
		static constexpr unsigned keypad[] = {KEY_KP0, KEY_KP1, KEY_KP2, KEY_KP3, KEY_KP4, KEY_KP5, KEY_KP6, KEY_KP7, KEY_KP8, KEY_KP9};
		return keypad[name[6] - '0'];
	}
	if (name.size() >= 2 && name[0] == 'F' && name.substr(1).find_first_not_of("0123456789") == name.npos)
	{
		unsigned f = std::stoul(name.substr(1));
		if (f >= 1 && f <= 10)
			return KEY_F1 + f - 1;
		if (f == 11)
			return KEY_F11;
		if (f == 12)
			return KEY_F12;
		if (f >= 13 && f <= 24)
			return KEY_F13 + f - 13;
	}
	static const std::map<std::string, unsigned> named = {
		{"Space", KEY_SPACE}, {"Enter", KEY_ENTER}, {"Escape", KEY_ESC}, {"Backspace", KEY_BACKSPACE}, {"Tab", KEY_TAB}, {"CapsLock", KEY_CAPSLOCK}, {"NumLock", KEY_NUMLOCK}, {"ScrollLock", KEY_SCROLLLOCK}, {"ShiftLeft", KEY_LEFTSHIFT}, {"ShiftRight", KEY_RIGHTSHIFT}, {"ControlLeft", KEY_LEFTCTRL}, {"ControlRight", KEY_RIGHTCTRL}, {"AltLeft", KEY_LEFTALT}, {"AltRight", KEY_RIGHTALT}, {"MetaLeft", KEY_LEFTMETA}, {"MetaRight", KEY_RIGHTMETA}, {"ContextMenu", KEY_COMPOSE}, {"ArrowUp", KEY_UP}, {"ArrowDown", KEY_DOWN}, {"ArrowLeft", KEY_LEFT}, {"ArrowRight", KEY_RIGHT}, {"Home", KEY_HOME}, {"End", KEY_END}, {"PageUp", KEY_PAGEUP}, {"PageDown", KEY_PAGEDOWN}, {"Insert", KEY_INSERT}, {"Delete", KEY_DELETE}, {"PrintScreen", KEY_SYSRQ}, {"Pause", KEY_PAUSE}, {"Comma", KEY_COMMA}, {"Period", KEY_DOT}, {"Semicolon", KEY_SEMICOLON}, {"Quote", KEY_APOSTROPHE}, {"Backquote", KEY_GRAVE}, {"Backslash", KEY_BACKSLASH}, {"BracketLeft", KEY_LEFTBRACE}, {"BracketRight", KEY_RIGHTBRACE}, {"Slash", KEY_SLASH}, {"Minus", KEY_MINUS}, {"Equal", KEY_EQUAL}, {"NumpadAdd", KEY_KPPLUS}, {"NumpadSubtract", KEY_KPMINUS}, {"NumpadMultiply", KEY_KPASTERISK}, {"NumpadDivide", KEY_KPSLASH}, {"NumpadDecimal", KEY_KPDOT}, {"NumpadEnter", KEY_KPENTER}, {"NumpadEqual", KEY_KPEQUAL}, {"IntlBackslash", KEY_102ND}, {"IntlRo", KEY_RO}, {"IntlYen", KEY_YEN}};
	auto it = named.find(name);
	return it == named.end() ? 0 : it->second;
}

struct AudioFile
{
	SF_INFO info{};
	std::unique_ptr<SNDFILE, decltype(&sf_close)> file{nullptr, &sf_close};

	explicit AudioFile(const fs::path &path)
	{
		file.reset(sf_open(path.c_str(), SFM_READ, &info));
		if (!file || info.frames <= 0 || info.channels < 1 || info.channels > 8 || info.samplerate < 8000 || info.samplerate > 192000)
			throw std::runtime_error("Cannot decode audio: " + path.filename().string() + " (supported: WAV, OGG, FLAC, MP3)");
	}
};

Pack load_mechvibes_pack(const fs::path &config_file)
{
	Json config = read_json(config_file);
	if (!config.is_object())
		throw std::runtime_error("Pack config must be an object");
	bool dx = config.contains("definitions") || config.contains("defs");
	Json definitions = dx ? config.value("definitions", config.value("defs", Json::object())) : config.value("defines", Json::object());
	if (!definitions.is_object() || definitions.empty() || definitions.size() > 4096)
		throw std::runtime_error("Invalid or missing key definitions");
	unsigned version = 1;
	if (!dx && config.contains("version"))
	{
		if (config["version"].is_number_unsigned() || config["version"].is_number_integer())
			version = config["version"].get<unsigned>();
	}
	if (!dx && version > 3)
		throw std::runtime_error("Unsupported Mechvibes config version");
	std::string method = dx ? config.value("definition_method", std::string("single")) : config.value("key_define_type", std::string("single"));
	std::string shared = dx ? config.value("audio_file", std::string()) : config.value("sound", std::string());
	auto root = fs::canonical(config_file.parent_path());
	Pack pack;
	std::map<std::string, std::unique_ptr<AudioFile>> files;
	std::map<std::string, unsigned> clips;
	size_t decoded_bytes = 0;
	auto sample = [&](const std::string &filename, double start, double length, bool whole = false) -> unsigned
	{
		if (!std::isfinite(start) || !std::isfinite(length) || start < 0 || (whole ? length != -1 : length <= 0) || length > 2000)
			throw std::runtime_error("Invalid clip timing (maximum 2 seconds)");
		auto rel = relative_path(filename);
		auto path = root / rel;
		if (!fs::is_regular_file(path) || fs::is_symlink(path))
			throw std::runtime_error("Missing audio file: " + rel.string());
		auto canonical = fs::canonical(path);
		auto within = canonical.lexically_relative(root);
		relative_path(within.string());
		std::ostringstream identity;
		identity << canonical.string() << '|' << std::setprecision(17) << start << '|' << length;
		auto key = identity.str();
		if (clips.contains(key))
			return clips.at(key);
		if (files.size() >= 256 && !files.contains(canonical.string()))
			throw std::runtime_error("Too many source audio files");
		auto &source = files[canonical.string()];
		if (!source)
			source = std::make_unique<AudioFile>(canonical);
		auto &info = source->info;
		if (start > double(info.frames) * 1000 / info.samplerate)
			throw std::runtime_error("Clip starts after end of recording");
		sf_count_t first = sf_count_t(std::llround(start * info.samplerate / 1000));
		sf_count_t count = length < 0 ? info.frames - first : std::min(info.frames - first, sf_count_t(std::llround(length * info.samplerate / 1000)));
		if (count <= 0 || count > info.samplerate * 2)
			throw std::runtime_error("Key sound must be nonempty and at most 2 seconds");
		if (sf_seek(source->file.get(), first, SEEK_SET) != first)
			throw std::runtime_error("Cannot seek sound recording");
		std::vector<float> raw(size_t(count) * info.channels), mono(size_t(count), 0.f);
		if (sf_readf_float(source->file.get(), raw.data(), count) != count)
			throw std::runtime_error("Truncated sound recording");
		float peak = 0;
		for (size_t i = 0; i < mono.size(); ++i)
		{
			for (int c = 0; c < info.channels; ++c)
				mono[i] += raw[i * info.channels + c] / info.channels;
			if (!std::isfinite(mono[i]))
				throw std::runtime_error("Nonfinite audio data");
			peak = std::max(peak, std::abs(mono[i]));
		}
		size_t begin = 0;
		while (begin + 1 < mono.size() && std::abs(mono[begin]) < peak * .002f)
			++begin;
		unsigned frames = std::max(1u, unsigned((mono.size() - begin) * double(rate) / info.samplerate));
		decoded_bytes += (frames + 1) * sizeof(float);
		if (decoded_bytes > decoded_limit || pack.keyed_samples.size() >= 4096)
			throw std::runtime_error("Imported pack exceeds 32 MiB decoded budget");
		Sample sound;
		sound.data.resize(frames + 1);
		float gain = peak > 1e-6f ? .7f / peak : 1.f;
		for (unsigned i = 0; i < frames; ++i)
		{
			double at = begin + i * double(info.samplerate) / rate;
			size_t a = std::min(size_t(at), mono.size() - 1);
			float fraction = float(at - a);
			sound.data[i] = (mono[a] * (1 - fraction) + mono[std::min(a + 1, mono.size() - 1)] * fraction) * gain;
		}
		sound.data[frames] = 0;
		unsigned index = pack.keyed_samples.size();
		pack.keyed_samples.push_back(std::move(sound));
		clips[key] = index;
		return index;
	};
	auto expand = [&](std::string pattern)
	{
		std::vector<std::string> names;
		static const std::regex range("\\{([0-9]+)-([0-9]+)\\}");
		std::smatch match;
		if (std::regex_search(pattern, match, range))
		{
			unsigned a = std::stoul(match[1]), b = std::stoul(match[2]);
			if (b < a || b - a >= 32 || b > 999999)
				throw std::runtime_error("Invalid filename range");
			for (unsigned i = a; i <= b; ++i)
				names.push_back(match.prefix().str() + std::to_string(i) + match.suffix().str());
		}
		else
			names.push_back(std::move(pattern));
		return names;
	};
	auto file_variants = [&](const std::string &name, std::vector<unsigned> &variants)
	{for(auto& file:expand(name))variants.push_back(sample(file,0,-1,true)); };
	for (auto it = definitions.begin(); it != definitions.end(); ++it)
	{
		if (it.key().ends_with("-up"))
			continue;
		unsigned code = mechvibes_key(it.key());
		if (!code || code >= pack.keys.size() || it.value().is_null())
			continue;
		auto &variants = pack.keys[code];
		const auto &definition = it.value();
		if (dx)
		{
			if (!definition.is_object())
				throw std::runtime_error("Invalid DX key definition");
			std::string audio = definition.value("audio_file", shared);
			if (definition.contains("file"))
				audio = definition["file"].get<std::string>();
			if (definition.contains("timing"))
			{
				for (auto &timing : definition["timing"])
				{
					if (!timing.is_array() || timing.size() != 2)
						throw std::runtime_error("Invalid DX timing");
					double begin = timing[0].get<double>(), end = timing[1].get<double>();
					variants.push_back(sample(audio, begin, end - begin));
				}
			}
			else
				file_variants(audio, variants);
		}
		else if (version == 3 && config.contains("sounds"))
		{
			if (!definition.is_array() || definition.empty())
				continue;
			auto reference = definition[0].get<std::string>();
			auto spec = config.at("sounds").at(reference);
			auto file = spec.value("file", shared);
			if (spec.contains("files"))
			{
				for (auto &f : spec["files"])
					file_variants(f.get<std::string>(), variants);
			}
			else if (spec.contains("clips"))
			{
				for (auto &clip : spec["clips"])
					variants.push_back(sample(file, clip.at(0).get<double>(), clip.at(1).get<double>()));
			}
			else if (spec.contains("clip"))
			{
				auto &clip = spec["clip"];
				variants.push_back(sample(file, clip.at(0).get<double>(), clip.at(1).get<double>()));
			}
			else
				file_variants(file, variants);
			pack.cycle_keys[code] = spec.value("mode", std::string()) == "cycle";
		}
		else if (method == "multi")
		{
			if (!definition.is_string())
				throw std::runtime_error("Expected audio filename in multi-file pack");
			file_variants(definition.get<std::string>(), variants);
		}
		else if (method == "single")
		{
			if (!definition.is_array() || definition.size() != 2)
				throw std::runtime_error("Expected [start_ms, duration_ms] in single-file pack");
			variants.push_back(sample(shared, definition[0].get<double>(), definition[1].get<double>()));
		}
		else
			throw std::runtime_error("Unsupported sound definition method");
		if (variants.size() > 32)
			throw std::runtime_error("More than 32 variations for one key");
		auto &fallback = pack.category_samples[category(code)];
		for (unsigned index : variants)
			if (std::find(fallback.begin(), fallback.end(), index) == fallback.end())
				fallback.push_back(index);
	}
	if (pack.keyed_samples.empty())
		throw std::runtime_error("Pack has no usable keyboard definitions");
	std::vector<unsigned> generic = pack.category_samples[0];
	if (!dx && version == 2 && method == "multi" && !shared.empty())
	{
		generic.clear();
		file_variants(shared, generic);
	}
	if (generic.empty())
		generic.push_back(0);
	for (auto &fallback : pack.category_samples)
		if (fallback.empty())
			fallback = generic;
	return pack;
}

std::string import_mechvibes_zip(const fs::path &zip, const fs::path &packs)
{
	if (!fs::is_regular_file(zip))
		throw std::runtime_error("ZIP not found: " + zip.string());
	if (fs::file_size(zip) > 128 * 1024 * 1024)
		throw std::runtime_error("ZIP exceeds 128 MiB");
	uint64_t hash = 14695981039346656037ULL;
	std::ifstream input(zip, std::ios::binary);
	char block[16384];
	while (input)
	{
		input.read(block, sizeof block);
		for (std::streamsize i = 0; i < input.gcount(); ++i)
		{
			hash ^= static_cast<unsigned char>(block[i]);
			hash *= 1099511628211ULL;
		}
	}
	if (!input.eof())
		throw std::runtime_error("Cannot read ZIP");
	std::string slug;
	for (unsigned char c : zip.stem().string())
	{
		if (slug.size() >= 40)
			break;
		if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
			slug += char(c);
		else if (c >= 'A' && c <= 'Z')
			slug += char(c - 'A' + 'a');
		else if (slug.empty() || slug.back() != '-')
			slug += '-';
	}
	if (slug.empty())
		slug = "mechvibes";
	std::ostringstream id;
	id << slug << '-' << std::hex << hash;
	std::string name = id.str();
	fs::create_directories(packs);
	fs::path final = packs / name;
	if (fs::is_directory(final))
	{
		(void)load_pack(final);
		return name;
	}
	std::string pattern = (packs / ".import-XXXXXX").string();
	std::vector<char> buffer(pattern.begin(), pattern.end());
	buffer.push_back(0);
	auto directory = mkdtemp(buffer.data());
	if (!directory)
		throw std::runtime_error("Cannot create import directory");

	struct Cleanup
	{
		fs::path path;

		~Cleanup()
		{
			if (!path.empty())
			{
				std::error_code ec;
				fs::remove_all(path, ec);
			}
		}
	} cleanup{directory};

	fs::path source = cleanup.path / "source";
	fs::create_directories(source);
	std::unique_ptr<archive, decltype(&archive_read_free)> reader(archive_read_new(), &archive_read_free);
	if (!reader)
		throw std::runtime_error("Cannot allocate ZIP reader");
	archive_read_support_format_zip(reader.get());
	archive_read_support_filter_all(reader.get());
	if (archive_read_open_filename(reader.get(), zip.c_str(), 16384) != ARCHIVE_OK)
		throw std::runtime_error("Cannot open ZIP archive");
	archive_entry *entry = nullptr;
	size_t bytes = 0, count = 0;
	std::set<std::string> seen;
	std::vector<fs::path> configs;
	int status;
	while ((status = archive_read_next_header(reader.get(), &entry)) == ARCHIVE_OK)
	{
		if (++count > 2048)
			throw std::runtime_error("Too many ZIP entries");
		const char *raw = archive_entry_pathname_utf8(entry);
		if (!raw)
			raw = archive_entry_pathname(entry);
		if (!raw)
			throw std::runtime_error("ZIP entry without a filename");
		auto relative = relative_path(raw);
		if (relative.string().starts_with("__MACOSX/"))
		{
			archive_read_data_skip(reader.get());
			continue;
		}
		if (archive_entry_symlink(entry) || archive_entry_hardlink(entry) || archive_entry_is_encrypted(entry) > 0)
			throw std::runtime_error("ZIP links and encrypted entries are unsupported");
		auto type = archive_entry_filetype(entry);
		auto path = source / relative;
		if (type == AE_IFDIR)
		{
			fs::create_directories(path);
			continue;
		}
		if (type != AE_IFREG || !seen.insert(relative.string()).second)
			throw std::runtime_error("Unsafe or duplicate ZIP entry");
		if (archive_entry_size(entry) < 0 || archive_entry_size(entry) > 128 * 1024 * 1024)
			throw std::runtime_error("ZIP entry exceeds limits");
		fs::create_directories(path.parent_path());
		std::ofstream output(path, std::ios::binary);
		if (!output)
			throw std::runtime_error("Cannot write imported file");
		la_ssize_t read;
		while ((read = archive_read_data(reader.get(), block, sizeof block)) > 0)
		{
			bytes += size_t(read);
			if (bytes > 256 * 1024 * 1024)
				throw std::runtime_error("ZIP exceeds 256 MiB unpacked");
			output.write(block, read);
			if (!output)
				throw std::runtime_error("Import write failed");
		}
		if (read < 0)
			throw std::runtime_error("Corrupt ZIP data");
		output.close();
		auto filename = relative.filename().string();
		std::transform(filename.begin(), filename.end(), filename.begin(), [](unsigned char c)
					   { return std::tolower(c); });
		if (filename == "config.json")
			configs.push_back(path);
	}
	if (status != ARCHIVE_EOF)
		throw std::runtime_error("Invalid or truncated ZIP archive");
	if (configs.size() != 1)
		throw std::runtime_error("ZIP must contain exactly one sound-pack config.json");
	auto validation = load_mechvibes_pack(configs[0]);
	(void)validation;
	Json original = read_json(configs[0]);
	Json manifest = {{"source_config", configs[0].lexically_relative(cleanup.path).generic_string()}, {"source_archive", fs::absolute(zip).string()}, {"name", original.value("name", zip.stem().string())}, {"format", "mechvibes"}};
	{
		std::ofstream out(cleanup.path / "creamykeys-import.json");
		out << manifest.dump(2) << '\n';
		if (!out)
			throw std::runtime_error("Import metadata write failed");
	}
	fs::rename(cleanup.path, final);
	cleanup.path.clear();
	return name;
}
} // namespace ck
