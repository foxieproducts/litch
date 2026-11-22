# Foxie Litch for iOS and Android

The companion app connects to a physical Litch over Bluetooth. In **Play**, follow a live board game or open a saved game to replay its moves. **Litchcraft** lets you paint squares and create animations. You can edit artwork offline and send it when your board is connected. Use **Setup** to select a board, change settings or install firmware.

In portrait, the Litchcraft tools span the workspace between the same side
margins as the canvas. This also gives the tools the full available width on
shorter screens where the square canvas has to shrink. In landscape the tools
stay in the dock beside the canvas.

## Help and privacy

Open **About → Litch Manual** for searchable, offline help with setup, the board controls, computer play, Duck chess, lighting and connection recovery. **Contact Support** opens an editable email draft. **About → Licenses** lists third-party software. The [app privacy policy](assets/privacy/foxie_litch_privacy_policy.md) is bundled in the app and links to the [Foxie Products privacy policy](https://foxieproducts.com/policies/privacy-policy).

## Building from source

The app uses Flutter 3.41.2. Install Xcode for iOS or the Android development tools for Android. From the repository root, run:

```sh
./litch doctor --scope flutter
./litch bootstrap flutter
./litch app build ios
./litch app build android
```

To run on a connected device, use `./litch app run ios` or `./litch app run android`. If several devices are connected, use `./litch app run --device <flutter-device-id>`. Use `./litch app analyze` for static analysis and `./litch app test -- test/owner_pairing_session_test.dart` to run a selected test.

The [technical reference](../docs/spec/README.md) describes the board connection, firmware updates and settings. The app gets official firmware from the [release and beta catalogs](../docs/release/ci_foundation.md).

## Publishing the companion app

Build both store packages from the same clean release checkout of [foxieproducts/litch](https://github.com/foxieproducts/litch) on `main` or its approved release tag. From the repository root, confirm the source identity and install the pinned Flutter dependencies.

```sh
git status --short
git rev-parse HEAD
python3 tools/release/release_identity.py check
cd app
flutter pub get --enforce-lockfile
```

The native version and build number come from [pubspec.yaml](pubspec.yaml). The app also records them in [AppReleaseIdentity](lib/src/models/app_release_identity.dart). To release a build you have already uploaded, select it in the store. For a changed binary, check the highest uploaded number in **both** stores, choose an unused higher number and commit the matching source values before packaging. Keep the source version and build number throughout export.

### Google Play

The release build has been verified with JDK 21 and Android SDK 36. Use `flutter doctor -v` to check the JDK Flutter selected. Use the existing Foxie Play upload key for `com.foxieproducts.litch_mobile`. On the signing machine, supply an ignored `android/key.properties` with `storeFile`, `storePassword`, `keyAlias` and `keyPassword`. The keystore path can be absolute or relative to `app/android/`. Keep the keystore outside the checkout and restrict access to the properties file.

From `app/`, explicitly request the store signing configuration. A plain local release build uses the QA key. For the manifest check, set `BUNDLETOOL_JAR` to your [bundletool JAR](https://developer.android.com/tools/bundletool).

```sh
ORG_GRADLE_PROJECT_storeSigning=true flutter build appbundle --release --no-pub
aab=build/app/outputs/bundle/release/app-release.aab
jarsigner -verify "$aab"
keytool -printcert -jarfile "$aab"
java -jar "$BUNDLETOOL_JAR" dump manifest --bundle="$aab" --module=base
```

Match the SHA-256 fingerprint to **Upload key certificate** in Play Console's **App integrity** page. Check the package, version name and version code in the bundle manifest. For an authorized delivery, open the existing Foxie Litch app in Play Console, select the intended track, create a draft release and upload this AAB. Confirm the parsed version and review the device coverage before sending changes for review or starting a rollout. Google Play signs the APKs it delivers to users. [Google's release guide](https://support.google.com/googleplay/android-developer/answer/9859348?hl=en)

### App Store Connect

Build the next store archive with the iOS 27 SDK or later. Runner and CocoaPods target iOS 15.0. Apple requires SDK 27 and a deployment target of at least 15.0 for uploads starting in April 2027. Xcode 27.2 includes SDK 27.2 and requires macOS Tahoe 26.6 or later. [Apple submission requirements](https://developer.apple.com/app-store/submitting/) · [Xcode 27.2 release notes](https://developer.apple.com/documentation/xcode-release-notes/xcode-27_2-release-notes)

After upgrading macOS and installing the release of Xcode, confirm the selected tools before preparing the archive.

```sh
sw_vers -productVersion
xcode-select -p
xcodebuild -version
xcrun --sdk iphoneos --show-sdk-version
```

With CocoaPods and access to the **Foxie Products, LLC** Apple Developer team, configure Flutter and open its workspace from `app/`. Runner checks the selected device SDK and deployment target during Archive.

```sh
flutter build ios --release --config-only --no-codesign --no-pub
open ios/Runner.xcworkspace
```

In Xcode, choose the **Runner** scheme and a generic iOS device, then **Product → Archive**. In Organizer check that the archive shows `com.foxieproducts.litchMobile`, the Foxie team, and the source version and build number. Before distributing, inspect the fresh archive. Set `ARCHIVE_PATH` to its path. Confirm `DTSDKName` reports `iphoneos27` or later, `MinimumOSVersion` is `15.0`, and the launch storyboard named in the app’s plist exists in its bundle. [Apple launch screen validation](https://developer.apple.com/documentation/technotes/tn3208-preparing-your-apps-launch-screen-to-meet-app-store-requirements)

```sh
runner="$ARCHIVE_PATH/Products/Applications/Runner.app"
plutil -extract DTSDKName raw "$runner/Info.plist"
plutil -extract MinimumOSVersion raw "$runner/Info.plist"
launch=$(plutil -extract UILaunchStoryboardName raw "$runner/Info.plist")
find "$runner" -type d -name "$launch.storyboardc" -print
xcrun vtool -show-build "$runner/Runner"
```

Use Organizer’s **Validate App** on the intended archive. For an authorized upload, use **Distribute App → Custom → App Store Connect → Upload**. Turn off **Manage version and build number** and leave **TestFlight internal testing only** off so the build remains eligible for the App Store. Use the team's authorized signing method. Organizer supports Apple's cloud-managed distribution certificate. Automatic signing can create or update provisioning profiles, App IDs and certificates.

For App Store review starting in April 2027, include iPhone Duo screenshots when the archive uses SDK 27.1 or later. [Apple screenshot specifications](https://developer.apple.com/help/app-store-connect/reference/app-information/screenshot-specifications/)

For an inspection-only IPA, choose **Export** in the same Organizer flow. A Mac with the matching local distribution signing identity and profile can also export an existing archive from `app/` with the checked-in [export settings](ios/AppStoreExportOptions.plist). Set `ARCHIVE_PATH` to the signed `.xcarchive`. If command-line export reports a missing distribution certificate and the team uses cloud signing, use Organizer with the authorized Apple account.

```sh
xcodebuild -exportArchive -archivePath "$ARCHIVE_PATH" \
  -exportOptionsPlist ios/AppStoreExportOptions.plist -exportPath build/ios/store-export
```

Before delivery, verify the distribution signature and embedded profile, and check that every embedded executable targets physical iOS. An arm64 simulator binary also reports arm64, so inspect its platform with `xcrun vtool -show-build`. After Apple processes the upload, select that build on the intended App Store version. Submitting the version for review and releasing it are separate store actions. [Apple's distribution guide](https://developer.apple.com/documentation/xcode/distributing-your-app-for-beta-testing-and-releases)
