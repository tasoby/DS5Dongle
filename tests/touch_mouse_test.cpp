#include "platform.h"
#include <cassert>

// The controller state the firmware is fed; contacts persist between reports.
static USBGetStateData st;

static void touch(unsigned finger, uint8_t id, int x, int y) {
    st.Touch.Finger[finger].Index = id;
    st.Touch.Finger[finger].NotTouching = 0;
    st.Touch.Finger[finger].FingerX = x;
    st.Touch.Finger[finger].FingerY = y;
}
static void lift(unsigned finger) { st.Touch.Finger[finger].NotTouching = 1; }
static void send() { touch_mouse_tick(st); }

// Back to an idle controller, a host with nothing held and the mouse mode on.
static void fresh() {
    lift(0); lift(1);
    st.ButtonPad = false;
    st.buttons = 0;
    cfg = {};
    cfg.touch_mouse_sensitivity = 30; // one count per touchpad unit keeps the numbers plain
    for (auto &s : button_cfg.shortcuts) s.trigger_a = BUTTON_SHORTCUT_DISABLED;
    ready = accept = mounted = connected = true;
    suspended = false;
    usb_touch_mouse_present = true;
    cfg.touch_mouse = TOUCH_MOUSE_OFF; send(); touch_mouse_task(); // drops all touch state
    cfg.touch_mouse = TOUCH_MOUSE_ON; send();
    now += 1000;
    host = {};
}

// Land finger 0 and let it outlive the tap window, so later deltas count in full.
static void glide_start(int x, int y) {
    touch(0, 1, x, y); send();
    now += 250; send();
}

static void test_pointer() {
    fresh();
    touch(0, 1, 500, 500); send();
    assert(host.reports == 0); // landing sets the origin only
    // While it could still become a tap, the contact holds the pointer still.
    now += 100; touch(0, 1, 540, 500); send();
    assert(host.x == 0);
    touch(0, 1, 590, 500); send();
    assert(host.x == 0); // past the tap radius now; the travel up to here is dropped
    touch(0, 1, 620, 510); send();
    assert(host.x == 30 && host.y == 10);
    touch(0, 1, 610, 500); send();
    assert(host.x == 20 && host.y == 0);

    // A new contact far away must not jump the pointer.
    lift(0); send();
    touch(0, 2, 1500, 900); send();
    touch(0, 2, 1501, 900); send();
    assert(host.x == 20 && host.y == 0);

    // A slow, small move starts once the tap window has passed.
    fresh();
    touch(0, 1, 500, 500); send();
    now += 150; touch(0, 1, 503, 500); send();
    assert(host.x == 0);
    now += 51; touch(0, 1, 504, 500); send();
    assert(host.x == 0);
    touch(0, 1, 507, 501); send();
    assert(host.x == 3 && host.y == 1);

    // With tapping off there is no window to wait for, only a small radius.
    fresh();
    cfg.touch_mouse_tap = 0;
    touch(0, 1, 500, 500); send();
    touch(0, 1, 510, 500); send();
    touch(0, 1, 520, 500); send();
    assert(host.x == 0);
    touch(0, 1, 530, 500); send();
    assert(host.x == 10);

    // The default speed is a third of that, and the fraction is carried over.
    fresh();
    cfg.touch_mouse_sensitivity = TOUCH_MOUSE_SENSITIVITY_DEFAULT;
    glide_start(500, 500);
    touch(0, 1, 509, 500); send();
    assert(host.x == 3);
    touch(0, 1, 510, 500); send();
    touch(0, 1, 511, 500); send();
    assert(host.x == 3);
    touch(0, 1, 512, 500); send();
    assert(host.x == 4);
    cfg.touch_mouse_sensitivity = 20; // applies at once
    touch(0, 1, 521, 500); send();
    assert(host.x == 10);

    // More than one report can carry is sent over the following passes.
    fresh();
    glide_start(100, 500);
    touch(0, 1, 400, 500); send();
    assert(host.x == 127);
    touch_mouse_task(); assert(host.x == 254);
    touch_mouse_task(); assert(host.x == 300);
    const unsigned n = host.reports;
    touch_mouse_task(); assert(host.reports == n);

    // A busy or failing endpoint loses nothing.
    fresh();
    glide_start(500, 500);
    ready = false;
    touch(0, 1, 510, 500); send();
    touch(0, 1, 520, 500); send();
    assert(host.x == 0);
    ready = true; accept = false; touch_mouse_task(); assert(host.x == 0);
    accept = true; touch_mouse_task(); assert(host.x == 20);
}

