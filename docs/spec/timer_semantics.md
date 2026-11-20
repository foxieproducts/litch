# TIMER setting semantics (Quick Setup: "Game clock")

This document defines the meaning of the persisted integer setting `TIMER`
(canonical menu option index 5 and the seventh board Quick Setup item), with
allowed values `0..6`.

The built-in clock is intended for casual play.

## 1) Value mapping

`TIMER` is an index into a small, fixed set of common time controls.

| `TIMER` | Label (UI) | Base time (min) | Increment (sec) | Notes |
|---:|---|---:|---:|---|
| 0 | OFF | 0 | 0 | Clock disabled |
| 1 | 1+0 | 1 | 0 | Bullet (simple) |
| 2 | 3+2 | 3 | 2 | Blitz (common online) |
| 3 | 5+0 | 5 | 0 | Blitz (OTB-friendly) |
| 4 | 10+0 | 10 | 0 | Rapid (simple) |
| 5 | 15+10 | 15 | 10 | Rapid (common OTB) |
| 6 | 30+0 | 30 | 0 | Classical-lite |

**Encoding:** base time is per-player. Increment is Fischer increment, applied after a move is committed.

## 2) When the clock runs

Clock time is tracked per chess side internally, while Home presents only
**P1** and **P2** time in ordinary UI.

- **Clock enabled:** `TIMER != 0`
- **Setting mutation:** `TIMER` uses the canonical acknowledged setting owner
  and is selectable immediately after New Game, including incomplete piece
  setup, the King/Queen setup prompt and background Hint analysis. Unlike
  orientation changes, selecting a time control does not access the engine.
  A changed value is rejected once the clock session starts or a move/Undo
  owns the board. Returning the first picked-up piece after time starts does
  not reopen the selector or refill time. An effective `Noop` remains a
  side-effect-free acknowledgement. A rejected write cannot reset either live
  clock.
- **Persisted authority:** a present clock record normally restores strictly;
  a timer mismatch, elapsed time, pending action, side mismatch in an enabled
  clock, timeout, or malformed record must not silently refill a played game.
  A well-formed disabled (`TIMER=0`) record carries no active-side authority:
  its canonical White marker must not invalidate a Black-to-move game. It must
  still have no started, running, paused or timed-out state, and zero remaining
  time and increment. One bounded repair
  is allowed when the position is a canonical untouched initial setup with no
  moved squares, undo history, queued CPU move, or interaction state, and an
  accepted start-owned `TIMER` change made the clock record stale. Retire only
  that stale record only when both saved clocks still equal the saved base
  time and it is not terminal; then apply the current `TIMER` setting. A valid
  terminal clock keeps its saved episode preset/result even when a previous
  candidate accepted a separate newer `TIMER` write. The newer preference is
  retained for explicit New Game. Optional terminal presentation cannot change
  this core restore policy. A partially elapsed non-terminal clock still fails
  closed. A repaired unused record is scheduled for durable removal.
- **Clock start condition:** a playable initial setup keeps both clocks at their
  full base time and begins a 750 ms setup-stability window. A disturbance that
  returns to the ready setup during that window rearms it, so a dropped or
  briefly tipped piece does not start play. After the stable boundary, the
  mover's clock starts from the authoritative observation time of the first
  legal pickup; if a legal pickup is already held when the boundary arrives,
  time starts at that boundary. Illegal pickup, setup correction, and an
  untouched ready board do not start either clock. The first committed move
  switches to the next side; it is not the clock-start event.
- **Clock stop condition:** stop both clocks on game end or when the game is
  abandoned/reset. Classic game end includes checkmate and forced draws. Duck
  variant game end is only an explicit `KingCaptured` or `Fowled` outcome;
  clock timeout is a separate terminal cause in either variant. Orthodox
  check/checkmate/draw state is ignored in Duck.

**Pause conditions:**
- Pause while the board is in a non-playable correction flow (CPU placement / mismatch correction).
- Pause while any illegal-state marker is active (human-illegal or CPU-illegal), since the game is effectively “blocked” until corrected.
- Pause while board Quick Setup is active. App settings browsing alone does not
  pause the board clock.
- In VS-CPU play, keep the CPU side running during engine search. Once a CPU
  move is selected and queued for the person to perform on the physical board,
  pause both sides. Resume the human side only after that physical move is
  confirmed and committed.

## 3) Switching sides and applying increment

- The active side’s clock runs whenever it is that side’s turn.
- **Switch moment:** when the move is committed to the engine (the same moment the engine side-to-move flips).
- **Increment moment:** immediately after a move is committed, add increment seconds to the side that just moved.

