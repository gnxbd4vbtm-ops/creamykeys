#include "core.hpp"
#include "input.hpp"
#include "mechvibes.hpp"
#include <algorithm>
#include <bitset>
#include <cmath>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <libevdev/libevdev.h>
#include <map>
#include <memory>
#include <pipewire/extensions/metadata.h>
#include <pipewire/pipewire.h>
#include <regex>
#include <signal.h>
#include <spa/param/audio/format-utils.h>
#include <sstream>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/file.h>
#include <sys/inotify.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
using namespace ck;
namespace fs = std::filesystem;

struct FD
{
	int n = -1;
	FD() = default;
	explicit FD(int x) : n(x) {};

	~FD()
	{
		if (n >= 0)
			close(n);
	}

	FD(const FD &) = delete;
	FD &operator=(const FD &) = delete;

	FD(FD &&o) noexcept : n(o.n)
	{
		o.n = -1;
	}

	FD &operator=(FD &&o) noexcept
	{
		if (n >= 0)
			close(n);
		n = o.n;
		o.n = -1;
		return *this;
	}

	operator int() const
	{
		return n;
	}
};

static fs::path home()
{
	auto p = getenv("HOME");
	if (!p)
		throw std::runtime_error("HOME is missing");
	return p;
}

static fs::path config_path()
{
	auto p = getenv("XDG_CONFIG_HOME");
	return (p ? fs::path(p) : home() / ".config") / "creamykeys/config.toml";
}

static fs::path data_dir()
{
	auto p = getenv("XDG_DATA_HOME");
	return (p ? fs::path(p) : home() / ".local/share") / "creamykeys/packs";
}

static fs::path pack_path(const std::string &name)
{
	auto user = data_dir() / name;
	if (fs::is_directory(user))
		return user;
	auto global = fs::path("/usr/local/share/creamykeys/packs") / name;
	if (fs::is_directory(global))
		return global;
	return fs::path("/usr/share/creamykeys/packs") / name;
}

static fs::path runtime()
{
	auto p = getenv("XDG_RUNTIME_DIR");
	if (!p)
		throw std::runtime_error("XDG_RUNTIME_DIR missing; run inside your user session");
	return fs::path(p) / "creamykeys";
}

static void arm(int fd, unsigned milliseconds, unsigned interval = 0)
{
	itimerspec t{};
	t.it_value.tv_sec = milliseconds / 1000;
	t.it_value.tv_nsec = (milliseconds % 1000) * 1000000;
	t.it_interval.tv_sec = interval / 1000;
	t.it_interval.tv_nsec = (interval % 1000) * 1000000;
	if (timerfd_settime(fd, 0, &t, nullptr) < 0)
		throw std::runtime_error("timerfd_settime failed");
}

static void drain(int fd)
{
	uint64_t v;
	while (read(fd, &v, sizeof v) > 0)
	{
	}
}

struct Audio
{
	Mixer &mixer;
	int notify;
	pw_thread_loop *loop = nullptr;
	pw_context *context = nullptr;
	pw_core *core = nullptr;
	pw_stream *stream = nullptr;
	pw_registry *registry = nullptr;
	pw_metadata *metadata = nullptr;
	spa_hook stream_hook{}, core_hook{}, registry_hook{}, metadata_hook{};
	std::string sink;
	uint32_t metadata_id = SPA_ID_INVALID;
	bool follow = true;
	std::atomic<int> state{PW_STREAM_STATE_UNCONNECTED};

	static void process(void *userdata)
	{
		auto &a = *static_cast<Audio *>(userdata);
		auto b = pw_stream_dequeue_buffer(a.stream);
		if (!b)
		{
			a.mixer.metrics.empty_buffers.fetch_add(1, std::memory_order_relaxed);
			return;
		}
		auto &d = b->buffer->datas[0];
		if (d.data)
		{
			unsigned frames =
				std::min(uint64_t(d.maxsize / (sizeof(float) * 2)),
						 b->requested ? b->requested : uint64_t(d.maxsize / (sizeof(float) * 2)));
			a.mixer.render(static_cast<float *>(d.data), frames);
			d.chunk->offset = 0;
			d.chunk->stride = sizeof(float) * 2;
			d.chunk->size = frames * sizeof(float) * 2;
			b->size = frames;
		}
		pw_stream_queue_buffer(a.stream, b);
	}

