// SPDX-License-Identifier: MIT
import Foundation
import CryptoKit
import Darwin

struct SetupIssue: Error, LocalizedError {
    let message:String
    var errorDescription:String? {message}
}
struct SetupCheck: Codable, Identifiable {
    let id:String, title:String, ready:Bool, detail:String
}
struct PayloadEntry:Codable {let path:String,sha256:String}
struct PayloadManifest:Codable {let schema:Int,revision:String;let files:[PayloadEntry]}
struct Device:Codable,Equatable,Identifiable {let serial:String,state:String;var id:String{serial}}
struct SteamGame: Identifiable, Equatable {
    let id: UInt32
    let name: String
}

struct GameVerification: Decodable {
    let verifiedAppIDs: [UInt32]

    func badge(for appID: UInt32) -> String {
        verifiedAppIDs.contains(appID) ? "Verified" : "Untested"
    }
}

struct CommandResult {let code:Int32,output:String}

// Commands receive argument arrays. No user/device path is interpreted by a shell.
enum Command {
    static func run(_ executable:URL,_ arguments:[String],timeout:TimeInterval=30) throws -> CommandResult {
        let process=Process(),pipe=Pipe()
        process.executableURL=executable;process.arguments=arguments
        process.standardOutput=pipe;process.standardError=pipe
        let fd=pipe.fileHandleForReading.fileDescriptor
        guard fcntl(fd,F_SETFL,fcntl(fd,F_GETFL)|O_NONBLOCK)>=0 else{throw SetupIssue(message:"Cannot capture setup output.")}
        try process.run()
        try? pipe.fileHandleForWriting.close()
        var data=Data(),buffer=[UInt8](repeating:0,count:8192)
        let deadline=ProcessInfo.processInfo.systemUptime+timeout
        var expired=false,terminatedAt:TimeInterval?
        func drain(){
            for _ in 0..<32 {
                let count=Darwin.read(fd,&buffer,buffer.count)
                if count<=0 {break}
                if data.count<2097152 {data.append(contentsOf:buffer.prefix(min(count,2097152-data.count)))}
            }
        }
        while process.isRunning {
            drain()
            let now=ProcessInfo.processInfo.systemUptime
            if !expired && now>=deadline {expired=true;terminatedAt=now;process.terminate()}
            if let at=terminatedAt,now-at>=2,process.isRunning {_ = Darwin.kill(process.processIdentifier,SIGKILL)}
            Thread.sleep(forTimeInterval:0.01)
        }
        process.waitUntilExit();drain()
        try? pipe.fileHandleForReading.close()
        if expired {throw SetupIssue(message:"A setup step timed out. Reconnect the headset and try again.")}
        return CommandResult(code:process.terminationStatus,output:String(decoding:data,as:UTF8.self))
    }
}

struct SetupEnvironment {
    let home:URL
    var steam:URL {home.appendingPathComponent("Library/Application Support/Steam")}
    var support:URL {home.appendingPathComponent("Library/Application Support/VRonMac")}
    var legacy:URL {home.appendingPathComponent("Library/Application Support/com.zagrios.obelisk")}
    var prefix: URL { gamePrefix(438100) }
    func gamePrefix(_ id: UInt32) -> URL {
        legacy.appendingPathComponent("Prefixes/steam-\(id)-windows-linux/prefix")
    }
    var runtime:URL {legacy.appendingPathComponent("Voile/v26.3.0-24/linux")}
    var compatibility:URL {steam.appendingPathComponent("compatibilitytools.d/notproton-vrconmac-lkd")}
    var steamApp:URL {steam.appendingPathComponent("Steam.AppBundle/Steam")}
    func exists(_ url:URL)->Bool {FileManager.default.fileExists(atPath:url.path)}
    var microphoneCheck: SetupCheck {
        let installed = exists(URL(fileURLWithPath: "/Library/Audio/Plug-Ins/HAL/VRConMac Microphone.driver"))
        return SetupCheck(id: "microphone", title: "Microphone support", ready: installed,
                          detail: installed ? "Virtual microphone installed" : "Virtual microphone driver is missing")
    }

