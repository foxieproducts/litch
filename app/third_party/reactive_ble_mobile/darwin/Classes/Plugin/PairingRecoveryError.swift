import Foundation
import struct CoreBluetooth.CBError
import var CoreBluetooth.CBErrorDomain

// Keep the public plugin failure code intact. This nonlocalized marker crosses
// its existing message field; the diagnostic remains available after newline.
func pairingRecoveryErrorMessage(_ error: Error) -> String {
    return pairingRecoveryErrorMessage([error])
}

func pairingRecoveryErrorMessage(_ errors: [Error]) -> String {
    let diagnostic = errors.map(String.init(describing:)).joined(separator: "\n")
    if #available(iOS 13.4, macOS 10.15, *) {
        if errors.contains(where: {
            let native = $0 as NSError
            return native.domain == CBErrorDomain && native.code == CBError.peerRemovedPairingInformation.rawValue
        }) {
            return "LITCH_PAIRING_KEY_MISSING:CBErrorDomain:14\n" + diagnostic
        }
    }
    return diagnostic
}