static void test_click() {
    fresh();
    glide_start(500, 500);
    touch(0, 1, 510, 500); send();
    assert(host.x == 10);
    // The press stops the pointer, also for the travel in the same report.
    st.ButtonPad = true; touch(0, 1, 515, 500); send();
    assert(host.buttons == MOUSE_BUTTON_LEFT && host.x == 10);
    touch(0, 1, 540, 500); send();
    assert(host.x == 10); // the finger rolling under the press
    // Dragging: past that radius the pointer follows while the button stays down.
    touch(0, 1, 560, 500); send();
    touch(0, 1, 600, 500); send();
    assert(host.buttons == MOUSE_BUTTON_LEFT && host.x == 50);
    st.ButtonPad = false; send();
    assert(host.buttons == 0 && host.left == 1 && host.right == 0);
    lift(0); now += 500; send();
    assert(host.left == 1); // a press is never also a tap

    // Two fingers on the pad make it the right button, for the whole press.
    fresh();
    touch(0, 1, 500, 500); touch(1, 2, 900, 500); send();
    st.ButtonPad = true; send();
    assert(host.buttons == MOUSE_BUTTON_RIGHT);
    lift(1); send();
    assert(host.buttons == MOUSE_BUTTON_RIGHT);
    st.ButtonPad = false; send();
    assert(host.buttons == 0 && host.right == 1 && host.left == 0);

    // A second finger joining a held press keeps the drag instead of scrolling.
    fresh();
    glide_start(500, 500);
    st.ButtonPad = true; send();
    touch(1, 2, 900, 500); send();
    now += 250;
    touch(0, 1, 550, 500); touch(1, 2, 900, 500); send(); // both are moving from here
    touch(0, 1, 590, 500); touch(1, 2, 940, 500); send();
    assert(host.buttons == MOUSE_BUTTON_LEFT && host.x == 40 && host.wheel == 0 && host.pan == 0);

    // The pad of a configured chord is not a click while the other button is held.
    fresh();
    auto &s = button_cfg.shortcuts[0];
    s = {};
    s.trigger_a = ButtonHome;
    s.trigger_b = ButtonPad;
    s.action = ShortcutActionTouchMouseToggle;
    touch(0, 1, 500, 500); send();
    st.buttons = 1u << ButtonHome; send();
    st.ButtonPad = true; send();
    assert(host.reports == 0);
    st.buttons = 0; send();
    assert(host.reports == 0); // still the same press
    st.ButtonPad = false; send();
    st.ButtonPad = true; send();
    assert(host.buttons == MOUSE_BUTTON_LEFT); // without the other button it clicks
}

static void test_tap() {
    fresh();
    touch(0, 1, 500, 500); send();
    now += 80; touch(0, 1, 540, 520); send(); // a careless tap, rolling over a millimetre
    assert(host.reports == 0);
    lift(0); send();
    assert(host.buttons == MOUSE_BUTTON_LEFT && host.x == 0 && host.y == 0);
    now += 10; touch_mouse_task(); assert(host.buttons == MOUSE_BUTTON_LEFT);
    now += 21; touch_mouse_task(); assert(host.buttons == 0 && host.left == 1);

    // Too slow, too far or switched off: no click.
    fresh();
    touch(0, 1, 500, 500); send();
    now += 201; lift(0); send();
    assert(host.reports == 0);
    touch(0, 2, 500, 500); send();
    touch(0, 2, 590, 500); send();
    lift(0); send();
    assert(host.left == 0);
    cfg.touch_mouse_tap = 0;
    now += 1000; touch(0, 3, 500, 500); send();
    lift(0); send();
    assert(host.reports == 0);

    // Two fingers tap the right button, also when they land and lift apart.
    fresh();
    touch(0, 1, 500, 500); send();
    now += 20; touch(1, 2, 900, 500); send();
    now += 40; lift(0); send();
    now += 20; lift(1); send();
    assert(host.buttons == MOUSE_BUTTON_RIGHT);
    now += 31; touch_mouse_task(); assert(host.buttons == 0 && host.right == 1 && host.left == 0);

    // A double tap is two clicks, with the up report in between even when the
    // second tap ends before the first click was released.
    fresh();
    touch(0, 1, 500, 500); send(); lift(0); send();
    assert(host.left == 1 && host.buttons == MOUSE_BUTTON_LEFT);
    now += 10; touch(0, 2, 500, 500); send(); lift(0); send();
    assert(host.left == 1 && host.buttons == MOUSE_BUTTON_LEFT);
    now += 31; touch_mouse_task(); assert(host.buttons == 0);
    touch_mouse_task(); assert(host.left == 2 && host.buttons == MOUSE_BUTTON_LEFT);
    now += 31; touch_mouse_task(); assert(host.buttons == 0 && host.left == 2);

    // The up report waits for an endpoint that is busy.
    fresh();
    touch(0, 1, 500, 500); send(); lift(0); send();
    ready = false; now += 31; touch_mouse_task();
    assert(host.buttons == MOUSE_BUTTON_LEFT);
    ready = true; touch_mouse_task();
    assert(host.buttons == 0);
}

