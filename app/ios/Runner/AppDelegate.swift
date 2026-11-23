import Flutter
import UIKit

/// Capture legacy evidence before FlutterAppDelegate creates its callback cache.
/// The generation marker is backed up; the observed identity deliberately is not.
enum AppUpgradeIdentityBootstrap {
  static func identityURL(home: URL) -> URL {
    home.appendingPathComponent("Library/Application Support/app_upgrade_identity/last_observed_v1.json")
  }

  static func prepare(
    home: URL = URL(fileURLWithPath: NSHomeDirectory()),
    bundle: Bundle = .main
  ) {
    guard let version = bundle.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String,
          let build = bundle.object(forInfoDictionaryKey: "CFBundleVersion") as? String,
          let buildCode = Int(build), buildCode > 0,
          let package = bundle.bundleIdentifier else { return }
    prepare(home: home, version: version, build: build, package: package)
  }

  static func prepare(home: URL, version: String, build: String, package: String) {
    let manager = FileManager.default
    let identity = identityURL(home: home)
    let generation = home.appendingPathComponent("Library/Application Support/app_upgrade_generation_v1.json")
    // Check all witnesses before creating either marker or invoking Flutter.
    let identityExists = manager.fileExists(atPath: identity.path)
    let modern = manager.fileExists(atPath: generation.path)
    let evidence = !identityExists && !modern ? stockEvidence(home: home) : nil
    let migrate = evidence != nil && version == "1.0.3" && (Int(build) ?? 0) > 51
    if !identityExists {
      let observed: [String: Any] = [
        "schema": 1, "platform": "iOS", "package": package,
        "version": migrate ? "1.0.2" : version,
        "build": migrate ? "51" : build, "installed": NSNull(),
      ]
      if let data = try? JSONSerialization.data(withJSONObject: observed) {
        try? writeIdentity(data, home: home)
      }
    }
    if !modern {
      let marker: [String: Any] = [
        "schema": 1, "generation": 2,
        "version": version, "build": build,
        "legacy_evidence": evidence ?? "none",
      ]
      do {
        try manager.createDirectory(at: generation.deletingLastPathComponent(), withIntermediateDirectories: true)
        try JSONSerialization.data(withJSONObject: marker).write(to: generation, options: .atomic)
        // Restored modern traces must never look like markerless stock data.
        var url = generation
        var values = URLResourceValues()
        values.isExcludedFromBackup = false
        try url.setResourceValues(values)
      } catch {
        // Storage failures cannot hold up normal startup. Modern trace records
        // also carry a generation, so a later launch does not adopt them.
      }
    }
  }

  static func writeIdentity(_ data: Data, home: URL) throws {
    var directory = identityURL(home: home).deletingLastPathComponent()
    let manager = FileManager.default
    try manager.createDirectory(at: directory, withIntermediateDirectories: true)
    var excluded = URLResourceValues()
    excluded.isExcludedFromBackup = true
    try directory.setResourceValues(excluded)
    var url = identityURL(home: home)
    try data.write(to: url, options: .atomic)
    try url.setResourceValues(excluded)
  }

  private static func read(_ url: URL, limit: Int) -> Data? {
    guard let values = try? url.resourceValues(forKeys: [.isRegularFileKey, .isSymbolicLinkKey, .fileSizeKey]),
          values.isRegularFile == true, values.isSymbolicLink != true,
          let size = values.fileSize, size > 0, size <= limit else { return nil }
    return try? Data(contentsOf: url)
  }