	void signal()
	{
		uint64_t one = 1;
		(void)write(notify, &one, sizeof one);
	}

	static void changed(void *p, pw_stream_state, pw_stream_state s, const char *)
	{
		auto &a = *static_cast<Audio *>(p);
		a.state.store(s);
		if (s == PW_STREAM_STATE_ERROR || s == PW_STREAM_STATE_UNCONNECTED)
			a.signal();
		if (s == PW_STREAM_STATE_PAUSED)
			a.route();
	}

	static void error(void *p, uint32_t id, int, int res, const char *)
	{
		if (id == PW_ID_CORE && res < 0)
		{
			auto &a = *static_cast<Audio *>(p);
			a.state.store(PW_STREAM_STATE_ERROR);
			a.signal();
		}
	}

	void route()
	{
		if (!metadata || !stream || sink.empty() || !follow)
			return;
		auto id = pw_stream_get_node_id(stream);
		if (id != SPA_ID_INVALID)
			pw_metadata_set_property(metadata, id, "target.object", "Spa:Id", sink.c_str());
	}

	static int property(void *p, uint32_t subject, const char *key, const char *, const char *value)
	{
		auto &a = *static_cast<Audio *>(p);
		if (subject == PW_ID_CORE && key && std::strcmp(key, "default.audio.sink") == 0)
		{
			a.sink.clear();
			if (value)
			{
				std::cmatch match;
				static const std::regex re("\\\"name\\\"\\s*:\\s*\\\"([^\\\"]+)\\\"");
				if (std::regex_search(value, match, re))
					a.sink = match[1];
			}
			a.route();
		}
		return 0;
	}

	static void global(void *p, uint32_t id, uint32_t, const char *type, uint32_t version, const spa_dict *props)
	{
		auto &a = *static_cast<Audio *>(p);
		if (std::strcmp(type, PW_TYPE_INTERFACE_Metadata) != 0 || a.metadata)
			return;
		const char *name = props ? spa_dict_lookup(props, PW_KEY_METADATA_NAME) : nullptr;
		if (!name || std::strcmp(name, "default"))
			return;
		a.metadata_id = id;
		a.metadata = static_cast<pw_metadata *>(pw_registry_bind(
			a.registry, id, type, std::min(version, uint32_t(PW_VERSION_METADATA)), 0));
		if (a.metadata)
		{
			static const pw_metadata_events events = {.version = PW_VERSION_METADATA_EVENTS,
													  .property = property};
			pw_metadata_add_listener(a.metadata, &a.metadata_hook, &events, &a);
		}
	}

	static void removed(void *p, uint32_t id)
	{
		auto &a = *static_cast<Audio *>(p);
		if (id == a.metadata_id && a.metadata)
		{
			spa_hook_remove(&a.metadata_hook);
			pw_proxy_destroy(reinterpret_cast<pw_proxy *>(a.metadata));
			a.metadata = nullptr;
			a.metadata_id = SPA_ID_INVALID;
			a.sink.clear();
		}
	}

