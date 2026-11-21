// SPDX-License-Identifier: MIT
import SwiftUI
import AppKit
import Darwin
import CoreGraphics
import IOKit.hid

@MainActor final class LauncherModel: ObservableObject {
    @Published private(set) var screenCaptureAllowed = false
    @Published private(set) var inputMonitoringAllowed = false

    func refreshPermissions() {
        screenCaptureAllowed = CGPreflightScreenCaptureAccess()
        inputMonitoringAllowed = IOHIDCheckAccess(kIOHIDRequestTypeListenEvent) == kIOHIDAccessTypeGranted
    }

    func requestScreenCapture() {
        _ = CGRequestScreenCaptureAccess()
        refreshPermissions()
    }

    func requestInputMonitoring() {
        _ = IOHIDRequestAccess(kIOHIDRequestTypeListenEvent)
        refreshPermissions()
    }

    @Published var checks: [SetupCheck] = []
    @Published var devices: [Device] = []
    @Published var games: [SteamGame] = []
    @Published private(set) var preparedGames: Set<UInt32> = []
    @Published var selectedSerial = ""
    @Published var selectedTab = "session"
    @Published var busy = false
    @Published var confirmSystemUninstall = false
    @Published var running = false
    @Published var tracking = false
    @Published var faceRunning = false
    @Published var message = "Checking this Mac…"
    @Published var trackerStatus: TrackerStatus?
    @Published private(set) var discoveredTrackers: Set<String> = []
    @Published var sessionDirectory: URL?
    @Published var needsRestore = false
    @Published var video = VideoSettings(text: "")

    let audio = AudioControl()
    let environment = SetupEnvironment(home: FileManager.default.homeDirectoryForCurrentUser)
    let payload = Bundle.main.resourceURL!.appendingPathComponent("payload")
    private lazy var verification: GameVerification = {
        let url = payload.appendingPathComponent("verified-games.json")
        guard let data = try? Data(contentsOf: url),
              let catalog = try? JSONDecoder().decode(GameVerification.self, from: data) else {
            return GameVerification(verifiedAppIDs: [])
        }
        return catalog
    }()

    func badge(for game: SteamGame) -> String { verification.badge(for: game.id) }

    func refreshGames() async {
        let environment = environment
        let result = await Task.detached {
            let games = environment.installedVRGames()
            return (games, Set(games.filter { environment.gameReady($0.id) }.map(\.id)))
        }.value
        games = result.0
        preparedGames = result.1
    }

    private var bridge: Process?
    private var tracker: Process?
    private var face: NSRunningApplication?
    private var faceOwned = false
    private var monitor: Task<Void, Never>?
    private var sessionLock: Int32 = -1
    private var graphicsConfig: URL {
        environment.home.appendingPathComponent("Library/Application Support/OXRSys/oxrsys-runtime.toml")
    }
    private var trackerRuntime: TrackerRuntime {
        TrackerRuntime(target: environment.prefix.appendingPathComponent("drive_c/vrconmac/vrconmac_xr.dll"),
                       payload: payload)
    }
    var ready: Bool {
        ["steam", "notproton", "runtime", "integration"].allSatisfy(isReady)
    }

    func isReady(_ id: String) -> Bool { checks.first(where: { $0.id == id })?.ready == true }

    func provision(_ id: String) async {
        guard !busy && !running else { return }
        busy = true
        defer {
            checks = environment.checks()
            busy = false
        }
        do {
            try lockSession()
            defer { releaseLock() }
            let installer = SetupInstaller(environment: environment, payload: payload)
            let updateName = id == "integration" && isReady(id) && environment.mediaToolInstalled && environment.steamDisplayNameNeedsUpdate
            message = id == "runtime" ? "Downloading and installing Windows Support…" : "Preparing Steam compatibility…"
            try await Task.detached {
                try Self.verify(installer.payload)
                if id == "runtime" { try installer.installWindowsSupport() }
                else if updateName { try installer.updateSteamDisplayName() }
                else { try installer.installSteamCompatibility() }
            }.value
            message = id == "runtime" ? "Windows Support installed." :
                "Steam compatibility installed. Restart Steam, then select VRonMac Linux engine in the game’s Compatibility settings."
        } catch { message = error.localizedDescription }
    }

    nonisolated static func verify(_ payload: URL) throws {
        let manifest = try JSONDecoder().decode(PayloadManifest.self,
            from: Data(contentsOf: payload.appendingPathComponent("manifest.json")))
        try SetupCore.verifyPayload(payload, manifest: manifest)
    }

    func refresh() async {
        guard !busy else { return }
        busy = true
        defer { busy = false }
        let environment = environment
        await refreshGames()
        let adb = payload.appendingPathComponent("platform-tools/adb")
        let result = await Task.detached {
            let output = (try? Command.run(adb, ["devices", "-l"], timeout: 12).output) ?? ""
            return (environment.checks(), SetupCore.devices(from: output))
        }.value
        checks = result.0
        devices = result.1
        if !devices.contains(where: { $0.serial == selectedSerial }) {
            selectedSerial = devices.count == 1 ? devices[0].serial : ""
        }
        if !running {
            needsRestore = trackerRuntime.isPreview()
            video = VideoSettings(text: (try? String(contentsOf: graphicsConfig, encoding: .utf8)) ?? "")
            message = ready ? "Steam and Windows support are ready. Connect the headset to the same Wi-Fi as this Mac."
                            : "Follow the steps in Setup to get ready for PCVR."
        }
    }

