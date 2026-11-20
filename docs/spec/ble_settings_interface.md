# Settings and controls over BLE

Android and iOS use the same connection session. The board reports capability contract 3 and the session uses wire version 4.

## Live values and actions

On a current schema-3 connection, live settings values are one atomically
assembled nine-fragment binary State snapshot and a setting edit is Ordered
opcode `24`. The app sends the stable key's typed normalized value through the
shared connection owner; firmware validates and applies it through the existing
`ControlMutationService`, returns its correlated Ordered outcome, then publishes
the resulting settings revision through State. The correlated outcome reports whether firmware accepted the edit. A torn snapshot remains
invisible and receives at most two bounded all-State repair requests.

The settings-enumerate characteristic supplies labels, types, ranges, enum options, categories
and descriptions. A complete binary State snapshot supplies live values to Setup. The exact binary
layout, stable key order and mutation indices are defined by
[settings_state_wire_codec.hpp](../../include/modules/settings_state_wire_codec.hpp) and the shared settings fixture.

Per-player Legal Moves, Check Cue and Automatic Hints are additive typed settings
at mutation indices 41..46. State 43 supplies their six preference bits and the
same settings revision as the unchanged 84-byte base. They are deliberately
omitted from descriptor enumeration. The apps add these Boolean controls when
State 43 matches the base revision. Until then their values are unavailable.
Coaching/O-hold writes the same persisted Automatic Hints key through the existing
mutation owner and reports its ordinary persistence revision.

Firmware 1.0 reserves
State key 47 and Ordered Settings mutation indices 48 and 49, which reject as
unsupported. Existing trial values under `PIECE_ANIMATION_STYLE`,
`P1_PIECE_ANIMATION_STYLE` and `P2_PIECE_ANIMATION_STYLE` survive settings load
and save as opaque, inactive data. The 84-byte Settings base and live-palette State 46
remain active.

CPU difficulty uses the same `VS_CPU_LEVEL` authority in the app and board Quick
menu. App changes update the board's displayed value even while the CPU row is
already open, including its icon-to-number handoff. Board adjustments update the
app's confirmed slider value. A finger-owned app drag keeps its preview through
incoming State, then resolves to acknowledged board truth on release or rejection.

Hint, Undo, New Game and owner-local Coaching use shared Ordered opcode `25`
on the same current connection. Firmware validates the fixed five-byte body,
then invokes the existing `ControlMutationService`; asynchronous Undo/New Game
retain the Ordered position until their coordinator emits a terminal result.
The app emits its existing pending/terminal control events from that one
connection transaction and uses Home State as live Coaching/Undo authority.
A fresh connection requests a new snapshot. A write that was accepted before
disconnect may still take effect. Read the new board state before retrying.
The [connection wire reference](../spec/connection_session_wire.md) defines
the session endpoints, acknowledgement and reconnection rules.

## Descriptor discovery

Read characteristic `6f2a20d1-89df-4ac6-a9f2-53f6f7db0102` on service
`6f2a20d1-89df-4ac6-a9f2-53f6f7db0101`. Firmware supplies chunks of
descriptors. Each object contains `it` (items), `nx` (next cursor) and `dn`
(complete). Firmware advances the cursor on each read. Start at index zero,
accept contiguous indices and publish the list only after `dn: true`.
If a chunk fails, discard the partial list, drain to the sequence boundary
and retry once on the same connection. Reopening Setup reuses the complete
descriptors for that connection. Edits still need a confirmed settings snapshot.

## Response Schema

Each enumerate item uses short JSON keys:

- `k` (string): setting key (stable identifier)
- `i` (number): current zero-based app-visible descriptor index
- `t` (string): value type (`u8`, `u32`, `bool`, `enum`, `str`)
- `c` (string): category label for grouping
- `v` (number | bool | string): current value
- `df` (number | bool | string): default value
- `mn` (number, optional): numeric minimum
- `mx` (number, optional): numeric maximum
- `opts` (string[], optional): allowed values for enum settings
- `d` (string, optional): user-facing description

Only settings intended for companion-app use are included.

Visible descriptor order is append-only. Existing clients must build the
notification index map from the enumerated `i` values instead of inferring
indices from keys. The former P1-X enable and hold descriptors retain reserved
retired indices 26 and 27; neither is enumerated or mutable.
`BOARD_SETUP_ASSISTANCE` remains at index 28 as a default-true Bool in the
Behavior category, followed by `MINIMAL_ANIMATIONS` at index 29.
`SETTLE_ANIM_STYLE` is a persisted Enum in Animation at index 30. Its options
are `landing_flash` (0), `mini_orbit` (1), `ripple` (2, default), `none` (3),
and `rainbow_orbit` (4). None disables optional placement and restore effects.
Move-confirmation policy and required physical guidance remain separate. These settings use the same Ordered edit and State readback as other settings.
The app shows Placement effect only when enumerated, in Board lights, with no
second Advanced Settings entry.

### Hint thinking style