	Audio(Mixer &m, int fd) : mixer(m), notify(fd)
	{
		try
		{
			loop = pw_thread_loop_new("creamykeys", nullptr);
			if (!loop)
				throw std::runtime_error("PipeWire loop allocation failed");
			context = pw_context_new(pw_thread_loop_get_loop(loop), nullptr, 0);
			if (!context)
				throw std::runtime_error("PipeWire context failed");
			core = pw_context_connect(context, nullptr, 0);
			if (!core)
				throw std::runtime_error("PipeWire unavailable");
			static const pw_core_events ce = []
			{
				pw_core_events e{};
				e.version = PW_VERSION_CORE_EVENTS;
				e.error = error;
				return e;
			}();
			pw_core_add_listener(core, &core_hook, &ce, this);
			registry = pw_core_get_registry(core, PW_VERSION_REGISTRY, 0);
			static const pw_registry_events re = {
				.version = PW_VERSION_REGISTRY_EVENTS, .global = global, .global_remove = removed};
			pw_registry_add_listener(registry, &registry_hook, &re, this);
			auto props = pw_properties_new(
				PW_KEY_APP_NAME, "CreamyKeys", PW_KEY_NODE_NAME, "creamykeys", PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Playback", PW_KEY_MEDIA_ROLE, "Game", PW_KEY_NODE_LATENCY, "128/48000", "node.stream.restore-target", "false", "node.dont-move", "false", nullptr);
			stream = pw_stream_new(core, "CreamyKeys", props);
			if (!stream)
				throw std::runtime_error("PipeWire stream failed");
			static const pw_stream_events se = []
			{
				pw_stream_events e{};
				e.version = PW_VERSION_STREAM_EVENTS;
				e.state_changed = changed;
				e.process = process;
				return e;
			}();
			pw_stream_add_listener(stream, &stream_hook, &se, this);
			uint8_t buffer[1024];
			spa_pod_builder builder = SPA_POD_BUILDER_INIT(buffer, sizeof buffer);
			spa_audio_info_raw info{};
			info.format = SPA_AUDIO_FORMAT_F32;
			info.rate = rate;
			info.channels = 2;
			info.position[0] = SPA_AUDIO_CHANNEL_FL;
			info.position[1] = SPA_AUDIO_CHANNEL_FR;
			const spa_pod *params[] = {
				spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &info)};
			auto flags = pw_stream_flags(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS | PW_STREAM_FLAG_INACTIVE);
			if (pw_stream_connect(stream, PW_DIRECTION_OUTPUT, PW_ID_ANY, flags, params, 1) < 0)
				throw std::runtime_error("PipeWire stream connect failed");
			if (pw_thread_loop_start(loop) < 0)
				throw std::runtime_error("PipeWire thread failed");
		}
		catch (...)
		{
			cleanup();
			throw;
		}
	}

	void cleanup()
	{
		if (loop)
			pw_thread_loop_stop(loop);
		if (metadata)
		{
			spa_hook_remove(&metadata_hook);
			pw_proxy_destroy(reinterpret_cast<pw_proxy *>(metadata));
			metadata = nullptr;
		}
		if (registry)
		{
			spa_hook_remove(&registry_hook);
			pw_proxy_destroy(reinterpret_cast<pw_proxy *>(registry));
			registry = nullptr;
		}
		if (stream)
		{
			spa_hook_remove(&stream_hook);
			pw_stream_destroy(stream);
			stream = nullptr;
		}
		if (core)
		{
			spa_hook_remove(&core_hook);
			pw_core_disconnect(core);
			core = nullptr;
		}
		if (context)
		{
			pw_context_destroy(context);
			context = nullptr;
		}
		if (loop)
		{
			pw_thread_loop_destroy(loop);
			loop = nullptr;
		}
	}

	~Audio()
	{
		cleanup();
	}

	void active(bool on)
	{
		pw_thread_loop_lock(loop);
		pw_stream_set_active(stream, on);
		pw_thread_loop_unlock(loop);
	}

	void update(const Config &c, Pack *pack = nullptr)
	{
		// The RT process callback runs on the data loop, independently of the
		// thread-loop lock. Swap prebuilt state on that loop with a blocking invoke.
		// Old strings/samples are destroyed by the caller after the invoke returns.
		struct Update
		{
			Mixer *mixer;
			Config config;
			Pack *pack;
		} update{&mixer, c, pack};

		pw_thread_loop_lock(loop);
		pw_stream_set_active(stream, false);
		auto apply = [](spa_loop *, bool, uint32_t, const void *, size_t, void *userdata) -> int
		{
			auto &u = *static_cast<Update *>(userdata);
			u.mixer->reset();
			std::swap(u.mixer->config, u.config);
			if (u.pack)
				std::swap(u.mixer->pack, *u.pack);
			return 0;
		};
		auto data_loop = pw_stream_get_data_loop(stream);
		if (!data_loop)
		{
			pw_thread_loop_unlock(loop);
			throw std::runtime_error("PipeWire data loop unavailable");
		}
		int res = pw_loop_invoke(data_loop, apply, 0, nullptr, 0, true, &update);
		follow = c.follow;
		const spa_dict_item item = {"node.dont-move", follow ? "false" : "true"};
		spa_dict dict = SPA_DICT_INIT(&item, 1);
		pw_stream_update_properties(stream, &dict);
		route();
		pw_thread_loop_unlock(loop);
		if (res < 0)
			throw std::runtime_error("PipeWire state swap failed");
	}
};

struct Device
{
	FD fd;
	libevdev *ev = nullptr;
	std::string path, identity;
	bool virtual_device = false;
	std::bitset<KEY_CNT> held;