    func scanTrackers() async {
        guard !busy && !tracking else { return }
        busy = true
        defer { busy = false }
        do {
            let needsLock = !running
            if needsLock { try lockSession() }
            defer { if needsLock { releaseLock() } }
            message = "Looking for powered-on trackers…"
            let payload = payload
            let serials = try await Task.detached {
                try Self.verify(payload)
                let directory = FileManager.default.temporaryDirectory
                    .appendingPathComponent("VRonMac-TrackerScan-" + UUID().uuidString)
                try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: false)
                defer { try? FileManager.default.removeItem(at: directory) }
                let result = try Command.run(payload.appendingPathComponent("tracker/service"),
                    ["--discover", directory.appendingPathComponent("lighthouse.config").path], timeout: 20)
                guard result.code == 0 else {
                    throw SetupIssue(message: "Tracker scan could not finish. Close other tracker apps and try again.")
                }
                return TrackerConfiguration.discoveredSerials(in: result.output)
            }.value
            discoveredTrackers.formUnion(serials)
            message = serials.isEmpty ? "No trackers found. Check that the receivers are connected and the trackers are on."
                : "Found \(serials.count) tracker(s) in this scan."
        } catch { message = error.localizedDescription }
    }

    func prepareHeadset() async {
        guard !busy && !running else { return }
        busy = true
        defer { busy = false }
        do {
            let device = try SetupCore.selectedDevice(devices, serial: selectedSerial.isEmpty ? nil : selectedSerial)
            let payload = payload
            message = "Installing the Quest client over USB…"
            try await Task.detached {
                try Self.verify(payload)
                try SetupCore.installHeadset(payload: payload, serial: device.serial)
            }.value
            message = "Quest client installed. Open it on your headset and allow the permissions you want to use."
        } catch { message = error.localizedDescription }
    }

    func refreshMicrophoneCheck() {
        guard let index = checks.firstIndex(where: { $0.id == "microphone" }) else { return }
        let current = environment.microphoneCheck
        if checks[index].ready != current.ready {
            checks[index] = current
            if !current.ready {
                message = "Virtual microphone removed. Restart your Mac before testing a fresh install."
            }
        }
    }

    func openMicrophoneInstaller(removing: Bool) {
        guard !busy && !running else { return }
        do {
            try Self.verify(payload)
            let name = removing ? "Remove VRConMac Microphone.pkg" : "VRConMac Microphone.pkg"
            guard NSWorkspace.shared.open(payload.appendingPathComponent(name)) else {
                throw SetupIssue(message: "Could not open the virtual microphone package.")
            }
            message = removing ? "Follow the prompts in Installer to remove the virtual microphone."
                : "Follow the prompts in Installer to install the virtual microphone."
        } catch { message = error.localizedDescription }
    }

    func saveVideo() {
        guard !running && !busy else { return }
        do {
            let current = FileManager.default.fileExists(atPath: graphicsConfig.path)
                ? try String(contentsOf: graphicsConfig, encoding: .utf8) : ""
            let updated = try video.applied(to: current)
            try FileManager.default.createDirectory(at: graphicsConfig.deletingLastPathComponent(),
                                                    withIntermediateDirectories: true)
            try updated.write(to: graphicsConfig, atomically: true, encoding: .utf8)
            message = "Saved OXRSys video settings for the next connection."
        } catch { message = error.localizedDescription }
    }

    private func lockSession() throws {
        try FileManager.default.createDirectory(at: environment.support, withIntermediateDirectories: true)
        let fd = open(environment.support.appendingPathComponent("session.lock").path, O_CREAT | O_RDWR, 0o600)
        guard fd >= 0 else { throw SetupIssue(message: "Could not open the session lock.") }
        guard flock(fd, LOCK_EX | LOCK_NB) == 0 else {
            close(fd)
            throw SetupIssue(message: "Another VRonMac session is already running.")
        }
        sessionLock = fd
        _ = fcntl(fd, F_SETFD, FD_CLOEXEC)
    }

    private func releaseLock() {
        if sessionLock >= 0 { close(sessionLock); sessionLock = -1 }
    }

    private func launch(_ relative: String, arguments: [String], log: URL,
                        variables: [String: String] = [:]) throws -> Process {
        _ = FileManager.default.createFile(atPath: log.path, contents: nil)
        let handle = try FileHandle(forWritingTo: log)
        defer { try? handle.close() }
        let process = Process()
        process.executableURL = payload.appendingPathComponent(relative)
        process.arguments = arguments
        process.environment = ProcessInfo.processInfo.environment.merging(variables) { _, override in override }
        process.standardOutput = handle
        process.standardError = handle
        try process.run()
        return process
    }

    private func tail(_ file: URL) -> String {
        guard let handle = try? FileHandle(forReadingFrom: file) else { return "" }
        defer { try? handle.close() }
        guard let size = try? handle.seekToEnd() else { return "" }
        _ = try? handle.seek(toOffset: size > 65_536 ? size - 65_536 : 0)
        return String(decoding: (try? handle.readToEnd()) ?? Data(), as: UTF8.self)
    }

    private func startBridge(in directory: URL, renderScale: Double, source: String, logName: String) async throws {
        let log = directory.appendingPathComponent(logName)
        let process = try launch("bridge/vrconmac-bridge",
            arguments: ["--backend", "openxr", "--render-scale", String(renderScale), "--test-pattern",
                        "--runtime-json", payload.appendingPathComponent("runtime/oxrsys-runtime.json").path],
            log: log, variables: ["VRCONMAC_CALIBRATION_HEAD_FEED": "1", "VRONMAC_CALIBRATION_SOURCE": source,
                                   "VRONMAC_CALIBRATION_SOURCE_FILE": directory.appendingPathComponent("calibration-source").path,
                                   "VRCONMAC_LISTEN_ANY": "0"])
        bridge = process
        for _ in 0..<100 {
            if !process.isRunning || tail(log).contains("[bridge] listening on") { break }
            try await Task.sleep(for: .milliseconds(100))
        }
        guard process.isRunning, tail(log).contains("[bridge] listening on") else {
            throw SetupIssue(message: "The bridge could not start. Open the session logs for details.")
        }
    }

    func reconnectHeadset(renderScale: Double, source: String) async {
        guard running && !busy, let directory = sessionDirectory else { return }
        busy = true
        defer { busy = false }
        do {
            if bridge?.isRunning != true {
                try checkPort(47811, tcp: true)
                try checkPort(47900, tcp: false)
                try await startBridge(in: directory, renderScale: renderScale, source: source,
                                      logName: "bridge-reconnect-" + UUID().uuidString + ".log")
            }
            let adb = payload.appendingPathComponent("platform-tools/adb")
            let serial = selectedSerial
            let reopened = await Task.detached {
                guard let listing = try? Command.run(adb, ["devices", "-l"], timeout: 8),
                      let device = try? SetupCore.selectedDevice(SetupCore.devices(from: listing.output),
                                                                 serial: serial.isEmpty ? nil : serial)
                else { return false }
                let args = ["-s", device.serial, "shell", "am"]
                guard let result = try? Command.run(adb, args + ["start", "-S", "-n",
                    "net.vrconmac.quest/com.oculus.NativeActivity"], timeout: 8) else { return false }
                return result.code == 0 && !result.output.contains("Error:")
            }.value
            await audio.stop()
            await enableSessionAudio()
            message = reopened ? "Quest client restarted. Wait for the game view to return."
                : "Bridge is ready. Quit and reopen VRonMac in the headset to reconnect to the running game."
        } catch {
            await terminate(bridge)
            bridge = nil
            message = error.localizedDescription
        }
    }

    func startConnection(renderScale: Double, source: String) async {
        guard !busy && !running else { return }
        busy = true
        defer { busy = false }
        do {
            guard !NSWorkspace.shared.runningApplications.contains(where: {
                $0.bundleIdentifier == "net.vrconmac.audio" ||
                ($0.bundleURL?.lastPathComponent.hasPrefix("VRConMac Audio") ?? false)
            }) else {
                throw SetupIssue(message: "Quit the separate VRConMac Audio app first.")
            }
            try lockSession()
            try checkPort(47811, tcp: true)
            try checkPort(47900, tcp: false)
            let payload = payload
            try await Task.detached { try Self.verify(payload) }.value
            try trackerRuntime.upgradeDiagnosticsIfNeeded()
            let directory = environment.support.appendingPathComponent("Sessions/" + UUID().uuidString)
            try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
            sessionDirectory = directory
            try await startBridge(in: directory, renderScale: renderScale, source: source, logName: "bridge.log")
            running = true
            if let device = try? SetupCore.selectedDevice(devices, serial: selectedSerial.isEmpty ? nil : selectedSerial) {
                _ = try? await Task.detached {
                    try Command.run(payload.appendingPathComponent("platform-tools/adb"),
                                    ["-s", device.serial, "shell", "am", "start", "-n",
                                     "net.vrconmac.quest/com.oculus.NativeActivity"], timeout: 5)
                }.value
            }
            message = "Bridge ready. Open the Quest client if it isn’t already running."
            await enableSessionAudio()
            startMonitoring()
        } catch {
            await terminate(bridge)
            bridge = nil
            running = false
            releaseLock()
            message = error.localizedDescription
        }
    }

    func startAudio() async {
        guard running && !busy else { return }
        busy = true
        defer { busy = false }
        await enableSessionAudio()
    }

    private func enableSessionAudio() async {
        if !audio.running { await audio.start() }
        if audio.running && audio.engine == nil { audio.enableMic() }
        if !audio.running || audio.engine == nil { message = audio.message }
    }

    func toggleFace() async {
        guard !busy, face?.isTerminated != false else { return }
        busy = true
        defer { busy = false }
        do {
            let payload = payload
            try await Task.detached { try Self.verify(payload) }.value
            let url = payload.appendingPathComponent("VRCFaceTracking.app")
            let identifier = Bundle(url: url)?.bundleIdentifier
            let existing = identifier.map { NSRunningApplication.runningApplications(withBundleIdentifier: $0) } ?? []
            if let app = existing.first {
                face = app
                faceOwned = false
            } else {
                let config = NSWorkspace.OpenConfiguration()
                config.activates = false
                face = try await NSWorkspace.shared.openApplication(at: url, configuration: config)
                faceOwned = true
            }
            faceRunning = true
            if monitor == nil { startMonitoring() }
        } catch { message = error.localizedDescription }
    }

    private func requireGameClosed() throws {
        let result = try Command.run(URL(fileURLWithPath: "/usr/sbin/lsof"),
            ["-nP", "-iTCP:47811", "-sTCP:ESTABLISHED", "-t"], timeout: 3)
        guard result.code == 1 && result.output.isEmpty else {
            throw SetupIssue(message: "Exit the VR game in Steam before changing the session.")
        }
    }

    func startTrackers(reference: String, manual: Bool, source: String, bodies: [String], applying: Bool = false) async {
        guard running && !busy, tracking == applying, let directory = sessionDirectory else { return }
        busy = true
        defer { busy = false }
        var switched = false
        do {
            let config = try TrackerConfiguration(reference: reference, manual: manual, bodies: bodies)
            if applying {
                let previous = tracker
                tracker = nil
                tracking = false
                trackerStatus = nil
                trackerStateDirectory = nil
                await terminate(previous)
            } else {
                try requireGameClosed()
            }
            try checkPort(47921, tcp: false)
            let trackerDirectory = directory.appendingPathComponent("Tracking-" + UUID().uuidString)
            try FileManager.default.createDirectory(at: trackerDirectory, withIntermediateDirectories: true)
            if !applying {
                try trackerRuntime.select(preview: true)
                switched = true
                needsRestore = true
            }
            try (manual ? source : "headset").write(to: directory.appendingPathComponent("calibration-source"),
                                                    atomically: true, encoding: .utf8)
            try? await Task.sleep(for: .milliseconds(150))
            tracker = try launch("tracker/service", arguments: [trackerDirectory.path, config.reference,
                config.manual ? "manual" : "continuous"] + config.bodies,
                log: trackerDirectory.appendingPathComponent("trackers.log"))
            trackerStateDirectory = trackerDirectory
            tracking = true
            message = manual ? "Hold the selected tracker and reference together, then press Calibrate."
                             : "With the reference mounted, slowly turn and tilt your head."
        } catch {
            message = error.localizedDescription
            if switched {
                do { try trackerRuntime.select(preview: false); needsRestore = false }
                catch { message += " Restore failed: " + error.localizedDescription }
            }
        }
    }

    private var trackerStateDirectory: URL?

    func calibrate() {
        guard tracking, let directory = trackerStateDirectory else { return }
        do {
            try Data().write(to: directory.appendingPathComponent("retry"), options: .atomic)
            message = "Keep the tracker fixed against the reference. Make slow figure eights and rotate across several axes."
        } catch { message = error.localizedDescription }
    }

    func prepareGame(_ game: SteamGame) async {
        guard ready && !running && !busy && games.contains(game) else { return }
        busy = true
        defer { busy = false }
        do {
            try lockSession()
            defer { releaseLock() }
            try requireGameClosed()
            let installer = SetupInstaller(environment: environment, payload: payload)
            message = "Preparing the Windows environment for \(game.name)…"
            try await Task.detached {
                try Self.verify(installer.payload)
                try installer.prepareGame(game.id)
            }.value
            preparedGames.insert(game.id)
            message = "\(game.name) is prepared. In Steam, select VRonMac Linux engine for this game."
        } catch { message = error.localizedDescription }
    }

    func exportLaunchDiagnostics(_ game: SteamGame) async {
        guard !busy else { return }
        let panel = NSSavePanel()
        panel.title = "Export Launch Diagnostics"
        panel.directoryURL = URL(fileURLWithPath: "/Users/Shared", isDirectory: true)
        panel.nameFieldStringValue = "VRonMac-Launch-\(game.id).txt"
        panel.message = "Save a report with recent Steam, VR runtime, and game logs. Review it before sharing."
        guard await panel.begin() == .OK, let destination = panel.url else { return }
        let environment = environment
        do {
            try await Task.detached {
                let report = environment.launchDiagnostics(appID: game.id, temporaryDirectory: FileManager.default.temporaryDirectory)
                try report.write(to: destination, atomically: true, encoding: .utf8)
            }.value
            NSWorkspace.shared.activateFileViewerSelecting([destination])
            message = "Launch diagnostics exported."
        } catch { message = error.localizedDescription }
    }

    func launchGame(_ game: SteamGame) {
        guard running, !busy, games.contains(game) else { return }
        if tracking && trackerStatus?.phase != "Ready" {
            message = "Finish tracker calibration or stop the tracker service before launching."
            return
        }
        let id = game.id
        guard ready else {
            message = "Finish the Steam and Windows Support steps in Setup before launching a game."
            return
        }
        guard environment.gameReady(id) else {
            message = "Prepare this game’s Windows environment from the Session tab before launching."
            return
        }
        do {
            let target = environment.gamePrefix(id).appendingPathComponent("drive_c/vrconmac/vrconmac_xr.dll")
            try TrackerRuntime(target: target, payload: payload).upgradeDiagnosticsIfNeeded()
        } catch {
            message = error.localizedDescription
            return
        }
        if !NSWorkspace.shared.open(URL(string: "steam://rungameid/\(id)")!) {
            message = "Steam could not open that game."
        }
    }

    private func startMonitoring() {
        monitor?.cancel()
        monitor = Task { [weak self] in
            while !Task.isCancelled, let self {
                if let directory = trackerStateDirectory,
                   let data = try? Data(contentsOf: directory.appendingPathComponent("status.json")) {
                    trackerStatus = try? JSONDecoder().decode(TrackerStatus.self, from: data)
                    discoveredTrackers.formUnion(trackerStatus?.discoveredSerials ?? [])
                }
                if tracking, tracker?.isRunning == false {
                    tracking = false
                    trackerStatus = nil
                    message = "The tracker service exited. See the session logs."
                }
                if face?.isTerminated == true {
                    face = nil
                    faceRunning = false
                    faceOwned = false
                    message = "VRCFaceTracking closed. Check its Local Network permission, then start it again."
                }
                if running && bridge?.isRunning == false {
                    message = "The bridge exited. Use Reconnect Headset to recover the current session."
                }
                try? await Task.sleep(for: .milliseconds(500))
            }
        }
    }

    private func terminate(_ process: Process?) async {
        guard let process, process.isRunning else { return }
        process.terminate()
        for _ in 0..<30 {
            if !process.isRunning { return }
            try? await Task.sleep(for: .milliseconds(100))
        }
        if process.isRunning { _ = Darwin.kill(process.processIdentifier, SIGKILL) }
        process.waitUntilExit()
    }

    func stopTrackers() async {
        guard !busy else { return }
        busy = true
        defer { busy = false }
        do {
            try requireGameClosed()
            await terminate(tracker)
            tracker = nil
            tracking = false
            trackerStatus = nil
            trackerStateDirectory = nil
            if needsRestore {
                try trackerRuntime.select(preview: false)
                needsRestore = false
            }
        } catch { message = error.localizedDescription }
    }

    func restoreRuntime() {
        guard !busy && !running else { return }
        do {
            try lockSession()
            defer { releaseLock() }
            try checkPort(47811, tcp: true)
            try requireGameClosed()
            try trackerRuntime.select(preview: false)
            needsRestore = false
            message = "Restored the standard VR runtime."
        } catch { message = error.localizedDescription }
    }

    func stop() async -> Bool {
        guard !busy else { return false }
        if !running {
            monitor?.cancel()
            monitor = nil
            if faceOwned { face?.terminate() }
            face = nil
            faceRunning = false
            faceOwned = false
            return true
        }
        busy = true
        defer { busy = false }
        do { try requireGameClosed() }
        catch { message = error.localizedDescription; return false }
        monitor?.cancel()
        monitor = nil
        await audio.stop()
        await terminate(tracker)
        await terminate(bridge)
        tracker = nil
        bridge = nil
        trackerStatus = nil
        trackerStateDirectory = nil
        tracking = false
        if faceOwned { face?.terminate() }
        face = nil
        faceOwned = false
        faceRunning = false
        running = false
        defer { releaseLock() }
        do {
            if needsRestore { try trackerRuntime.select(preview: false); needsRestore = false }
            message = "Session stopped."
            return true
        } catch { message = error.localizedDescription; return false }
    }
}

