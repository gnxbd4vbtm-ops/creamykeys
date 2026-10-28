#pragma once
#include <filesystem>
#include <libevdev/libevdev.h>
#include <linux/input-event-codes.h>
#include <string>

namespace ck
{
// Identify actual typing/navigation capabilities, including sparse macro devices.
// A device's bus value does not reliably distinguish uinput from real hardware.
inline bool keyboard_capability(unsigned code)
{
	return (code >= KEY_ESC && code <= KEY_KPDOT) || (code >= KEY_102ND && code <= KEY_DELETE) || code == KEY_KPEQUAL || code == KEY_KPCOMMA || (code >= KEY_HANGEUL && code <= KEY_COMPOSE) || (code >= KEY_F13 && code <= KEY_F24) || (code >= KEY_FN && code <= KEY_FN_RIGHT_SHIFT);
}

inline bool keyboard_device(const libevdev *device)
{
	for (unsigned code = 1; code < KEY_CNT; ++code)
		if (keyboard_capability(code) && libevdev_has_event_code(device, EV_KEY, code))
			return true;
	return false;
}

// Devices created through uinput live under /sys/devices/virtual/input. Their
// bus value is often BUS_USB, so the sysfs location is the reliable signal.
inline bool virtual_input(const std::string &path, const std::string &sysfs = "/sys/class/input")
{
	std::error_code ec;
	auto device = std::filesystem::canonical(std::filesystem::path(sysfs) / std::filesystem::path(path).filename() / "device", ec);
	return !ec && device.string().find("/devices/virtual/") != std::string::npos;
}

inline std::string input_identity(const char *phys, const char *uniq, const std::string &path)
{
	const bool physical_identity = phys && *phys;
	std::string identity = physical_identity ? phys : path;
	if (physical_identity)
	{
		auto suffix = identity.rfind("/input");
		if (suffix != identity.npos)
			identity.resize(suffix);
	}
	if (uniq && *uniq)
	{
		identity += ':';
		identity += uniq;
	}
	return identity;
}

inline bool keyboard_event(unsigned code)
{
	return code > KEY_RESERVED && code < KEY_CNT && !(code >= BTN_MISC && code < KEY_OK) && !(code >= BTN_DPAD_UP && code <= BTN_DPAD_RIGHT) && !(code >= BTN_TRIGGER_HAPPY1 && code <= BTN_TRIGGER_HAPPY40);
}
} // namespace ck
