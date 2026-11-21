// SPDX-License-Identifier: MIT
import Foundation
import CryptoKit
import Darwin

struct TrackerStatus: Decodable {
    var phase: String
    var samples: Int
    var target: Int
    var bodyTracked: Int
    var bodyTarget: Int?
    var lastError: String
    var discoveredSerials: [String]?
}

struct TrackerConfiguration {
    let reference: String
    let manual: Bool
    let bodies: [String]

    static func discoveredSerials(in output: String) -> Set<String> {
        let prefix = "VRONMAC_TRACKER "
        return Set(output.split(separator: "\n").compactMap { line in
            guard line.hasPrefix(prefix) else { return nil }
            let serial = String(line.dropFirst(prefix.count)).uppercased()
            return isSerial(serial) ? serial : nil
        })
    }

    private static func isSerial(_ serial: String) -> Bool {
        serial.range(of: #"^LHR-[0-9A-F]{8}$"#, options: .regularExpression) != nil
    }

    init(reference: String, manual: Bool, bodies: [String]) throws {
        self.reference = reference.trimmingCharacters(in: .whitespacesAndNewlines).uppercased()
        self.manual = manual
        self.bodies = bodies.map { $0.trimmingCharacters(in: .whitespacesAndNewlines).uppercased() }
            .filter { !$0.isEmpty }
        let serials = [self.reference] + self.bodies
        guard serials.allSatisfy(Self.isSerial) else {
            throw SetupIssue(message: "Scan for trackers and choose a valid alignment tracker.")
        }
        guard Set(self.bodies).count == self.bodies.count,
              manual || !self.bodies.contains(self.reference) else {
            throw SetupIssue(message: "Use each body tracker once. A mounted reference needs its own tracker.")
        }
    }
}

struct TrackerRuntime {
    let target: URL
    let payload: URL
    static let legacyOriginalHash = "2c44d854ef24515827f2696e312aaf4b083270e1204d12009d284d42dd7aa7d1"
    static let legacyPreviewHash = "d7787b4fe9d67685e87af3f9f2008d8086fd4359a9f37186d1c0913ab7d2096e"

    private static let previousOriginalHash = "bd55e0b2004fc9fa606ecde89da3c45704bf304490cc6b777bee7541ad1fa32f"
    private let originalHash = "26c970227a8719e8ec1122b9a4f02cdb1476937ca3a8e9935302accc960dfe6c"
    private static let previousPreviewHash = "0122d34f11fa0194c07fa1a5a1eda8cd8af1f885eac91b01151b7384fe7836fa"
    private let previewHash = "58e4f0bbfe295dc5d4275ccc9141e5d518b0a82f1c04b002a888bd71ceb07582"

    static func isLegacy(_ data: Data) -> Bool {
        let current = SHA256.hash(data: data).map { String(format: "%02x", $0) }.joined()
        return current == legacyOriginalHash || current == legacyPreviewHash
    }

    func isLegacy() -> Bool {
        (try? Data(contentsOf: target)).map(Self.isLegacy) ?? false
    }

    private func hash(_ data: Data) -> String {
        SHA256.hash(data: data).map { String(format: "%02x", $0) }.joined()
    }

    func upgradeDiagnosticsIfNeeded() throws {
        guard let data = try? Data(contentsOf: target) else { return }
        let current = hash(data)
        if current == Self.previousOriginalHash || current == Self.previousPreviewHash {
            try select(preview: current == Self.previousPreviewHash)
        }
    }

    func isPreview() -> Bool {
        guard let data = try? Data(contentsOf: target) else { return false }
        return [previewHash, Self.previousPreviewHash, Self.legacyPreviewHash].contains(hash(data))
    }

    func select(preview: Bool) throws {
        guard target.resolvingSymlinksInPath().path == target.standardizedFileURL.path else {
            throw SetupIssue(message: "The game runtime path resolves through a symbolic link.")
        }
        let current = hash(try Data(contentsOf: target))
        guard [originalHash, previewHash, Self.previousOriginalHash, Self.previousPreviewHash,
               Self.legacyOriginalHash, Self.legacyPreviewHash].contains(current) else {
            throw SetupIssue(message: "The installed game runtime differs from both known versions. Check the compatibility setup.")
        }
        let expected = preview ? previewHash : originalHash
        guard current != expected else { return }
        let source = payload.appendingPathComponent(preview ? "tracker/runtime.dll" : "tracker/original.dll")
        let data = try Data(contentsOf: source)
        guard hash(data) == expected else { throw SetupIssue(message: "The bundled runtime failed verification.") }
        let permissions = try FileManager.default.attributesOfItem(atPath: target.path)[.posixPermissions]
        let staged = target.deletingLastPathComponent().appendingPathComponent(".vronmac-" + UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: staged) }
        try data.write(to: staged, options: .withoutOverwriting)
        if let permissions { try FileManager.default.setAttributes([.posixPermissions: permissions], ofItemAtPath: staged.path) }
        try FileManager.default.replaceItemAt(target, withItemAt: staged)
    }
}

