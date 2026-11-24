# Touchpad mouse

The dongle can present the DualSense touchpad to the host as a regular USB mouse.
Nothing has to run on the host: it sees one more HID mouse on the adapter.

The base firmware includes this with `ENABLE_WAKE_HID`. A custom build without it
retains the configuration but has no mouse interface.

## Enabling

```powershell
python tools/config_tool.py set touch_mouse=1
# Unplug/replug the adapter here to enumerate the mouse interface.
```

`touch_mouse` has three values:

- `0`: Off (default). No mouse interface.
- `1`: On. The touchpad drives the mouse as soon as the controller connects.
- `2`: Standby. The mouse interface is present but idle until the mouse mode is
  switched on with a shortcut (see [Switching at runtime](#switching-at-runtime)).

Changing between `0` and the other two values needs a USB reconnect, because it
adds or removes a USB interface. Changing between `1` and `2` applies at once.

## Gestures

| Touchpad                         | Mouse                  |
|----------------------------------|------------------------|
| Move one finger                  | Move the pointer       |
| Press the pad                    | Left button            |
| Press the pad with two fingers   | Right button           |
| Press and move                   | Drag                   |
| Tap                              | Left click             |
| Tap with two fingers             | Right click            |
| Move two fingers up/down         | Scroll wheel           |
| Move two fingers left/right      | Horizontal scroll      |

Scrolling follows the fingers: moving them down scrolls towards the top of the
page. A scroll stays on the axis it started on until a finger lifts.

A press is assigned its button when the pad goes down and keeps it until the pad
comes back up.

The pointer holds still where a click is aimed:

- A finger that has just landed does not move the pointer while it could still
  become a tap, that is for its first 200 ms unless it travels about 2 mm
  first. A tap therefore never moves the pointer. With `touch_mouse_tap=0`
  there is no such wait.
- Pressing the pad stops the pointer until the finger has travelled about
  1 mm, from where on it drags.

## Settings

- `touch_mouse_sensitivity` — pointer speed, `1`..`50`. The speed is
  proportional to the value: `20` is twice as fast as the default `10`. At the
  default a swipe across the whole pad is 640 pointer counts, which favours
  fine control over reach; the host applies its own pointer speed and
  acceleration on top. A new value applies at once, so it can be tried out
  without reconnecting.
- `touch_mouse_tap` — `1` (default) lets a tap click; `0` leaves clicking to
  pressing the pad.

```powershell
python tools/config_tool.py set touch_mouse_sensitivity=15 touch_mouse_tap=0
```

## Switching at runtime

Many games switch their button prompts to mouse and keyboard as soon as the
pointer moves, so the mouse mode can be switched from the controller. Assign the
`touch_mouse_toggle` action to any shortcut slot:

```powershell
python tools/config_tool.py shortcut "7=PS+Pad:touch_mouse_toggle"
```

Each firing switches the mouse mode on or off. With `touch_mouse=1` the mode
starts on, with `touch_mouse=2` it starts off; the switch is not saved, so a
restart returns to that starting state. The action does nothing while
`touch_mouse=0`.

A chord that contains the pad does not click: pressing the pad while the chord's
other button is already held is left to the shortcut. Hold the other button
first.

## What the game sees

While the mouse mode is on, the gamepad report shows the touchpad untouched and
not pressed, so a game does not react to the same input twice. While the mode is
off, the touchpad reaches the game unchanged.

The mouse reads the physical pad before button remapping. A button remapped
*to* `Pad` still reaches the game as a pad press in mouse mode.

## Notes

- Enabling the mouse also enumerates the keyboard and consumer control
  interfaces, as `enable_keyboard=1` does. The optional interfaces are appended
  in a fixed order, which keeps every HID instance number constant.
- A configuration tool that does not know these settings (older
  `config_tool.py`, a web config that has not been updated) leaves them as they
  are when it saves.

## Wire format / review notes

- `Config_body` gains three trailing bytes: `touch_mouse`,
  `touch_mouse_sensitivity`, `touch_mouse_tap`. Existing offsets are unchanged
  and `CONFIG_VERSION` is not bumped; on a stored config from older firmware the
  new bytes read `0xFF` and validate to their defaults (`0`, `10`, `1`).
- A `0xF6` config update whose `touch_mouse_sensitivity` byte is `0` keeps the
  three stored values. `0` is outside the valid range, and it is what a sender
  that zero-pads a shorter `Config_body` produces.
- Shortcut action `3` (`ShortcutActionTouchMouseToggle`), no payload. Like
  `bt_disconnect` it accepts tap, double-tap and chord triggers, not `@hold`.
- USB: interface after the consumer control, HID instance 3, EP IN `0x89`,
  1 ms interval, not a boot device. 5-byte report without report ID: buttons,
  X, Y, wheel, AC Pan (the layout of TinyUSB's `hid_mouse_report_t`).
- Motion and wheel steps that do not fit one report, or meet a busy endpoint,
  are kept and sent by the main-loop pass.
- Held buttons are released on mode off, Bluetooth disconnect and after USB
  resume. USB re-enumeration starts from a clean state.
- A tap sends its down report, then its up report 30 ms later; a second tap
  inside that time is queued behind the up report.
- Pointer counts are `travel * touch_mouse_sensitivity / 30`, with the remainder
  carried into the next report.
- Host tests: `python tests/test_touch_mouse.py`.
