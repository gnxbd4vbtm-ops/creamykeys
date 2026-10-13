#include "mechvibes.hpp"
#include <archive.h>
#include <archive_entry.h>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <linux/input-event-codes.h>
#include <new>
#include <unistd.h>
#include <vector>
static thread_local bool forbid_allocation = false;

void *operator new(std::size_t n)
{
	if (forbid_allocation)
		std::abort();
	if (void *p = std::malloc(n ? n : 1))
		return p;
	throw std::bad_alloc();
}

void operator delete(void *p) noexcept
{
	std::free(p);
}

void operator delete(void *p, std::size_t) noexcept
{
	std::free(p);
}

#define CHECK(x)                          \
	do                                    \
	{                                     \
		if (!(x))                         \
			throw std::runtime_error(#x); \
	} while (0)
namespace fs = std::filesystem;

static std::string contents(const fs::path &p)
{
	std::ifstream f(p, std::ios::binary);
	return {std::istreambuf_iterator<char>(f), {}};
}

static void zip(const fs::path &path, const std::vector<std::pair<std::string, std::string>> &entries)
{
	auto a = archive_write_new();
	archive_write_set_format_zip(a);
	CHECK(archive_write_open_filename(a, path.c_str()) == ARCHIVE_OK);
	for (auto &[name, data] : entries)
	{
		auto e = archive_entry_new();
		archive_entry_set_pathname(e, name.c_str());
		archive_entry_set_filetype(e, AE_IFREG);
		archive_entry_set_perm(e, 0644);
		archive_entry_set_size(e, data.size());
		CHECK(archive_write_header(a, e) == ARCHIVE_OK);
		CHECK(archive_write_data(a, data.data(), data.size()) == ssize_t(data.size()));
		archive_entry_free(e);
	}
	CHECK(archive_write_close(a) == ARCHIVE_OK);
	archive_write_free(a);
}

int main(int argc, char **argv)
{
	CHECK(argc == 2);
	auto normal = contents(fs::path(argv[1]) / "normal-1.wav"), enter = contents(fs::path(argv[1]) / "enter-1.wav");
	auto root = fs::temp_directory_path() / ("creamykeys-import-test-" + std::to_string(getpid()));
	fs::create_directories(root);

	struct Cleanup
	{
		fs::path p;

		~Cleanup()
		{
			std::error_code ec;
			fs::remove_all(p, ec);
		}
	} cleanup{root};

	auto multi = root / "multi.zip";
	zip(multi, {{"nested/config.json", R"({"name":"Fixture","key_define_type":"multi","sound":"unused.ogg","defines":{"30":"a.wav","28":"enter.wav","3612":"enter.wav","3613":"a.wav","57416":"enter.wav","31":null}})"}, {"nested/a.wav", normal}, {"nested/enter.wav", enter}, {"nested/LICENSE.txt", "Attribution fixture"}});
	auto name = ck::import_mechvibes_zip(multi, root / "packs");
	CHECK(name == ck::import_mechvibes_zip(multi, root / "packs"));
	auto p = ck::load_pack(root / "packs" / name);
	CHECK(p.keyed_samples.size() == 2);
	CHECK(p.keys[KEY_A].size() == 1);
	CHECK(p.keys[KEY_KPENTER] == p.keys[KEY_ENTER]);
	CHECK(p.keys[KEY_RIGHTCTRL] == p.keys[KEY_A]);
	CHECK(p.keys[KEY_UP] == p.keys[KEY_ENTER]);
	CHECK(p.keys[KEY_ENTER] != p.keys[KEY_A]);
	CHECK(fs::exists(root / "packs" / name / "source/nested/LICENSE.txt"));
	for (auto &f : p.category_samples)
		CHECK(!f.empty());
	ck::Mixer mixer;
	mixer.pack = std::move(p);
	mixer.config.pitch = 0;
	mixer.config.randomize = false;
	mixer.config.volume = 1;
	float audio[256]{};
	auto idx = mixer.pack.keys[KEY_A][0];
	CHECK(mixer.queue.push({0, ck::now_ns(), KEY_A}));
	forbid_allocation = true;
	mixer.render(audio, 128);
	forbid_allocation = false;
	for (unsigned i = 0; i < 128; ++i)
	{
		float x = mixer.pack.keyed_samples[idx].data[i];
		CHECK(std::abs(audio[i * 2] - x / std::sqrt(1 + x * x)) < .00001f);
	}
	CHECK(mixer.metrics.played == 1);
	auto single = root / "single.zip";
	zip(single, {{"config.json", R"({"key_define_type":"single","sound":"shared.wav","defines":{"30":[0,50],"48":[50,50],"28":[0,50],"91":null}})"}, {"shared.wav", normal}});
	name = ck::import_mechvibes_zip(single, root / "packs");
	p = ck::load_pack(root / "packs" / name);
	CHECK(p.keyed_samples.size() == 2);
	CHECK(p.keys[KEY_A] == p.keys[KEY_ENTER]);
	CHECK(p.keys[KEY_A] != p.keys[KEY_B]);
	CHECK(p.keyed_samples[0].data.size() <= 2401);
	auto dx = root / "dx.zip";
	zip(dx, {{"config.json", R"({"audio_file":"audio.wav","definitions":{"KeyA":{"timing":[[0,50],[50,100]]},"Space":{"timing":[[0,50]]}}})"}, {"audio.wav", normal}});
	name = ck::import_mechvibes_zip(dx, root / "packs");
	p = ck::load_pack(root / "packs" / name);
	CHECK(p.keys[KEY_A].size() == 2);
	CHECK(p.keys[KEY_SPACE].size() == 1);
	auto ranges = root / "ranges.zip";
	zip(ranges, {{"config.json", R"({"version":2,"key_define_type":"multi","sound":"a-{0-1}.wav","defines":{"30":"a-{0-1}.wav","30-up":"not-loaded.wav"}})"}, {"a-0.wav", normal}, {"a-1.wav", enter}});
	name = ck::import_mechvibes_zip(ranges, root / "packs");
	p = ck::load_pack(root / "packs" / name);
	CHECK(p.keys[KEY_A].size() == 2);
	auto cycling = root / "cycling.zip";
	zip(cycling, {{"config.json", R"({"version":3,"sounds":{"cycle":{"files":["a.wav","enter.wav"],"mode":"cycle"}},"defines":{"30":["cycle"]}})"}, {"a.wav", normal}, {"enter.wav", enter}});
	name = ck::import_mechvibes_zip(cycling, root / "packs");
	p = ck::load_pack(root / "packs" / name);
	CHECK(p.cycle_keys[KEY_A] && p.keys[KEY_A].size() == 2);
	mixer.reset();
	mixer.pack = std::move(p);
	mixer.config.voices = 1;
	for (unsigned press = 0; press < 3; ++press)
	{
		CHECK(mixer.queue.push({0, ck::now_ns(), KEY_A}));
		forbid_allocation = true;
		mixer.render(audio, 128);
		forbid_allocation = false;
		auto selected = mixer.pack.keys[KEY_A][press % 2];
		for (unsigned i = 0; i < 128; ++i)
		{
			float x = mixer.pack.keyed_samples[selected].data[i];
			CHECK(std::abs(audio[i * 2] - x / std::sqrt(1 + x * x)) < .00001f);
		}
	}
	auto bad = root / "bad.zip";
	zip(bad, {{"../escape", "bad"}, {"config.json", "{}"}});
	bool rejected = false;
	try
	{
		ck::import_mechvibes_zip(bad, root / "packs");
	}
	catch (const std::exception &)
	{
		rejected = true;
	}
	CHECK(rejected);
	CHECK(!fs::exists(root / "escape"));
	for (auto &e : fs::directory_iterator(root / "packs"))
		CHECK(!e.path().filename().string().starts_with(".import-"));
	auto invalid = root / "invalid-timing.zip";
	zip(invalid, {{"config.json", R"({"key_define_type":"single","sound":"audio.wav","defines":{"30":[0,-1]}})"}, {"audio.wav", normal}});
	rejected = false;
	try
	{
		ck::import_mechvibes_zip(invalid, root / "packs");
	}
	catch (const std::exception &)
	{
		rejected = true;
	}
	CHECK(rejected);
	CHECK(ck::mechvibes_key("57424") == KEY_DOWN);
	CHECK(ck::mechvibes_key("61000") == KEY_KP8);
	CHECK(ck::mechvibes_key("ControlRight") == KEY_RIGHTCTRL);
	CHECK(ck::mechvibes_key("F24") == KEY_F24);
	std::cout << "ZIP layout/cache/attribution, traversal rejection, legacy scan codes, key-specific allocation-free rendering, single clips, multi files, DX timing, filename ranges and release exclusion passed\n";
}
