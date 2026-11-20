# Controls

V3.3 and V3.31 launch boards have six controls on each end: Up, Left, Down,
Right, X and O. The [pin map](pin_map.md#physical-controls) lists their GPIOs
and the corresponding V3, V2 and V1 prototype connections.

## Shared Electrical And Behavioral Rules

- All buttons use `INPUT_PULLUP`.
- All buttons are active-low.
- V3.3 and V3.31 capture both GPIO edges. The first contact closure registers
  the press. The contact must stay open for 20 ms before another tap can
  register. Firmware keeps the edge timestamps when applying tap and hold
  gestures, even if it was busy when the button moved.
- Earlier boards and the polling fallback debounce both transitions for 20 ms.
- Direction buttons only:
  - before repeat: `250 ms`
  - repeat interval: `19 ms` for Up/Down, `50 ms` for Left/Right
- Default long-hold behavior is disabled (`0 ms`) unless another policy layer opts in.

On a launch board, `input capture status` reports the active mode, pending
edges and overflow count. For a side-by-side check, release the buttons, then
use `input capture poll` or `input capture irq`. A reboot restores interrupt
capture.

Each direction pad has four active inputs. The older center nets are unused.
During play, tap X for Undo or hold it to open Quick Setup. Tap O for a Hint
or hold it to toggle Coaching. For the two-button gesture, press X and O on
the same end within 750 ms of each other during play. See the
[board controls guide](../controls_and_menu.md) for the actions in play and Litchcraft.

## Control banks and player ends

P1 and P2 on the circuit board identify physical ends. Swapping player ends or
rotating gameplay can change which player uses each end. Each direction is
relative to the person seated at that end. Factory inspection always names the
physical end printed on the board.

The V3 P2 direction pad is mounted at a different angle. V3.3 and V3.31 use
individual switches. Firmware maps both layouts to the same player controls.
The retained V1 prototype profile maps one bank of four directions and two face buttons.

## Boot-Time Control Sequences

- On V3.3 and V3.31 launch boards, holding either physical Up during cold
  boot requests fresh hardware inspection. A saved PASS stays protected
  until a fresh result is saved. See the [functional test](../release/factory_programming.md#functional-test).
- For a user factory reset, hold Down on exactly one fitted bank at power-on.
  Release Down, X and O. Within the ten-second prompt, hold X+O together on
  the same bank for two seconds to confirm. In an active hardware test, a
  five-second X+O hold instead records a factory-test Override.

## Sleep / Wake Behavior

- All four directions on each fitted bank can wake the board. On V3.3/V3.31
  and older V3, P1 O, P2 X and P2 O also wake via EXT1. Every S3 handles
  P1 X/GPIO0 separately: the ULP latches the press and wakes on stable release.
  V1 uses the directional inputs only.
- After wake, input is ignored until all buttons are released and 200 ms has
  elapsed. Before sleep teardown the wake controls must be released and the
  selected source must have been configured successfully.
