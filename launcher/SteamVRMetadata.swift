// SPDX-License-Identifier: MIT
import Foundation

enum SteamVRMetadata {
    static func appIDs(in data: Data, installed: Set<UInt32>) -> Set<UInt32> {
        let bytes = [UInt8](data)
        var reader = Reader(bytes: bytes, position: 0, end: bytes.count)
        guard let magic = reader.integer(UInt32.self),
              (0x07564427...0x07564429).contains(magic),
              reader.skip(4) else { return [] }

        var keys: [String]?
        if magic == 0x07564429 {
            guard let offset = reader.integer(UInt64.self),
                  offset >= reader.position, offset <= bytes.count else { return [] }
            reader.end = Int(offset)
            var table = Reader(bytes: bytes, position: Int(offset), end: bytes.count)
            guard let count = table.integer(UInt32.self),
                  count <= table.end - table.position else { return [] }
            var names: [String] = []
            for _ in 0..<count {
                guard let name = table.string() else { return [] }
                names.append(name)
            }
            keys = names
        }

        let prefix = magic == 0x07564427 ? 40 : 60
        var result: Set<UInt32> = []
        while let id = reader.integer(UInt32.self), id != 0,
              let size = reader.integer(UInt32.self) {
            let start = reader.position
            guard reader.skip(Int(size)) else { break }
            guard installed.contains(id), size >= prefix else { continue }
            var entry = Reader(bytes: bytes, position: start + prefix, end: reader.position)
            if supportsVR(&entry, keys: keys) { result.insert(id) }
        }
        return result
    }

    private static func supportsVR(_ reader: inout Reader, keys: [String]?) -> Bool {
        var path: [String] = []
        var found = false
        while let type = reader.integer(UInt8.self) {
            if type == 8 || type == 11 {
                if path.isEmpty { return found }
                path.removeLast()
                continue
            }

            let key: String
            if let keys {
                guard let index = reader.integer(UInt32.self), index < keys.count else { return false }
                key = keys[Int(index)]
            } else {
                guard let name = reader.string() else { return false }
                key = name
            }

            var value: String?
            switch type {
            case 0:
                guard path.count < 64 else { return false }
                path.append(key)
                continue
            case 1:
                guard let string = reader.string() else { return false }
                value = string
            case 2:
                guard let number = reader.integer(UInt32.self) else { return false }
                if number == 1 { value = "1" }
            case 3, 4, 6:
                guard reader.skip(4) else { return false }
            case 7, 10:
                guard reader.skip(8) else { return false }
            default:
                return false
            }

            if value == "1" {
                if path == ["appinfo", "common", "category"],
                   ["category_52", "category_53", "category_54"].contains(key) {
                    found = true
                }
                if path == ["appinfo", "common"], ["openvrsupport", "onlyvrsupport"].contains(key) {
                    found = true
                }
            }
            if path.count == 4, path.starts(with: ["appinfo", "config", "launch"]),
               key == "type", let value,
               ["vr", "openvr", "openxr", "oculusvr"].contains(value) {
                found = true
            }
        }
        return found && path.isEmpty
    }

    private struct Reader {
        let bytes: [UInt8]
        var position: Int
        var end: Int

        mutating func skip(_ count: Int) -> Bool {
            guard count <= end - position else { return false }
            position += count
            return true
        }

        mutating func integer<T: FixedWidthInteger>(_ type: T.Type) -> T? {
            let offset = position
            guard skip(MemoryLayout<T>.size) else { return nil }
            return bytes.withUnsafeBytes {
                T(littleEndian: $0.loadUnaligned(fromByteOffset: offset, as: type))
            }
        }

        mutating func string() -> String? {
            guard let terminator = bytes[position..<end].firstIndex(of: 0) else { return nil }
            let value = String(decoding: bytes[position..<terminator], as: UTF8.self)
            position = terminator + 1
            return value
        }
    }
}
