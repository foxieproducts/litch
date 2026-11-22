# Chessboard artwork

The app maps FEN tokens to the versioned Litch cameo assets:

- `p` → [cameos_v3/pawn.png](cameos_v3/pawn.png)
- `r` → [cameos_v3/rook.png](cameos_v3/rook.png)
- `n` → [cameos_v3/knight.png](cameos_v3/knight.png)
- `b` → [cameos_v3/bishop.png](cameos_v3/bishop.png)
- `q` → [cameos_v3/queen.png](cameos_v3/queen.png)
- `k` → [cameos_v3/king.png](cameos_v3/king.png)
- Duck square → [cameos_v2/duck.png](cameos_v2/duck.png)

Uppercase and lowercase FEN tokens share one asset. The renderer preserves the
source ivory-on-charcoal treatment for White and inverts only those neutral
tones for Black. Pure-green key facets in the source are replaced at runtime
with the owning player's Litch color. The app mirrors the firmware's 0..255
RGB wheel and shares the sRGB display conversion across the cameo tint and
player-color previews. The conversion preserves the source shading and the
neutral fallback.
Logical P1 owns White and P2 owns Black. Player-end placement and board
rotation move their presentation without changing that ownership; the deployed
`white_owner` compatibility field does not assign a different logical player.
Settings notifications update the accents while the app is open. The amber duck remains neutral.

## Asset format and licensing

The app bundles the seven transparent PNGs listed above. Their longest edge is
512 pixels. The board renderer requests a 256-pixel decode width. The amber
duck also serves as the Duck Chess status icon.

Artwork is outside the root software license, and the documentation image grant
covers only the images it lists. [Software license](../../../LICENSE) ·
[Documentation and image license](../../../LICENSE-DOCUMENTATION)