final class LauncherDelegate: NSObject, NSApplicationDelegate {
    static weak var model: LauncherModel?
    func applicationShouldTerminate(_ sender: NSApplication) -> NSApplication.TerminateReply {
        Task { @MainActor in
            let stopped = await Self.model?.stop() ?? true
            sender.reply(toApplicationShouldTerminate: stopped)
        }
        return .terminateLater
    }
}

struct LauncherView: View {
    @ObservedObject var model: LauncherModel
    @AppStorage("renderScale") var scale = 0.85
    @AppStorage("gameAppID") var appID = ""
    @AppStorage("mountedReference") var mounted = true
    @AppStorage("referenceSerial") var reference = ""
    @AppStorage("hipSerial") var hip = ""
    @AppStorage("leftFootSerial") var leftFoot = ""
    @AppStorage("rightFootSerial") var rightFoot = ""
    @AppStorage("manualReference") var manualSource = "left"

    var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            HStack {
                Image(systemName: "visionpro").font(.system(size: 34))
                VStack(alignment: .leading) {
                    Text("VRonMac").font(.largeTitle.bold())
                    Text("PCVR on Apple Silicon").foregroundStyle(.secondary)
                }
                Spacer()
                Text("Local Preview").font(.caption).padding(8).background(.orange.opacity(0.15), in: Capsule())
            }
            Text(model.message).frame(maxWidth: .infinity, alignment: .leading)
                .fixedSize(horizontal: false, vertical: true)
            if model.busy { ProgressView().controlSize(.small) }
            TabView(selection: $model.selectedTab) {
                session.tabItem { Label("Session", systemImage: "play.circle") }.tag("session")
                setup.tabItem { Label("Setup", systemImage: "checklist") }.tag("setup")
                video.tabItem { Label("Video", systemImage: "display") }.tag("video")
                tracking.tabItem { Label("Trackers", systemImage: "sensor") }.tag("trackers")
            }
        }
        .padding(22)
        .frame(width: 650, height: 530)
        .task {
            LauncherDelegate.model = model
            if model.checks.isEmpty {
                await model.refresh()
                if !model.ready { model.selectedTab = "setup" }
            }
        }
        .onChange(of: model.games) { _, games in
            if !model.tracking && !games.contains(where: { String($0.id) == appID }) { appID = "" }
        }
    }

    private var selectedGame: SteamGame? {
        model.games.first { String($0.id) == appID }
    }

    private var session: some View {
        Form {
            Section("Headset") {
                Button(model.running ? "Stop Headset Connection" : "Start Headset Connection") {
                    Task {
                        if model.running { _ = await model.stop() }
                        else { await model.startConnection(renderScale: scale, source: mounted ? "headset" : manualSource) }
                    }
                }.buttonStyle(.borderedProminent).disabled(model.busy)
                if model.running {
                    Button("Reconnect Headset") {
                        Task { await model.reconnectHeadset(renderScale: scale, source: mounted ? "headset" : manualSource) }
                    }.disabled(model.busy)
                }
                AudioStatusView(audio: model.audio, connected: model.running) { Task { await model.startAudio() } }
                    .disabled(model.busy)
            }
            Section("Tracking") {
                Button(model.faceRunning ? "VRCFaceTracking Running" : "Start VRCFaceTracking") {
                    Task { await model.toggleFace() }
                }.disabled(model.busy || model.faceRunning)
                Button(model.tracking ? "Stop VR Tracker Service" : "Start VR Tracker Service") {
                    Task {
                        if model.tracking { await model.stopTrackers() }
                        else { await model.startTrackers(reference: reference, manual: !mounted, source: manualSource, bodies: [hip, leftFoot, rightFoot]) }
                    }
                }.disabled(model.busy || !model.running || appID != "438100" || reference.isEmpty)
                if let status = model.trackerStatus {
                    Text("\(status.phase) · \(status.bodyTracked)/\(status.bodyTarget ?? 3) body trackers")
                    if status.phase == "Collecting calibration" {
                        ProgressView(value: Double(status.samples), total: Double(max(1, status.target)))
                    }
                    if !status.lastError.isEmpty { Text(status.lastError).font(.caption) }
                }
                if model.tracking { Button("Calibrate") { model.calibrate() }.disabled(model.busy) }
            }
            Section("Steam") {
                if model.games.isEmpty {
                    Text("No installed Steam games with VR support found. Open Steam and refresh the list.").foregroundStyle(.secondary)
                } else {
                    Picker("Installed VR game", selection: $appID) {
                        Text("Choose a game").tag("")
                        ForEach(model.games) { game in Text(game.name).tag(String(game.id)) }
                    }.disabled(model.tracking)
                }
                if let game = selectedGame {
                    LabeledContent("Compatibility", value: model.badge(for: game))
                    if !model.preparedGames.contains(game.id) {
                        Button("Prepare " + game.name) { Task { await model.prepareGame(game) } }
                            .disabled(!model.ready || model.running || model.busy)
                    }
                    Button("Launch " + game.name) { model.launchGame(game) }
                        .disabled(!model.ready || !model.preparedGames.contains(game.id) || !model.running ||
                                  model.busy || (model.tracking && model.trackerStatus?.phase != "Ready"))
                    if model.tracking && model.trackerStatus?.phase != "Ready" {
                        Text("Finish tracker calibration, or stop the tracker service to launch with headset tracking only.")
                            .font(.caption).foregroundStyle(.secondary)
                    }
                    Button("Export Launch Diagnostics…") { Task { await model.exportLaunchDiagnostics(game) } }
                        .disabled(model.busy)
                }
                Button("Refresh Games") { Task { await model.refreshGames() } }.disabled(model.busy || model.tracking)
                Text("In Steam, choose VRonMac Linux engine under the game’s Properties → Compatibility. Select the VR launch option if Steam asks.").font(.caption)
                if let directory = model.sessionDirectory {
                    Button("Open Session Logs") { NSWorkspace.shared.open(directory) }
                }
            }
        }.formStyle(.grouped)
    }

    private var setup: some View {
        Form {
            Section("PCVR setup") {
                Text("Work through these steps in order. The virtual microphone is optional.")
                    .font(.caption).foregroundStyle(.secondary)
                ForEach(model.checks) { check in
                    LabeledContent {
                        HStack(spacing: 10) {
                            setupAction(check)
                            Image(systemName: check.ready ? "checkmark.circle.fill" : "circle")
                                .foregroundStyle(check.ready ? Color.green : Color.secondary)
                                .accessibilityLabel(check.ready ? "Ready" : "Pending")
                        }
                    } label: {
                        VStack(alignment: .leading) {
                            Text(check.title)
                            Text(check.detail).font(.caption).foregroundStyle(.secondary)
                        }
                    }
                }
                Button("Check Again") { Task { await model.refresh() } }
                if model.needsRestore && !model.running {
                    Button("Restore Standard VR Runtime") { model.restoreRuntime() }
                }
            }
            Section("Permissions") {
                LabeledContent {
                    Button(model.screenCaptureAllowed ? "Allowed" : "Request Access") {
                        model.requestScreenCapture()
                    }.disabled(model.screenCaptureAllowed)
                    privacyLink("ScreenCapture")
                } label: {
                    VStack(alignment: .leading) {
                        Text("Screen & System Audio Recording")
                        Text("Streams Mac game audio to the headset.").font(.caption).foregroundStyle(.secondary)
                    }
                }
                LabeledContent {
                    Button(model.inputMonitoringAllowed ? "Allowed" : "Request Access") {
                        model.requestInputMonitoring()
                    }.disabled(model.inputMonitoringAllowed)
                    privacyLink("ListenEvent")
                } label: {
                    VStack(alignment: .leading) {
                        Text("Input Monitoring")
                        Text("Optional for USB tracker discovery. macOS may also prompt for the tracker helper.")
                            .font(.caption).foregroundStyle(.secondary)
                    }
                }
                LabeledContent {
                    privacyLink("LocalNetwork")
                } label: {
                    VStack(alignment: .leading) {
                        Text("Local Network")
                        Text("Allow access when macOS prompts on the first headset connection.")
                            .font(.caption).foregroundStyle(.secondary)
                    }
                }
            }
            Section("System extensions") {
                if let microphone = model.checks.first(where: { $0.id == "microphone" }) {
                    if microphone.ready {
                        Button("Uninstall System Extensions") { model.confirmSystemUninstall = true }
                    }
                }
                Text("Manages the VRonMac virtual microphone for all users on this Mac. Administrator approval is required.")
                    .font(.caption)
            }.disabled(model.running)
            Section("Quest USB setup") {
                Picker("Device", selection: $model.selectedSerial) {
                    Text("Select a device").tag("")
                    ForEach(model.devices) { device in
                        Text("…\(device.serial.suffix(4)) · \(device.state)").tag(device.serial)
                    }
                }
                Button("Install Quest Client") { Task { await model.prepareHeadset() } }
                    .disabled(model.running || model.selectedSerial.isEmpty)
                Text("Use USB for setup. The headset streams over Wi-Fi.").font(.caption)
            }
        }.formStyle(.grouped).disabled(model.busy)
            .onAppear { model.refreshPermissions() }
            .onReceive(NotificationCenter.default.publisher(for: NSApplication.didBecomeActiveNotification)) { _ in
                model.refreshPermissions()
                if !model.busy { model.checks = model.environment.checks() }
            }
            .onReceive(Timer.publish(every: 1, on: .main, in: .common).autoconnect()) { _ in
                model.refreshMicrophoneCheck()
            }
            .alert("Uninstall System Extensions?", isPresented: $model.confirmSystemUninstall) {
                Button("Uninstall", role: .destructive) { model.openMicrophoneInstaller(removing: true) }
                Button("Cancel", role: .cancel) {}
            } message: {
                Text("This removes the VRonMac virtual microphone for all users. Steam, NotProton and your games stay installed. Quit any other apps using the driver first. Restart your Mac afterward to unload the driver.")
            }

    }

    private func privacyLink(_ pane: String) -> some View {
        Link("Settings", destination: URL(string: "x-apple.systempreferences:com.apple.preference.security?Privacy_\(pane)")!)
    }

    @ViewBuilder private func setupAction(_ check: SetupCheck) -> some View {
        switch check.id {
        case "steam":
            dependencyAction(check, name: "Steam", bundleID: "com.valvesoftware.steam",
                             download: URL(string: "https://store.steampowered.com/about/")!)
        case "notproton":
            dependencyAction(check, name: "NotProton", bundleID: "com.notproton.NotProton",
                             download: URL(string: "https://github.com/NotProtonNot/NotProton/releases")!)
        case "runtime":
            Button("Install") { Task { await model.provision(check.id) } }
                .disabled(check.ready || model.running)
        case "integration":
            let updateTool = check.ready && !model.environment.mediaToolInstalled
            let updateName = check.ready && model.environment.steamDisplayNameNeedsUpdate
            Button(updateTool ? "Update" : updateName ? "Update Name" : "Set Up") { Task { await model.provision(check.id) } }
                .disabled((check.ready && !updateTool && !updateName) || model.running || !model.isReady("steam") ||
                          !model.isReady("notproton") || !model.isReady("runtime"))
        case "microphone":
            Button("Install System Extensions") { model.openMicrophoneInstaller(removing: false) }
                .disabled(check.ready || model.running)
        default:
            EmptyView()
        }
    }

    private func dependencyAction(_ check: SetupCheck, name: String, bundleID: String, download: URL) -> some View {
        let installed = NSWorkspace.shared.urlForApplication(withBundleIdentifier: bundleID)
        return Button(installed == nil && !check.ready ? "Get " + name : "Open " + name) {
            NSWorkspace.shared.open(installed ?? download)
        }.disabled(check.ready)
    }

    private var video: some View {
        Form {
            Section("Game rendering") {
                Slider(value: $scale, in: 0.5...1.0, step: 0.05) {
                    Text("Render scale · \(Int((scale * 100).rounded()))%")
                }
                Text("Applies when the bridge starts.").font(.caption)
            }
            Section("Headset stream · HEVC") {
                Stepper("Bitrate · \(model.video.bitrate) Mbps", value: $model.video.bitrate, in: 1...200)
                Slider(value: $model.video.resolution, in: 0.25...1.0, step: 0.05) {
                    Text("Stream resolution · \(Int((model.video.resolution * 100).rounded()))%")
                }
                Picker("Foveated encoding", selection: $model.video.foveation) {
                    ForEach(["off", "light", "medium", "high"], id: \.self) { Text($0.capitalized) }
                }
                Picker("Adaptive bitrate", selection: $model.video.adaptation) {
                    Text("Off").tag("off")
                    Text("Bitrate only").tag("bitrate")
                    Text("Bitrate and quality").tag("full")
                }
                Picker("Encoder preset", selection: $model.video.preset) {
                    ForEach(["speed", "balanced", "quality"], id: \.self) { Text($0.capitalized) }
                }
                Button("Save OXRSys Video Settings") { model.saveVideo() }
                Text("Saves to the shared OXRSys configuration. Reconnect to apply.").font(.caption)
            }
        }.formStyle(.grouped).disabled(model.running || model.busy)
    }

    private var tracking: some View {
        Form {
            Section("Tracker discovery") {
                Button("Scan for Trackers") { Task { await model.scanTrackers() } }.disabled(model.tracking)
                if model.tracking {
                    Text("New trackers appear as the running service picks up their poses.").font(.caption)
                }
                Text("Plug in the USB receivers and turn on your trackers. To identify one, scan with only that tracker powered on. Assignments are saved automatically.")
                    .font(.caption)
            }
            Section("Alignment") {
                Toggle("Continuous alignment with a mounted tracker", isOn: $mounted)
                if !mounted {
                    Picker("Quest reference", selection: $manualSource) {
                        Text("Left controller").tag("left")
                        Text("Right controller").tag("right")
                        Text("Headset").tag("headset")
                    }
                }
                trackerPicker(mounted ? "Mounted tracker" : "Handheld tracker", selection: $reference,
                              excluding: mounted ? [hip, leftFoot, rightFoot] : [], emptyLabel: "Choose a tracker")
                Text(mounted ? "The mounted tracker is reserved for alignment."
                             : "Hold the tracker firmly against the chosen reference. Press Calibrate in Session and move in slow figure eights.")
                    .font(.caption)
            }
            Section("Body trackers") {
                trackerPicker("Hip", selection: $hip, excluding: [leftFoot, rightFoot, mounted ? reference : ""])
                trackerPicker("Left foot", selection: $leftFoot, excluding: [hip, rightFoot, mounted ? reference : ""])
                trackerPicker("Right foot", selection: $rightFoot, excluding: [hip, leftFoot, mounted ? reference : ""])
                Text("Leave all three unassigned to send body trackers automatically. Assign their roles in the game.")
                    .font(.caption)
            }
            if model.tracking {
                Button("Apply Tracker Changes") {
                    Task {
                        await model.startTrackers(reference: reference, manual: !mounted, source: manualSource,
                                                  bodies: [hip, leftFoot, rightFoot], applying: true)
                    }
                }.disabled(reference.isEmpty)
                Text("Applying changes restarts alignment while keeping the headset connected. Calibrate again after swapping trackers.")
                    .font(.caption)
            }
        }.formStyle(.grouped).disabled(model.busy)
    }
    private func trackerPicker(_ title: String, selection: Binding<String>,
                               excluding: [String], emptyLabel: String = "Unassigned") -> some View {
        let saved = Set([reference, hip, leftFoot, rightFoot].filter { !$0.isEmpty })
        let available = model.discoveredTrackers.union(saved).subtracting(excluding)
        let choices = available.union([selection.wrappedValue]).filter { !$0.isEmpty }.sorted()
        return Picker(title, selection: selection) {
            Text(emptyLabel).tag("")
            ForEach(choices, id: \.self) { serial in
                Text("Tracker \(serial.dropFirst(4))").tag(serial)
            }
        }
    }

}

