#include "touch_mouse.h"

#ifdef ENABLE_WAKE_HID

#include <cstdlib>
#include "bt.h"
#include "button_shortcut.h"
#include "button_utils.h"
#include "config.h"
#include "tusb.h"
#include "pico/time.h"
#include "usb.h"
#include "utils.h"

// The touchpad reports X 0..1919 and Y 0..1079, about 37 units per millimetre.

// A tap is a contact that stays this close to where it landed and lifts in time.
// Until a contact has done neither it may still become a tap, and it holds the
// pointer still: a tap never moves the pointer off the thing it clicks.
static constexpr int32_t TAP_MAX_TRAVEL = 80;
static constexpr uint32_t TAP_MAX_MS = 200;
static constexpr uint32_t TAP_CLICK_MS = 30;
// With tapping off there is no tap to wait for. This small radius only keeps a
// finger that has just landed from nudging the pointer.
static constexpr int32_t MOVE_START_TRAVEL = 12;
// Pressing the pad rolls the finger. The pointer stays put until the finger has
// travelled this far, which then is a drag.
static constexpr int32_t CLICK_HOLD_TRAVEL = 40;
// Travel of each finger per wheel notch while two fingers scroll.
static constexpr int32_t SCROLL_STEP = 80;
// touch_mouse_sensitivity 30 is one pointer count per touchpad unit. The default
// 10 is a third of that: a thumb on a pad this small needs the fine control more
// than the reach.
static constexpr int32_t SENSITIVITY_UNIT = 30;

struct Contact {
    bool down = false;
    bool moving = false; // Its deltas move the pointer; false while it is held.
    uint8_t id = 0;      // The controller numbers each new contact.
    int32_t x = 0, y = 0;
    int32_t start_x = 0, start_y = 0; // Where it landed.
    // While not moving: it starts once it is hold_travel away from the anchor,
    // or, if hold_timed, once hold_until has passed.
    int32_t anchor_x = 0, anchor_y = 0;
    int32_t hold_travel = 0;
    bool hold_timed = false;
    absolute_time_t hold_until = nil_time;
};

enum class ScrollAxis : uint8_t { None, Vertical, Horizontal };
// A tap is a timed click: the down report, then the up report TAP_CLICK_MS later.
enum class TapPhase : uint8_t { Idle, Down, Up };

static uint8_t applied_mode = TOUCH_MOUSE_OFF; // Config value mode_active was derived from.
static bool mode_active = false;
static bool capturing = false; // The touchpad drives the mouse right now.
static bool tracking = false;  // State below holds something since the last clear.

static Contact contacts[2];
static bool pad_was_pressed = true;
static uint8_t click_buttons = 0; // Held by the physical pad press.

// One touch gesture runs from the first contact landing to the last one lifting.
static bool gesture_open = false;
static bool gesture_is_tap = false;
static uint8_t gesture_fingers = 0; // Most contacts down at once.
static absolute_time_t gesture_deadline = nil_time;

static TapPhase tap_phase = TapPhase::Idle;
static bool tap_down_sent = false;
static uint8_t tap_button = 0;
static uint8_t tap_queued = 0; // A second tap that arrived before the first finished.
static absolute_time_t tap_release_at = nil_time;

static ScrollAxis scroll_axis = ScrollAxis::None;
static int32_t scroll_x = 0, scroll_y = 0; // Summed travel of both fingers.

// Not yet reported. Motion is in 1/SENSITIVITY_UNIT counts and keeps its fraction.
static int32_t pending_x = 0, pending_y = 0;
static int32_t pending_wheel = 0, pending_pan = 0;
static uint8_t sent_buttons = 0; // What the host last saw.

// Forget the touch state. sent_buttons stays, so a button the host still sees
// down gets its up report from the next service pass.
static void clear_state() {
    if (!tracking) return;
    tracking = false;
    contacts[0] = contacts[1] = {};
    // Counts as already pressed: a pad held while the mode comes on is the
    // shortcut that switched it, not a click.
    pad_was_pressed = true;
    click_buttons = 0;
    gesture_open = false;
    tap_phase = TapPhase::Idle;
    tap_down_sent = false;
    tap_button = tap_queued = 0;
    scroll_axis = ScrollAxis::None;
    scroll_x = scroll_y = 0;
    pending_x = pending_y = 0;
    pending_wheel = pending_pan = 0;
}

static void sync_mode() {
    const uint8_t mode = get_config().touch_mouse;
    if (mode == applied_mode) return;
    applied_mode = mode;
    mode_active = mode == TOUCH_MOUSE_ON;
    clear_state();
}

static void start_tap(uint8_t button) {
    tap_phase = TapPhase::Down;
    tap_down_sent = false;
    tap_button = button;
}

