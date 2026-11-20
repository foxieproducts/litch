# Saved game data

The board saves the last committed chess position and the interaction state needed to resume a game. The app can retain completed games for replay.

## Persisted game state

- Core keys:
  - `GAME_FEN`
  - `GAME_ACCEPTED_PLAY` is an optional boolean, written only when true. It
    retains accepted-play provenance across a timed final Undo and save/reload.
    A completed final Undo at the canonical opening with no chess timer clears
    it and restores full fresh-game setup; explicit New Game also clears it.
    Records without this key use
    position/history inference. It does not reset consumed clock time.
  - `GAME_CLOCK_V1` retains its 11-field clock record. Optional
    `GAME_CLOCK_TERMINAL_V1` stores `[1, session, epoch, canonicalFen, duckChessUci]`
    only for a timed-out staged Duck half. Exact clock/position binding and the
    existing Duck rules validate its immutable presentation; it contains no
    resumable action. Invalid/stale presentation never invalidates a valid
    stopped clock/result, and unused data is removed on the next game write.
  - `GAME_MOVED_SQUARES` (array of indices)
  - `GAME_INTERACTION_V1` is an optional versioned compact hex supplement for a
    non-idle tagged interaction. It stores only bounded phase/plan/progress/token
    state; `GAME_FEN` remains the last committed engine position. The key
    name remains stable while the current payload schema is version 3; the loader also reads
    version-1 and version-2 records.
  - `GAME_VARIANT_V1` stores the latched/undecided variant, tracked duck square,
    Duck turn phase, pending staged chess move (including promotion suffix), and
    explicit variant outcome.
  - `GAME_DUCK_UNDO_V1` stores one validated duck-from/duck-to pair aligned with
    each persisted chess undo entry. A missing pair is valid only for the final
    king-capture entry, which skips relocation; ordinary entries require a
    distinct-square pair, pairs form a continuous chain, and the tail must end
    at the durable current duck square. Missing or inconsistent Duck history
    fails restore, while Classic snapshots remain compatible without this key.
  - `GAME_CPU_MOVE` stores the Classic move fields and optionally stores the
    selected Duck destination. A queued Duck CPU prompt may restore only when
    its chess move and required destination both validate against the restored
    FEN/duck position; a king-capture move is the only valid omission.
  - `GAME_FIRST_PLAYER` selects which physical near/far end, relative to literal
    `BOARD_ROTATE`, is logical P1; the other is logical P2. Logical P1 always
    opens (internally engine White); the initial FEN side-to-move does not change.
    Gameplay controls, square/Hall mapping and player-owned status cues follow
    that end assignment: logical-to-raw gameplay rotation is `BOARD_ROTATE XOR
    (GAME_FIRST_PLAYER == 2)`. Logical FEN coordinates and stored near/far palettes
    remain unchanged. P1 occupies the bottom of Play normally and the top after
    swapping ends. File letters and rank numbers on all four edges follow the
    pieces. The upper-left square is a8 normally and h1 after swapping ends.
    Spoken square names and move guidance use the same algebraic coordinates.
    End hues stay fixed in either board pose.
    Litchcraft ignores this gameplay assignment completely, including raw Hall,
    controls, artwork, palette and status presentation.
    `BOARD_ROTATE` compensates a physical half-turn of the board. Canonical
    near/far palettes (`P1_END_PIECE_WHEEL` / `P2_END_PIECE_WHEEL`) and
    same-numbered compatibility aliases never swap with `GAME_FIRST_PLAYER`.
    Logical player colors are selected from their assigned fixed end slots.
    On the first corrected upgrade, canonical colors win over stale swapped
    aliases and private `PLAYER_PALETTE_V1=1` records synchronized provenance.
    Later older-firmware alias edits/resets import into the same end slot before
    resynchronization. Settings containing only the numbered aliases import P1 to P1 and P2 to P2.
    Physical Up/Down shortcuts edit the normalized
    player's assigned end palette visible at those controls. A tap takes one wheel step;
    each held repeat takes two allowed steps in the same direction, including
    wraparound and the existing other-player color exclusion. An
    opening-arrangement change at the untouched opening queues the K/Q
    correction described below.
    `BOARD_ROTATE` alone applies the literal 180-degree board/menu rotation.
    Animation and semantic-frame composers remain orientation-free: they emit
    logical board squares and logical P1/P2 status edges. The final hardware
    mapping boundary applies gameplay square/end assignment, or literal
    orientation alone for Litchcraft, exactly once. Physically turning the board
    while changing `BOARD_ROTATE` cancels in the viewer frame; both app board
    bottoms represent the physical near edge. Fixed
    physical indicators (including the printed USB controls) and physical
    surface effects are added only after that mapping; manufacturing, amplifier
    test, and explicitly raw physical diagnostics remain physical exceptions.
  - `VS_CPU_PLAYER` stores which logical player the CPU controls (`1`=P1 /
    `2`=P2, default P2). CPU turn and undo-ply policy derive only from this
    setting; player-side placement and board rotation do not change the CPU's
    logical seat or chess side.
- Undo history (optimized to avoid nested JSON):
  - `GAME_UNDO_BASE_FEN`
  - `GAME_UNDO_BASE_MOVED_SQUARES`
  - `GAME_UNDO_HISTORY` is persisted as a compact hex string (2 chars per byte)
    containing a flat byte array of 7 fields per move:
    1) side
    2) moved piece
    3) captured piece
    4) captured square
    5) promoted piece
    6) from
    7) to

    Notes:
    - Undo history is bounded to a fixed 128-move tail so snapshots reliably fit
      within the SettingsStore JSON document. On the next committed move, the
      oldest FEN/chess/duck unit rolls out atomically and the newest combined
      turn remains aligned and undoable.
    - The runtime ply-position ring uses the existing 43-byte game-history
      position codec for all six FEN fields, including both uint32 move
      counters. It retains 129 positions for the same 128-move tail. Engine
      and persisted FEN remain canonical text; the neutral duck and tracked
      halfmove clock remain in their existing aligned owners. Repetition
      compares board, side and castling, excluding move counters. For Classic
      repetition only, an en-passant target distinguishes positions when a legal
      capture exists; unavailable or pinned-only captures do not. Raw FEN,
      journal/protocol position authority and ordinary cache keys retain the
      original en-passant metadata. Duck retains its separate outcome rules.
    - Play shows the FEN fullmove number at the last completed turn. It starts
      from the supplied position's number and increases after Black commits;
      a pending Duck relocation retains the previous number. Undo, rebranch,
      reload and rolling the retained history preserve this chronology.
    - A save can encode `GAME_UNDO_HISTORY` as a JSON integer array. This format
      is still accepted on load.



For the save and recovery implementations, see [game persistence](../../include/modules/game_persistence.hpp) and the [persistence coordinator](../../include/modules/game_persistence_coordinator.hpp).