    var notProtonRunner: URL? {
        let directory = steam.appendingPathComponent("compatibilitytools.d")
        let files = FileManager.default
        let entries = (try? files.contentsOfDirectory(atPath: directory.path)) ?? []
        let preferred = ["notproton", "notproton-26.3"]
        let names = preferred + entries.sorted().filter {
            !preferred.contains($0) && $0.range(
                of: #"^notproton-(?:[0-9]+(?:\.[0-9]+)+|(?:fex-rosetta|fex|preview)(?:-[0-9]+)?)$"#,
                options: .regularExpression) != nil
        }
        return names.lazy.map { directory.appendingPathComponent($0 + "/run") }.first {
            files.isExecutableFile(atPath: $0.path) &&
                files.fileExists(atPath: $0.deletingLastPathComponent().appendingPathComponent("compatibilitytool.vdf").path)
        }
    }
    var notProtonReady: Bool { notProtonRunner != nil }
    var windowsSupportReady: Bool {
        FileManager.default.isExecutableFile(atPath: runtime.appendingPathComponent("engine/fexdarwin-run").path)
    }

    var steamVersion: String? {
        let manifest = steamApp.appendingPathComponent("Contents/MacOS/package/steam_client_signed-2_osx.manifest")
        guard let text = try? String(contentsOf: manifest, encoding: .utf8),
              let version = Self.value("version", in: text), !version.isEmpty else { return nil }
        return version
    }

    var steamDisplayNameNeedsUpdate: Bool {
        let manifest = compatibility.appendingPathComponent("compatibilitytool.vdf")
        return (try? String(contentsOf: manifest, encoding: .utf8))?.contains("VRConMac Linux engine") == true
    }

    func checks() -> [SetupCheck] {
        let steamVersion = steamVersion
        let clientVersion=((try? String(contentsOf:compatibility.appendingPathComponent("steam-client/version"),encoding:.utf8)) ?? "").trimmingCharacters(in:.whitespacesAndNewlines)
        let matched = steamVersion.map { !$0.isEmpty && $0 == clientVersion } ?? false
        let toolInstalled = FileManager.default.isExecutableFile(atPath: compatibility.appendingPathComponent("run").path)
            && exists(compatibility.appendingPathComponent("steam-client/linux64/steamclient.so"))
        let integrationDetail: String
        if !toolInstalled || clientVersion.isEmpty {
            integrationDetail = "Install the VRonMac compatibility tool and its matching Linux Steam client."
        } else if let steamVersion, !matched {
            integrationDetail = "Steam is build \(steamVersion). Refresh the Linux Steam client (currently \(clientVersion)) to match."
        } else if matched {
            integrationDetail = "Steam and the Linux compatibility client match (build \(clientVersion))."
        } else {
            integrationDetail = "Could not read the installed Steam build. Open Steam and refresh Setup."
        }
        return [
          .init(id:"steam",title:"Steam",ready:steamVersion != nil,detail:steamVersion != nil ? "Steam installation found" : "Install Steam, open it and sign in."),
          .init(id:"notproton",title:"NotProton",ready:notProtonReady,detail:notProtonReady ? "Steam runner installed" : "Install NotProton and finish its Steam setup."),
          .init(id:"runtime",title:"Windows Support",ready:windowsSupportReady,detail:windowsSupportReady ? "Voile Linux engine installed" : "Download and install Voile (about 440 MB)."),
          .init(id:"integration",title:"Steam compatibility",ready:toolInstalled && matched,detail:(!notProtonReady || !windowsSupportReady) && !matched ? "Finish NotProton and Windows Support first." : integrationDetail),
          microphoneCheck
        ]
    }
    static func value(_ key:String,in text:String)->String? {
        let pattern="\\\""+NSRegularExpression.escapedPattern(for:key)+"\\\"\\s*\\\"([^\\\"]*)\\\""
        guard let expression=try? NSRegularExpression(pattern:pattern),let match=expression.firstMatch(in:text,range:NSRange(text.startIndex...,in:text)),let range=Range(match.range(at:1),in:text) else{return nil}
        return String(text[range])
    }
    private func steamLibraries() -> [URL] {
        var libraries=[steam]
        let data=(try? String(contentsOf:steam.appendingPathComponent("steamapps/libraryfolders.vdf"),encoding:.utf8)) ?? ""
        if let regex=try? NSRegularExpression(pattern:"\\\"path\\\"\\s*\\\"([^\\\"]+)\\\"") {
            for match in regex.matches(in:data,range:NSRange(data.startIndex...,in:data)) {
                if let range=Range(match.range(at:1),in:data) {libraries.append(URL(fileURLWithPath:String(data[range]).replacingOccurrences(of:"\\\\",with:"\\")))}
            }
        }
        return libraries
    }

