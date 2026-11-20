# Litch UCI Runner

`litch_uci` exposes the production Litch chess engine through the Universal
Chess Interface for local matches and Elo experiments. It is a headless host
tool and has no SDL dependency.

## Build

From the repository root:

```bash
cmake -S tools/uci -B build/uci
cmake --build build/uci --target litch_uci -j2
```

The executable is `build/uci/litch_uci`.

## Try a position

The following position has exactly one legal move. The runner should print `bestmove a8b8`.

```bash
printf 'uci\nisready\nposition fen k7/8/2K5/8/8/8/8/R7 b - - 0 1\ngo nodes 128 depth 1\nquit\n' \
  | build/uci/litch_uci
```

CPU first completes a reply-aware preliminary comparison of up to two ordinary
plies, using static leaves within the requested depth ceiling. It then starts
its usual quiescence-inclusive Full search at depth one. Final JSON includes
`preliminary_only`; a tagged preliminary depth two followed by Full depth one
is stage precedence, not a monotonic depth or guaranteed strength improvement.
The production profile allows up to 16,384 nodes for the first preliminary
root, ending on its completion. An explicit `go nodes` ceiling also clamps that
reserve, so a calibrated host work budget cannot buy extra first-root work.

The JSON reports both limits and actual work. An interrupted Full search can
retain its complete preliminary result. Exhaustion before any complete root
retains the production allocation-free ranked legal Seed decision. Seed has
completed depth zero and is distinct from a completed searched root. Terminal
positions still return `bestmove 0000`. Cancellation and time limits remain
active; continuous Hint uses its own production policy.

See [the Elo harness](../elo/README.md) for match examples.

## Board CPU policy for matches

The default runner uses the full selected search profile. Enable
`setoption name LitchCpuPolicy value true` to reproduce the board’s CPU move selection, including the opening search ramp. Classic level 1 completes one
static ply with a 256-node ceiling; level 2 completes two static plies with a
4,096-node ceiling, including the opponent's reply. Neither continues into Full
search or quiescence. Every level retains its best completed move; no deliberate
random replacement occurs. Unfinished roots retain the legal Seed. It calls the same production selection helper and captures a
legal Seed before search admission, retaining it after search/start failure.

`position startpos moves ...` establishes complete opening history. The first
three CPU turns use the existing ramp in [vs_cpu_difficulty.hpp](../../include/modules/vs_cpu_difficulty.hpp); level 11 uses
profiles 3, 7, then 11. Levels 1–3 have no ramp. Every committed move by either
side evolves the production seed. Imported `position fen ...` positions use the
full selected profile, including an imported initial FEN. `ucinewgame` clears
history and restores the configured base seed. Setting `Seed` after a position
recomputes its replay seed. Uppercase promotion suffixes and implicit Queen
promotions normalize to the board's canonical UCI text before seed evolution.
The replay also supplies the board's opening-book prefix (through sixteen plies)
and reversible-move clock. Imported history has an unknown clock until a retained
pawn move or capture establishes it; a FEN clock alone is not treated as proof.

CPU JSON records report selected/profile levels, actual search limits, replay
seed, game knowledge, workspace payload, decision provenance, searched move,
`best_score` and `best_score_valid` in Classic centipawns,
selection rejection and the legacy `novice_substitution` key (always false).
`static_leaf_only` identifies the configured novice horizon; completed-root
provenance still distinguishes its actual static work from Seed fallback. `decision_source` is
0=Search, 1=Book, 2=Seed; `root_kind` is 0=Unavailable, 1=Book,
2=ClassicPreliminary, 3=ClassicFull, 4=DuckCompound, 5=Seed. A legal Seed with
depth zero is valid. `go nodes` and `go depth` strictly limit the profile.
The `classic-fast-deadlines-v3` policy, reported as `cpu_policy_id`, gives Classic
levels 1–11 deadlines of 900, 900, 630, 1,400, 2,800, 4,900, 7,000, 10,500,
17,500, 28,000 and 60,000 ms. Levels 3–10 use 70% of the previous deadlines while
retaining their node/depth ceilings. The selected-level deadline applies
throughout the opening ramp; level 11 therefore retains 60,000 ms even while
using profile 3 or 7. These are ceilings: static, forced or completed-depth
searches can finish earlier. An explicit `go movetime` overrides the deadline
for host experiments. Continuous Hint mode runs its own search.

For S3 comparisons, run full-profile searches on the board and use the measured
work as the host node budget. Measure elapsed time, heap and stack use on the
board itself.

The executable regression suite requires the Elo tools' Python dependencies:

```bash
LITCH_UCI_PATH="$PWD/build/uci/litch_uci" \
  python3 -m unittest discover -s tools/uci/tests -v
```

It exercises the actual CLI across levels 1–11, both CPU colors, opening stages,
imported positions, new-game reset, seed option ordering, promotion replay and
unavailable analysis.

## Production continuous hints

For first-result reliability, `hintfirst N` runs the production engine's continuous
search only through its first completed scored Full root, under a diagnostic node
cap `N` (1..16,000,000) and the existing 90-second deadline. It reports first
publication work, provenance, completed roots, and host microseconds separately
from later refinement. Each probe starts fresh, including repeated same-FEN
commands. This command measures search work. The board separately controls when
it first displays a hint, including its 500 ms minimum delay. When the node cap
is reached without a completed, scored root, the command reports a miss.

The seeded legal-play screen uses one engine process and stops at the first
missing, illegal, or unanalysed result. It deduplicates rule positions, records
legal replay histories, excludes actual terminal games, and checks mate and
stalemate controls separately. Its default 5,000 positions use an 8-million-node
per-position cap, a 600-million-node total cap, and a 600-second host limit:

```bash
.venv/bin/python tools/uci/hint_reliability.py --engine build/uci/litch_uci \
  --output /tmp/litch-hint-results.jsonl
```

The sample checks hint availability and the legality of moves. Imported FENs have
no opening-book history. Use games against human or engine opponents to measure
playing strength.

`setoption name ContinuousHint value true` makes `go movetime N` return the
production background hint mailbox at N milliseconds (500..90000), before
cleanup. It bypasses CPU difficulty/node caps. No completed candidate returns
0000; this mode does not fabricate a fallback. Opponent mode remains default.

The host-only `hintbench 1000 3000 10000 30000 90000` command samples all waits
within one uninterrupted search and emits JSON `info string` records.
`sample_ms` includes observation scheduling delay; nodes are the last engine
callback observation. `calculation_ms_observed` freezes when worker completion
is first observed (1ms polling, not hardware CPU time). `search_active=false`
shows natural/safety exhaustion before the deadline. `completed_root_depth`
travels atomically with the published move, so wall-time samples identify the
completed iteration without reading mutable engine state. A missing result has
depth zero. See the Elo harness for measurement limits and paired match methodology.

`hintbenchnodes 1024 4096 16384 65536 262144` starts fresh analysis of the current
FEN and samples one production continuous search at observed node boundaries.
It emits `hint_node_sample`
records with the requested budget, observed nodes, completed root depth and
published move. Callbacks can observe a boundary late; use `nodes_observed`,
not the requested number, for comparisons. All samples share one search, and
the final node budget stops it. Classic search reports ordinary chess plies.
Human Hint accepts scored completed roots under its worker-owned 500 ms
first-reveal rule; it has no depth-five floor and never reveals a Seed.
The runner samples on the search thread. The results measure work in nodes.
