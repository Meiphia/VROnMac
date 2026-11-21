// SPDX-License-Identifier: MIT
import Foundation
import CryptoKit
import Darwin

struct SetupInstaller {
    let environment: SetupEnvironment
    let payload: URL
    private let files = FileManager.default

    func installWindowsSupport() throws {
        guard !environment.windowsSupportReady else { return }
        let target = environment.runtime.deletingLastPathComponent()
        try staged(in: target.deletingLastPathComponent()) { staging in
            let archive = staging.appendingPathComponent("voile.tar.xz")
            try download(URL(string: "https://github.com/Zagrios/Voile/releases/download/v26.3.0-24/voile-v26.3.0-24.tar.xz")!,
                         to: archive, maxBytes: 439583920)
            try verify(archive, sha256: "6151007373b1b48dad8ef4908723d7e93ef3356ab2f903f864c053c6317f166c")
            try run("/usr/bin/tar", ["-xf", archive.path, "-C", staging.path], step: "Extract Windows support", timeout: 180)
            let extracted = staging.appendingPathComponent("voile-v26.3.0-24")
            guard files.isExecutableFile(atPath: extracted.appendingPathComponent("linux/engine/fexdarwin-run").path),
                  environment.exists(extracted.appendingPathComponent("steam/steamclient64.dll")) else {
                throw SetupIssue(message: "The Windows support archive is incomplete.")
            }
            try publish(extracted, to: target)
        }
    }

    func updateSteamDisplayName() throws {
        let manifest = environment.compatibility.appendingPathComponent("compatibilitytool.vdf")
        let contents = try String(contentsOf: manifest, encoding: .utf8)
        guard contents.contains("VRConMac Linux engine") else { return }
        try contents.replacingOccurrences(of: "VRConMac Linux engine", with: "VRonMac Linux engine")
            .write(to: manifest, atomically: true, encoding: .utf8)
    }

    func installSteamCompatibility() throws {
        guard let hostVersion = environment.steamVersion else {
            throw SetupIssue(message: "Open Steam once so it can finish installing, then try again.")
        }
        guard let runner = environment.notProtonRunner, environment.windowsSupportReady else {
            throw SetupIssue(message: "Finish NotProton and Windows Support setup first.")
        }
        let base = URL(string: "https://client-update.akamai.steamstatic.com/")!
        let destination = environment.compatibility
        try staged(in: destination.deletingLastPathComponent()) { staging in
            let tool = staging.appendingPathComponent("tool")
            try files.copyItem(at: payload.appendingPathComponent("steam-tool"), to: tool)
            let client = tool.appendingPathComponent("steam-client")
            let installedClient = destination.appendingPathComponent("steam-client")
            let installedVersion = try? String(contentsOf: installedClient.appendingPathComponent("version"), encoding: .utf8)
                .trimmingCharacters(in: .whitespacesAndNewlines)
            if installedVersion == hostVersion && environment.exists(installedClient.appendingPathComponent("linux64/steamclient.so")) {
                try files.copyItem(at: installedClient, to: client)
            } else {
                let manifest = staging.appendingPathComponent("manifest")
                try download(base.appendingPathComponent("steam_client_ubuntu12"), to: manifest, maxBytes: 2_000_000)
                let text = try String(contentsOf: manifest, encoding: .utf8)
                guard SetupEnvironment.value("version", in: text) == hostVersion else {
                    throw SetupIssue(message: "Valve’s stable Linux client does not match your Steam build yet. Use stable Steam and try again once both builds match.")
                }
                let package = try Self.sdkPackage(in: text)
                let archive = staging.appendingPathComponent("sdk.zip")
                try download(base.appendingPathComponent(package.name), to: archive, maxBytes: 300_000_000)
                try verify(archive, sha256: package.sha256)
                try files.createDirectory(at: client, withIntermediateDirectories: false)
                try run("/usr/bin/unzip", ["-q", archive.path, "linux64/steamclient.so", "linux64/crashhandler.so", "-d", client.path],
                        step: "Extract the Linux Steam client")
                for name in ["steamclient.dll", "steamclient64.dll"] {
                    let source = environment.runtime.deletingLastPathComponent().appendingPathComponent("steam/" + name)
                    try files.copyItem(at: source, to: client.appendingPathComponent(name))
                }
                try (hostVersion + "\n").write(to: client.appendingPathComponent("version"), atomically: true, encoding: .utf8)
            }
            try (runner.deletingLastPathComponent().lastPathComponent + "\n")
                .write(to: tool.appendingPathComponent("helper-tool"), atomically: true, encoding: .utf8)
            let template = tool.appendingPathComponent("compatibilitytool.vdf")
            let runnerVDF = runner.deletingLastPathComponent().appendingPathComponent("compatibilitytool.vdf")
            let notProtonVDF = (try? String(contentsOf: runnerVDF, encoding: .utf8)) ?? ""
            let platform = SetupEnvironment.value("to_oslist", in: notProtonVDF) == "windows" ? "windows" : "macos"
            let contents = try String(contentsOf: template, encoding: .utf8)
            try contents.replacingOccurrences(of: "TO_OSLIST", with: platform)
                .replacingOccurrences(of: "\"vrconmac-lkd\"", with: "\"notproton-vrconmac-lkd\"")
                .write(to: template, atomically: true, encoding: .utf8)
            guard environment.steamVersion == hostVersion else {
                throw SetupIssue(message: "Steam updated during setup. Try again to get the matching client.")
            }
            try publish(tool, to: destination)
        }
    }

