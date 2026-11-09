// swift-tools-version:5.9
// PenCastCore: PDF media scanning + geometry, shared by the iPad app and the
// `pencast-scan` CLI (spike S1). Builds on macOS so it can be checked without Xcode.
import PackageDescription

let package = Package(
    name: "PenCastCore",
    platforms: [.iOS(.v17), .macOS(.v14)],
    products: [.library(name: "PenCastCore", targets: ["PenCastCore"])],
    targets: [
        .target(name: "PenCastCore"),
        .executableTarget(name: "pencast-scan", dependencies: ["PenCastCore"]),
    ]
)
