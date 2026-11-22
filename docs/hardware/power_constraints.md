# Power Constraints

## Variant Summary

| Variant | Rail Gate | USB-C CC Sense | Ambient Sensors | LED Budgeting | Confidence |
| --- | --- | --- | --- | --- | --- |
| `esp32s3` | yes | yes | yes, `2x VEML6030` | yes | `confirmed` |
| `esp32c3` | no live rail gate | no | no | basic brightness only, no CC-aware budgeting | `confirmed` |

## S3 LED Current Budget Contract

The S3 firmware reserves 234 mA for the system and LED rail. Without a
brownout latch, the output ramp targets a 1,234 mA modeled board limit
(1,000 mA for LEDs), including on an unknown or default-current source. At
boot the ramp starts at 450 mA total. Brownout protection holds it there,
leaving 216 mA for LEDs. These are firmware model limits. USB descriptor and
source-current requirements are discussed below.
[Envelope selection](../../include/modules/led_power_envelope.hpp)

To supply the modeled maximum board load on a V3.3 or V3.31, use a 5 V
USB-C supply and cable
that can deliver at least 1.5 A. This provides headroom over the modeled
1,234 mA target. A computer port advertising default USB current can supply
less than the renderer requests. Current draw during boot, bright output,
suspend and wake still needs measurement on the assembled launch board.

The V3.3/V3.31 launch board has three-channel RGB emitters on both chains.
Its firmware estimates up to 20 mA per color channel and applies the
`128/256` output gain. The same whole-board limits apply.
[Firmware power model](../../src/firmware_led_power_budget.cpp) ·
[Board profile](../../include/hardware/board_hardware_profile.hpp)

The earlier RGBW calibration measured:

- core baseline: about `118 mA`
- ambient baseline: about `120 mA`
- rail baseline: about `234 mA`
- per-die full-scale current: about `5.07 / 5.13 / 5.15 / 12.22 mA` for red / green / blue / white

The S3 TinyUSB descriptor declares bus-powered operation and requests `500mA`
as its configured USB 2.0 maximum. The runtime authority model deliberately
keeps that configured request separate from both the Type-C Rp tier and the USB
data-bus state: Default USB Power permits `100mA` before configuration, Litch's
margin-limited `450mA` envelope after configuration, and less than `2.5mA`
during suspend. Confirmed `1.5A` or `3.0A` Type-C current is a separate source
authorization.

Those values are currently modeled but not yet enforced during boot or USB
suspend. The measured `118mA` rail-off core baseline is already above the
pre-configuration unit load. The ESP32-S3 full-speed USB peripheral also cannot
wake the chip from light sleep, so a transparent suspend/light-sleep/USB-resume
path is unavailable. Current hardware therefore needs an explicit product
choice between disconnecting into a physical-input-wake sleep state on Default
USB Power suspend, or requiring a confirmed higher-current Type-C source. Do
not present the descriptor or LED limiter alone as proof of USB bus-current
compliance.

The limiter checks the quantized output for the selected emitters, including
RGB-to-RGBW conversion on V2/V3.0. The `ledpower` command labels requested and
applied values as the last modeled frame; fractional modeled milliamps round
upward at the hard boundary.

The raw `amptest` characterization flow is the sole intentional limiter
exception because it measures physical channel draw. It starts each channel at
level `1`, prints an explicit warning, and requires a current-limited bench
source. The normal renderer, boot-recovery surface, boot hardware-test renderer,
sleep fade, `setled` diagnostic renderer, and `amptest verify` remain inside the
active envelope. Pre-settings boot visuals are forced to the conservative
envelope. Boot recovery applies a tighter source-independent `120 mA` LED cap;
on S3 its modeled total is therefore at most `354 mA` including the calibrated
`234 mA` fixed load.

The white factory inspection field on V3.3/V3.31 lights the red, green and
blue channels. The launch board has no separate white die.

## USB-C Source Classification

