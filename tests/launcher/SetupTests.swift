import Foundation
import CryptoKit
@main struct Tests {
 static func require(_ condition:Bool,_ message:String){precondition(condition,message)}
 static func throwsError(_ body:()throws->Void){do{try body();fatalError("Expected an error")}catch{}}
 static func main()throws {
  let root=FileManager.default.temporaryDirectory.appendingPathComponent("vrconmac-setup-tests-"+UUID().uuidString)
  try FileManager.default.createDirectory(at:root,withIntermediateDirectories:true)
  defer{try? FileManager.default.removeItem(at:root)}
  let env=SetupEnvironment(home:root)
  let absent=env.checks();require(!absent.first{$0.id=="steam"}!.ready,"No Steam on fixture")
  let stream="List of devices attached\nQ1 unauthorized usb:1\nQ2 device product:seacliff\nQ3 offline\n* daemon started successfully\n"
  let devices=SetupCore.devices(from:stream);require(devices.count==3,"ADB states")
  require(try SetupCore.selectedDevice(devices,serial:nil).serial=="Q2","Authorized selection")
  throwsError{_ = try SetupCore.selectedDevice(devices,serial:"Q1")}
  throwsError{_ = try SetupCore.selectedDevice(devices+[Device(serial:"Q4",state:"device")],serial:nil)}
  let payload=root.appendingPathComponent("payload");try FileManager.default.createDirectory(at:payload,withIntermediateDirectories:true)
  let file=payload.appendingPathComponent("component");try Data("original".utf8).write(to:file)
  let hash=SHA256.hash(data:Data("original".utf8)).map{String(format:"%02x",$0)}.joined()
  let manifest=PayloadManifest(schema:1,revision:"test",files:[.init(path:"component",sha256:hash)])
  try SetupCore.verifyPayload(payload,manifest:manifest)
  try Data("tampered".utf8).write(to:file);throwsError{try SetupCore.verifyPayload(payload,manifest:manifest)}
  for path in ["../escape","/absolute"]{throwsError{try SetupCore.verifyPayload(payload,manifest:.init(schema:1,revision:"test",files:[.init(path:path,sha256:hash)]))}}
  try FileManager.default.createSymbolicLink(at:payload.appendingPathComponent("external"),withDestinationURL:root.appendingPathComponent("private"))
  throwsError{try SetupCore.verifyPayload(payload,manifest:.init(schema:1,revision:"test",files:[.init(path:"external",sha256:hash)]))}
  let result=try Command.run(URL(fileURLWithPath:"/bin/echo"),["literal ; $(not-a-command) with spaces"])
  require(result.code==0 && result.output.contains("$(not-a-command)"),"Arguments are literal")
  throwsError{_ = try Command.run(URL(fileURLWithPath:"/usr/bin/yes"),[],timeout:0.15)}
  let folders=[env.steamApp,env.compatibility.appendingPathComponent("steam-client"),env.runtime.appendingPathComponent("engine"),env.prefix.appendingPathComponent("drive_c/vrconmac"),env.steam.appendingPathComponent("steamapps/common/VRChat"),env.steamApp.appendingPathComponent("Contents/MacOS/package")]
  for folder in folders{try FileManager.default.createDirectory(at:folder,withIntermediateDirectories:true)}
  for file in [env.compatibility.appendingPathComponent("run"),env.runtime.appendingPathComponent("engine/fexdarwin-run"),env.prefix.appendingPathComponent("drive_c/vrconmac/vrconmac_xr.dll"),env.prefix.appendingPathComponent("system.reg"),env.steam.appendingPathComponent("steamapps/common/VRChat/VRChat.exe")] {try Data().write(to:file)}
  require(!env.notProtonReady,"VRonMac's own runner does not count as NotProton")
  let namedRunner = env.steam.appendingPathComponent("compatibilitytools.d/notproton-26.3/run")
  for executable in [namedRunner, env.compatibility.appendingPathComponent("run"),
                     env.runtime.appendingPathComponent("engine/fexdarwin-run")] {
      try FileManager.default.createDirectory(at: executable.deletingLastPathComponent(), withIntermediateDirectories: true)
      try Data().write(to: executable)
      try FileManager.default.setAttributes([.posixPermissions: 0o755], ofItemAtPath: executable.path)
  }
  try Data().write(to: namedRunner.deletingLastPathComponent().appendingPathComponent("compatibilitytool.vdf"))
  require(env.notProtonRunner == namedRunner,"Fresh CrossOver 26.3 runner is recognized without the legacy name")
  let sdk = env.compatibility.appendingPathComponent("steam-client/linux64/steamclient.so")
  try FileManager.default.createDirectory(at: sdk.deletingLastPathComponent(), withIntermediateDirectories: true)
  try Data().write(to: sdk)
  let host=env.steamApp.appendingPathComponent("Contents/MacOS/package/steam_client_signed-2_osx.manifest")
  try Data("\"version\" \"1792618285\"".utf8).write(to:host)
  try Data("1792618285\n".utf8).write(to:env.compatibility.appendingPathComponent("steam-client/version"))
  require(env.checks().filter{$0.id != "microphone"}.allSatisfy(\.ready),"Fixture installation recognized")
  try Data("\"version\" \"9999999999\"".utf8).write(to:host)
  require(!env.checks().first{$0.id=="integration"}!.ready,"Stale Linux client refused")
  try Data("9999999999\n".utf8).write(to:env.compatibility.appendingPathComponent("steam-client/version"))
  require(env.checks().first{$0.id=="integration"}!.ready,"A new matching build works without a launcher update")
  try Data().write(to:host)
  require(!env.checks().first{$0.id=="integration"}!.ready,"Unknown host build refused")
  let packageName = "bins_sdk_ubuntu12.zip." + String(repeating: "a", count: 40)
  let packageHash = String(repeating: "b", count: 64)
  let packageManifest = #""bins_sdk_ubuntu12" { "file" "\#(packageName)" "sha2" "\#(packageHash)" }"#
  let parsedPackage = try SetupInstaller.sdkPackage(in: packageManifest)
  require(parsedPackage.name == packageName && parsedPackage.sha256 == packageHash,"Valve SDK manifest parsed")
  throwsError { _ = try SetupInstaller.sdkPackage(in: packageManifest.replacingOccurrences(of: packageName, with: "../other.zip")) }
  require(!env.gameReady(438100),"An unregistered game is pending")
  try Data(#""ActiveRuntime"="C:\\vrconmac\\vrconmac_xr.json""#.utf8).write(to: env.prefix.appendingPathComponent("system.reg"))
  let graphics = env.runtime.appendingPathComponent("windows")
  let system32 = env.prefix.appendingPathComponent("drive_c/windows/system32")
  for directory in [graphics, system32] {
      try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
  }
  for name in SetupEnvironment.graphicsDLLs {
      try Data(("Voile " + name).utf8).write(to: graphics.appendingPathComponent(name))
  }
  try Data("Wine placeholder".utf8).write(to: system32.appendingPathComponent("d3d11.dll"))
  require(!env.gameReady(438100),"Registered prefix without Voile graphics needs repair")
  let openVR = env.prefix.appendingPathComponent("drive_c/users/crossover/AppData/Local/openvr")
  try FileManager.default.createDirectory(at: openVR, withIntermediateDirectories: true)
  let pathRegistry = openVR.appendingPathComponent("openvrpaths.vrpath")
  try Data(#"{"runtime":["C:\vrconmac\opencomposite"]}"#.utf8).write(to: pathRegistry)
  require(!env.openVRPathReady(pathRegistry),"Single Windows backslashes are invalid JSON")
  try SetupInstaller(environment: env, payload: payload).prepareGame(438100)
  require(env.openVRPathReady(pathRegistry),"Prepare repairs the OpenVR path registry")
  require(env.gameReady(438100) && !env.gameReady(1234),"Game readiness follows its own prefix")
  let otherLibrary = root.appendingPathComponent("External Steam Library")
  try FileManager.default.createDirectory(at:otherLibrary.appendingPathComponent("steamapps/common/Another Game"),withIntermediateDirectories:true)
  let primaryApps = env.steam.appendingPathComponent("steamapps")
  let externalApps = otherLibrary.appendingPathComponent("steamapps")
  try #""libraryfolders" { "1" { "path" "\#(otherLibrary.path)" } }"#.write(
      to:primaryApps.appendingPathComponent("libraryfolders.vdf"),atomically:true,encoding:.utf8)
  func addGame(_ id: UInt32, _ name: String, _ folder: String, _ apps: URL, flags: Int = 4) throws {
      let manifest = #""AppState" { "appid" "\#(id)" "name" "\#(name)" "StateFlags" "\#(flags)" "installdir" "\#(folder)" }"#
      try manifest.write(to:apps.appendingPathComponent("appmanifest_\(id).acf"),atomically:true,encoding:.utf8)
  }
  try addGame(438100,"VRChat","VRChat",primaryApps)
  try addGame(1234,"Another Game","Another Game",externalApps)
  try addGame(1235,"Partial Download","VRChat",primaryApps,flags:2)
  try addGame(1236,"Missing Files","Missing",primaryApps)
  let games = env.installedGames()
  require(games.map(\.id) == [1234,438100],"Installed games from primary and secondary libraries")
  func uint32(_ value: UInt32) -> [UInt8] {
      withUnsafeBytes(of: value.littleEndian) { Array($0) }
  }
  func field(_ type: UInt8, _ key: String) -> [UInt8] {
      [type] + Array(key.utf8) + [0]
  }
  func record(_ id: UInt32, _ metadata: [UInt8]) -> [UInt8] {
      let body = [UInt8](repeating: 0, count: 60) + field(0, "appinfo") + metadata + [8, 8]
      return uint32(id) + uint32(UInt32(body.count)) + body
  }
  try addGame(1237,"Flat Game","Another Game",externalApps)
  let category = field(0, "common") + field(0, "category") + field(2, "category_53") + uint32(1) + [8, 8]
  let launch = field(0, "config") + field(0, "launch") + field(0, "0")
  let vrLaunch = launch + field(1, "type") + Array("openxr".utf8) + [0, 8, 8, 8]
  let flat = launch + field(1, "type") + Array("default".utf8) + [0, 8, 8, 8]
  let cache = env.steam.appendingPathComponent("appcache/appinfo.vdf")
  try FileManager.default.createDirectory(at: cache.deletingLastPathComponent(), withIntermediateDirectories: true)
  var appInfo = uint32(0x07564428) + uint32(1)
  appInfo += record(1237, flat)
  appInfo += record(1234, vrLaunch)
  appInfo += record(438100, category)
  appInfo += record(1236, category)
  try Data(appInfo + uint32(0)).write(to: cache)
  require(env.installedVRGames().map(\.id) == [1234,438100],"VR metadata filters flat games and keeps untested VR games")
  try Data(appInfo.prefix(14)).write(to: cache)
  require(env.installedVRGames().isEmpty,"Truncated metadata cannot add games")
  let project = URL(fileURLWithPath: #filePath).deletingLastPathComponent().deletingLastPathComponent().deletingLastPathComponent()
  let verification = try JSONDecoder().decode(GameVerification.self,from:Data(contentsOf:project.appendingPathComponent("launcher/verified-games.json")))
  require(verification.badge(for:438100) == "Verified","Known compatibility badge")
  require(verification.badge(for:1234) == "Untested","Unlisted games remain selectable")
  print("PASS: Steam library discovery and independent compatibility badges")
  var video=VideoSettings(text:"[streaming]\nbitrate_mbps = 71\ntransport = \"wifi\"\n[logging]\nfile_logging = true\n")
  require(video.bitrate==71,"Adopt installed bitrate")
  video.bitrate=95
  let updated=try video.applied(to:"[streaming]\nbitrate_mbps = 71\ntransport = \"wifi\"\n[logging]\nfile_logging = true\n")
  require(VideoSettings(text:updated).bitrate==95 && updated.contains("transport = \"wifi\"") && updated.contains("file_logging = true"),"Video edit preserves unrelated settings")
  let scan = "VRONMAC_TRACKER lhr-a2dc7d79\nVRONMAC_TRACKER LHR-A2DC7D79\nVRONMAC_TRACKER invalid\nOther output"
  require(TrackerConfiguration.discoveredSerials(in: scan) == ["LHR-A2DC7D79"],"Discovery keeps valid unique serials")
  let manual=try TrackerConfiguration(reference:"lhr-a2dc7d79",manual:true,bodies:["LHR-A2DC7D79"])
  require(manual.bodies==[manual.reference],"A manual reference can be a body tracker")
  throwsError{_ = try TrackerConfiguration(reference:manual.reference,manual:false,bodies:manual.bodies)}
  throwsError{_ = try TrackerConfiguration(reference:manual.reference,manual:true,bodies:manual.bodies+manual.bodies)}
  let fixture=root.appendingPathComponent("runtime-fixture")
  try FileManager.default.createDirectory(at:fixture.appendingPathComponent("tracker"),withIntermediateDirectories:true)
  let target=fixture.appendingPathComponent("installed.dll")
  let runtime=TrackerRuntime(target:target,payload:fixture)
  try Data("unexpected".utf8).write(to:target)
  throwsError{try runtime.select(preview:true)}
  if let binaryFixtures = ProcessInfo.processInfo.environment["VRONMAC_RUNTIME_FIXTURES"] {
      let sources = URL(fileURLWithPath: binaryFixtures)
      for name in ["original.dll", "runtime.dll"] {
          try FileManager.default.copyItem(at:sources.appendingPathComponent(name),to:fixture.appendingPathComponent("tracker/"+name))
      }
      try FileManager.default.removeItem(at:target)
      try FileManager.default.copyItem(at:sources.appendingPathComponent("original.dll"),to:target)
      try runtime.select(preview:true)
      require(runtime.isPreview(),"Tracker runtime swap")
      try runtime.select(preview:false)
      require(!runtime.isPreview(),"Runtime restore")
      print("PASS: verified runtime swap/restore using supplied binary fixtures")
  } else {
      print("SKIP: runtime swap/restore requires VRONMAC_RUNTIME_FIXTURES")
  }
  print("PASS: video configuration, manual tracker roles and unknown runtime rejection")
  print("PASS: setup detection, matching Steam clients, USB authorization/selection, payload hashes/path boundaries, literal arguments, bounded command timeout")
 }
}
