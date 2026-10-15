// OWNER: builder-B (ue)
// Lists on-screen windows (owner, pid, layer, bounds). Used by tools/ue/_common.sh to prove engine runs
// open no window and that no system dialog is pending. Needs no Screen Recording permission.
import CoreGraphics
import Foundation
// usage: winlist [pid...]  -> lists on-screen windows (owner, pid, layer, bounds); filters by pid when given
let want = Set(CommandLine.arguments.dropFirst().compactMap { Int($0) })
let info = CGWindowListCopyWindowInfo([.optionOnScreenOnly], kCGNullWindowID) as? [[String: Any]] ?? []
for w in info {
    let pid = w[kCGWindowOwnerPID as String] as? Int ?? -1
    if !want.isEmpty && !want.contains(pid) { continue }
    let owner = w[kCGWindowOwnerName as String] as? String ?? "?"
    let layer = w[kCGWindowLayer as String] as? Int ?? 0
    let b = w[kCGWindowBounds as String] as? [String: Any] ?? [:]
    print("\(pid)\t\(owner)\tlayer=\(layer)\t\(b["Width"] ?? 0)x\(b["Height"] ?? 0)@\(b["X"] ?? 0),\(b["Y"] ?? 0)")
}
