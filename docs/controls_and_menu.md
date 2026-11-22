# Using the board controls

Each end of a V3.3 or V3.31 board has four direction buttons, X and Circle. Controls at your end follow your player when you swap player ends. Open **About → Litch Manual** in the app for the illustrated guide.

## Play

| What you want to do | Board control |
| --- | --- |
| Show or hide a suggested move | Tap Circle on your turn. Follow the lights from the suggested piece to its destination. |
| Turn Coaching on or off for your side | Hold Circle for one second. With Coaching on, hints appear automatically on your turns. |
| Undo a move | Tap X and return the pieces to the squares the board lights. Tap X again while the guidance is active to cancel. |
| Open Quick Setup | Hold X for one second. Press Left or Right to choose an item, Up or Down to change it, and X to close. To close from Bluetooth without clearing, first select another item. The Bluetooth pairing-clear item closes as soon as clearing succeeds, while its brief success lights finish. The menu also closes after ten seconds of inactivity. |
| Start a new game | Hold X and Circle together at the same end for about two seconds during play. |
| Adjust your player color | Press Up or Down at your end when no menu is open. |

During setup the board can mark the two starting king squares. Place the pieces in their starting positions. An unexpected occupied square gets its own correction cue. During Undo, a pulsing square shows where to put the piece back. If the move captured a piece, return the mover first, then replace the captured piece where indicated.

Quick Setup lets you adjust the computer opponent, player ends, brightness, light-square lighting, board rotation and game clock. The clock pauses while the menu is open. See the [clock presets](spec/timer_semantics.md).

## Promotion

With **Promotion → Queen**, replace a pawn reaching the last rank with a queen. With **Promotion → Choose**, use Left and Right to cycle through Queen, Rook, Bishop and Knight. Press Circle to confirm the piece shown on the board.

## Litchcraft face controls

Either end edits the same artwork. Tap X to undo or Circle to redo. Hold X for one second to erase the displayed frame, then tap X to undo the erase if needed. Hold Circle for one second to switch between Paint and Erase. Up and Down adjust color. Left and Right open or step through the palette. Use the app for animation frames and playback. Hold X and Circle together at one end to leave Litchcraft.

## Sleep

Choose **Connected → Sleep** in the app. Wait for the board to save and go dark. Press any direction button to wake it and release the button as the board restarts. Settings and the saved game reload on wake. See the [control map](hardware/controls.md#sleep--wake-behavior) for other wake inputs by revision.

## Boot recovery

If the board starts with a repeating perimeter pattern and gameplay is unavailable, open **Setup** in the app to see the board health and recovery options. To retry a boot while keeping settings and saved games, hold Circle alone for about 1.5 seconds. If the problem returns, use **Contact Support** and include the pattern you saw.

A recovery reset deletes settings and game data. To choose that reset while already in boot recovery, hold X and Circle together at the same end for five seconds and keep holding through the final two-second confirmation. Release either button before completion to cancel. For firmware installation, see [USB installation and recovery](release/ci_foundation.md#usb-installation-and-recovery).

## Hardware test mode

Newly assembled V3.3 and V3.31 boards enter the built-in inspection at first startup. Holding Up during power-on requests another inspection. The [blank-board programming guide](release/factory_programming.md#functional-test) explains the lights, controls, sensors and wireless check.
