#pragma once

#include <cstdint>

struct USBGetStateData;

// Config_body::touch_mouse
constexpr uint8_t TOUCH_MOUSE_OFF = 0;     // No mouse interface.
constexpr uint8_t TOUCH_MOUSE_ON = 1;      // Mouse interface; the touchpad drives it from the start.
constexpr uint8_t TOUCH_MOUSE_STANDBY = 2; // Mouse interface; idle until the toggle shortcut fires.

// Config_body::touch_mouse_sensitivity: pointer speed relative to the default of 10.
constexpr uint8_t TOUCH_MOUSE_SENSITIVITY_MIN = 1;
constexpr uint8_t TOUCH_MOUSE_SENSITIVITY_MAX = 50;
constexpr uint8_t TOUCH_MOUSE_SENSITIVITY_DEFAULT = 10;

#ifdef ENABLE_WAKE_HID
// Feed one controller input report. Reads the physical state, before remapping.
void touch_mouse_tick(const USBGetStateData &state);
// Main-loop service: sends what is pending even when Bluetooth stops reporting.
void touch_mouse_task();
// While the touchpad drives the mouse, the gamepad report shows it untouched.
void touch_mouse_filter(USBGetStateData &report);
// Shortcut action: switch the mouse mode on or off. Always handled.
bool touch_mouse_toggle();
#else
// No mouse interface to report into: keep the call sites clean, like button_shortcut.h.
static inline void touch_mouse_tick(const USBGetStateData &) {}
static inline void touch_mouse_task() {}
static inline void touch_mouse_filter(USBGetStateData &) {}
static inline bool touch_mouse_toggle() { return true; }
#endif