	Device(FD f, libevdev *e, std::string p, std::string ident, bool virt)
		: fd(std::move(f)), ev(e), path(std::move(p)), identity(std::move(ident)), virtual_device(virt) {}

	~Device()
	{
		libevdev_free(ev);
	}
};

struct Daemon
{
	FD ep{epoll_create1(EPOLL_CLOEXEC)}, lock, server, signals, watch, hotplug, idle, retry, bench,
		notify;
	std::map<int, std::unique_ptr<Device>> devices;
	std::map<int, FD> clients;

	struct Last
	{
		unsigned code = 0;
		int64_t timestamp = 0;
	};

	std::map<std::string, Last> last;
	Mixer mixer;
	Config config;
	std::unique_ptr<Audio> audio;
	bool running = true, audio_active = false, benchmarking = false;
	unsigned bench_ticks = 0, bench_total = 500, bench_categories = 6;
	unsigned denied = 0;
	fs::path socket_path;

	void add(int fd)
	{
		if (fd < 0)
			throw std::runtime_error("File descriptor creation failed");
		epoll_event e{};
		e.events = EPOLLIN;
		e.data.fd = fd;
		if (epoll_ctl(ep, EPOLL_CTL_ADD, fd, &e) < 0)
			throw std::runtime_error("epoll add failed");
	}

	void del(int fd)
	{
		epoll_ctl(ep, EPOLL_CTL_DEL, fd, nullptr);
	}

	void scan()
	{
		if (!config.enabled || benchmarking)
			return;
		std::vector<std::string> paths;
		std::error_code ec;
		for (auto &e : fs::directory_iterator("/dev/input", ec))
			if (e.path().filename().string().starts_with("event"))
				paths.push_back(e.path());
		for (auto it = devices.begin(); it != devices.end();)
		{
			if (std::find(paths.begin(), paths.end(), it->second->path) == paths.end())
			{
				del(it->first);
				it = devices.erase(it);
			}
			else
				++it;
		}
		denied = 0;
		for (auto &p : paths)
		{
			bool known = false;
			for (auto &[fd, d] : devices)
			{
				(void)fd;
				if (d->path == p)
					known = true;
			}
			if (known)
				continue;
			FD fd(open(p.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC));
			if (fd.n < 0)
			{
				if (errno == EACCES)
					++denied;
				continue;
			}
			libevdev *ev = nullptr;
			if (libevdev_new_from_fd(fd, &ev) < 0)
				continue;
			if (!keyboard_device(ev))
			{
				libevdev_free(ev);
				continue;
			}
			libevdev_set_clock_id(ev, CLOCK_MONOTONIC);
			const char *phys = libevdev_get_phys(ev);
			const char *uniq = libevdev_get_uniq(ev);
			std::string identity = input_identity(phys, uniq, p);
			bool virtual_device = virtual_input(p) || libevdev_get_id_bustype(ev) == BUS_VIRTUAL;
			int n = fd;
			add(n);
			devices.emplace(n, std::make_unique<Device>(std::move(fd), ev, p, identity, virtual_device));
		}
	}

	void close_devices()
	{
		for (auto &[fd, d] : devices)
		{
			(void)d;
			del(fd);
		}
		devices.clear();
		last.clear();
	}

	void connect_audio()
	{
		audio.reset();
		mixer.reset();
		audio_active = false;
		try
		{
			audio = std::make_unique<Audio>(mixer, notify);
			audio->update(config);
			audio_active = config.enabled && config.idle_ms == 0;
			if (audio_active)
				audio->active(true);
			arm(retry, 0);
		}
		catch (const std::exception &e)
		{
			audio.reset();
			mixer.reset();
			audio_active = false;
			std::cerr << e.what() << "; retrying\n";
			arm(retry, 2000);
		}
	}

	void emit(unsigned cat, uint64_t timestamp, unsigned key = 0)
	{
		if (!audio || !config.enabled)
			return;
		if (!mixer.queue.push({cat, timestamp, key}))
		{
			mixer.metrics.dropped.fetch_add(1);
			return;
		}
		if (!audio_active)
		{
			audio->active(true);
			audio_active = true;
		}
		arm(idle, config.idle_ms);
	}

