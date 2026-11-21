#!/usr/bin/env python3
"""Legacy 0.1 preview packager. Use package-preview.py for the current launcher."""
from pathlib import Path
import hashlib
import json
import os
import plistlib
import shutil
import subprocess

root=Path(__file__).resolve().parents[1]
app=root/'dist/launcher/VRConMac.app'
contents=app/'Contents'
(contents/'MacOS').mkdir(parents=True,exist_ok=True)
payload=contents/'Resources/payload'
payload.mkdir(parents=True,exist_ok=True)
for name in ('bridge','runtime','platform-tools','Licenses'):(payload/name).mkdir(exist_ok=True)
shutil.copy2(root/'build/launcher/VRConMac',contents/'MacOS/VRConMac')
shutil.copy2(root/'bridge/build/vrconmac-bridge',payload/'bridge/vrconmac-bridge')
for name in ('liboxrsys-runtime.dylib','oxrsys-runtime.json'):
 shutil.copy2(root/'runtime-host/oxrsys-audio-tracking'/name,payload/'runtime'/name)
shutil.copytree(root/'dist/audio-tracking/VRCFaceTracking Mac.app',payload/'VRCFaceTracking.app',dirs_exist_ok=True)
for name in ('VRConMac-Quest.apk','VRConMac Microphone.pkg'):shutil.copy2(root/'dist/audio-tracking'/name,payload/name)
sdk=Path(os.environ.get('ANDROID_HOME',''))/'platform-tools'
if not (sdk/'adb').is_file():raise SystemExit('Set ANDROID_HOME to the SDK containing official platform-tools')
for name in ('adb','NOTICE.txt','source.properties'):shutil.copy2(sdk/name,payload/'platform-tools'/name)
if (sdk/'lib64').exists():shutil.copytree(sdk/'lib64',payload/'platform-tools/lib64',dirs_exist_ok=True)
for source,name in [('LICENSE','VRConMac-MIT.txt'),('vendor/OXRSys/LICENSE','OXRSys-MPL.txt'),('vendor/VRCFaceTracking/LICENSE','VRCFaceTracking-Apache.txt'),('vendor/BlackHole/LICENSE','BlackHole-GPL.txt'),('tracking/quest-pro/ALXR-LICENSE','FaceMapping-MIT.txt')]:
 if (root/source).exists():shutil.copy2(root/source,payload/'Licenses'/name)
info=dict(CFBundleExecutable='VRConMac',CFBundleIdentifier='net.vrconmac.launcher.preview',CFBundleName='VRConMac',CFBundleDisplayName='VRConMac Developer Preview',CFBundlePackageType='APPL',CFBundleVersion='1',CFBundleShortVersionString='0.1.0',LSMinimumSystemVersion='27.0',NSHighResolutionCapable=True,LSMultipleInstancesProhibited=True,NSScreenCaptureUsageDescription='Send VRChat sound to your Quest headset.',NSAudioCaptureUsageDescription='Send Mac game audio to your Quest headset.',NSMicrophoneUsageDescription='Connect the headset microphone to VRChat.')
(contents/'Info.plist').write_bytes(plistlib.dumps(info))
# Sign before hashing: signing can change the bytes of a component.
for f in payload.rglob('*'):
 if not f.is_file():continue
 with f.open('rb') as stream:magic=stream.read(4)
 if magic in (b'\xcf\xfa\xed\xfe',b'\xca\xfe\xba\xbe',b'\xfe\xed\xfa\xcf') and 'VRCFaceTracking.app' not in f.parts:
  subprocess.run(['codesign','--force','--sign','-',str(f)],check=True,capture_output=True)
files=[]
for f in sorted(payload.rglob('*')):
 if f.is_file() and f.name!='manifest.json':files.append(dict(path=f.relative_to(payload).as_posix(),sha256=hashlib.sha256(f.read_bytes()).hexdigest()))
(payload/'manifest.json').write_text(json.dumps(dict(schema=1,revision='0.1.0-local-preview',files=files),indent=2)+'\n')
subprocess.run(['codesign','--force','--sign','-',str(app)],check=True)
subprocess.run(['codesign','--verify','--deep','--strict',str(app)],check=True)
print('Built local developer preview:',app)
