#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <linux/input-event-codes.h>
#include <string>
#include <vector>

namespace ck
{
constexpr unsigned rate = 48000, categories = 6, capacity = 512, voice_capacity = 64;
static_assert(std::atomic<unsigned>::is_always_lock_free);
static_assert(std::atomic<uint64_t>::is_always_lock_free);

struct Config
{
	bool enabled = true, randomize = true, repeat = false, follow = true,
	     voice_chat = true, mix_mic = true, mic_only = true;
	float volume = .35f, pitch = .025f;
	unsigned voices = 24, idle_ms = 5000;
	std::string pack = "creamy";
};

Config load_config(const std::filesystem::path &);
void save_config(const std::filesystem::path &, const Config &);
unsigned category(unsigned code);

struct Sample
{
	std::vector<float> data;
};

struct Pack : std::array<std::vector<Sample>, categories>
{
	// Imported packs retain exact key-to-sample mappings and share repeated clips.
	std::vector<Sample> keyed_samples;
	std::array<std::vector<unsigned>, KEY_CNT> keys;
	std::array<std::vector<unsigned>, categories> category_samples;
	std::array<bool, KEY_CNT> cycle_keys{};
};

Pack load_pack(const std::filesystem::path &);
uint64_t now_ns();

struct Event
{
	unsigned category;
	uint64_t timestamp;
	unsigned key = 0;
};

class Queue
{
	alignas(64) std::atomic<unsigned> head{0};
	alignas(64) std::atomic<unsigned> tail{0};
	std::array<Event, capacity> data{};

  public:
	bool push(Event e)
	{
		auto h = head.load(std::memory_order_relaxed);
		auto n = (h + 1) % capacity;
		if (n == tail.load(std::memory_order_acquire))
			return false;
		data[h] = e;
		head.store(n, std::memory_order_release);
		return true;
	}

	bool pop(Event &e)
	{
		auto t = tail.load(std::memory_order_relaxed);
		if (t == head.load(std::memory_order_acquire))
			return false;
		e = data[t];
		tail.store((t + 1) % capacity, std::memory_order_release);
		return true;
	}
};

struct Metrics
{
	std::atomic<uint64_t> played{0}, dropped{0}, stolen{0}, callbacks{0}, latency_sum{0},
		latency_max{0}, empty_buffers{0};
	std::atomic<unsigned> quantum{0};
};

class Mixer
{
	struct Voice
	{
		const Sample *sample = nullptr;
		double pos = 0, step = 1;
		uint64_t age = 0;
	};

	std::array<Voice, voice_capacity> voices{};
	uint64_t serial = 0;
	uint32_t rng = 0x81d92b7;
	std::array<unsigned, KEY_CNT> cycle_positions{};
	float random();

  public:
	Config config;
	Pack pack;
	Queue queue;
	Metrics metrics;
	void reset(); // only while the PipeWire data loop is stopped
	void render(float *stereo, unsigned frames);
};
} // namespace ck
