// mlx_activate.swift — activation host app for MlxRDMA.dext.
// Submits an OSSystemExtensionRequest (activation) so sysextd picks up the
// dext embedded in this app bundle (Contents/Library/SystemExtensions/).
// Run the binary from the app bundle. Developer mode is only a local fallback
// while the Apple production entitlement is pending.
import Foundation
import SystemExtensions

final class RequestDelegate: NSObject, OSSystemExtensionRequestDelegate {
    func request(_ request: OSSystemExtensionRequest, didFinishWithResult result: OSSystemExtensionRequest.Result) {
        print("RESULT: \(result == .completed ? "completed" : String(describing: result))")
        exit(result == .completed ? 0 : 1)
    }

    func request(_ request: OSSystemExtensionRequest, didFailWithError error: Error) {
        let ns = error as NSError
        print("FAILED: [\(ns.domain) \(ns.code)] \(ns.localizedDescription)")
        if let reason = ns.localizedFailureReason { print("reason: \(reason)") }
        exit(1)
    }

    func requestNeedsUserApproval(_ request: OSSystemExtensionRequest) {
        print("NEEDS APPROVAL: System Settings > General > Login Items & Extensions > Allow")
    }

    func request(_ request: OSSystemExtensionRequest,
                 actionForReplacingExtension existing: OSSystemExtensionProperties,
                 withExtension ext: OSSystemExtensionProperties) -> OSSystemExtensionRequest.ReplacementAction {
        print("REPLACE: \(existing.bundleVersion) -> \(ext.bundleVersion)")
        return .replace
    }
}

let delegate = RequestDelegate()
let arguments = Set(CommandLine.arguments.dropFirst())
let deactivate = arguments.contains("--deactivate")
if arguments.contains("--help") || arguments.contains("-h") {
    print("usage: mlx_activate [--activate|--deactivate] [bundle-identifier]")
    exit(0)
}
if arguments.contains("--activate") && deactivate {
    print("FAILED: --activate and --deactivate are mutually exclusive")
    exit(2)
}

/* The identifier is an argument so the extension that used to be called
 * com.mlx5.rdma.dext can still be deactivated after the rename — a
 * deactivation request has to name the identifier that is actually installed,
 * and that one is no longer this bundle's. Anything that is not a flag is
 * taken as the identifier. */
let defaultIdentifier = "com.melondma.rdma.dext"
let identifier = arguments.first(where: { !$0.hasPrefix("-") }) ?? defaultIdentifier

let request: OSSystemExtensionRequest
if deactivate {
    print("REQUEST: deactivate \(identifier)")
    request = OSSystemExtensionRequest.deactivationRequest(
        forExtensionWithIdentifier: identifier, queue: DispatchQueue.main)
} else {
    print("REQUEST: activate \(identifier)")
    request = OSSystemExtensionRequest.activationRequest(
        forExtensionWithIdentifier: identifier, queue: DispatchQueue.main)
}
request.delegate = delegate
OSSystemExtensionManager.shared.submitRequest(request)
RunLoop.main.run()