The default CC ADC thresholds are 320 for default USB current, 580 for 1.5 A
and 850 for 3 A. An ambiguous pair leaves the advertised current unknown.
Three guarded CC1/CC2 pairs on the same active pin, at least one second apart,
can confirm a strong advertisement. With safety settings available, a confirmed
source clears brownout protection automatically. A sustained drop on a
confirmed pin triggers a full-pair recheck. A CC reading reports the source's
advertisement. Cable drop and available voltage margin require measurement.
[Source qualification](../../src/firmware_led_power_source.cpp) ·
[Hardware contract](hardware_overview.md#target-variants)

## Brightness Policy That Touches Hardware Limits

The user brightness setting spans 0–8. At levels 1–8 the slider sets the
bright-room ceiling. In a dark room Auto scales output to about one quarter
of that ceiling, with a minimum scale of 12/256 for low levels. The same base
applies before a usable sensor reading. Level 0 stays at its near-dark setting. The revision's emitter gain and
the final current limiter apply to the resulting frame.
[Render policy](../../src/firmware_led_power_budget.cpp)

## Switched LED rail

On the launch boards GPIO11 controls the LED rail. Before enabling it, firmware drives both LED data pins low, waits for the gate to settle and applies a hardware PWM precharge. V2 uses GPIO13. Earlier S3 profiles use timed GPIO pulses. See the [firmware sequence](../../src/power_rail.cpp).

### Shared V2/V3 gate-circuit model

The V3 schematic dated 2026-08-29 documents the same `5V_EN` circuit used on
V2. `VBUS` feeds the switched `+5V` domain through P-channel MOSFET Q1
(`SC015P2G`). N-channel Q2 (`2N7002`) pulls Q1's gate low when the ESP32 drives
`5V_EN` high. R13 (`100 kOhm`) holds Q2 off while the ESP32 pin is reset or
high-impedance, and R11 (`100 kOhm`) then pulls Q1's gate back to its source, so
the hardware default is rail off. R12 (`330 Ohm`) limits transient Q2
gate-charge current. C11 and C13 provide `20 uF` of nominal bulk capacitance on
the switched output.

C10 (`100 nF`) is in parallel with R11 from Q1 source to gate. The resulting
nominal `10 ms` R-C constant mainly shapes gate recovery/turn-off; Q2 actively
sinks the gate on enable, so this network must not be treated as a specified
`10 ms` soft-start or current limit. It does participate in Q1 gate recovery
between firmware pulses. The pulse train is the current intentional
inrush-shaping mechanism, but its actual Q1 gate waveform, `+5V` rise, and peak
input current are physical properties to measure rather than infer from the
digital timing alone.

Disable / sleep path:

- fade frames pass through the same revision-specific power limiter as normal frames
- confirm asynchronous LED quiescence before changing rail or data routing
- disconnect and drive both LED data pins low
- drive `5V_EN` low, retaining the existing 1 ms hold
- keep leakage down on non-wake GPIOs

On V3.31, disabling the LED rail leaves the 3.3 V sensor supply on.
[Hall wiring](sensor_topology.md)

## Sleep And Leakage Rules

- Before sleep, the firmware attempts to release and, if needed, recover the I2C bus.
- If the I2C bus does not return to idle-high, sleep is refused.
- Wake buttons must be released and a hardware wake source must be configured
  before destructive teardown continues.
- Any sleep-entry return after rail/peripheral shutdown causes a restart rather
  than resuming in a partially powered-down state.
- After an EXT1/deep-sleep wake, RTC wake pads are returned to digital GPIO mode
  before the normal button-input layer starts.
- Non-wake inputs are floated or have pulls disabled where possible.
- LED data and rail-control pins are driven low for leakage control.

## Reset And Stability Assumptions

- Brownout, power-on, and external resets are treated as hard resets.
- Software, panic, interrupt watchdog, task watchdog, and watchdog resets are treated as crashy resets.
- A confirmed brownout reset activates protection. Normal reboots retain it.
  A qualified strong source or an explicit reset of protection can clear it.
- A complete rail collapse can report `POWERON`, just like unplugging the
  board. Distinguishing those cases requires voltage or rail-good evidence.

## C3 Notes

- The repo does not define a C3 USB-C current classification path.
- The repo does not define a live C3 `5V_EN` sequence.
- The repo does not provide measured C3 current baselines comparable to the S3 amptest references.
