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

For 1.0, we played 20,000 randomized Classic games to the board’s end conditions and 10,000 Duck games. A separate 20,000-game Classic run exercised Undo, canceled moves, extra pieces and missing pieces. We independently checked all **6,006,535** displayed hints for legality, including the duck placement when required. A separate comparison checked the engine’s legal moves in **4,742,735** Classic positions.

We measured playing strength by playing each opening from both sides against Stockfish 17.1. The 1.0 configuration played 650 games across eleven levels, including 150 at level 11. Its first 550 games paired 25 openings with both colors at each level. It scored 81 wins, 13 draws and 56 losses at level 11, for an estimated **1,928 Elo** (95% interval **1,753–2,153**). [Match scores and test conditions](tools/elo/classic_strength_1_0.json) · [Ratings and measured board speed](https://github.com/foxieproducts/litch-max/blob/9594558de4a95065d455e996471affb188b08a99/README.md#classic-playing-strength)

## Building the included engine

The firmware uses [Litch-Max](https://github.com/foxieproducts/litch-max/tree/9594558de4a95065d455e996471affb188b08a99), checked out at `lib/litch-max` as a Git submodule. The Litch commit pins the engine revision. Follow the [recursive clone instructions](README.md#building-or-repairing-litch), or run `git submodule update --init --recursive` in an existing Git checkout before building. Its own README assumes a standalone checkout. Keep build output outside `lib/litch-max` so generated files do not enter the engine source tree. From the repository root, with CMake 3.20 or newer and C11/C++17 compilers installed:

```sh
cmake -S lib/litch-max -B build/litch-max -DCMAKE_BUILD_TYPE=Release
cmake --build build/litch-max --parallel 2
ctest --test-dir build/litch-max --output-on-failure
```

These are host engine tests. For a library-only build, configure with `-DBUILD_TESTING=OFF`. The separately maintained [Litch-Max repository](https://github.com/foxieproducts/litch-max) also contains examples and engine documentation.