    private func upgradeGameRuntime(_ prefix: URL) throws {
        let runtime = TrackerRuntime(target: prefix.appendingPathComponent("drive_c/vrconmac/vrconmac_xr.dll"), payload: payload)
        if runtime.isLegacy() { try runtime.select(preview: runtime.isPreview()) }
    }

    func prepareGame(_ appID: UInt32) throws {
        guard environment.windowsSupportReady else {
            throw SetupIssue(message: "Install Windows Support in Setup first.")
        }
        guard !environment.gameReady(appID) else { return }
        let installedPrefix = environment.gamePrefix(appID)
        if environment.gameRuntimeRegistered(installedPrefix) {
            try installGraphics(in: installedPrefix)
            try installOpenVRPaths(in: installedPrefix)
            try upgradeGameRuntime(installedPrefix)
            return
        }
        let home = installedPrefix.deletingLastPathComponent()
        try staged(in: home.deletingLastPathComponent()) { staging in
            // Build a new prefix off to the side. Use an existing initialized prefix in place.
            let existing = environment.exists(home.appendingPathComponent("prefix/system.reg"))
            let gameHome = existing ? home : staging.appendingPathComponent("game")
            let prefix = gameHome.appendingPathComponent("prefix")
            let engine = environment.runtime.appendingPathComponent("engine")
            func wine(_ command: String, step: String) throws {
                guard !gameHome.path.contains(";"), !environment.runtime.path.contains(";") else {
                    throw SetupIssue(message: "The Windows engine cannot use a folder path containing a semicolon.")
                }
                let guest = [
                    "HOME=/home/user", "USER=user", "LOGNAME=user", "PATH=/opt/wine/bin:/usr/bin:/bin",
                    "LANG=en_US.UTF-8", "WINEPREFIX=/home/user/prefix", "WINEARCH=win64", "WINEDEBUG=-all"
                ].flatMap { ["--env", $0] }
                let guestHelpers = environment.runtime.appendingPathComponent("guest")
                let helperBind = environment.exists(guestHelpers.appendingPathComponent("steam-hostpid.so"))
                    ? ";/opt/obelisk=" + guestHelpers.path + ":ro" : ""
                try run("/usr/bin/env", [
                    "FEXDARWIN_LKD=1", "FEXDARWIN_ROOT=" + environment.runtime.appendingPathComponent("root").path,
                    "FEXDARWIN_ROOT_RO=1",
                    "FEXDARWIN_BIND=/opt/wine=" + environment.runtime.appendingPathComponent("wine").path +
                        ":ro;/home/user=" + gameHome.path + helperBind,
                    engine.appendingPathComponent("run-with-timeout.sh").path, "90",
                    engine.appendingPathComponent("fexdarwin-run").path, "--cwd", "/home/user"
                ] + guest + ["/bin/sh", "-c", command], step: step, timeout: 100)
            }
            if !existing {
                try files.createDirectory(at: gameHome, withIntermediateDirectories: false)
                try wine("/opt/wine/bin/wine wineboot --init && /opt/wine/bin/wineserver -w",
                         step: "Initialize the Windows environment")
            }

            if existing {
                for registry in ["system.reg", "user.reg"] {
                    try copyIfMissing(prefix.appendingPathComponent(registry),
                                      to: prefix.appendingPathComponent(registry + ".before-vrconmac"))
                }
            }
            let assets = payload.appendingPathComponent("game-runtime")
            let runtime = prefix.appendingPathComponent("drive_c/vrconmac")
            for directory in ["opencomposite/bin", "opencomposite/config", "opencomposite/logs"] {
                try files.createDirectory(at: runtime.appendingPathComponent(directory), withIntermediateDirectories: true)
            }
            try copyIfMissing(payload.appendingPathComponent("tracker/original.dll"),
                              to: runtime.appendingPathComponent("vrconmac_xr.dll"))
            for name in ["vrconmac.ini", "vrconmac_xr.json", "GameRuntime.reg"] {
                try copyIfMissing(assets.appendingPathComponent(name), to: runtime.appendingPathComponent(name))
            }
            try copyIfMissing(assets.appendingPathComponent("vrclient_x64.dll"),
                              to: runtime.appendingPathComponent("opencomposite/bin/vrclient_x64.dll"))

            try installOpenVRPaths(in: prefix)
            var importRegistry = #"/opt/wine/bin/wine regedit /S 'C:\vrconmac\GameRuntime.reg'"#
            if appID == 438100 {
                let dll = "windows.networking.connectivity.dll"
                let target = prefix.appendingPathComponent("drive_c/windows/system32/" + dll)
                let backup = target.deletingLastPathComponent().appendingPathComponent(dll + ".before-vrcm-network-cost")
                if environment.exists(target) { try copyIfMissing(target, to: backup) }
                let replacement = try Data(contentsOf: assets.appendingPathComponent(dll))
                try replacement.write(to: target, options: .atomic)
                try copyIfMissing(assets.appendingPathComponent("VRChatNetworking.reg"),
                                  to: runtime.appendingPathComponent("VRChatNetworking.reg"))
                importRegistry += #" && /opt/wine/bin/wine regedit /S 'C:\vrconmac\VRChatNetworking.reg'"#
            }
            try installGraphics(in: prefix)
            try wine(importRegistry + " && /opt/wine/bin/wineserver -w", step: "Register the VR runtime")
            guard environment.gamePrefixReady(prefix) else {
                throw SetupIssue(message: "Wine did not register the VR runtime.")
            }
            if !existing {
                guard !environment.exists(home) else {
                    throw SetupIssue(message: "A partial Windows environment already exists. Setup left it in place.")
                }
                try files.moveItem(at: gameHome, to: home)
            }
        }
    }