	void input(int fd)
	{
		auto it = devices.find(fd);
		if (it == devices.end())
			return;
		auto &d = *it->second;
		input_event e{};
		for (;;)
		{
			int rc = libevdev_next_event(d.ev, LIBEVDEV_READ_FLAG_NORMAL, &e);
			if (rc == -EAGAIN)
				break;
			if (rc == LIBEVDEV_READ_STATUS_SYNC)
			{
				while (libevdev_next_event(d.ev, LIBEVDEV_READ_FLAG_SYNC, &e) >= 0)
				{
				}
				d.held.reset();
				continue;
			}
			if (rc < 0)
			{
				del(fd);
				devices.erase(fd);
				break;
			}
			if (e.type != EV_KEY || !keyboard_event(e.code))
				continue;
			// Virtual macro devices often inject KEY_2. Ignore only that key
			// from virtual devices so physical keyboards still play normally.
			if (d.virtual_device && e.code == KEY_2)
				continue;
			if (e.value == 0)
			{
				d.held.reset(e.code);
				continue;
			}
			if (e.value == 2 && !config.repeat)
				continue;
			if (e.value == 1 && d.held.test(e.code))
				continue;
			d.held.set(e.code);
			int64_t timestamp = int64_t(e.time.tv_sec) * 1000000000 + e.time.tv_usec * 1000;
			auto &previous = last[d.identity];
			if (previous.code == e.code && std::abs(timestamp - previous.timestamp) < 2000000)
				continue;
			previous = {e.code, timestamp};
			emit(category(e.code), timestamp > 0 ? uint64_t(timestamp) : now_ns(), e.code);
		}
	}

	void reload()
	{
		auto next = load_config(config_path());
		Pack p;
		bool changed = next.pack != config.pack;
		if (changed)
			p = load_pack(pack_path(next.pack));
		if (audio)
			audio->update(next, changed ? &p : nullptr);
		else
		{
			mixer.reset();
			if (changed)
				mixer.pack = std::move(p);
			mixer.config = next;
		}
		audio_active = false;
		config = next;
		arm(idle, 0);
		if (audio && config.enabled && config.idle_ms == 0)
		{
			audio->active(true);
			audio_active = true;
		}
		close_devices();
		scan();
	}

	std::string status()
	{
		auto &m = mixer.metrics;
		auto n = m.played.load();
		std::ostringstream s;
		s << "enabled=" << std::boolalpha << config.enabled << " keyboards=" << devices.size()
		  << " inaccessible_devices=" << denied << " volume=" << config.volume
		  << " pack=" << config.pack << " idle_timeout_ms=" << config.idle_ms
		  << " audio_state=" << (audio ? audio->state.load() : -1)
		  << " audio_active=" << audio_active << " benchmark=" << benchmarking << "\nplayed=" << n
		  << " queue_drops=" << m.dropped.load() << " voice_steals=" << m.stolen.load()
		  << " callbacks=" << m.callbacks.load() << " empty_buffers=" << m.empty_buffers.load()
		  << " last_quantum=" << m.quantum.load() << " frames @48000 Hz\ninput_to_callback_mean_ms="
		  << (n ? double(m.latency_sum.load()) / n / 1e6 : 0)
		  << " max_ms=" << double(m.latency_max.load()) / 1e6
		  << " (software only; synthetic benchmark timestamps are enqueue times)\n";
		return s.str();
	}

