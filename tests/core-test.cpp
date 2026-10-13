#include "core.hpp"
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <linux/input-event-codes.h>
#include <new>
#include <stdexcept>
#include <thread>
static thread_local bool forbid_allocation = false;

void *operator new(std::size_t size)
{
	if (forbid_allocation)
		std::abort();
	if (void *p = std::malloc(size ? size : 1))
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

int main(int argc, char **argv)
{
	CHECK(argc == 2);
	ck::Queue q;
	for (unsigned i = 0; i < ck::capacity - 1; ++i)
		CHECK(q.push({i, i}));
	CHECK(!q.push({0, 0}));
	ck::Event e;
	for (unsigned i = 0; i < ck::capacity - 1; ++i)
	{
		CHECK(q.pop(e));
		CHECK(e.timestamp == i);
	}
	CHECK(!q.pop(e));
	std::thread producer([&]
						 {
        for (unsigned i = 0; i < 200000; ++i)
            while (!q.push({0, i}))
                std::this_thread::yield(); });
	for (unsigned i = 0; i < 200000; ++i)
	{
		while (!q.pop(e))
			std::this_thread::yield();
		CHECK(e.timestamp == i);
	}
	producer.join();
	ck::Mixer mixer;
	mixer.pack = ck::load_pack(argv[1]);
	mixer.config.voices = 2;
	CHECK(ck::category(KEY_A) == 0);
	CHECK(ck::category(KEY_SPACE) == 1);
	CHECK(ck::category(KEY_ENTER) == 2);
	CHECK(ck::category(KEY_BACKSPACE) == 3);
	CHECK(ck::category(KEY_LEFTSHIFT) == 4);
	CHECK(ck::category(KEY_F1) == 5);
	for (unsigned c = 0; c < 6; ++c)
		CHECK(mixer.queue.push({c, ck::now_ns()}));
	float out[512]{};
	forbid_allocation = true;
	mixer.render(out, 256);
	forbid_allocation = false;
	CHECK(mixer.metrics.played == 6);
	CHECK(mixer.metrics.stolen == 4);
	bool audible = false;
	for (float f : out)
	{
		CHECK(std::isfinite(f) && std::abs(f) <= 1);
		audible |= std::abs(f) > .001;
	}
	CHECK(audible);
	mixer.reset();
	mixer.render(out, 256);
	for (float f : out)
		CHECK(f == 0);
	CHECK(mixer.queue.push({0, ck::now_ns() - 1000000000}));
	mixer.render(out, 256);
	CHECK(mixer.metrics.dropped == 1);
	auto path = std::filesystem::temp_directory_path() / "creamykeys-config-test.toml";
	ck::Config c;
	c.enabled = false;
	c.volume = .7;
	c.idle_ms = 0;
	ck::save_config(path, c);
	auto loaded = ck::load_config(path);
	CHECK(!loaded.enabled && loaded.volume == c.volume && loaded.idle_ms == 0);
	{
		std::ofstream invalid(path);
		invalid << "volume = nan\n";
	}
	bool rejected = false;
	try
	{
		(void)ck::load_config(path);
	}
	catch (const std::exception &)
	{
		rejected = true;
	}
	CHECK(rejected);
	std::filesystem::remove(path);
	std::cout << "Queue concurrency, category routing, voice stealing, clipping, stale-event "
				 "rejection, silence, pack decoding and config roundtrip passed\n";
}