`HINT_THINKING_STYLE` appends after existing visible descriptors as a persisted
Display Enum: stored/wire options `foxie_sweep` (0), `player_clockwise` (1),
`none` (2), `beachball` (3) and `cauldron_brewing` (4). Fresh/reset/unset
preferences use `cauldron_brewing`; all explicit saved alternatives retain their
values. App Board lights labels it **Hint thinking
style** and uses wrapping choice pills ordered None, Clockwise in player color, Litch Sweep,
then Beachball and Cauldron Brewing. Cauldron uses only the requesting human's O
in their selected color while a manual Hint is pending; CPU work keeps Beachball
when this style is selected. Beachball rotates rainbow hues across the four D-pad LEDs and
uses a static rainbow with Minimal Animations. It uses
the existing mutation/persistence owner. Shared-session live authority appends
the first `u16` in State code 39 and mutation index 39; original 80-byte Settings
prefix, indices 0..38, retired slots and Settings domain epoch 2 are unchanged. The app requires
all nine same-identity fragments. See the
[wire contract](../spec/connection_session_wire.md).

### Pawn promotion

`PROMOTION_MODE` appends after all existing visible descriptors as a persisted
Behavior Enum with options `queen` and `choose`, default `queen`. The app shows
Promotion with Queen / Choose as short named choices in Behavior, using its
existing acknowledged settings editor and live-state confirmation. With Queen selected, promotion follows move confirmation automatically. With Choose selected, the player selects one of four pieces and confirms with a fresh O press. Changing this
setting affects the next new promotion; restored pending promotions reopen Choose.

This is Settings domain epoch **2**: Ordered opcode24 body version2, promotion
mutation index40, and a strict 84-byte retained snapshot with four-byte code39
tail. All nine fragments must have the same identity. A malformed tail or an
unsupported mutation version fails validation. See the [wire contract](../spec/connection_session_wire.md).

### Colors at the two board ends

`P1_END_PIECE_WHEEL` and `P2_END_PIECE_WHEEL` store the near and far colors.
`P1_PIECE_WHEEL` and `P2_PIECE_WHEEL` are the corresponding same-numbered
aliases in the descriptor list. Changing `GAME_FIRST_PLAYER` assigns players
to ends while the stored colors stay at those ends. Play also applies
`BOARD_ROTATE` when mapping squares. Litchcraft uses physical rotation and
keeps its canvas orientation independent of player assignment.

Firmware keeps the two hues at least 32 steps apart on the 256-step wheel.
A conflicting request takes the nearest allowed hue. On a tie, firmware
subtracts 32 from the opposite hue, wrapping around the wheel. After an
Applied Checkpoint, the app confirms an adjusted hue from a fresh, complete
Settings State on the same connection at the result revision or newer. The
pair must contain the expected hue and the opposite hue known at submission.
When the adjustment yields the stored hue, firmware retains the same bytes
and revision. Reading those nine fragments again confirms the edit.

The persisted `PLAYER_PALETTE_V1` marker records synchronization of the end
slots and aliases. When loading a record without that marker, firmware keeps
a canonical slot when present and imports its same-numbered alias when
missing. Loading and resaving artwork, games or settings preserves their
stored data formats. See [saved settings and game state](../spec/behavioral_requirements_current.md).

### Automatic brightness

`AUTO_BRIGHTNESS` is appended after `SETTLE_ANIM_STYLE`; deployed descriptor
indices remain unchanged. It is a visible persisted boolean, default true,
using ordinary deferred persistence and existing acknowledged settings writes.
The app shows its Connected switch only when the setting is available. With
Auto on, `BRIGHTNESS` is the saved maximum and ambient adjustment dims within it;
with Auto off, the same level is steady manual brightness. Toggling Auto never
changes `BRIGHTNESS`. False does not disable either ambient sensor or its raw
diagnostics.

### Light-square appearance

The retired `BLE_CONNECTION_INDICATOR` and `V3_BUTTON_LED_EMULATION` descriptors
retain historical indices 32 and 33. Neither is enumerated, persisted, notified,
or mutable. Loading saved settings removes only those obsolete keys; compact
State writes zero in both reservations and later field positions stay unchanged.

Three persisted Display descriptors append after those reserved slots.
They use the existing acknowledged settings endpoint and ordinary deferred
persistence:

- `LIGHT_SQUARE_MODE` is an enum with `white`, then `color`; default `white`.
- `LIGHT_SQUARE_HUE` is a `u32` continuous wheel value `0..255`; default `0`.
- `LIGHT_SQUARE_BRIGHTNESS` is a `u32` intensity `1..8`; default `8`.

Their visible indices are 34, 35, and 36 respectively. The existing
`WHITE_SQUARES_ON` boolean remains the sole on/off setting. Brightness therefore
does not include an overlapping Off value. Mode and brightness apply together:
White and Color retain the same saved intensity when switching between them.
Hue remains persisted while White is selected, but affects output only in Color
mode. Hue and Brightness use the existing coalesced settings writer for live
board updates during a drag; release confirms the latest value. Incoming values
and old acknowledgements cannot replace the finger-owned draft.
The app omits controls whose descriptors are unavailable.
All resulting output continues through global/ambient brightness, RGBW
conversion, and current limiting.