	std::string command(const std::string &text)
	{
		std::istringstream s(text);
		std::string cmd, arg, extra;
		s >> cmd >> arg >> extra;
		if (!extra.empty())
			throw std::runtime_error("Too many arguments");
		if (cmd == "status")
			return status();
		if (cmd == "quit")
		{
			running = false;
			return "Stopping\n";
		}
		if (cmd == "reload")
		{
			reload();
			return "Reloaded\n";
		}
		if (cmd == "benchmark")
		{
			if (!config.enabled || !audio)
				throw std::runtime_error("Enable daemon and connect PipeWire first");
			if (benchmarking)
				throw std::runtime_error("Benchmark already running");
			if (arg != "" && arg != "burst" && arg != "typing")
				throw std::runtime_error("Benchmark mode must be typing or burst");
			close_devices();
			benchmarking = true;
			bench_ticks = 0;
			bench_total = arg == "typing" ? 50 : 500;
			bench_categories = arg == "typing" ? 1 : 6;
			unsigned interval = arg == "typing" ? 100 : 10;
			arm(bench, interval, interval);
			return std::string("Synthetic ") + (arg == "typing" ? "typing (10 events/s)" : "burst (600 category events/s)") + " for 5 seconds; keyboard capture paused. Run status after completion.\n";
		}
		auto next = config;
		if (cmd == "enable")
			next.enabled = true;
		else if (cmd == "disable")
			next.enabled = false;
		else if (cmd == "toggle")
			next.enabled = !next.enabled;
		else if (cmd == "volume")
		{
			size_t pos = 0;
			next.volume = std::stof(arg, &pos);
			if (pos != arg.size() || !std::isfinite(next.volume) || next.volume < 0 || next.volume > 1)
				throw std::runtime_error("Volume must be 0..1");
		}
		else if (cmd == "pack")
		{
			if (arg.empty() || arg.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") != arg.npos)
				throw std::runtime_error("Invalid pack name");
			next.pack = arg;
		}
		else
			throw std::runtime_error("Unknown command");
		if (next.pack != config.pack)
		{
			auto checked = load_pack(pack_path(next.pack));
			(void)checked;
		}
		save_config(config_path(), next);
		reload();
		return status();
	}

	Daemon()
	{
		config = load_config(config_path());
		mixer.config = config;
		mixer.pack = load_pack(pack_path(config.pack));
		auto rt = runtime();
		fs::create_directories(rt);
		fs::permissions(rt, fs::perms::owner_all);
		lock = FD(open((rt / "daemon.lock").c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600));
		if (lock.n < 0 || flock(lock, LOCK_EX | LOCK_NB) < 0)
			throw std::runtime_error("Daemon already running or lock unavailable");
		socket_path = rt / "control.sock";
		if (socket_path.string().size() >= sizeof(sockaddr_un::sun_path))
			throw std::runtime_error("Runtime path too long");
		unlink(socket_path.c_str());
		server = FD(socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
		sockaddr_un address{};
		address.sun_family = AF_UNIX;
		std::strcpy(address.sun_path, socket_path.c_str());
		if (bind(server, reinterpret_cast<sockaddr *>(&address), sizeof address) < 0 || listen(server, 16) < 0)
			throw std::runtime_error("Cannot create control socket");
		chmod(socket_path.c_str(), 0600);
		add(server);
		sigset_t mask;
		sigemptyset(&mask);
		sigaddset(&mask, SIGTERM);
		sigaddset(&mask, SIGINT);
		sigaddset(&mask, SIGHUP);
		pthread_sigmask(SIG_BLOCK, &mask, nullptr);
		signals = FD(signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC));
		add(signals);
		watch = FD(inotify_init1(IN_NONBLOCK | IN_CLOEXEC));
		if (inotify_add_watch(watch, config_path().parent_path().c_str(), IN_CLOSE_WRITE | IN_MOVED_TO) < 0)
			throw std::runtime_error("Cannot watch config directory");
		add(watch);
		hotplug = FD(inotify_init1(IN_NONBLOCK | IN_CLOEXEC));
		if (inotify_add_watch(hotplug, "/dev/input", IN_CREATE | IN_DELETE | IN_ATTRIB) < 0)
			throw std::runtime_error("Cannot watch keyboards");
		add(hotplug);
		idle = FD(timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC));
		retry = FD(timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC));
		bench = FD(timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC));
		notify = FD(eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC));
		add(idle);
		add(retry);
		add(bench);
		add(notify);
		connect_audio();
		scan();
		std::cerr << "CreamyKeys ready; keyboards=" << devices.size() << "\n";
	}

	~Daemon()
	{
		audio.reset();
		if (!socket_path.empty())
			unlink(socket_path.c_str());
	}

	void run()
	{
		std::array<epoll_event, 32> events;
		while (running)
		{
			int count = epoll_wait(ep, events.data(), events.size(), -1);
			if (count < 0)
			{
				if (errno == EINTR)
					continue;
				throw std::runtime_error("epoll_wait failed");
			}
			for (int i = 0; i < count; ++i)
			{
				int fd = events[i].data.fd;
				try
				{
					if (fd == signals)
					{
						signalfd_siginfo si{};
						while (read(fd, &si, sizeof si) > 0)
						{
							if (si.ssi_signo == SIGHUP)
								reload();
							else
								running = false;
						}
					}
					else if (fd == watch || fd == hotplug)
					{
						alignas(inotify_event) char buf[8192];
						bool relevant = false;
						ssize_t n;
						while ((n = read(fd, buf, sizeof buf)) > 0)
							for (size_t at = 0; at < size_t(n);)
							{
								auto *e = reinterpret_cast<inotify_event *>(buf + at);
								if (fd == hotplug || (e->len && std::strcmp(e->name, "config.toml") == 0))
									relevant = true;
								at += sizeof(*e) + e->len;
							}
						if (relevant)
						{
							if (fd == watch)
								reload();
							else
								scan();
						}
					}
					else if (fd == idle)
					{
						drain(fd);
						if (audio)
							audio->active(false);
						audio_active = false;
					}
					else if (fd == retry)
					{
						drain(fd);
						connect_audio();
					}
					else if (fd == notify)
					{
						drain(fd);
						if (audio && audio->state.load() <= PW_STREAM_STATE_UNCONNECTED)
						{
							audio.reset();
							mixer.reset();
							audio_active = false;
							arm(retry, 1000);
						}
					}
					else if (fd == bench)
					{
						uint64_t ticks = 0;
						(void)read(fd, &ticks, sizeof ticks);
						for (uint64_t t = 0; t < ticks && bench_ticks < bench_total; ++t)
						{
							for (unsigned c = 0; c < bench_categories; ++c)
							{
								static constexpr unsigned keys[] = {KEY_A, KEY_SPACE, KEY_ENTER, KEY_BACKSPACE, KEY_LEFTSHIFT, KEY_F1};
								unsigned cat = bench_categories == 1 ? bench_ticks % categories : c;
								emit(cat, now_ns(), keys[cat]);
							}
							++bench_ticks;
						}
						if (bench_ticks >= bench_total)
						{
							arm(bench, 0);
							benchmarking = false;
							scan();
							std::cerr << "Synthetic benchmark complete\n";
						}
					}
					else if (fd == server)
					{
						for (;;)
						{
							FD client(
								accept4(server, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC));
							if (client.n < 0)
								break;
							ucred peer{};
							socklen_t len = sizeof peer;
							if (getsockopt(client, SOL_SOCKET, SO_PEERCRED, &peer, &len) < 0 || peer.uid != getuid() || clients.size() >= 16)
								continue;
							int n = client;
							add(n);
							clients.emplace(n, std::move(client));
						}
					}
					else if (clients.contains(fd))
					{
						char b[256];
						auto n = recv(fd, b, sizeof b, MSG_DONTWAIT);
						if (n == -1 && errno == EAGAIN)
							continue;
						std::string response;
						try
						{
							response = n > 0 && n < 256 ? command(std::string(b, n))
														: "error: Invalid command size\n";
						}
						catch (const std::exception &e)
						{
							response = std::string("error: ") + e.what() + "\n";
						}
						if (n > 0)
							send(fd, response.data(), response.size(), MSG_NOSIGNAL);
						del(fd);
						clients.erase(fd);
					}
					else
						input(fd);
				}
				catch (const std::exception &e)
				{
					std::cerr << "Operation failed: " << e.what() << '\n';
				}
			}
		}
	}
};

