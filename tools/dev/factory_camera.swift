import AppKit
import AVFoundation
import CoreImage
import Darwin
import Foundation
import Vision

private struct Label {
    let payload: String
    let symbology: String
}

private struct CameraFrame: Encodable {
    let state: String
    let payload: String?
    let symbology: String?

    init(state: String, label: Label? = nil) {
        self.state = state
        payload = label?.payload
        symbology = label?.symbology
    }

    var displayText: String {
        switch state {
        case "label": return "Read \(payload ?? "")"
        case "ambiguous": return "Show one label at a time."
        default: return "Hold one board label in the preview."
        }
    }
}

private struct FrameSize {
    let width: Int
    let height: Int

    var bytesPerRow: Int { width * 4 }
    var bytesPerFrame: Int { bytesPerRow * height }

    func readImages(from stream: FileHandle, process: (CGImage) throws -> Void) throws {
        while true {
            // The Android stream occupies a single dispatch work item for the
            // whole batch. Drain Foundation and Vision temporaries each frame.
            let reachedEnd = try autoreleasepool {
                guard let image = try readImage(from: stream) else { return true }
                try process(image)
                return false
            }
            if reachedEnd { return }
        }
    }

    private func readImage(from stream: FileHandle) throws -> CGImage? {
        // Read pipe fragments directly into the image's backing storage.
        guard let data = CFDataCreateMutable(nil, bytesPerFrame) else { throw FrameError.incompleteImage }
        CFDataSetLength(data, bytesPerFrame)
        guard let bytes = CFDataGetMutableBytePtr(data) else { throw FrameError.incompleteImage }
        var filled = 0
        while filled < bytesPerFrame {
            let count = Darwin.read(stream.fileDescriptor, bytes.advanced(by: filled), bytesPerFrame - filled)
            if count == 0 {
                if filled == 0 { return nil }
                throw FrameError.incompleteImage
            }
            if count < 0 {
                if errno == EINTR { continue }
                throw POSIXError(POSIXErrorCode(rawValue: errno) ?? .EIO)
            }
            filled += count
        }
        guard let provider = CGDataProvider(data: data),
              let image = CGImage(width: width, height: height, bitsPerComponent: 8, bitsPerPixel: 32,
                                  bytesPerRow: bytesPerRow, space: CGColorSpaceCreateDeviceRGB(),
                                  bitmapInfo: [.byteOrder32Little, CGBitmapInfo(rawValue: CGImageAlphaInfo.noneSkipFirst.rawValue)],
                                  provider: provider, decode: nil, shouldInterpolate: true,
                                  intent: .defaultIntent) else {
            throw FrameError.incompleteImage
        }
        return image
    }
}

private enum FrameError: LocalizedError {
    case incompleteImage

    var errorDescription: String? { "The selected camera sent an incomplete frame." }
}

private func nativeCameras() -> [AVCaptureDevice] {
    AVCaptureDevice.DiscoverySession(
        deviceTypes: [.builtInWideAngleCamera, .continuityCamera, .external],
        mediaType: .video, position: .unspecified
    ).devices.filter(\.isConnected)
}

private func listNativeCameras() throws {
    let choices = nativeCameras().map { camera in
        let kind: String
        switch camera.deviceType {
        case .continuityCamera: kind = "continuity"
        case .external: kind = "external"
        default: kind = "built-in"
        }
        return ["id": camera.uniqueID, "name": camera.localizedName, "type": kind]
    }
    try FileHandle.standardOutput.write(contentsOf: JSONEncoder().encode(choices) + Data([0x0A]))
}

private enum CameraSource {
    case stream(FrameSize, String)
    case native(AVCaptureDevice)

    var name: String {
        switch self {
        case .stream(_, let name): return name
        case .native(let camera): return camera.localizedName
        }
    }
}

private func failCamera(_ message: String) -> Never {
    FileHandle.standardError.write(Data("litch: camera preview: \(message)\n".utf8))
    exit(1)
}

