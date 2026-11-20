# Contributing

You can build and modify Litch under the [software](LICENSE), [hardware](LICENSE-HARDWARE) and [documentation](LICENSE-DOCUMENTATION) licenses. Foxie is not currently accepting external pull requests.

For help with a board or the companion app, see [Support](SUPPORT.md). Use the private reporting instructions for [security issues](SECURITY.md) or [conduct concerns](CODE_OF_CONDUCT.md).

## Firmware and app checks

Start with the [firmware setup](README.md#building-or-repairing-litch). With that environment active, run the opening pawn journey on the host.

```sh
./litch test native --filter test_empty_piece_led_virtual_litch_journey
```

[Follow a pawn from e2 to Play](docs/e2e4.md) explains the journey’s destination lights, move history and side to move, and shows how to continue into the app. Select another `test/test_*` suite when working on a different area. The [app guide](app/README.md#building-from-source) covers Flutter and the mobile checks.

### 1.0 test coverage

The Flutter suite exercises Android and iOS app behavior, including interrupted Bluetooth writes, reconnects, firmware updates and saved games. Its board-to-app tests run the firmware on a virtual board that accepts Hall sensor and button input. Board tests cover castling, en passant, promotions and the four Classic draw conditions. We tested king-removal resignation with all five end-of-game animations and with Minimal Animations enabled. [App and firmware test setup](tools/qa/README.md)

On the 1.0 baseline, we played 20,000 randomized Classic games to the board’s end conditions and 10,000 Duck games. A separate 20,000-game Classic run exercised Undo, canceled moves, extra pieces and missing pieces. We independently checked all **6,006,535** displayed hints for legality, including the duck placement when required. A separate comparison checked the engine’s legal moves in **4,742,735** Classic positions.

We measured playing strength by playing each opening from both sides against Stockfish 17.1. The 1.0 baseline played 550 games across eleven levels. The optimized 1.0 configuration played 650 games, including 150 at level 11. It scored 81 wins, 13 draws and 56 losses at level 11, for an estimated **1,928 Elo** (95% interval **1,753–2,153**). [Match scores and test conditions](tools/elo/classic_strength_1_0.json) · [Ratings and measured board speed](https://github.com/foxieproducts/litch-max/blob/35a68cf77b1362df701472c1191e80ef0cf58e00/README.md#classic-playing-strength)

## Building the included engine

The firmware uses the [included Litch-Max source](lib/litch-max/) at [revision 675933e](https://github.com/foxieproducts/litch-max/tree/675933ed740663fdeb0ead3e1fcf3a68ba21900b). The [snapshot manifest](lib/litch-max.snapshot.json) records its Git tree and file hashes. Its own README assumes a standalone checkout. In this repository, keep build output outside `lib/litch-max`: the firmware build verifies the snapshot's complete file set and rejects added files there. From the repository root, with CMake 3.20 or newer and C11/C++17 compilers installed:

```sh
cmake -S lib/litch-max -B build/litch-max -DCMAKE_BUILD_TYPE=Release
cmake --build build/litch-max --parallel 2
ctest --test-dir build/litch-max --output-on-failure
```

These are host engine tests. For a library-only build, configure with `-DBUILD_TESTING=OFF`. The separately maintained [Litch-Max repository](https://github.com/foxieproducts/litch-max) also contains examples and engine documentation.
