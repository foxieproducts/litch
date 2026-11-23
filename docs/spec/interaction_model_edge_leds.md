# Board controls and edge LEDs

Each player has a direction pad and two face buttons. The firmware maps the physical ends of the board to the players after placement or a board rotation. For inputs, menu navigation and what each light means during play, see the [board controls guide](../controls_and_menu.md). For pin assignments and the V3.31 perimeter and button LEDs, see the [controls map](../hardware/controls.md) and [LED topology](../hardware/led_topology.md).

Firmware menu options and wraparound selection are defined in the [menu controller](../../include/modules/menu_controller.hpp).

## Move and capture guidance

On a V3.3 board, all four LEDs carry a move across an empty square. A left-to-right sweep starts on the left square LED, passes through both inward-facing piece LEDs together, and exits on the right square LED. A top-to-bottom sweep starts on the top piece LED, passes through both outward-facing square LEDs together, and exits on the bottom piece LED. A diagonal sweep starts on the two incoming LEDs together and ends on the two outgoing LEDs together. The groups fade into one another. Opposite moves reverse their order. Rotating the board turns both the path and each square's LED sequence by 180 degrees.

A knight follows its long leg first and turns at the elbow. The nearby cells determine where the light enters and leaves each square, including along an oblique Duck move. When a piece occupies a path square, its inward LEDs keep their piece or pickup indication. The outward LEDs carry the passing light. An empty legal square regains its ordinary indication when the sweep moves on. Minimal Animations uses steady guidance.

In Duck chess, a manual Hint or Coaching shows the piece move first. The occupied Duck keeps its ordinary yellow piece lights while the player moves the chess piece. When the board accepts the chess move and asks for the Duck, its yellow route starts at the Duck and shows the suggested landing. This order also applies to captures and to steady guidance in Minimal Animations.

For a shown capture, both outer lights on the opposing piece's square pulse amber together twice. Each pulse lasts 120 ms, with a 120 ms gap and an 840 ms pause after the second pulse. The rails stay dim amber between pulses. Each bright pulse requests 1.5 times the ordinary emitter brightness, subject to available current and the output ceiling. The lights under the piece stay dark. Minimal Animations uses steady orange at ordinary brightness. The removal cue ends as soon as the captured piece is lifted. Once the capturing piece lands, its square shows the mover's settling cue. In en passant the captured pawn and the landing occupy different squares, and each has its own cue.

If a player lifts the opposing piece first, each legal capturer takes a turn showing its path. The inward LEDs on the vacated square stay dark. An en passant path ends on the pawn's actual landing square. Returning the opposing piece cancels the paths. Lifting a legal capturer selects its landing. With Minimal Animations the available capturers stay marked until the player chooses one.