static int service(const char *action)
{
	auto pid = fork();
	if (pid < 0)
		return 1;
	if (pid == 0)
	{
		if (std::strcmp(action, "is-active") == 0)
			execlp("systemctl", "systemctl", "--user", "is-active", "--quiet", "creamykeys.service", nullptr);
		else
			execlp("systemctl", "systemctl", "--user", "--quiet", action, "creamykeys.service", nullptr);
		_exit(127);
	}
	int status = 0;
	while (waitpid(pid, &status, 0) < 0)
		if (errno != EINTR)
			return 1;
	return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}

// User service enablement remembers a deliberate on/off choice across logins.
// No startup controller or auxiliary process is needed when the choice is off.
static int set_running(bool on, bool restart = false)
{
	if (on)
	{
		auto config = load_config(config_path());
		if (!config.enabled)
		{
			config.enabled = true;
			save_config(config_path(), config);
		}
		int result = service(restart ? "restart" : "start");
		if (result == 0)
			result = service("enable");
		return result;
	}
	int result = service("disable");
	if (result == 0)
		result = service("stop");
	return result;
}

static int toggle_running()
{
	const bool active = service("is-active") == 0;
	const int result = set_running(!active);
	if (result == 0)
		std::cout << "CreamyKeys " << (active ? "off" : "on") << '\n';
	return result;
}

