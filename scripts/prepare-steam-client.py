#!/usr/bin/env python3
"""Download Valve's Linux Steam client matched to the installed Mac Steam build."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import zipfile

BASE = "https://client-update.akamai.steamstatic.com/"
PROJECT = Path(__file__).resolve().parents[1]

def version(text):
    match = re.search(r'"version"\s*"(\d+)"', text)
    if not match:
        raise SystemExit("Steam manifest has no version")
    return match.group(1)

def fetch(url, target):
    subprocess.run(["curl", "--fail", "--location", "--silent", "--show-error",
                    "--max-time", "90", url, "--output", str(target)], check=True)

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--steam-dir", type=Path, default=Path.home() / "Library/Application Support/Steam")
    parser.add_argument("--runtime", type=Path)
    args = parser.parse_args()
    package = args.steam_dir / "Steam.AppBundle/Steam/Contents/MacOS/package"
    manifest = package / "steam_client_signed-2_osx.manifest"
    if not manifest.is_file():
        raise SystemExit(f"Cannot identify installed Steam build: {manifest}")
    host_version = version(manifest.read_text())
    runtime = args.runtime
    if runtime is None:
        candidates = list((Path.home() / "Library/Application Support/com.zagrios.obelisk/Voile").glob("v*/linux"))
        candidates = [p for p in candidates if (p / "engine/fexdarwin-run").is_file()]
        if not candidates:
            raise SystemExit("Voile Linux engine not found; specify --runtime")
        runtime = max(candidates, key=lambda p: p.stat().st_mtime)
    parent = PROJECT / "compat-tool/vrconmac-lkd"
    destination = parent / "steam-client"
    with tempfile.TemporaryDirectory(prefix=".steam-client-", dir=parent) as temporary:
        staging = Path(temporary)
        linux_manifest = staging / "manifest"
        fetch(BASE + "steam_client_ubuntu12", linux_manifest)
        text = linux_manifest.read_text()
        if version(text) != host_version:
            raise SystemExit(f"Linux stable build {version(text)} differs from installed Mac Steam {host_version}; refusing to mix versions")
        block = re.search(r'"bins_sdk_ubuntu12"\s*\{([^}]+)\}', text)
        if not block:
            raise SystemExit("Linux Steam SDK package not found")
        fields = dict(re.findall(r'"([^"\n]+)"\s*"([^"\n]+)"', block.group(1)))
        filename = fields["file"]
        if not re.fullmatch(r"bins_sdk_ubuntu12\.zip\.[0-9a-f]{40}", filename):
            raise SystemExit("Unexpected Steam SDK package name")
        archive = staging / "sdk.zip"
        fetch(BASE + filename, archive)
        digest = hashlib.sha256(archive.read_bytes()).hexdigest()
        if digest != fields["sha2"]:
            raise SystemExit("Steam SDK SHA-256 verification failed")
        payload = staging / "steam-client"
        (payload / "linux64").mkdir(parents=True)
        with zipfile.ZipFile(archive) as package_zip:
            for name in ("linux64/steamclient.so", "linux64/crashhandler.so"):
                (payload / name).write_bytes(package_zip.read(name))
        for name in ("steamclient.dll", "steamclient64.dll"):
            shutil.copy2(runtime / "steam" / name, payload / name)
        (payload / "version").write_text(host_version + "\n")
        (payload / "source.json").write_text(json.dumps({"version": host_version, "url": BASE + filename, "sha256": digest}, indent=2) + "\n")
        # Replace only our dedicated downloaded files, never Steam or Voile.
        destination.mkdir(exist_ok=True)
        shutil.copytree(payload, destination, dirs_exist_ok=True)
    print(f"Prepared Linux Steam client {host_version}: {destination}")

if __name__ == "__main__":
    main()
