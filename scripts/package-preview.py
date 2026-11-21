#!/usr/bin/env python3
"""Assemble a preview using an explicit, previously built component bundle."""
import argparse
import hashlib
import json
from pathlib import Path
import plistlib
import shutil
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--components-app", required=True, type=Path,
                        help="Existing VRonMac app supplying the native/runtime payload")
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--identity", default="-", help="codesign identity, default ad-hoc")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    component_app = args.components_app.resolve(strict=True)
    destination = args.output.absolute()
    if destination.exists():
        raise SystemExit(f"Output already exists: {destination}")
    binary = root / "build/launcher/VRonMac"
    if not binary.is_file():
        raise SystemExit("Build the launcher with bash scripts/build-launcher.sh first.")
    subprocess.run([str(component_app / "Contents/MacOS/VRonMac"), "--verify-payload"], check=True)
    subprocess.run(["codesign", "--verify", "--deep", "--strict", str(component_app)], check=True)
    destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=destination.parent, prefix=".preview-") as staging:
        app = Path(staging) / "VRonMac.app"
        shutil.copytree(component_app, app, symlinks=True)
        executable = app / "Contents/MacOS/VRonMac"
        shutil.copy2(binary, executable)
        payload = app / "Contents/Resources/payload"
        licenses = payload / "Licenses"
        licenses.mkdir(exist_ok=True)
        shutil.copy2(root / "LICENSE", licenses / "VRonMac-MIT.txt")
        shutil.copy2(root / "docs/release-status.md", licenses / "Release-Status.md")
        shutil.copy2(root / "docs/third-party-sources.md", licenses / "Source-Provenance.md")
        shutil.copy2(root / "vendor/OXRSys/LICENSE", licenses / "OXRSys-MPL.txt")
        files = []
        for path in sorted(payload.rglob("*")):
            if path.is_symlink():
                path.resolve(strict=True).relative_to(payload.resolve())
            if path.is_file() and path != payload / "manifest.json":
                files.append({"path": path.relative_to(payload).as_posix(),
                              "sha256": hashlib.sha256(path.read_bytes()).hexdigest()})
        (payload / "manifest.json").write_text(json.dumps(
            {"schema": 1, "revision": "0.2.15-preview.1", "files": files}, indent=2) + "\n")
        info_path = app / "Contents/Info.plist"
        info = plistlib.loads(info_path.read_bytes())
        info["CFBundleVersion"] = "18"
        info["CFBundleShortVersionString"] = "0.2.15"
        info_path.write_bytes(plistlib.dumps(info))
        subprocess.run(["codesign", "--force", "--sign", args.identity, str(app)], check=True)
        subprocess.run(["codesign", "--verify", "--deep", "--strict", str(app)], check=True)
        subprocess.run([str(executable), "--verify-payload"], check=True)
        app.rename(destination)
    print(destination)


if __name__ == "__main__":
    main()
