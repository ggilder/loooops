# Spec for PureData external to manage UI on the Organelle S2

## Context

The Organelle has a 128 x 64 pixel OLED display capable of displaying 5 lines of text (not counting the top line used for VU meters). Each line can be up to 21 characters long. The display is monochrome.

The Organelle has 4 analog knobs and 1 encoder knob/push button.

## Requirements

Design a PureData external that can be used to manage multiple pages of controls on the Organelle's display. For example, the first menu page might look like:

```
1. Volume 100%
2. Volume 20%
3. Volume 56%
4. Volume 80%
REC 1: -4.56s
```

Where the first 4 lines represent the 4 analog knobs, and the last represents a status message. When the user turns the encoder knob to the right, the external receives the message "enc 1" and updates the display to show the next menu page, which might look like:

```
1. Speed -1x
2. Speed 0.5x
3. Speed 1x
4. Speed 2x
REC 1: -4.56s
```

The external briefly flashes larger text "Speed" on the center of the display to indicate the change in menu page. When the user turns the encoder knob to the left, the external receives the message "enc 0" and updates the display back to the previous menu page.

The external also interprets/routes input from the knobs. For example, if the user turns the first knob, the external receives the message "knob 1 0.75" (where 0.75 is the normalized value of the knob). All incoming control values range from 0.0 to 1.0, and the external should be responsible for converting these to the appropriate display values (e.g., converting 0.75 to "Volume 75%"). The menu page definitions should include default starting values. Because these are analog knobs, we want them to have a latching "soft takeover" behavior, where the value only starts to change when the user turns the knob to a value close to the current value (e.g. within 0.01 of the current 0-1 value). This prevents sudden jumps when the knob is at a different position (e.g. from adjusting on a different menu page).

Each menu page defines ids for each of the 4 controls, which is prepended to the outgoing control value. e.g. if knob 1 is controlling the first "Volume" line, and the id for that parameter is "vol1", then when the user turns knob 1 to within the soft takeover threshold, the value starts being sent to the external's outlet as "vol1 0.75". This allows the patch designer to route the control values appropriately.

## Parameter Decoration

The external accepts a `decorate` message that appends optional text to a control's displayed value. For example, if the first line has id `vol1` and the external receives `decorate vol1 (M)`, the display changes to:

```
1. Volume 97% (M)
```

Sending `decorate vol1` with no additional text removes the decoration:

```
1. Volume 97%
```

Decorations are stored per control id and persist until explicitly cleared or overwritten.

## Knob Scaling

Each control definition supports optional `min` and `max` values. The raw 0.0–1.0 knob input is linearly mapped to `[min, max]` before formatting and output. For example, with `min = -100` and `max = 100`, an input of `0.5` maps to `0` and would display as `0%` for a Percentage-type control.

## Set Knobs (Discrete Snap Values)

Controls can define an optional ordered list of discrete `set` values (in real/display units). The incoming 0.0–1.0 knob value is mapped to an integer index into the list: `index = round(rawValue * (numSetValues - 1))`. For example, with 14 values:

```
set = [-2, -1.5, -1.3333, -1, -0.75, -0.6667, -0.5, 0.5, 0.6667, 0.75, 1, 1.3333, 1.5, 2]
```

A raw value of `0.0` selects index 0 (`-2`), `0.5` selects index 6 (`-0.5`), and `1.0` selects index 13 (`2`).

The outlet emits the actual discrete value at the selected index (not normalized 0–1) for set-type controls. Soft-takeover latch detection uses the normalized position of the current index: `currentIndex / (numSetValues - 1)`.

## Latching / Soft Takeover

When the active menu page changes, **all controls on the new page return to soft-takeover (unlatched) mode**. A knob will not update its parameter or the display until the physical knob is turned to within 0.01 (in 0–1 space) of the current parameter value. This prevents sudden jumps when switching between pages.

## Implementation

The menu pages and options should be defined using C++ classes in a way that is easy to extend with new menu pages and options.

To update the display, the external sends messages on its second outlet. The display messages work as follows:

```
# To set the text of a line:
/oled/line/2 2. Volume 100%

# To draw a message "Speed" in the center of the display:
/oled/gFillArea 3 5 23 113 24 0
/oled/gBox 3 5 23 113 24 1
/oled/gPrintln 3 9 28 16 1 Speed
/oled/gFlip 3

# After a delay, to clear the message in the center of the display, the external can just re-send all the /oled/line/n messages.
```

Knob and encoder input messages (described above) are received on the external's first inlet. The external can also receive a "status" message with an arbitrary list of PureData atoms, which should be stored and displayed on the last line of the display.
