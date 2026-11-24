"""Host regression: python tests/test_touch_mouse.py (requires g++ on PATH)."""
import importlib.util
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('config_tool', ROOT / 'tools/config_tool.py')
tool = importlib.util.module_from_spec(spec)
spec.loader.exec_module(tool)

PLATFORM = '''#pragma once
#include <cstdint>
#include <cstring>
#include "button_shortcut.h"
#include "touch_mouse.h"
using absolute_time_t = uint64_t;
constexpr absolute_time_t nil_time = 0;
inline uint64_t now = 0;
inline auto make_timeout_time_ms(uint32_t ms) { return now + ms; }
inline bool time_reached(absolute_time_t t) { return now >= t; }
constexpr uint8_t ButtonHome = 20, ButtonPad = 21, Disable = 27;
struct TouchFingerData {
    uint32_t Index : 7;
    uint32_t NotTouching : 1;
    uint32_t FingerX : 12;
    uint32_t FingerY : 12;
};
struct TouchData { TouchFingerData Finger[2]; uint8_t Timestamp; };
struct USBGetStateData {
    uint32_t buttons = 0; // every button but the pad, by ButtonId
    bool ButtonPad = false;
    TouchData Touch{};
};
inline bool button_is_pressed(const USBGetStateData& s, uint8_t b) {
    return b < 27 && (s.buttons & (1u << b));
}
struct Config_body {
    uint8_t touch_mouse = TOUCH_MOUSE_ON;
    uint8_t touch_mouse_sensitivity = TOUCH_MOUSE_SENSITIVITY_DEFAULT;
    uint8_t touch_mouse_tap = 1;
};
inline Config_body cfg;
inline Config_body& get_config() { return cfg; }
struct Button { ButtonShortcut shortcuts[BUTTON_SHORTCUT_COUNT]{}; };
inline Button button_cfg;
inline Button& get_button() { return button_cfg; }
inline bool shortcut_slot_valid(const ButtonShortcut& s) { return s.trigger_a < Disable; }
constexpr uint8_t USB_TOUCH_MOUSE_INSTANCE = 3;
inline bool usb_keyboard_only = false, usb_reconfiguring = false;
inline bool usb_touch_mouse_present = true;
inline bool ready = true, accept = true, mounted = true, suspended = false;
inline bool connected = true;
inline bool tud_mounted() { return mounted; }
inline bool tud_suspended() { return suspended; }
inline bool bt_is_connected() { return connected; }
inline bool tud_hid_n_ready(uint8_t instance) {
    return ready && instance == USB_TOUCH_MOUSE_INSTANCE;
}
enum { MOUSE_BUTTON_LEFT = 1, MOUSE_BUTTON_RIGHT = 2 };
// What the host makes of the reports: a pointer position and click counts.
struct Host {
    unsigned reports = 0, left = 0, right = 0;
    uint8_t buttons = 0;
    int x = 0, y = 0, wheel = 0, pan = 0;
};
inline Host host;
inline bool tud_hid_n_mouse_report(uint8_t, uint8_t, uint8_t buttons, int8_t x, int8_t y,
                                   int8_t wheel, int8_t pan) {
    if (!accept) return false;
    const uint8_t pressed = buttons & ~host.buttons;
    if (pressed & MOUSE_BUTTON_LEFT) ++host.left;
    if (pressed & MOUSE_BUTTON_RIGHT) ++host.right;
    host.buttons = buttons;
    host.x += x; host.y += y; host.wheel += wheel; host.pan += pan;
    ++host.reports;
    return true;
}
'''


class TouchMouseTests(unittest.TestCase):
    def test_protocol(self):
        slot, value = tool.parse_shortcut_assignment('7=Home+Pad:touch_mouse_toggle')
        self.assertEqual(value[2], tool.SHORTCUT_ACTION_TOUCH_MOUSE_TOGGLE)
        self.assertTrue(tool.shortcut_slot_valid(value))
        data = bytearray(tool.SHORTCUT_STORAGE_SIZE)
        tool.pack_shortcut(data, slot, value)
        self.assertEqual(tool.unpack_shortcut(data, slot), value)
        with self.assertRaises(SystemExit):
            tool.parse_shortcut_assignment('1=L1@hold:touch_mouse_toggle')
        # The mouse settings are appended, so older fields keep their offsets.
        self.assertEqual(tool.FIELD_NAMES[-3:],
                         ['touch_mouse', 'touch_mouse_sensitivity', 'touch_mouse_tap'])
        self.assertEqual(tool.BODY_SIZE, 28)

    def test_firmware(self):
        with tempfile.TemporaryDirectory() as directory:
            tmp = Path(directory)
            # Compile the real state machine, substituting only platform headers.
            for name in ('touch_mouse.cpp', 'touch_mouse.h', 'button_shortcut.h'):
                shutil.copyfile(ROOT / 'src' / name, tmp / name)
            (tmp / 'pico').mkdir()
            for name in ('bt.h', 'button_utils.h', 'config.h', 'utils.h', 'usb.h',
                         'tusb.h', 'pico/time.h'):
                (tmp / name).write_text('#include "platform.h"\n')
            (tmp / 'platform.h').write_text(PLATFORM)
            shutil.copyfile(ROOT / 'tests/touch_mouse_test.cpp', tmp / 'test.cpp')
            exe = tmp / 'test.exe'
            subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-DENABLE_WAKE_HID',
                            '-I', str(tmp), str(tmp / 'touch_mouse.cpp'),
                            str(tmp / 'test.cpp'), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True)
            # Without the extra HID interfaces the module compiles to nothing.
            subprocess.run(['g++', '-std=c++17', '-I', str(tmp), '-c',
                            str(tmp / 'touch_mouse.cpp'), '-o', str(tmp / 'standard.o')],
                           check=True)


if __name__ == '__main__':
    unittest.main()
