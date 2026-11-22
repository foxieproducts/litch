import Flutter
import UIKit

class SceneDelegate: FlutterSceneDelegate {
  override func sceneDidBecomeActive(_ scene: UIScene) {
    super.sceneDidBecomeActive(scene)
    (window?.rootViewController as? LitchFlutterViewController)?.resumeNativePresentation()
  }
}

/// Full-screen iPhones may lock for landscape half-turns. iPad windows follow
/// their UIKit scene orientation, including while resizing or editing.
@objc(LitchFlutterViewController)
class LitchFlutterViewController: FlutterViewController {
  var orientationChanged: (() -> Void)?
  private var physicalOrientation: UIInterfaceOrientation?
  private var textEditing = false
  private var keyboardVisible = false
  private var visible = false
  private var nativePresentation = false
  private var compensatingLock = false
  private var lastLockPreference: Bool?

  private var usesPhoneOrientationLock: Bool { UIDevice.current.userInterfaceIdiom == .phone }

  var sceneOrientation: UIInterfaceOrientation? {
    guard let scene = viewIfLoaded?.window?.windowScene else { return nil }
    if #available(iOS 16.0, *) { return scene.effectiveGeometry.interfaceOrientation }
    return scene.interfaceOrientation
  }

  @available(iOS 26.0, *)
  override var prefersInterfaceOrientationLocked: Bool {
    usesPhoneOrientationLock && visible && !textEditing && !keyboardVisible && !nativePresentation &&
      presentedViewController == nil &&
      viewIfLoaded?.window?.windowScene?.activationState == .foregroundActive &&
      physicalOrientation?.isLandscape == true &&
      sceneOrientation?.isLandscape == true
  }

  override func viewDidLoad() {
    super.viewDidLoad()
    UIDevice.current.beginGeneratingDeviceOrientationNotifications()
    let center = NotificationCenter.default
    center.addObserver(self, selector: #selector(deviceMoved),
      name: UIDevice.orientationDidChangeNotification, object: nil)
    center.addObserver(self, selector: #selector(keyboardWillShow),
      name: UIResponder.keyboardWillShowNotification, object: nil)
    center.addObserver(self, selector: #selector(keyboardWillHide),
      name: UIResponder.keyboardWillHideNotification, object: nil)
  }

  override func viewDidAppear(_ animated: Bool) {
    super.viewDidAppear(animated)
    visible = true
    nativePresentation = false
    deviceMoved()
  }

  override func viewWillDisappear(_ animated: Bool) {
    visible = false
    updateLockPreference()
    super.viewWillDisappear(animated)
  }

  override func viewWillTransition(
    to size: CGSize, with coordinator: UIViewControllerTransitionCoordinator
  ) {
    super.viewWillTransition(to: size, with: coordinator)
    coordinator.animate(alongsideTransition: nil) { [weak self] _ in
      self?.geometryChanged()
    }
  }

  override func present(
    _ viewControllerToPresent: UIViewController,
    animated flag: Bool, completion: (() -> Void)? = nil
  ) {
    // Release the preference before UIKit owns a share sheet, picker or dialog.
    // Do not disable UIView animations or force a private orientation value.
    prepareNativePresentation()
    super.present(viewControllerToPresent, animated: flag) { [weak self] in
      self?.nativePresentation = false
      self?.updateLockPreference()
      completion?()
    }
  }

  func prepareNativePresentation() {
    nativePresentation = true
    updateLockPreference()
  }

  func resumeNativePresentation() {
    nativePresentation = false
    updateLockPreference()
    orientationChanged?()
  }

  func setEditing(_ value: Bool) {
    textEditing = value
    updateLockPreference()
    guard usesPhoneOrientationLock, value, let physicalOrientation, physicalOrientation.isLandscape,
      physicalOrientation != sceneOrientation,
      let scene = viewIfLoaded?.window?.windowScene
    else { return }
    if #available(iOS 16.0, *) {
      let mask: UIInterfaceOrientationMask = physicalOrientation == .landscapeLeft
        ? .landscapeLeft : .landscapeRight
      scene.requestGeometryUpdate(.iOS(interfaceOrientations: mask))
    } else {
      UIViewController.attemptRotationToDeviceOrientation()
    }
  }

  @objc private func keyboardWillShow() {
    keyboardVisible = true
    updateLockPreference()
  }

  @objc private func keyboardWillHide() {
    keyboardVisible = false
    updateLockPreference()
  }

  @objc private func deviceMoved() {
    handleDeviceOrientation(UIDevice.current.orientation)
  }

  func handleDeviceOrientation(_ orientation: UIDeviceOrientation) {
    switch orientation {
    // Device and interface landscape names are intentionally opposite.
    case .landscapeLeft: physicalOrientation = .landscapeRight
    case .landscapeRight: physicalOrientation = .landscapeLeft
    case .portrait: physicalOrientation = .portrait
    case .portraitUpsideDown: physicalOrientation = .portraitUpsideDown
    default: physicalOrientation = nil
    }
    updateLockPreference()
    orientationChanged?()
  }

  private func updateLockPreference() {
    if #available(iOS 26.0, *) {
      let preference = prefersInterfaceOrientationLocked
      guard preference != lastLockPreference else { return }
      lastLockPreference = preference
      setNeedsUpdateOfPrefersInterfaceOrientationLocked()
    }
  }

  func geometryChanged() {
    updateLockPreference()
    orientationChanged?()
  }

  func orientationState() -> [String: Any] {
    let interface = sceneOrientation
    if #available(iOS 26.0, *), usesPhoneOrientationLock,
      viewIfLoaded?.window?.windowScene?.effectiveGeometry.isInterfaceOrientationLocked == true {
      compensatingLock = true
    }
    if physicalOrientation == interface || interface?.isLandscape != true {
      compensatingLock = false
    }
    // Keep compensation until the actual interface catches up on lock release.
    // An OS version or requested lock alone never authorizes a Flutter flip.
    let compensate = compensatingLock && physicalOrientation?.isLandscape == true &&
      physicalOrientation != interface
    let resolved = compensate ? physicalOrientation : interface
    let side: Any
    switch resolved {
    case .landscapeLeft: side = "left"
    case .landscapeRight: side = "right"
    default: side = NSNull()
    }
    return ["side": side, "compensateHalfTurn": compensate]
  }

  deinit {
    NotificationCenter.default.removeObserver(self)
    UIDevice.current.endGeneratingDeviceOrientationNotifications()
  }

}