    var mediaToolInstalled: Bool {
        exists(compatibility.appendingPathComponent("media.sh")) &&
            exists(compatibility.appendingPathComponent("media/wine/mf-range.dll"))
    }

    func gameReady(_ id: UInt32) -> Bool {
        let prefix = gamePrefix(id)
        guard gamePrefixReady(prefix),
              let data = try? Data(contentsOf: prefix.appendingPathComponent("drive_c/vrconmac/vrconmac_xr.dll"))
        else { return false }
        return !TrackerRuntime.isLegacy(data)
    }

    static let graphicsDLLs = ["d3d11.dll", "dxgi.dll", "d3d12.dll"]

    static let openVRRuntime = #"C:\vrconmac\opencomposite"#

    func openVRPathFiles(in prefix: URL) -> [URL] {
        let users = prefix.appendingPathComponent("drive_c/users")
        let entries = (try? FileManager.default.contentsOfDirectory(at: users, includingPropertiesForKeys: [.isDirectoryKey])) ?? []
        return entries.filter {
            $0.lastPathComponent != "Public" && (try? $0.resourceValues(forKeys: [.isDirectoryKey]).isDirectory) == true
        }.map { $0.appendingPathComponent("AppData/Local/openvr/openvrpaths.vrpath") }
    }

    func openVRPathReady(_ path: URL) -> Bool {
        guard let data = try? Data(contentsOf: path),
              let registry = (try? JSONSerialization.jsonObject(with: data)) as? [String: Any] else { return false }
        return registry["jsonid"] as? String == "vrpathreg" &&
            registry["version"] as? Int == 1 &&
            registry["runtime"] as? [String] == [Self.openVRRuntime]
    }

    func gamePrefixReady(_ prefix: URL) -> Bool {
        let pathFiles = openVRPathFiles(in: prefix)
        return gameRuntimeRegistered(prefix) && !pathFiles.isEmpty && pathFiles.allSatisfy(openVRPathReady)
            && Self.graphicsDLLs.allSatisfy { name in
            let source = runtime.appendingPathComponent("windows/" + name)
            let target = prefix.appendingPathComponent("drive_c/windows/system32/" + name)
            return exists(source) && FileManager.default.contentsEqual(atPath: source.path, andPath: target.path)
        }
    }

