// swift-tools-version: 5.9

import PackageDescription

let package = Package(
    name: "VoiceStick",
    platforms: [
        .macOS(.v12)
    ],
    products: [
        .executable(name: "VoiceStickApp", targets: ["VoiceStickApp"])
    ],
    targets: [
        .executableTarget(
            name: "VoiceStickApp",
            dependencies: ["CZlib"],
            path: "Sources/VoiceStickApp",
            exclude: ["Info.plist"],
            linkerSettings: [
                .unsafeFlags([
                    "-Xlinker", "-sectcreate",
                    "-Xlinker", "__TEXT",
                    "-Xlinker", "__info_plist",
                    "-Xlinker", "Sources/VoiceStickApp/Info.plist",
                ])
            ]
        ),
        .target(
            name: "CZlib",
            path: "Sources/CZlib",
            publicHeadersPath: "."
        )
    ]
)