struct VideoSettings {
    var bitrate = 80
    var resolution = 0.90
    var foveation = "off"
    var adaptation = "off"
    var preset = "speed"

    private static func key(_ line: String) -> String? {
        let trimmed = line.trimmingCharacters(in: .whitespaces)
        guard !trimmed.hasPrefix("#"), !trimmed.hasPrefix("["), let eq = trimmed.firstIndex(of: "=") else { return nil }
        return String(trimmed[..<eq]).trimmingCharacters(in: .whitespaces)
    }

    init(text: String) {
        var values: [String: String] = [:]
        for line in text.components(separatedBy: .newlines) {
            if let key = Self.key(line), let eq = line.firstIndex(of: "=") {
                values[key] = String(line[line.index(after: eq)...])
                    .components(separatedBy: "#")[0].trimmingCharacters(in: .whitespacesAndNewlines)
                    .trimmingCharacters(in: CharacterSet(charactersIn: "\""))
            }
        }
        if let value = values["bitrate_mbps"].flatMap(Int.init), (1...200).contains(value) { bitrate = value }
        if let value = values["resolution_scale"].flatMap(Double.init), (0.25...1).contains(value) { resolution = value }
        if let value = values["foveated_encoding_preset"], ["off", "light", "medium", "high"].contains(value) { foveation = value }
        if let value = values["abr_mode"], ["off", "bitrate", "full"].contains(value) { adaptation = value }
        if let value = values["encoder_preset"], ["quality", "balanced", "speed"].contains(value) { preset = value }
    }

    func applied(to text: String) throws -> String {
        guard (1...200).contains(bitrate), (0.25...1).contains(resolution),
              ["off", "light", "medium", "high"].contains(foveation),
              ["off", "bitrate", "full"].contains(adaptation),
              ["quality", "balanced", "speed"].contains(preset) else {
            throw SetupIssue(message: "Choose supported video settings.")
        }
        let values = [
            "bitrate_mbps": String(bitrate),
            "resolution_scale": String(resolution),
            "foveated_encoding_preset": "\"\(foveation)\"",
            "abr_mode": "\"\(adaptation)\"",
            "encoder_preset": "\"\(preset)\""
        ]
        var seen = Set<String>()
        var lines = text.components(separatedBy: "\n").map { line -> String in
            guard let key = Self.key(line), let value = values[key] else { return line }
            seen.insert(key)
            return "\(key) = \(value)"
        }
        let missing = values.keys.sorted().filter { !seen.contains($0) }.map { "\($0) = \(values[$0]!)" }
        let streaming = lines.firstIndex { $0.trimmingCharacters(in: .whitespacesAndNewlines) == "[streaming]" }
        if let streaming {
            lines.insert(contentsOf: missing, at: streaming + 1)
        } else if !missing.isEmpty {
            lines.append(contentsOf: ["", "[streaming]"] + missing)
        }
        return lines.joined(separator: "\n") + (lines.last == "" ? "" : "\n")
    }
}

func checkPort(_ port: UInt16, tcp: Bool) throws {
    let fd = Darwin.socket(AF_INET, tcp ? SOCK_STREAM : SOCK_DGRAM, 0)
    guard fd >= 0 else { throw SetupIssue(message: "Could not open a local socket.") }
    defer { close(fd) }
    if tcp {
        var one: Int32 = 1
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, socklen_t(MemoryLayout<Int32>.size))
    }
    var address = sockaddr_in()
    address.sin_len = UInt8(MemoryLayout<sockaddr_in>.size)
    address.sin_family = sa_family_t(AF_INET)
    address.sin_port = port.bigEndian
    address.sin_addr.s_addr = inet_addr("127.0.0.1")
    let bound = withUnsafePointer(to: &address) { pointer in
        pointer.withMemoryRebound(to: sockaddr.self, capacity: 1) {
            Darwin.bind(fd, $0, socklen_t(MemoryLayout<sockaddr_in>.size))
        }
    }
    guard bound == 0, !tcp || listen(fd, 1) == 0 else {
        throw SetupIssue(message: "Local port \(port) is unavailable. Close any previous bridge or tracker service and try again.")
    }
}