    func gameRuntimeRegistered(_ prefix: URL) -> Bool {
        guard exists(prefix.appendingPathComponent("drive_c/vrconmac/vrconmac_xr.dll")),
              let registry = try? String(contentsOf: prefix.appendingPathComponent("system.reg"), encoding: .utf8)
        else { return false }
        return registry.contains(#""ActiveRuntime"="C:\\vrconmac\\vrconmac_xr.json""#)
    }

    func launchDiagnostics(appID: UInt32, temporaryDirectory: URL) -> String {
        let files = FileManager.default
        let standardPrefix = gamePrefix(appID)
        let mediaPrefix = support.appendingPathComponent("Media/v1/home/prefix")
        let prefix = appID == 438100 && exists(mediaPrefix.appendingPathComponent("system.reg")) ? mediaPrefix : standardPrefix
        var report = [
            "VRonMac launch diagnostics • Steam app \(appID) • \(Date())",
            "macOS: " + ProcessInfo.processInfo.operatingSystemVersionString,
            "Game runtime registered: \(gameRuntimeRegistered(prefix))",
            "Game environment ready: \(gameReady(appID))",
            "NotProton runner: " + (notProtonRunner?.deletingLastPathComponent().lastPathComponent ?? "missing")
        ]
        report += checks().map { "\($0.title): \($0.detail)" }
        var logs = steamLibraries().map {
            $0.appendingPathComponent("steamapps/compatdata/\(appID)/vrconmac-lkd.log")
        }
        logs += [
            temporaryDirectory.appendingPathComponent("vrconmac-lkd.log"),
            steam.appendingPathComponent("logs/gameprocess_log.txt"),
            prefix.appendingPathComponent("drive_c/vrconmac/vrconmac_xr.log")
        ]

        let profiles = prefix.appendingPathComponent("drive_c/users")
        for profile in (try? files.contentsOfDirectory(at: profiles, includingPropertiesForKeys: nil)) ?? [] {
            if appID == 438100 {
                let gameLogs = profile.appendingPathComponent("AppData/LocalLow/VRChat/VRChat")
                let entries = (try? files.contentsOfDirectory(at: gameLogs, includingPropertiesForKeys: nil)) ?? []
                logs += entries.filter { $0.lastPathComponent.hasPrefix("output_log_") && $0.pathExtension == "txt" }
                    .sorted { $0.lastPathComponent > $1.lastPathComponent }.prefix(1)
            }
            let eac = profile.appendingPathComponent("AppData/Roaming/EasyAntiCheat")
            if let entries = files.enumerator(at: eac, includingPropertiesForKeys: nil, options: [.skipsHiddenFiles]) {
                for case let url as URL in entries where url.lastPathComponent == "anticheatlauncher.log" {
                    logs.append(url)
                }
            }
        }
        var included = 0
        for url in logs where included < 16 {
            guard let handle = try? FileHandle(forReadingFrom: url) else { continue }
            defer { try? handle.close() }
            guard let size = try? handle.seekToEnd(), size > 0 else { continue }
            do {
                try handle.seek(toOffset: size - min(size, 65_536))
                guard let data = try handle.read(upToCount: 65_536) else { continue }
                let modified = (try? url.resourceValues(forKeys: [.contentModificationDateKey]))?.contentModificationDate
                report.append("\n--- \(url.lastPathComponent) · modified \(modified.map { String(describing: $0) } ?? "unknown") ---\n"
                              + String(decoding: data, as: UTF8.self).replacingOccurrences(of: home.path, with: "~"))
                included += 1
            } catch { continue }
        }
        if included == 0 { report.append("No launch logs found. Export again after reproducing the issue.") }
        return report.joined(separator: "\n")
    }

    func installedVRGames() -> [SteamGame] {
        let games = installedGames()
        guard !games.isEmpty,
              let data = try? Data(contentsOf: steam.appendingPathComponent("appcache/appinfo.vdf"))
        else { return [] }
        let vrIDs = SteamVRMetadata.appIDs(in: data, installed: Set(games.map(\.id)))
        return games.filter { vrIDs.contains($0.id) }
    }

    func installedGames() -> [SteamGame] {
        let files = FileManager.default
        var games: [UInt32: SteamGame] = [:]
        for library in steamLibraries() {
            let steamapps = library.appendingPathComponent("steamapps")
            guard let entries = try? files.contentsOfDirectory(at: steamapps, includingPropertiesForKeys: nil) else {
                continue
            }
            for manifest in entries where manifest.lastPathComponent.hasPrefix("appmanifest_") && manifest.pathExtension == "acf" {
                let idText = manifest.deletingPathExtension().lastPathComponent.dropFirst("appmanifest_".count)
                guard let id = UInt32(idText), id > 0, id != 228980, games[id] == nil,
                      let text = try? String(contentsOf: manifest, encoding: .utf8),
                      Self.value("appid", in: text).flatMap(UInt32.init) == id,
                      let name = Self.value("name", in: text), !name.isEmpty,
                      let state = Self.value("StateFlags", in: text).flatMap(UInt32.init), state & 4 != 0,
                      let folder = Self.value("installdir", in: text),
                      !folder.isEmpty, folder != ".", folder != "..", !folder.contains("/"), !folder.contains("\\")
                else { continue }
                let directory = steamapps.appendingPathComponent("common").appendingPathComponent(folder)
                var isDirectory: ObjCBool = false
                guard files.fileExists(atPath: directory.path, isDirectory: &isDirectory), isDirectory.boolValue else {
                    continue
                }
                games[id] = SteamGame(id: id, name: name)
            }
        }
        return games.values.sorted {
            let order = $0.name.localizedStandardCompare($1.name)
            return order == .orderedSame ? $0.id < $1.id : order == .orderedAscending
        }
    }
}

enum SetupCore {
    static func devices(from text:String)->[Device] {
        text.split(separator:"\n").compactMap{line in
            let fields=line.split(whereSeparator:{$0==" " || $0=="\t"})
            guard fields.count>=2,["device","unauthorized","offline"].contains(String(fields[1])) else{return nil}
            return Device(serial:String(fields[0]),state:String(fields[1]))
        }
    }
    static func selectedDevice(_ devices:[Device],serial:String?)throws->Device {
        let matches=devices.filter{$0.state=="device" && (serial==nil || $0.serial==serial)}
        guard matches.count==1 else{throw SetupIssue(message:matches.isEmpty ? "Connect and unlock your Quest, then allow USB debugging inside it." : "Choose the Quest you want to set up.")}
        return matches[0]
    }
    static func verifyPayload(_ root:URL,manifest:PayloadManifest)throws {
        guard manifest.schema==1 else{throw SetupIssue(message:"This app uses an unsupported component manifest.")}
        var seen=Set<String>()
        for entry in manifest.files {
            let relative=entry.path
            guard !relative.hasPrefix("/"),!relative.split(separator:"/").contains(".."),seen.insert(relative).inserted else{throw SetupIssue(message:"A bundled component path is invalid.")}
            let file=root.appendingPathComponent(relative).standardizedFileURL
            let resolved=file.resolvingSymlinksInPath()
            let base=root.resolvingSymlinksInPath().path+"/"
            guard resolved.path.hasPrefix(base) else{throw SetupIssue(message:"A bundled component points outside this app.")}
            let handle=try FileHandle(forReadingFrom:file);defer{try? handle.close()}
            var hash=SHA256()
            while let chunk=try handle.read(upToCount:1048576),!chunk.isEmpty {hash.update(data:chunk)}
            let actual=hash.finalize().map{String(format:"%02x",$0)}.joined()
            guard actual==entry.sha256 else{throw SetupIssue(message:"A bundled component has changed. Download a fresh copy of VRonMac.")}
        }
    }
    static func installHeadset(payload:URL,serial:String)throws {
        let apk=payload.appendingPathComponent("VRConMac-Quest.apk")
        let adb=payload.appendingPathComponent("platform-tools/adb")
        let model=try Command.run(adb,["-s",serial,"shell","getprop","ro.product.model"])
        guard model.code==0 && model.output.lowercased().contains("quest") else{throw SetupIssue(message:"The selected device is not a supported Meta Quest.")}
        let result=try Command.run(adb,["-s",serial,"install","-r",apk.path],timeout:120)
        guard result.code==0 && result.output.contains("Success") else{throw SetupIssue(message:"The Quest app could not be installed. Unlock the headset and reconnect USB before retrying.")}
    }
}