private final class Preview: NSObject, NSApplicationDelegate, AVCaptureVideoDataOutputSampleBufferDelegate {
    private let source: CameraSource
    private let imageView = NSView()
    private let status = NSTextField(wrappingLabelWithString: "Waiting for the selected camera…")
    private let frames = DispatchQueue(label: "Litch factory camera frames", qos: .userInitiated)
    private let encoder = JSONEncoder()
    private let request = VNDetectBarcodesRequest()
    private lazy var imageContext = CIContext()
    private var session: AVCaptureSession?
    private var cameraObservers: [NSObjectProtocol] = []
    private var lastNativeFrameTime: TimeInterval = 0
    private var window: NSWindow?

    init(source: CameraSource) {
        self.source = source
        super.init()
        request.revision = VNDetectBarcodesRequestRevision4
        request.symbologies = [.qr, .dataMatrix]
    }

    func applicationDidFinishLaunching(_ notification: Notification) {
        showWindow()
        switch source {
        case .stream(let size, _):
            frames.async { [self] in
                do {
                    try size.readImages(from: .standardInput, process: scan)
                } catch {
                    failCamera(error.localizedDescription)
                }
                DispatchQueue.main.async { NSApplication.shared.terminate(nil) }
            }
        case .native(let camera):
            openNative(camera)
        }
    }

    private func openNative(_ camera: AVCaptureDevice) {
        let allowAccess = "Allow the factory preview in System Settings → Privacy & Security → Camera."
        switch AVCaptureDevice.authorizationStatus(for: .video) {
        case .authorized:
            startNative(camera)
        case .notDetermined:
            AVCaptureDevice.requestAccess(for: .video) { [self] granted in
                guard granted else { failCamera(allowAccess) }
                startNative(camera)
            }
        default:
            failCamera(allowAccess)
        }
    }

    private func startNative(_ camera: AVCaptureDevice) {
        frames.async { [self] in
            do {
                let capture = try nativeSession(for: camera)
                session = capture
                let center = NotificationCenter.default
                cameraObservers.append(center.addObserver(forName: AVCaptureDevice.wasDisconnectedNotification,
                                                          object: camera, queue: nil) { _ in
                    failCamera("The selected camera disconnected.")
                })
                cameraObservers.append(center.addObserver(forName: AVCaptureSession.runtimeErrorNotification,
                                                          object: capture, queue: nil) { notification in
                    let error = notification.userInfo?[AVCaptureSessionErrorKey] as? NSError
                    failCamera(error?.localizedDescription ?? "The selected camera stopped.")
                })
                capture.startRunning()
                guard capture.isRunning else { failCamera("Could not start the selected camera.") }
            } catch {
                failCamera(error.localizedDescription)
            }
        }
    }

    private func nativeSession(for camera: AVCaptureDevice) throws -> AVCaptureSession {
        let capture = AVCaptureSession()
        let input = try AVCaptureDeviceInput(device: camera)
        let output = AVCaptureVideoDataOutput()
        output.alwaysDiscardsLateVideoFrames = true
        output.setSampleBufferDelegate(self, queue: frames)
        capture.beginConfiguration()
        defer { capture.commitConfiguration() }
        guard capture.canAddInput(input), capture.canAddOutput(output) else {
            failCamera("Could not configure the selected camera.")
        }
        capture.addInput(input)
        capture.addOutput(output)
        let resolutions: [AVCaptureSession.Preset] = [.hd1920x1080, .hd1280x720, .vga640x480, .medium]
        guard let resolution = resolutions.first(where: capture.canSetSessionPreset) else {
            failCamera("The selected camera has no supported preview resolution.")
        }
        capture.sessionPreset = resolution
        return capture
    }

    func captureOutput(_ output: AVCaptureOutput, didOutput sample: CMSampleBuffer, from connection: AVCaptureConnection) {
        let now = ProcessInfo.processInfo.systemUptime
        guard now - lastNativeFrameTime >= 1.0 / 6, let buffer = CMSampleBufferGetImageBuffer(sample) else { return }
        lastNativeFrameTime = now
        do {
            try autoreleasepool {
                let image = CIImage(cvPixelBuffer: buffer)
                let center = image.extent.insetBy(dx: image.extent.width / 4, dy: image.extent.height / 4).integral
                guard let zoomed = imageContext.createCGImage(image, from: center) else {
                    failCamera("Could not read a frame from the selected camera.")
                }
                try scan(zoomed)
            }
        } catch {
            failCamera(error.localizedDescription)
        }
    }

