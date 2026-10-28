#include "input.hpp"
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#define CHECK(x)                          \
	do                                    \
	{                                     \
		if (!(x))                         \
			throw std::runtime_error(#x); \
	} while (0)

int main()
{
	std::unique_ptr<libevdev, decltype(&libevdev_free)> d(libevdev_new(), &libevdev_free);
	CHECK(d);
	CHECK(!ck::keyboard_device(d.get()));
	libevdev_enable_event_code(d.get(), EV_KEY, BTN_LEFT, nullptr);
	CHECK(!ck::keyboard_device(d.get()));
	libevdev_enable_event_code(d.get(), EV_KEY, KEY_POWER, nullptr);
	CHECK(!ck::keyboard_device(d.get()));
	libevdev_enable_event_code(d.get(), EV_KEY, KEY_2, nullptr);
	libevdev_enable_event_code(d.get(), EV_KEY, KEY_R, nullptr);
	libevdev_enable_event_code(d.get(), EV_KEY, KEY_TAB, nullptr);
	for (int bus : {BUS_USB, BUS_VIRTUAL, 0})
	{
		libevdev_set_id_bustype(d.get(), bus);
		CHECK(ck::keyboard_device(d.get()));
	}
	CHECK(ck::keyboard_event(KEY_2) && ck::keyboard_event(KEY_R) && ck::keyboard_event(KEY_TAB));
	CHECK(ck::keyboard_event(KEY_FN) && ck::keyboard_event(KEY_VOLUMEUP));
	CHECK(!ck::keyboard_event(BTN_LEFT) && !ck::keyboard_event(BTN_RIGHT));
	CHECK(!ck::keyboard_event(BTN_DPAD_UP) && !ck::keyboard_event(BTN_TRIGGER_HAPPY1));
	CHECK(!ck::keyboard_event(KEY_RESERVED) && !ck::keyboard_event(KEY_CNT));
	libevdev_disable_event_code(d.get(), EV_KEY, KEY_2);
	libevdev_disable_event_code(d.get(), EV_KEY, KEY_R);
	libevdev_disable_event_code(d.get(), EV_KEY, KEY_TAB);
	CHECK(!ck::keyboard_device(d.get()));
	libevdev_enable_event_code(d.get(), EV_KEY, KEY_F13, nullptr);
	CHECK(ck::keyboard_device(d.get()));
	CHECK(ck::input_identity("", "", "/dev/input/event16") == "/dev/input/event16");
	CHECK(ck::input_identity(nullptr, nullptr, "/dev/input/event17") == "/dev/input/event17");
	CHECK(ck::input_identity("usb-1/input0", "", "ignored") == "usb-1");
	CHECK(ck::input_identity("usb-1/input1", "unit", "ignored") == "usb-1:unit");
	namespace fs = std::filesystem;
	const fs::path root = fs::temp_directory_path() / "creamykeys-input-test";
	fs::remove_all(root);
	fs::create_directories(root / "class" / "event0");
	fs::create_directories(root / "class" / "event1");
	fs::create_directories(root / "devices" / "virtual" / "input" / "input0");
	fs::create_directories(root / "devices" / "pci0" / "input" / "input1");
	fs::create_symlink(root / "devices" / "virtual" / "input" / "input0", root / "class" / "event0" / "device");
	fs::create_symlink(root / "devices" / "pci0" / "input" / "input1", root / "class" / "event1" / "device");
	CHECK(ck::virtual_input("/dev/input/event0", (root / "class").string()));
	CHECK(!ck::virtual_input("/dev/input/event1", (root / "class").string()));
	CHECK(!ck::virtual_input("/dev/input/event2", (root / "class").string()));
	fs::remove_all(root);
	std::cout << "Sparse macro capabilities, physical/virtual detection, USB/virtual/unspecified buses, function-only devices and mouse-button exclusion passed\n";
}