static void test_scroll() {
    fresh();
    touch(0, 1, 500, 300); touch(1, 2, 900, 300); send();
    now += 250; send();
    touch(0, 1, 500, 380); touch(1, 2, 900, 380); send();
    assert(host.wheel == 1); // fingers down turn the wheel up
    touch(0, 1, 500, 420); touch(1, 2, 900, 420); send();
    assert(host.wheel == 1); // half a step so far
    touch(0, 1, 500, 460); touch(1, 2, 900, 460); send();
    assert(host.wheel == 2);
    // The scroll stays on its axis, and never moves the pointer or taps.
    touch(0, 1, 800, 460); touch(1, 2, 1200, 460); send();
    assert(host.pan == 0);
    touch(0, 1, 800, 300); touch(1, 2, 1200, 300); send();
    assert(host.wheel == 0 && host.x == 0 && host.y == 0);
    lift(0); lift(1); send();
    assert(host.buttons == 0 && host.left == 0 && host.right == 0);

    // Sideways: the content follows the fingers.
    fresh();
    touch(0, 1, 500, 300); touch(1, 2, 500, 700); send();
    now += 250; send();
    touch(0, 1, 580, 300); touch(1, 2, 580, 700); send();
    assert(host.pan == -1 && host.wheel == 0);
    touch(0, 1, 420, 300); touch(1, 2, 420, 700); send();
    assert(host.pan == 1);
}

static void test_filter() {
    fresh();
    touch(0, 1, 500, 500);
    st.ButtonPad = true;
    send();
    USBGetStateData report = st;
    touch_mouse_filter(report);
    assert(report.Touch.Finger[0].NotTouching && report.Touch.Finger[1].NotTouching);
    assert(!report.ButtonPad && report.Touch.Finger[0].FingerX == 500);

    // Mouse mode off: the game keeps the touchpad.
    touch_mouse_toggle(); send();
    report = st;
    touch_mouse_filter(report);
    assert(!report.Touch.Finger[0].NotTouching && report.ButtonPad);

    // Configured, but the host has not enumerated the mouse interface yet.
    fresh();
    usb_touch_mouse_present = false;
    touch(0, 1, 500, 500); send();
    report = st;
    touch_mouse_filter(report);
    assert(!report.Touch.Finger[0].NotTouching && host.reports == 0);
}

static void test_mode() {
    // Standby does nothing until the toggle, and the toggle works both ways.
    fresh();
    cfg.touch_mouse = TOUCH_MOUSE_STANDBY;
    glide_start(500, 500);
    touch(0, 1, 560, 500); send();
    st.ButtonPad = true; send();
    assert(host.reports == 0);
    // Coming on with the pad held (the toggle chord) is not a click or a jump.
    touch_mouse_toggle(); send();
    touch(0, 1, 561, 500); send();
    assert(host.reports == 0);
    st.ButtonPad = false; send();
    lift(0); send();
    assert(host.reports == 0);
    now += 1000;
    glide_start(500, 500);
    touch(0, 1, 530, 500); send();
    assert(host.x == 30);
    // Going off releases what is held.
    st.ButtonPad = true; send();
    assert(host.buttons == MOUSE_BUTTON_LEFT);
    touch_mouse_toggle(); touch_mouse_task();
    assert(host.buttons == 0);
    touch(0, 1, 600, 500); send();
    assert(host.x == 30);

    // Switched off in the config, the toggle has nothing to switch.
    fresh();
    cfg.touch_mouse = TOUCH_MOUSE_OFF;
    touch_mouse_toggle();
    glide_start(500, 500);
    touch(0, 1, 560, 500); send();
    assert(host.reports == 0);

    // A changed config value takes effect without a restart.
    cfg.touch_mouse = TOUCH_MOUSE_ON; send();
    now += 250; send();
    touch(0, 1, 590, 500); send();
    assert(host.x == 30);
}

static void test_release() {
    // The controller drops off with the button down.
    fresh();
    touch(0, 1, 500, 500); send();
    st.ButtonPad = true; send();
    assert(host.buttons == MOUSE_BUTTON_LEFT);
    connected = false; touch_mouse_task();
    assert(host.buttons == 0);

    // The host suspends with the button down: released once it is back.
    fresh();
    touch(0, 1, 500, 500); send();
    st.ButtonPad = true; send();
    suspended = true; send(); touch_mouse_task();
    assert(host.buttons == MOUSE_BUTTON_LEFT);
    suspended = false; touch_mouse_task();
    assert(host.buttons == 0);
    send();
    assert(host.buttons == 0); // the pad was down throughout: not a new press

    // A re-enumerated host starts clean; nothing is sent for the old state.
    fresh();
    touch(0, 1, 500, 500); send();
    st.ButtonPad = true; send();
    mounted = false; touch_mouse_task();
    mounted = true;
    const unsigned n = host.reports;
    touch_mouse_task();
    assert(host.reports == n);
}

int main() {
    test_pointer();
    test_click();
    test_tap();
    test_scroll();
    test_filter();
    test_mode();
    test_release();
}
