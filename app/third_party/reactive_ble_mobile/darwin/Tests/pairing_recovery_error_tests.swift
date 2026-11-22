import Foundation
import CoreBluetooth

@main enum PairingRecoveryErrorTests {
    static func main() {
        let marker = "LITCH_PAIRING_KEY_MISSING:CBErrorDomain:14\n"
        let missing = NSError(domain: CBErrorDomain, code: CBError.peerRemovedPairingInformation.rawValue,
                              userInfo: [NSLocalizedDescriptionKey: "Localized diagnostic"])
        let att = NSError(domain: "CBATTErrorDomain", code: 14,
                          userInfo: [NSLocalizedDescriptionKey: "Peer removed pairing information"])
        let timeout = NSError(domain: CBErrorDomain, code: CBError.connectionTimeout.rawValue)
        precondition(pairingRecoveryErrorMessage(missing).hasPrefix(marker))
        precondition(pairingRecoveryErrorMessage(missing).contains("Localized diagnostic"))
        precondition(pairingRecoveryErrorMessage(att) == String(describing: att))
        precondition(pairingRecoveryErrorMessage(timeout) == String(describing: timeout))
        precondition(pairingRecoveryErrorMessage([timeout, missing]).hasPrefix(marker))
        precondition(pairingRecoveryErrorMessage([timeout, missing]).contains(String(describing: timeout)))
        precondition(pairingRecoveryErrorMessage([]).isEmpty)
        print("PASS: production pairing recovery error mapping; exact domain/code, diagnostic and discovery aggregation")
    }
}