static int8_t clamp_delta(int32_t value) {
    return static_cast<int8_t>(value > 127 ? 127 : value < -127 ? -127 : value);
}

// Send the pending state if it differs from what the host has, then advance the
// tap, which waits on its own reports going out.
static void service() {
    if (tap_phase == TapPhase::Down && tap_down_sent && time_reached(tap_release_at)) {
        tap_phase = TapPhase::Up;
    }
    const uint8_t buttons = click_buttons | (tap_phase == TapPhase::Down ? tap_button : 0);
    const int8_t x = clamp_delta(pending_x / SENSITIVITY_UNIT);
    const int8_t y = clamp_delta(pending_y / SENSITIVITY_UNIT);
    const int8_t wheel = clamp_delta(pending_wheel);
    const int8_t pan = clamp_delta(pending_pan);
    if (buttons != sent_buttons || x || y || wheel || pan) {
        if (!tud_hid_n_ready(USB_TOUCH_MOUSE_INSTANCE)) return;
        if (!tud_hid_n_mouse_report(USB_TOUCH_MOUSE_INSTANCE, 0, buttons, x, y, wheel, pan)) return;
        sent_buttons = buttons;
        pending_x -= x * SENSITIVITY_UNIT;
        pending_y -= y * SENSITIVITY_UNIT;
        pending_wheel -= wheel;
        pending_pan -= pan;
    }
    if (tap_phase == TapPhase::Down && !tap_down_sent) {
        tap_down_sent = true;
        tap_release_at = make_timeout_time_ms(TAP_CLICK_MS);
    } else if (tap_phase == TapPhase::Up) {
        // The up report is out, so a queued tap starts with a fresh down report.
        tap_phase = TapPhase::Idle;
        if (tap_queued) {
            start_tap(tap_queued);
            tap_queued = 0;
        }
    }
}

// Keeps `capturing` current. Returns false when no report can reach the host.
static bool update_capturing() {
    sync_mode();
    capturing = false;
    if (!usb_touch_mouse_present || usb_keyboard_only || usb_reconfiguring || !tud_mounted()) {
        // The host has no mouse interface, or has just enumerated a new one.
        clear_state();
        sent_buttons = 0;
        return false;
    }
    if (tud_suspended()) {
        clear_state(); // The up report follows after resume.
        return false;
    }
    capturing = mode_active;
    return true;
}

static int32_t travel(const Contact &contact) {
    return std::abs(contact.x - contact.start_x) + std::abs(contact.y - contact.start_y);
}

// Keep the pointer still under this contact until it has travelled `radius` from here.
static void hold(Contact &contact, int32_t radius) {
    contact.moving = false;
    contact.anchor_x = contact.x;
    contact.anchor_y = contact.y;
    contact.hold_travel = radius;
    contact.hold_timed = false;
}

static void track_contact(Contact &contact, const TouchFingerData &finger,
                          int32_t &dx, int32_t &dy) {
    dx = dy = 0;
    if (finger.NotTouching) {
        contact.down = false;
        return;
    }
    const int32_t x = finger.FingerX;
    const int32_t y = finger.FingerY;
    if (!contact.down || contact.id != finger.Index) {
        // A new contact only sets the origin; it must not jump the pointer.
        contact = {};
        contact.down = true;
        contact.id = finger.Index;
        contact.x = contact.start_x = x;
        contact.y = contact.start_y = y;
        if (get_config().touch_mouse_tap) {
            hold(contact, TAP_MAX_TRAVEL);
            contact.hold_timed = true;
            contact.hold_until = make_timeout_time_ms(TAP_MAX_MS);
        } else {
            hold(contact, MOVE_START_TRAVEL);
        }
        return;
    }
    if (contact.moving) {
        dx = x - contact.x;
        dy = y - contact.y;
    }
    contact.x = x;
    contact.y = y;
    if (contact.moving) return;
    // The travel up to here is dropped, not replayed as one jump.
    const int32_t held = std::abs(x - contact.anchor_x) + std::abs(y - contact.anchor_y);
    if (held > contact.hold_travel || (contact.hold_timed && time_reached(contact.hold_until))) {
        contact.moving = true;
    }
}

// A chord wins over the click, like it wins over a lone tap in button_shortcut:
// pressing the pad while the chord's other button is held is not a click.
static bool pad_claimed_by_chord(const USBGetStateData &state) {
    for (const auto &shortcut : get_button().shortcuts) {
        if (!shortcut_slot_valid(shortcut) || shortcut.trigger_b >= Disable) continue;
        uint8_t other;
        if (shortcut.trigger_a == ButtonPad) {
            other = shortcut.trigger_b;
        } else if (shortcut.trigger_b == ButtonPad) {
            other = shortcut.trigger_a;
        } else {
            continue;
        }
        if (button_is_pressed(state, other)) return true;
    }
    return false;
}