    private func showWindow() {
        let window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 760, height: 475),
                              styleMask: [.titled, .closable, .resizable], backing: .buffered, defer: false)
        window.title = "Litch Factory Camera – \(source.name)"
        window.minSize = NSSize(width: 420, height: 280)
        window.level = .floating
        window.collectionBehavior = [.canJoinAllSpaces, .fullScreenAuxiliary]
        window.isReleasedWhenClosed = false
        imageView.wantsLayer = true
        imageView.layer?.backgroundColor = NSColor.black.cgColor
        imageView.layer?.contentsGravity = .resizeAspect
        status.font = .systemFont(ofSize: 16)
        status.isSelectable = true
        status.maximumNumberOfLines = 2
        let content = window.contentView!
        for view in [imageView, status] {
            view.translatesAutoresizingMaskIntoConstraints = false
            content.addSubview(view)
        }
        NSLayoutConstraint.activate([
            imageView.topAnchor.constraint(equalTo: content.topAnchor),
            imageView.leadingAnchor.constraint(equalTo: content.leadingAnchor),
            imageView.trailingAnchor.constraint(equalTo: content.trailingAnchor),
            imageView.bottomAnchor.constraint(equalTo: status.topAnchor, constant: -12),
            status.leadingAnchor.constraint(equalTo: content.leadingAnchor, constant: 16),
            status.trailingAnchor.constraint(equalTo: content.trailingAnchor, constant: -16),
            status.bottomAnchor.constraint(equalTo: content.bottomAnchor, constant: -14),
        ])
        self.window = window
        window.center()
        // Keep batch keyboard controls in the terminal that launched the preview.
        window.orderFrontRegardless()
    }

    private func scan(_ image: CGImage) throws {
        let handler = VNImageRequestHandler(cgImage: image)
        try handler.perform([request])
        let observations = request.results ?? []
        let labels = observations.compactMap { observation -> Label? in
            guard let payload = observation.payloadStringValue,
                  (1...128).contains(payload.utf8.count) else { return nil }
            let symbology = observation.symbology == .qr ? "QR" : "Data Matrix"
            return Label(payload: payload, symbology: symbology)
        }
        let scan: CameraFrame
        if observations.isEmpty {
            scan = CameraFrame(state: "clear")
        } else if observations.count == 1, let label = labels.first {
            scan = CameraFrame(state: "label", label: label)
        } else {
            scan = CameraFrame(state: "ambiguous")
        }
        try FileHandle.standardOutput.write(contentsOf: encoder.encode(scan) + Data([0x0A]))
        // Wait for the display to accept this frame before holding another.
        DispatchQueue.main.sync { [self] in
            imageView.layer?.contents = image
            status.stringValue = scan.displayText
        }
    }

    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { true }
}

let arguments = Array(CommandLine.arguments.dropFirst())
if arguments == ["--list-native"] {
    try listNativeCameras()
    exit(0)
}
private let source: CameraSource
if arguments.count == 2, arguments[0] == "--native" {
    guard let camera = nativeCameras().first(where: { $0.uniqueID == arguments[1] }) else {
        failCamera("The selected native camera is no longer available.")
    }
    source = .native(camera)
} else if arguments.count == 3,
          let width = Int(arguments[0]), (1...1920).contains(width),
          let height = Int(arguments[1]), (1...1920).contains(height) {
    source = .stream(FrameSize(width: width, height: height), arguments[2])
} else {
    FileHandle.standardError.write(Data("litch: the camera preview requires an exact native ID or frame dimensions and a camera name.\n".utf8))
    exit(2)
}
let app = NSApplication.shared
private let preview = Preview(source: source)
app.setActivationPolicy(.regular)
app.delegate = preview
withExtendedLifetime(preview) { app.run() }
