#!/usr/bin/env python3
"""Package locally built ARM64 apps, HAL microphone driver and experimental runtime."""
from pathlib import Path
import json
import os
import plistlib
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / 'build/audio-tracking'
DIST = Path(os.environ.get('VRCONMAC_DIST_DIR', str(BUILD / 'rebuild-check')))
DIST.mkdir(parents=True, exist_ok=True)

def run(*args, **kwargs):
    subprocess.run([str(a) for a in args], check=True, **kwargs)

def bundle(name, executable, identity):
    app = DIST / (name + '.app')
    contents = app / 'Contents'
    (contents / 'MacOS').mkdir(parents=True, exist_ok=True)
    (contents / 'Resources').mkdir(exist_ok=True)
    info = dict(CFBundleExecutable=executable, CFBundleIdentifier=identity,
                CFBundleName=name, CFBundleDisplayName=name,
                CFBundlePackageType='APPL', CFBundleVersion='1',
                CFBundleShortVersionString='1.0', NSHighResolutionCapable=True,
                LSMinimumSystemVersion='14.0')
    if executable == 'VRConMacAudio':
        info.update(NSScreenCaptureUsageDescription='Send Mac game audio to your Quest headset over the local network.',
                    NSAudioCaptureUsageDescription='Send Mac game audio to your Quest headset.',
                    NSMicrophoneUsageDescription='Connect your Quest microphone to VRChat through the virtual input device.')
    (contents / 'Info.plist').write_bytes(plistlib.dumps(info))
    shutil.copy2(BUILD / executable, contents / 'MacOS' / executable)
    return app

audio = bundle('VRConMac Audio', 'VRConMacAudio', 'net.vrconmac.audio')
run('codesign', '--force', '--sign', '-', audio)
face = bundle('VRCFaceTracking Mac', 'VRCFaceTrackingLauncher', 'net.vrconmac.vrcfacetracking')
face_files = face / 'Contents/Resources/vrcft'
shutil.copytree(BUILD / 'vrcft', face_files, dirs_exist_ok=True)
modules = face_files / 'BundledModules'
modules.mkdir(exist_ok=True)
shutil.copy2(BUILD / 'module/VRConMac.Tracking.dll', modules / 'VRConMac.Tracking.dll')
shutil.copy2(ROOT / 'tracking/quest-pro/ALXR-LICENSE', modules / 'ALXR-LICENSE')
shutil.copy2(ROOT / 'vendor/VRCFaceTracking/LICENSE', face / 'Contents/Resources/VRCFaceTracking-LICENSE')
entitlements = BUILD / 'dotnet-entitlements.plist'
entitlements.write_bytes(plistlib.dumps({'com.apple.security.cs.allow-jit': True,
    'com.apple.security.cs.allow-unsigned-executable-memory': True,
    'com.apple.security.cs.disable-library-validation': True}))
for file in face_files.rglob('*'):
    if not file.is_file():
        continue
    with file.open('rb') as stream:
        magic = stream.read(4)
    if magic in (b'\xcf\xfa\xed\xfe', b'\xca\xfe\xba\xbe', b'\xfe\xed\xfa\xcf'):
        args = ['codesign', '--force', '--sign', '-']
        if file.name in ('VRCFaceTracking', 'VRCFaceTracking.ModuleProcess'):
            args += ['--options', 'runtime', '--entitlements', entitlements]
        run(*args, file)
run('codesign', '--force', '--sign', '-', '--entitlements', entitlements, face)

driver = BUILD / 'VRConMac Microphone.driver'
contents = driver / 'Contents'
(contents / 'Resources').mkdir(exist_ok=True)
factory = '0942f324-3ee2-43e9-b276-d03b6ea73d61'
(contents / 'Info.plist').write_bytes(plistlib.dumps(dict(
    CFBundleExecutable='VRConMacMicrophone', CFBundleIdentifier='net.vrconmac.microphone',
    CFBundleName='VRConMac Microphone', CFBundlePackageType='BNDL', CFBundleVersion='1',
    CFBundleShortVersionString='1.0', CFPlugInFactories={factory: 'BlackHole_Create'},
    CFPlugInTypes={'443ABAB8-E7B3-491A-B985-BEB9187030DB': [factory]})))
shutil.copy2(ROOT / 'vendor/BlackHole/LICENSE', contents / 'Resources/COPYING')
run('codesign', '--force', '--sign', '-', driver)
package_root = BUILD / 'package-root'
shutil.copytree(driver, package_root / driver.name, dirs_exist_ok=True)
run('pkgbuild', '--root', package_root, '--install-location', '/Library/Audio/Plug-Ins/HAL',
    '--identifier', 'net.vrconmac.microphone', '--version', '1.0', '--filter', '/\\._',
    DIST / 'VRConMac Microphone.pkg', env={**os.environ, 'COPYFILE_DISABLE': '1'})

runtime = DIST / 'runtime'
runtime.mkdir(parents=True, exist_ok=True)
shutil.copy2(BUILD / 'runtime/runtime/liboxrsys-runtime.dylib', runtime / 'liboxrsys-runtime.dylib')
(runtime / 'oxrsys-runtime.json').write_text(json.dumps({'file_format_version':'1.0.0',
    'runtime': {'name':'OXRSys VRConMac Audio and Tracking', 'library_path':'./liboxrsys-runtime.dylib'}}, indent=2) + '\n')
for app in (audio, face, driver):
    run('codesign', '--verify', '--deep', '--strict', app)
print(f'Packaged apps and installer: {DIST}')