    private func installOpenVRPaths(in prefix: URL) throws {
        let paths = environment.openVRPathFiles(in: prefix)
        guard !paths.isEmpty else { throw SetupIssue(message: "Wine did not create a Windows user profile.") }

        let runtime = SetupEnvironment.openVRRuntime
        let registry: [String: Any] = [
            "config": [runtime + #"\config"#],
            "external_drivers": NSNull(),
            "jsonid": "vrpathreg",
            "log": [runtime + #"\logs"#],
            "runtime": [runtime],
            "version": 1
        ]
        let data = try JSONSerialization.data(withJSONObject: registry, options: [.prettyPrinted, .sortedKeys])
        for path in paths where !environment.openVRPathReady(path) {
            let directory = path.deletingLastPathComponent()
            try files.createDirectory(at: directory, withIntermediateDirectories: true)
            if environment.exists(path) {
                try copyIfMissing(path, to: directory.appendingPathComponent("openvrpaths.vrpath.before-vrconmac"))
            }
            try data.write(to: path, options: .atomic)
        }
    }

    private func installGraphics(in prefix: URL) throws {
        let system32 = prefix.appendingPathComponent("drive_c/windows/system32")
        for name in SetupEnvironment.graphicsDLLs {
            let source = environment.runtime.appendingPathComponent("windows/" + name)
            let target = system32.appendingPathComponent(name)
            guard !files.contentsEqual(atPath: source.path, andPath: target.path) else { continue }
            let replacement = try Data(contentsOf: source)
            try replacement.write(to: target, options: .atomic)
        }
    }

    private func copyIfMissing(_ source: URL, to target: URL) throws {
        if !environment.exists(target) { try files.copyItem(at: source, to: target) }
    }

    static func sdkPackage(in text: String) throws -> (name: String, sha256: String) {
        guard let range = text.range(of: #""bins_sdk_ubuntu12"\s*\{[^}]+\}"#, options: .regularExpression),
              let name = SetupEnvironment.value("file", in: String(text[range])),
              name.range(of: #"^bins_sdk_ubuntu12\.zip\.[a-f0-9]{40}$"#, options: .regularExpression) != nil,
              let hash = SetupEnvironment.value("sha2", in: String(text[range])),
              hash.range(of: #"^[a-f0-9]{64}$"#, options: .regularExpression) != nil else {
            throw SetupIssue(message: "Valve’s Steam manifest did not contain a valid Linux client package.")
        }
        return (name, hash)
    }

    private func staged(in parent: URL, operation: (URL) throws -> Void) throws {
        try files.createDirectory(at: parent, withIntermediateDirectories: true)
        let staging = parent.appendingPathComponent(".vronmac-" + UUID().uuidString)
        try files.createDirectory(at: staging, withIntermediateDirectories: false)
        defer { try? files.removeItem(at: staging) }
        try operation(staging)
    }

    private func publish(_ source: URL, to target: URL) throws {
        if environment.exists(target) {
            // Exchange complete directories atomically. The prior version moves into staging.
            guard renameatx_np(AT_FDCWD, source.path, AT_FDCWD, target.path, UInt32(RENAME_SWAP)) == 0 else {
                throw SetupIssue(message: "Could not replace \(target.lastPathComponent). Close Steam and try again.")
            }
        } else {
            try files.moveItem(at: source, to: target)
        }
    }

    private func download(_ url: URL, to file: URL, maxBytes: Int) throws {
        try run("/usr/bin/curl", ["--fail", "--location", "--silent", "--show-error", "--proto", "=https",
                "--proto-redir", "=https", "--max-time", "900", "--max-filesize", String(maxBytes),
                "--output", file.path, url.absoluteString], step: "Download from \(url.host ?? "the release server")", timeout: 910)
    }

    private func verify(_ file: URL, sha256: String) throws {
        let handle = try FileHandle(forReadingFrom: file)
        defer { try? handle.close() }
        var hash = SHA256()
        while let chunk = try handle.read(upToCount: 1_048_576), !chunk.isEmpty { hash.update(data: chunk) }
        guard hash.finalize().map({ String(format: "%02x", $0) }).joined() == sha256 else {
            throw SetupIssue(message: "The download failed its integrity check. Try again.")
        }
    }

    private func run(_ path: String, _ arguments: [String], step: String, timeout: TimeInterval = 60) throws {
        let result = try Command.run(URL(fileURLWithPath: path), arguments, timeout: timeout)
        guard result.code == 0 else {
            let detail = result.output.trimmingCharacters(in: .whitespacesAndNewlines)
            throw SetupIssue(message: detail.isEmpty ? "\(step) failed." : "\(step) failed. \(detail.suffix(300))")
        }
    }
}