static void update_click(const USBGetStateData &state, bool pad, uint8_t fingers) {
    if (pad && !pad_was_pressed) {
        // The button is chosen on the press and held until the release, so a
        // finger joining or leaving mid-press does not swap it.
        if (!pad_claimed_by_chord(state)) {
            click_buttons = fingers >= 2 ? MOUSE_BUTTON_RIGHT : MOUSE_BUTTON_LEFT;
        }
    } else if (!pad) {
        click_buttons = 0;
    }
    pad_was_pressed = pad;
}

static void update_gesture(uint8_t fingers, bool pad, bool travelled) {
    if (fingers) {
        if (!gesture_open) {
            gesture_open = true;
            gesture_is_tap = true;
            gesture_fingers = 0;
            gesture_deadline = make_timeout_time_ms(TAP_MAX_MS);
        }
        if (fingers > gesture_fingers) gesture_fingers = fingers;
        if (pad || travelled) gesture_is_tap = false;
        return;
    }
    if (!gesture_open) return;
    gesture_open = false;
    if (!gesture_is_tap || time_reached(gesture_deadline) || !get_config().touch_mouse_tap) return;
    const uint8_t button = gesture_fingers >= 2 ? MOUSE_BUTTON_RIGHT : MOUSE_BUTTON_LEFT;
    if (tap_phase == TapPhase::Idle) {
        start_tap(button);
    } else {
        tap_queued = button;
    }
}

// Content follows the fingers: moving them down turns the wheel up. The first
// axis to travel a full step keeps the scroll until a finger lifts.
static void update_scroll(int32_t dx, int32_t dy) {
    constexpr int32_t step = 2 * SCROLL_STEP; // dx and dy sum both fingers.
    scroll_x += dx;
    scroll_y += dy;
    if (scroll_axis == ScrollAxis::None) {
        if (std::abs(scroll_y) >= step && std::abs(scroll_y) >= std::abs(scroll_x)) {
            scroll_axis = ScrollAxis::Vertical;
        } else if (std::abs(scroll_x) >= step) {
            scroll_axis = ScrollAxis::Horizontal;
        } else {
            return;
        }
    }
    // Travel across the locked axis is left to pile up unread; lifting a finger clears it.
    if (scroll_axis == ScrollAxis::Vertical) {
        pending_wheel += scroll_y / step;
        scroll_y %= step;
    } else {
        pending_pan -= scroll_x / step;
        scroll_x %= step;
    }
}

void touch_mouse_tick(const USBGetStateData &state) {
    if (!update_capturing()) return;
    if (!capturing) {
        clear_state();
        service(); // Releases a button held when the mode went off.
        return;
    }
    tracking = true;

    int32_t dx[2], dy[2];
    uint8_t fingers = 0;
    bool travelled = false;
    for (unsigned i = 0; i < 2; ++i) {
        track_contact(contacts[i], state.Touch.Finger[i], dx[i], dy[i]);
        if (!contacts[i].down) continue;
        ++fingers;
        if (travel(contacts[i]) > TAP_MAX_TRAVEL) travelled = true;
    }
    const bool pad = state.ButtonPad;
    if (pad && !pad_was_pressed) {
        for (unsigned i = 0; i < 2; ++i) {
            if (!contacts[i].down) continue;
            hold(contacts[i], CLICK_HOLD_TRAVEL);
            dx[i] = dy[i] = 0;
        }
    }
    update_click(state, pad, fingers);
    update_gesture(fingers, pad, travelled);

    if (fingers == 2 && !click_buttons) {
        update_scroll(dx[0] + dx[1], dy[0] + dy[1]);
    } else {
        scroll_axis = ScrollAxis::None;
        scroll_x = scroll_y = 0;
        // One finger steers the pointer. With a button held, two fingers keep
        // steering it by their average, so a drag does not turn into a scroll.
        const int32_t scale = get_config().touch_mouse_sensitivity;
        const int32_t divisor = fingers == 2 ? 2 : 1;
        pending_x += (dx[0] + dx[1]) * scale / divisor;
        pending_y += (dy[0] + dy[1]) * scale / divisor;
    }
    service();
}

void touch_mouse_task() {
    if (!update_capturing()) return;
    // Without input reports nothing would ever release a held button.
    if (!capturing || !bt_is_connected()) clear_state();
    service();
}

void touch_mouse_filter(USBGetStateData &report) {
    if (!capturing) return;
    report.Touch.Finger[0].NotTouching = 1;
    report.Touch.Finger[1].NotTouching = 1;
    report.ButtonPad = 0;
}

bool touch_mouse_toggle() {
    sync_mode();
    if (applied_mode != TOUCH_MOUSE_OFF) {
        mode_active = !mode_active;
        clear_state();
    }
    return true;
}

#endif // ENABLE_WAKE_HID