static int request(const std::string &cmd)
{
	FD fd(socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0));
	timeval timeout{3, 0};
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof timeout);
	sockaddr_un a{};
	a.sun_family = AF_UNIX;
	auto path = (runtime() / "control.sock").string();
	if (path.size() >= sizeof a.sun_path)
		throw std::runtime_error("Runtime path too long");
	std::strcpy(a.sun_path, path.c_str());
	if (connect(fd, reinterpret_cast<sockaddr *>(&a), sizeof a) < 0)
		throw std::runtime_error("Daemon unavailable; run creamykeys start");
	if (send(fd, cmd.data(), cmd.size(), MSG_NOSIGNAL) < 0)
		throw std::runtime_error("Control send failed");
	char b[4096];
	auto n = recv(fd, b, sizeof b, 0);
	if (n <= 0)
		throw std::runtime_error("No daemon response");
	std::string result(b, n);
	std::cout << result;
	return result.starts_with("error:") ? 1 : 0;
}

int main(int argc, char **argv)
{
	try
	{
		if (argc < 2)
			return toggle_running();
		std::string cmd = argv[1];
		if (cmd == "-s" || cmd == "--soundpack")
		{
			if (argc != 3)
				throw std::runtime_error("Usage: creamykeys -s PATH.zip | --soundpack PATH.zip");
			const auto name = import_mechvibes_zip(fs::path(argv[2]), data_dir());
			std::cout << "Imported sound pack: " << name << '\n';
			if (fs::exists(runtime() / "control.sock"))
				return request("pack " + name);
			auto config = load_config(config_path());
			config.pack = name;
			save_config(config_path(), config);
			std::cout << "Selected; run creamykeys to turn it on\n";
			return 0;
		}
		if (cmd == "daemon")
		{
			pw_init(nullptr, nullptr);
			{
				Daemon d;
				d.run();
			}
			pw_deinit();
			return 0;
		}
		if (cmd == "start" || cmd == "stop" || cmd == "restart")
		{
			if (argc != 2)
				throw std::runtime_error("Invalid arguments");
			return set_running(cmd != "stop", cmd == "restart");
		}
		if (cmd == "help" || cmd == "--help" || cmd == "-h")
		{
			std::cout << "creamykeys: toggle daemon on/off and remember across reboots\n"
						 "creamykeys start|stop|restart|status|enable|disable|toggle|volume 0..1|"
						 "pack list|pack set NAME|benchmark [typing|burst]|reload\n"
						 "creamykeys -s|--soundpack PATH.zip: import and select a Mechvibes pack\n";
			return 0;
		}
		if (cmd == "toggle")
		{
			if (argc != 2)
				throw std::runtime_error("Invalid arguments");
			return toggle_running();
		}
		if (cmd == "status" && argc == 2 && !fs::exists(runtime() / "control.sock"))
		{
			std::cout << "CreamyKeys off (not running)\n";
			return 0;
		}
		if (cmd == "pack")
		{
			if (argc == 3 && std::string(argv[2]) == "list")
			{
				std::map<std::string, fs::path> packs;
				for (auto &base : {fs::path("/usr/share/creamykeys/packs"),
								   fs::path("/usr/local/share/creamykeys/packs"),
								   data_dir()})
				{
					std::error_code ec;
					for (auto &e : fs::directory_iterator(base, ec))
						if (e.is_directory())
							packs[e.path().filename().string()] = e.path();
				}
				for (auto &[name, path] : packs)
					std::cout << name << " " << path.string() << '\n';
				return 0;
			}
			if (argc == 4 && std::string(argv[2]) == "set")
				return request("pack " + std::string(argv[3]));
			throw std::runtime_error("Usage: creamykeys pack list | pack set NAME");
		}
		if ((cmd == "volume" && argc != 3) || (cmd == "benchmark" && argc != 2 && argc != 3) || (cmd != "volume" && cmd != "benchmark" && argc != 2))
			throw std::runtime_error("Invalid arguments");
		return request(cmd + (argc == 3 ? " " + std::string(argv[2]) : ""));
	}
	catch (const std::exception &e)
	{
		std::cerr << "creamykeys: " << e.what() << '\n';
		return 1;
	}
}
