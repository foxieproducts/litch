import Flutter
import UIKit
import XCTest
@testable import Runner

private final class LandscapeSceneController: LitchFlutterViewController {
  var appeared: (() -> Void)?
  override var sceneOrientation: UIInterfaceOrientation? { .landscapeRight }

  override func viewDidAppear(_ animated: Bool) {
    super.viewDidAppear(animated)
    appeared?()
  }
}

@MainActor
final class RunnerTests: XCTestCase {
  private func withLandscapeController(
    _ body: (LandscapeSceneController) throws -> Void
  ) throws {
    guard #available(iOS 26.0, *) else { throw XCTSkip("Requires iOS 26 orientation locking") }
    let scene = try XCTUnwrap(UIApplication.shared.connectedScenes.compactMap {
      $0 as? UIWindowScene
    }.first { $0.activationState == .foregroundActive })
    let previousKeyWindow = scene.windows.first { $0.isKeyWindow }
    let window = UIWindow(windowScene: scene)
    let controller = LandscapeSceneController(project: nil, nibName: nil, bundle: nil)
    let appeared = expectation(description: "Test controller appears in the real scene")
    controller.appeared = { appeared.fulfill() }
    window.rootViewController = controller
    window.makeKeyAndVisible()
    wait(for: [appeared], timeout: 5)
    controller.appeared = nil
    defer {
      window.isHidden = true
      window.rootViewController = nil
      previousKeyWindow?.makeKey()
    }
    XCTAssertTrue(controller.view.window?.windowScene === scene)
    XCTAssertEqual(scene.activationState, .foregroundActive)
    try body(controller)
  }

  func testAmbiguousPoseReleasesLandscapeLockAndKeepsSceneSide() throws {
    try withLandscapeController { controller in
      guard #available(iOS 26.0, *) else { return }
      let phone = UIDevice.current.userInterfaceIdiom == .phone
      controller.handleDeviceOrientation(.unknown)
      XCTAssertFalse(controller.prefersInterfaceOrientationLocked,
        "No known physical pose must not acquire a landscape lock")
      for pose: UIDeviceOrientation in [.faceUp, .faceDown, .unknown] {
        controller.handleDeviceOrientation(.landscapeLeft)
        XCTAssertEqual(controller.prefersInterfaceOrientationLocked, phone)
        var notifications = 0
        controller.orientationChanged = { notifications += 1 }
        controller.handleDeviceOrientation(pose)
        XCTAssertFalse(controller.prefersInterfaceOrientationLocked,
          "Ambiguous pose must release the previous landscape preference: \(pose)")
        XCTAssertEqual(notifications, 1)
        XCTAssertEqual(controller.orientationState()["side"] as? String, "right")
        XCTAssertEqual(controller.orientationState()["compensateHalfTurn"] as? Bool, false)
        controller.orientationChanged = nil
      }
    }
  }

  func testPortraitReleasesLockAndLandscapeCanRecover() throws {
    try withLandscapeController { controller in
      guard #available(iOS 26.0, *) else { return }
      let phone = UIDevice.current.userInterfaceIdiom == .phone
      controller.handleDeviceOrientation(.landscapeLeft)
      XCTAssertEqual(controller.prefersInterfaceOrientationLocked, phone)
      if !phone {
        controller.handleDeviceOrientation(.landscapeRight)
        XCTAssertFalse(controller.prefersInterfaceOrientationLocked)
        XCTAssertEqual(controller.orientationState()["side"] as? String, "right")
        XCTAssertEqual(controller.orientationState()["compensateHalfTurn"] as? Bool, false)
      }
      controller.handleDeviceOrientation(.portrait)
      XCTAssertFalse(controller.prefersInterfaceOrientationLocked)
      controller.handleDeviceOrientation(.landscapeLeft)
      XCTAssertEqual(controller.prefersInterfaceOrientationLocked, phone)
      controller.setEditing(true)
      XCTAssertFalse(controller.prefersInterfaceOrientationLocked)
      controller.setEditing(false)
      XCTAssertEqual(controller.prefersInterfaceOrientationLocked, phone)
      controller.handleDeviceOrientation(.portraitUpsideDown)
      XCTAssertFalse(controller.prefersInterfaceOrientationLocked)
      controller.handleDeviceOrientation(.landscapeLeft)
      XCTAssertEqual(controller.prefersInterfaceOrientationLocked, phone)
      controller.prepareNativePresentation()
      XCTAssertFalse(controller.prefersInterfaceOrientationLocked)
      controller.resumeNativePresentation()
      XCTAssertEqual(controller.prefersInterfaceOrientationLocked, phone)
    }
  }
}
