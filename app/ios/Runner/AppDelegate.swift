import Flutter
import UIKit

@main
@objc class AppDelegate: FlutterAppDelegate, FlutterImplicitEngineDelegate {
  private var supportChannel: FlutterMethodChannel?
  private var externalLinksChannel: FlutterMethodChannel?
  private var interfaceOrientationChannel: FlutterMethodChannel?

  private static var orientationController: LitchFlutterViewController? {
    UIApplication.shared.connectedScenes
      .compactMap { $0 as? UIWindowScene }
      .first(where: { $0.activationState == .foregroundActive })?
      .windows.first(where: { $0.isKeyWindow })?.rootViewController as? LitchFlutterViewController
  }

  override func application(
    _ application: UIApplication,
    didFinishLaunchingWithOptions launchOptions: [UIApplication.LaunchOptionsKey: Any]?
  ) -> Bool {
    return super.application(application, didFinishLaunchingWithOptions: launchOptions)
  }

  func didInitializeImplicitFlutterEngine(_ engineBridge: FlutterImplicitEngineBridge) {
    GeneratedPluginRegistrant.register(with: engineBridge.pluginRegistry)
    let channel = FlutterMethodChannel(
      name: "com.foxieproducts.litch/support",
      binaryMessenger: engineBridge.applicationRegistrar.messenger()
    )
    channel.setMethodCallHandler { call, result in
      guard call.method == "openEmail" else {
        result(FlutterMethodNotImplemented)
        return
      }
      guard
        let arguments = call.arguments as? [String: Any],
        let recipient = arguments["recipient"] as? String,
        let subject = arguments["subject"] as? String,
        let body = arguments["body"] as? String,
        recipient == "support@foxieproducts.com",
        subject.count <= 160,
        body.count <= 6000,
        var components = URLComponents(string: "mailto:\(recipient)")
      else {
        result(false)
        return
      }
      components.queryItems = [
        URLQueryItem(name: "subject", value: subject),
        URLQueryItem(name: "body", value: body),
      ]
      guard let url = components.url, UIApplication.shared.canOpenURL(url) else {
        result(false)
        return
      }
      AppDelegate.orientationController?.prepareNativePresentation()
      UIApplication.shared.open(url, options: [:]) { opened in
        result(opened)
      }
    }
    supportChannel = channel

    let linksChannel = FlutterMethodChannel(
      name: "com.foxieproducts.litch/external_links",
      binaryMessenger: engineBridge.applicationRegistrar.messenger()
    )
    linksChannel.setMethodCallHandler { call, result in
      guard call.method == "openUrl" else {
        result(FlutterMethodNotImplemented)
        return
      }
      guard
        let arguments = call.arguments as? [String: Any],
        let urlString = arguments["url"] as? String,
        [
          "https://foxieproducts.com/litch",
          "https://foxieproducts.com/policies/privacy-policy",
          "https://github.com/foxieproducts",
          "https://duckchess.com",
        ].contains(urlString),
        let url = URL(string: urlString),
        UIApplication.shared.canOpenURL(url)
      else {
        result(false)
        return
      }
      AppDelegate.orientationController?.prepareNativePresentation()
      UIApplication.shared.open(url, options: [:]) { opened in
        result(opened)
      }
    }
    externalLinksChannel = linksChannel

    let orientationChannel = FlutterMethodChannel(
      name: "com.foxieproducts.litch/interface_orientation",
      binaryMessenger: engineBridge.applicationRegistrar.messenger()
    )
    orientationChannel.setMethodCallHandler { call, result in
      let controller = AppDelegate.orientationController
      if call.method == "orientationState" {
        controller?.orientationChanged = { [weak orientationChannel] in
          orientationChannel?.invokeMethod("orientationChanged", arguments: nil)
        }
        result(controller?.orientationState() ?? ["side": NSNull(), "compensateHalfTurn": false])
        return
      }
      if call.method == "editing" {
        controller?.setEditing(call.arguments as? Bool ?? false)
        result(nil)
        return
      }
      guard call.method == "landscapeSide" else {
        result(FlutterMethodNotImplemented)
        return
      }
      let orientation = UIApplication.shared.connectedScenes
        .compactMap { $0 as? UIWindowScene }
        .first(where: { $0.activationState == .foregroundActive })?
        .interfaceOrientation
      switch orientation {
      case .landscapeLeft:
        result("left")
      case .landscapeRight:
        result("right")
      default:
        result(nil)
      }
    }
    interfaceOrientationChannel = orientationChannel
  }
}
