# Running the board artwork diagnostic

This development app sends five 256-frame artworks to a physical board. The default transfer overwrites resident artwork. Use a spare board and a mobile development device. To check identity without selecting or transferring artwork, launch [the diagnostic](frame_store_core_acceptance.dart) from the app directory with these Flutter arguments.

```sh
flutter run -t tool/frame_store_core_acceptance.dart \
  --dart-define=CORE_TARGET=<board-bluetooth-identifier> \
  --dart-define=CORE_FIRMWARE_SHA256=<board-firmware-sha256> \
  --dart-define=CORE_IDENTITY_ONLY=true
```

Use the Android Bluetooth address in uppercase or the iOS BLE UUID. For a transfer on a disposable board, omit `CORE_IDENTITY_ONLY`. The tool uploads, reconnects and reads back each test artwork. Default runs leave the test artwork on the board.

For preservation mode, save any app draft and close the normal app first. Keep the same app ID, signer and app data. Set `CORE_BACKUP_ARTWORK=true`. This mode requires two populated residents, paused at frame zero with no elapsed phase or Undo/Redo history. It copies the original artworks and persisted mobile files before testing, then attempts to restore and verify both residents. If interrupted, keep the app data and rerun with the same board and image, `CORE_BACKUP_ARTWORK=true` and `CORE_RESUME=<saved-directory-or-run-id>`. Mobile backups remain under `<application support>/core-acceptance-backups/`. Saved mobile files can be retrieved separately. The tool never automatically restores them over the phone's library.
