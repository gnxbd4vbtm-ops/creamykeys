#include "core.hpp"
#include "mechvibes.hpp"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <linux/input-event-codes.h>
#include <memory>
#include <nlohmann/json.hpp>
#include <set>
#include <sndfile.h>
#include <sstream>
#include <stdexcept>
#include <time.h>
#include <unistd.h>

namespace ck
{
uint64_t now_ns()
{
	timespec t{};
	clock_gettime(CLOCK_MONOTONIC, &t);
	return uint64_t(t.tv_sec) * 1000000000 + t.tv_nsec;
}

static std::string trim(std::string s)
{
	auto a = s.find_first_not_of(" \t\r\n");
	if (a == s.npos)
		return {};
	return s.substr(a, s.find_last_not_of(" \t\r\n") - a + 1);
}

Config load_config(const std::filesystem::path &path)
{
	Config c;
	std::ifstream f(path);
	if (!f)
		throw std::runtime_error("Cannot read config");
	std::string line;
	unsigned ln = 0;
	std::set<std::string> keys;
	while (std::getline(f, line))
	{
		++ln;
		auto comment = line.find('#');
		line = trim(line.substr(0, comment));
		if (line.empty())
			continue;
		auto sep = line.find('=');
		if (sep == line.npos)
			throw std::runtime_error("Invalid config line " + std::to_string(ln));
		auto key = trim(line.substr(0, sep)), v = trim(line.substr(sep + 1));
		if (!keys.insert(key).second)
			throw std::runtime_error("Duplicate config key: " + key);
		auto boolean = [&]
		{
			if (v == "true")
				return true;
			if (v == "false")
				return false;
			throw std::runtime_error("Expected boolean for " + key);
		};
		auto number = [&]
		{
			size_t n = 0;
			float x = std::stof(v, &n);
			if (n != v.size() || !std::isfinite(x))
				throw std::runtime_error("Invalid number");
			return x;
		};
		if (key == "enabled")
			c.enabled = boolean();
		else if (key == "randomize_samples")
			c.randomize = boolean();
		else if (key == "play_on_repeat")
			c.repeat = boolean();
		else if (key == "follow_default_sink")
			c.follow = boolean();
		else if (key == "volume")
			c.volume = number();
		else if (key == "pitch_variation")
			c.pitch = number();
		else if (key == "max_voices")
		{
			auto n = number();
			if (n < 1 || n > 64 || std::floor(n) != n)
				throw std::runtime_error("max_voices must be 1..64");
			c.voices = unsigned(n);
		}
		else if (key == "idle_timeout_ms")
		{
			auto n = number();
			if (n < 0 || n > 60000 || std::floor(n) != n)
				throw std::runtime_error("idle_timeout_ms must be 0..60000");
			c.idle_ms = unsigned(n);
		}
		else if (key == "sound_pack")
		{
			if (v.size() < 3 || v.front() != '"' || v.back() != '"')
				throw std::runtime_error("Pack must be quoted");
			c.pack = v.substr(1, v.size() - 2);
			if (c.pack.find_first_not_of(
					"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-")
				!= c.pack.npos)
				throw std::runtime_error("Invalid pack name");
		}
		else
			throw std::runtime_error("Unknown config key: " + key);
	}
	if (c.volume < 0 || c.volume > 1 || c.pitch < 0 || c.pitch > .1f)
		throw std::runtime_error("volume must be 0..1, pitch_variation 0..0.1");
	return c;
}

void save_config(const std::filesystem::path &p, const Config &c)
{
	auto tmp = p;
	tmp += ".tmp." + std::to_string(getpid());
	{
		std::ofstream f(tmp);
		if (!f)
			throw std::runtime_error("Cannot write config");
		f << std::boolalpha << "enabled = " << c.enabled << "\nvolume = " << c.volume
		  << "\nsound_pack = \"" << c.pack << "\"\npitch_variation = " << c.pitch
		  << "\nrandomize_samples = " << c.randomize << "\nplay_on_repeat = " << c.repeat
		  << "\nmax_voices = " << c.voices << "\nfollow_default_sink = " << c.follow
		  << "\nidle_timeout_ms = " << c.idle_ms << '\n';
		f.flush();
		if (!f)
			throw std::runtime_error("Config write failed");
	}
	std::filesystem::rename(tmp, p);
}

unsigned category(unsigned k)
{
	switch (k)
	{
		case KEY_SPACE:
			return 1;
		case KEY_ENTER:
		case KEY_KPENTER:
			return 2;
		case KEY_BACKSPACE:
			return 3;
		case KEY_LEFTSHIFT:
		case KEY_RIGHTSHIFT:
		case KEY_LEFTCTRL:
		case KEY_RIGHTCTRL:
		case KEY_LEFTALT:
		case KEY_RIGHTALT:
		case KEY_LEFTMETA:
		case KEY_RIGHTMETA:
			return 4;
		default:
			if ((k >= KEY_1 && k <= KEY_0) || (k >= KEY_Q && k <= KEY_P) || (k >= KEY_A && k <= KEY_L) || (k >= KEY_Z && k <= KEY_M))
				return 0;
			return 5;
	}
}

Pack load_pack(const std::filesystem::path &dir)
{
	auto manifest = dir / "creamykeys-import.json";
	if (std::filesystem::exists(manifest))
	{
		if (std::filesystem::file_size(manifest) > 1024 * 1024)
			throw std::runtime_error("Import metadata too large");
		std::ifstream file(manifest);
		nlohmann::json metadata = nlohmann::json::parse(file);
		auto rel = std::filesystem::path(metadata.at("source_config").get<std::string>());
		if (rel.empty() || rel.is_absolute())
			throw std::runtime_error("Invalid imported config path");
		for (auto &part : rel)
			if (part == "..")
				throw std::runtime_error("Invalid imported config path");
		auto root = std::filesystem::canonical(dir);
		auto resolved = std::filesystem::canonical(root / rel).lexically_relative(root);
		for (auto &part : resolved)
			if (part == "..")
				throw std::runtime_error("Imported config outside pack directory");
		return load_mechvibes_pack(root / rel);
	}
	Pack p;
	const char *names[] = {"normal", "space", "enter", "backspace", "modifier", "special"};
	size_t total = 0;
	for (unsigned cat = 0; cat < categories; ++cat)
	{
		std::vector<std::filesystem::path> files;
		for (auto &e : std::filesystem::directory_iterator(dir))
			if (e.is_regular_file() && e.path().extension() == ".wav" && e.path().stem().string().starts_with(std::string(names[cat]) + "-"))
				files.push_back(e.path());
		std::sort(files.begin(), files.end());
		if (files.empty() || files.size() > 32)
			throw std::runtime_error("Each category needs 1..32 WAV variations");
		for (auto &path : files)
		{
			SF_INFO info{};
			std::unique_ptr<SNDFILE, decltype(&sf_close)> file(
				sf_open(path.c_str(), SFM_READ, &info), &sf_close);
			auto f = file.get();
			if (!f)
				throw std::runtime_error("Cannot decode sample: " + path.string());
			if (info.frames <= 0 || info.frames > info.samplerate / 2 || info.channels < 1 || info.channels > 2 || info.samplerate < 8000 || info.samplerate > 192000)
			{
				throw std::runtime_error(
					"Samples must be mono/stereo, 8..192 kHz, at most 0.5 seconds");
			}
			std::vector<float> raw(size_t(info.frames) * info.channels);
			auto read = sf_readf_float(f, raw.data(), info.frames);
			file.reset();
			if (read != info.frames)
				throw std::runtime_error("Truncated sample");
			std::vector<float> mono(info.frames);
			for (size_t i = 0; i < mono.size(); ++i)
			{
				for (int ch = 0; ch < info.channels; ++ch)
					mono[i] += raw[i * info.channels + ch] / info.channels;
				if (!std::isfinite(mono[i]))
					throw std::runtime_error("Non-finite sample");
			}
			float peak = 0;
			for (float x : mono)
				peak = std::max(peak, std::abs(x));
			if (peak < 1e-6f)
				throw std::runtime_error("Silent sample");
			size_t start = 0;
			while (start + 1 < mono.size() && std::abs(mono[start]) < peak * .002f)
				++start;
			unsigned count = unsigned((mono.size() - start) * double(rate) / info.samplerate);
			Sample s;
			s.data.resize(count + 1);
			for (unsigned i = 0; i < count; ++i)
			{
				double at = start + i * double(info.samplerate) / rate;
				auto a = size_t(at);
				float frac = at - a;
				s.data[i] = (mono[a] * (1 - frac) + mono[std::min(a + 1, mono.size() - 1)] * frac) * (.7f / peak);
			}
			s.data[count] = 0;
			total += s.data.size() * sizeof(float);
			if (total > 8 * 1024 * 1024)
				throw std::runtime_error("Pack exceeds 8 MiB decoded budget");
			p[cat].push_back(std::move(s));
		}
	}
	return p;
}

float Mixer::random()
{
	rng ^= rng << 13;
	rng ^= rng >> 17;
	rng ^= rng << 5;
	return float(rng >> 8) / 16777216.f;
}

void Mixer::reset()
{
	cycle_positions.fill(0);
	for (auto &v : voices)
		v = {};
	Event e;
	while (queue.pop(e))
	{
	};
}

void Mixer::render(float *out, unsigned frames)
{
	std::fill_n(out, size_t(frames) * 2, 0.f);
	metrics.quantum.store(frames, std::memory_order_relaxed);
	metrics.callbacks.fetch_add(1, std::memory_order_relaxed);
	Event event;
	for (unsigned consumed = 0; consumed < capacity && queue.pop(event); ++consumed)
	{
		if (!config.enabled || event.category >= categories)
			continue;
		const auto &routes = (event.key < KEY_CNT && !pack.keys[event.key].empty())
								 ? pack.keys[event.key]
								 : pack.category_samples[event.category];
		if (routes.empty() && pack[event.category].empty())
			continue;
		auto now = now_ns();
		auto delay = now >= event.timestamp ? now - event.timestamp : 0;
		if (delay > 250000000)
		{
			metrics.dropped.fetch_add(1, std::memory_order_relaxed);
			continue;
		}
		metrics.latency_sum.fetch_add(delay, std::memory_order_relaxed);
		auto prev = metrics.latency_max.load(std::memory_order_relaxed);
		if (delay > prev)
			metrics.latency_max.store(delay, std::memory_order_relaxed);
		const Sample *selected = nullptr;
		if (!routes.empty())
		{
			unsigned index = 0;
			if (event.key < KEY_CNT && pack.cycle_keys[event.key])
				index = cycle_positions[event.key]++ % routes.size();
			else if (config.randomize)
				index = std::min(unsigned(random() * routes.size()), unsigned(routes.size() - 1));
			selected = &pack.keyed_samples[routes[index]];
		}
		else
		{
			auto &variants = pack[event.category];
			unsigned index = config.randomize ? std::min(unsigned(random() * variants.size()), unsigned(variants.size() - 1)) : 0;
			selected = &variants[index];
		}
		Voice *slot = nullptr;
		for (unsigned i = 0; i < config.voices; ++i)
			if (!voices[i].sample)
			{
				slot = &voices[i];
				break;
			}
		if (!slot)
		{
			slot = &voices[0];
			for (unsigned i = 1; i < config.voices; ++i)
				if (voices[i].age < slot->age)
					slot = &voices[i];
			metrics.stolen.fetch_add(1, std::memory_order_relaxed);
		}
		*slot = {selected, 0, 1 + (random() * 2 - 1) * config.pitch, ++serial};
		metrics.played.fetch_add(1, std::memory_order_relaxed);
	}
	for (unsigned i = 0; i < config.voices; ++i)
	{
		auto &v = voices[i];
		if (!v.sample)
			continue;
		const auto &s = v.sample->data;
		for (unsigned f = 0; f < frames; ++f)
		{
			auto pos = size_t(v.pos);
			if (pos + 1 >= s.size())
			{
				v.sample = nullptr;
				break;
			}
			float a = float(v.pos - pos), x = (s[pos] * (1 - a) + s[pos + 1] * a) * config.volume;
			out[f * 2] += x;
			v.pos += v.step;
		}
	}
	// Smooth saturator has unity gain near zero and a strict peak bound.
	for (unsigned f = 0; f < frames; ++f)
	{
		float x = out[f * 2];
		float y = x / std::sqrt(1 + x * x);
		out[f * 2] = out[f * 2 + 1] = y;
	}
}
} // namespace ck