struct AudioStatusView: View {
    @ObservedObject var audio: AudioControl
    let connected: Bool
    let retry: () -> Void
    var body: some View {
        if connected {
            if !audio.counts.isEmpty { Text(audio.counts).font(.caption) }
            if !audio.running || audio.engine == nil { Button("Start Audio and Microphone", action: retry) }
        }
    }
}

struct LauncherApp: App {
    @NSApplicationDelegateAdaptor(LauncherDelegate.self) var delegate
    @StateObject var model = LauncherModel()
    var body: some Scene {
        Window("VRonMac", id: "main") { LauncherView(model: model) }
            .windowResizability(.contentSize)
    }
}

@main struct LauncherEntry {
    @MainActor static func main() {
        if CommandLine.arguments.contains("--verify-payload") {
            do {
                try LauncherModel.verify(Bundle.main.resourceURL!.appendingPathComponent("payload"))
                print("PASS: bundled component hashes and path boundaries")
            } catch {
                FileHandle.standardError.write(Data((error.localizedDescription + "\n").utf8))
                exit(1)
            }
            return
        }
        if CommandLine.arguments.contains("--doctor") {
            let environment = SetupEnvironment(home: FileManager.default.homeDirectoryForCurrentUser)
            let encoder = JSONEncoder()
            encoder.outputFormatting = [.prettyPrinted, .sortedKeys]
            if let data = try? encoder.encode(environment.checks()) { FileHandle.standardOutput.write(data) }
            return
        }
        LauncherApp.main()
    }
}
