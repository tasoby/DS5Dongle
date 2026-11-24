//
// Created by awalol on 2026/3/4.
//

#ifndef DS5_BRIDGE_USB_H
#define DS5_BRIDGE_USB_H

#include <cstdint>

extern bool usb_keyboard_only;
extern bool usb_sleep_mode;
extern bool usb_reconfiguring;
uint8_t usb_keyboard_instance();
// HID instance of the touchpad mouse. It is fixed because the mouse always comes
// with the keyboard (1) and consumer (2) interfaces; see tud_descriptor_configuration_cb.
constexpr uint8_t USB_TOUCH_MOUSE_INSTANCE = 3;
// True while the configuration the host enumerated includes the mouse interface.
extern bool usb_touch_mouse_present;
void usb_reconnect(bool keyboard_only);
void usb_reconnect_sleep();

extern uint8_t mute[2]; // 0: SPEAKER(0x02) 1: MIC(0x05)
extern float volume[2]; // 0: SPEAKER(0x02) 1: MIC(0x05)

#endif //DS5_BRIDGE_USB_H