Duck Chess v1 treats the chess move and mandatory duck relocation as one clock
turn:

- The moving side's clock continues during chess-piece settle, promotion choice,
  capture/castling/en-passant cleanup, and valid duck lift/place progress.
- Staging the chess move does not switch clocks or apply increment. The clock
  switches and increment is applied exactly once only after the duck reaches a
  different empty square and the combined turn commits.
- Returning the duck to its source or correcting a relocation does not apply
  increment. Reducer-owned correction and other non-playable mismatch states
  pause under the normal correction policy.
- Capturing the opposing king is the terminal exception: it commits and stops
  the clock immediately without a duck relocation.

When a next-side pickup ends timed settle, the committed move switches the
clock and applies the mover's increment once. The same observation then owns
the next turn; its valid held piece must not leave the clock paused by the
temporary mismatch before action acknowledgement. Circle mode still requires
O before that move can commit.

Edge cases:
- **Promotion:** promotion selection is part of the moving side’s turn. In
  Classic, time runs until the choice commits the move. In Duck, the selected
  suffix remains staged and time continues through the mandatory duck
  relocation until the combined turn commits.
- **Undo:** when undo rewinds engine state, recompute which side is active from
  the engine; do not apply increment. Duck undo reverses the recorded duck move
  and chess move as one history unit and likewise applies no increment.
- **Post-commit grace takeback:** forward commit and turn/clock transition happen
  immediately. A successful automatic grace takeback restores the exact
  pre-move clock snapshot, removes any Fischer increment, and refunds time spent
  by the other side during the grace interval. Cancel restores the exact
  post-move snapshot. Ordinary manual Undo retains its existing policy.
- **VS CPU:** CPU time is tracked under the same authority as human time. During
  async CPU thinking, the CPU clock runs. Physical execution of the selected
  CPU move pauses both sides, and confirmation switches to and resumes the human
  side. If CPU search is cancelled before a move is committed, no increment is
  applied.

## 4) Display requirements (menu + board)

The current UI is:

- The seventh board Quick Setup item steps through `0..6` with Up/Down through the
  canonical acknowledged setting-mutation path.
- When `TIMER` changes, display a tiny numeric on-board overlay (<=2 chars)
  showing:
	- `0` when disabled
	- base minutes when enabled (e.g., `3`, `10`, `15`, `30`)
	- optional: use color to hint increment (e.g., green when increment is non-zero)
- Do not use a letter abbreviation for the disabled state; board-grid letters
  are not assumed legible on any hardware revision.
- Home begins its **Board actions** card below the game surface with one compact
  P1, P2, and **Game clock** row. Player clocks use the configured
  assigned physical-end colors as accents while labels and time remain the
  non-color authority.
  Disabled clocks say **No clock**. Before the first move, the preset control
  uses the acknowledged `TIMER` write path, even before setup is complete.
  After play starts or while a move/Undo owns the board, it stays visible as
  read-only status and explains
  when it can be changed without describing the supported clock as unavailable
  or moving the chessboard. If a start-only write races the beginning of play,
  Home invalidates the stale selector, visibly resynchronizes, and adopts the
  board's authoritative in-game clock state before offering another change.
- During play, the low-time warning overlay (king square red) remains the
  physical-board clock presentation. Timeout is terminal and Home names the
  winner on time.
- On timeout, the board uses the selected existing victory animation and the
  explicit winner's assigned home edge at fatal priority. It keeps the final
  acknowledged chess placement, including a staged Duck chess half, without
  adding incomplete history or increment. The coordinator retires pending
  actions/search; late Duck placement and Undo cannot revive play. Only an
  explicit New Game/reset/load boundary clears the result. Ordinary start-only
  settings remain unavailable on the terminal opening.
- A stopped clock and winner restore independently of optional terminal Duck
  presentation. The bounded `GAME_CLOCK_TERMINAL_V1` supplement binds the
  completed chess-half UCI to clock session/epoch and canonical FEN. Stale or
  malformed presentation is ignored with a diagnostic; the valid terminal
  clock remains stopped. Earlier firmware ignores the supplement and restores
  its canonical terminal board. Core malformed clock authority still fails
  closed. No resumable Duck plan is stored with timeout.

## 5) BLE compatibility

BLE should treat `TIMER` as a stable, versioned enum-like value:

- **Read:** expose the raw `0..6` plus (optionally) derived `baseSeconds` and `incrementSeconds`.
- **Write:** accept `0..6`. Reject/clip out-of-range values.
- **Forward compatibility:** reserve the right to add an “extended/custom clock” mode later without changing existing meanings of `0..6` (e.g., new BLE message type for custom base/inc, or a separate key).