  private static func stockEvidence(home: URL) -> String? {
    let trace = home.appendingPathComponent("Library/Application Support/app_trace")
    var stock = false
    let formatter = ISO8601DateFormatter()
    formatter.formatOptions = [.withInternetDateTime, .withFractionalSeconds]
    for slot in 0..<7 {
      guard let data = read(trace.appendingPathComponent("app_trace_\(slot).jsonl"), limit: 64 * 1024),
            let text = String(data: data, encoding: .utf8) else { continue }
      let lines = text.split(separator: "\n")
      guard let first = lines.first,
            let header = try? JSONSerialization.jsonObject(with: Data(first.utf8)) as? [String: Any],
            header["schema"] as? Int == 1, header["type"] as? String == "app_trace_segment",
            header["bucket"] is Int else { continue }
      for line in lines.dropFirst() where line.utf8.count <= 512 {
        guard let record = try? JSONSerialization.jsonObject(with: Data(line.utf8)) as? [String: Any],
              record["schema"] as? Int == 1, record["type"] as? String == "app_trace" else { continue }
        if record["app_generation"] != nil { return nil }
        if record["event"] as? String == "app.started",
           let mode = record["build_mode"] as? String, ["debug", "profile", "release"].contains(mode),
           let timestamp = record["ts"] as? String, formatter.date(from: timestamp) != nil,
           let sequence = record["seq"] as? Int, sequence > 0,
           let monotonic = record["mono_ms"] as? Int, monotonic >= 0 { stock = true }
      }
    }
    if let data = read(trace.appendingPathComponent("console_snapshot.json"), limit: 320 * 1024),
       let record = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
       record["schema"] as? Int == 1 {
      if record["app_generation"] != nil { return nil }
      if let process = record["process_id"] as? Int, process > 0,
         let started = record["process_started_at"] as? String, formatter.date(from: started) != nil,
         let captured = record["captured_at"] as? String, formatter.date(from: captured) != nil,
         let revision = record["revision"] as? Int, revision > 0,
         record["connection_generation"] is Int, record["connected"] is Bool,
         record["lines"] is [String], record["source_line_count"] is Int { stock = true }
    }
    guard stock else { return nil }
    let cache = home.appendingPathComponent("Library/Caches/flutter_callback_cache.json")
    if let values = try? cache.resourceValues(forKeys: [.isRegularFileKey, .isSymbolicLinkKey, .isExcludedFromBackupKey]),
       values.isRegularFile == true, values.isSymbolicLink != true,
       values.isExcludedFromBackup == true { return "stock_trace_and_local_flutter_cache" }
    // A used stock container remains a returning installation after cache
    // eviction or a data restore. Clean redownloads have no valid stock trace.
    return "stock_trace_without_flutter_cache"
  }
}

@main
@objc class AppDelegate: FlutterAppDelegate, FlutterImplicitEngineDelegate {
  private var supportChannel: FlutterMethodChannel?
  private var externalLinksChannel: FlutterMethodChannel?
  private var interfaceOrientationChannel: FlutterMethodChannel?
  private var appUpgradeIdentityChannel: FlutterMethodChannel?

  override init() {
    AppUpgradeIdentityBootstrap.prepare()
    super.init()
  }

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
    let identityChannel = FlutterMethodChannel(
      name: "com.foxieproducts.litch/app_upgrade_identity",
      binaryMessenger: engineBridge.applicationRegistrar.messenger()
    )
    identityChannel.setMethodCallHandler { call, result in
      guard call.method == "read" || call.method == "write" else {
        result(FlutterMethodNotImplemented)
        return
      }
      do {
        let home = URL(fileURLWithPath: NSHomeDirectory())
        let url = AppUpgradeIdentityBootstrap.identityURL(home: home)
        if call.method == "read" {
          guard FileManager.default.fileExists(atPath: url.path) else {
            result(nil)
            return
          }
          let attributes = try FileManager.default.attributesOfItem(atPath: url.path)
          guard let size = attributes[.size] as? NSNumber, size.intValue <= 1024 else {
            result(nil)
            return
          }
          result(String(data: try Data(contentsOf: url), encoding: .utf8))
          return
        }
        guard let value = call.arguments as? String,
              let data = value.data(using: .utf8), data.count <= 1024 else {
          result(false)
          return
        }
        try AppUpgradeIdentityBootstrap.writeIdentity(data, home: home)
        result(true)
      } catch {
        if call.method == "read" {
          result(nil)
        } else {
          result(false)
        }
      }
    }
    appUpgradeIdentityChannel = identityChannel
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
