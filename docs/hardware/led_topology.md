# LED Topology

| Firmware profile | Square LEDs | Perimeter LEDs | Button LEDs | Outputs used | Emitter and wire order |
| --- | ---: | ---: | ---: | ---: | --- |
| **V3.3 / V3.31 launch (S3)** | 256 | 32 | 12 | **300** | SK6812SIDE-A RGB, GRB |
| V3 (S3) | 256 | 32 | 0 | 288 | SK6805SIDE-FRGBW, GRBW |
| V2 (S3) | 192 | 32 | 0 | 224 | SK6805SIDE-FRGBW, GRBW |
| V1 prototype (C3) | 192 | 32 virtual | 0 | 192 | WS2812x RGB, GRB |

## V3.3 / V3.31 launch wiring

GPIO 47 (RMT0) drives the square chain, LED1–LED256. Each of the 64 squares
has four emitters, wired left, top, right, bottom. Left and right face outward
to illuminate the square. Top and bottom face inward to illuminate the piece.
Squares run row-major from the top left in PCB view. Both chains send 24-bit
GRB and use a 300 µs reset interval. [LED output mapping](../../include/hardware/board_led_output_mapping.hpp) · [Renderer](../../include/hardware/neopixel_led_renderer.hpp)

GPIO 33 (RMT1) drives LED257–LED300. With P2 at the top in PCB view, the
32 perimeter positions run clockwise from the top left. The chain passes
through six physical button LEDs at each player end. [Status-chain mapping](../../include/hardware/status_led_chain_profile.hpp)

| Status-chain index (zero-based) | PCB designators | Position or local button order |
| --- | --- | --- |
| 0–3 | LED257–LED260 | Perimeter 0–3 |
| 4–9 | LED261–LED266 | P2 O, Up, Right, Down, Left, X |
| 10–25 | LED267–LED282 | Perimeter 4–19 |
| 26–31 | LED283–LED288 | P1 O, Up, Right, Down, Left, X |
| 32–43 | LED289–LED300 | Perimeter 20–31 |

## Rendering and rotation

The board frame carries left, top, right and bottom independently for each
square. The canonical index runs from `0 = a8` to `63 = h1`. The four-pixel
profiles use physical index `4 × square + offset`. A 180-degree rotation
maps the square to `63 − square`, swaps left with right and top with bottom.
The three-role frame addresses `square0`, `piece`, `square2` at
`3 × square + offset`. Its piece role feeds both inward LEDs on four-pixel
boards. V1/V2 collapse independent top and bottom requests to their rounded
mean on the single piece LED. The hardware mapper applies these rules before
current estimation and limiting. [Board frame](../../include/modules/board_led_frame.hpp) · [Output mapping](../../include/hardware/board_led_output_mapping.hpp)

A 180-degree perimeter rotation adds 16 modulo 32 before the mapper skips
the button LEDs. During ordinary play, status follows the player-end and
board-rotation settings. Menus and the sleep countdown follow board rotation.
Control cues follow the same bank swap as button inputs, preserving each
button's local direction. [Orientation](../../include/modules/orientation.hpp) · [Render path](../../src/led_animation_render_output.cpp)

## Earlier ESP32-S3 wiring

V2 and V3 use GPIO 47 for board data and GPIO 33 for the separate 32-LED
perimeter. Both chains use GRBW and an 80 µs reset. V3 has the same four-LED
square layout as the launch board. Its uninterrupted perimeter runs from
LED257 to LED288. The V2 square chain has three LEDs per square. Its exact
perimeter entry point and winding are documented only indirectly. [Revision map](../../config/hardware/led_map.yaml)

## V1 ESP32-C3 virtual perimeter

The C3 build keeps `EDGE_LED_COUNT=0` and blends status into perimeter square LEDs.

- Each corner square carries its two adjacent perimeter positions on distinct outer LED slots.
- Virtual edge indices `0..7` and `16..23` blend into `square0` slots.
- Virtual edge indices `8..15` and `24..31` blend into `square2` slots.
- Blend alpha is capped at `160`, so virtual edge UI never fully overrides square semantics.

| Virtual Edge Indices | Meaning | Backing Squares |
| --- | --- | --- |
| `0..7` | North edge | `h8, g8, f8, e8, d8, c8, b8, a8` |
| `8..15` | East edge | `h8, h7, h6, h5, h4, h3, h2, h1` |
| `16..23` | South edge | `a1, b1, c1, d1, e1, f1, g1, h1` |
| `24..31` | West edge | `a1, a2, a3, a4, a5, a6, a7, a8` |

The C3 consumes the same semantic status frame and overlay priorities as S3.
Only the final hardware adapter differs. The exact outward-facing corner-slot
ordering is a firmware mapping; it has not been physically confirmed on the
V1 prototype.

Firmware can also clear 32 appended GPIO 2 pixels. Their physical population on the V1 prototype is unconfirmed.
