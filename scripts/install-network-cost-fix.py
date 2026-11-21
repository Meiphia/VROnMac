#!/usr/bin/env python3
"""Install the tested Wine network-cost fix into VRChat's Linux prefix."""
import argparse
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

PROJECT = Path(__file__).resolve().parents[1]
KEY = r"Software\Wine\AppDefaults\VRChat.exe\DllOverrides"
DLL = "windows.networking.connectivity.dll"

def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def atomic_copy(source, target):
    fd, name = tempfile.mkstemp(prefix=".vrcm-network-cost-", dir=target.parent)
    os.close(fd)
    temporary = Path(name)
    try:
        shutil.copy2(source, temporary)
        os.replace(temporary, target)
    finally:
        temporary.unlink(missing_ok=True)

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--prefix", type=Path, default=Path.home() / "Library/Application Support/com.zagrios.obelisk/Prefixes/steam-438100-windows-linux/prefix")
    args = parser.parse_args()
    prefix = args.prefix.resolve()
    registry = prefix / "user.reg"
    target = prefix / "drive_c/windows/system32" / DLL
    source = PROJECT / "compatibility/wine-connectivity" / DLL
    if not registry.is_file() or not target.is_file():
        raise SystemExit("Expected an initialized VRChat prefix with its Wine networking library")
    wanted = digest(source)
    verified = False
    probes = list((PROJECT / "work").glob("connectivity-test.*/game/probe-result.txt"))
    probes.append(PROJECT / "docs/validation/network-cost/probe-result.txt")
    for result in probes:
        if not result.is_file():
            continue
        text = result.read_text(errors="replace")
        tested_dll = result.parent / DLL
        if "IDENTITY: VRConMac connectivity fix version 1 verified" in text and "PASS:" in text and tested_dll.is_file() and digest(tested_dll) == wanted:
            verified = True
            break
    if not verified:
        raise SystemExit("This exact DLL has no passing runtime probe; run scripts/test-connectivity.sh first")
    # Preserve the original prefix files. Replace directory entries atomically,
    # so even a symlink at the DLL path cannot modify the shared Voile runtime.
    for path in (registry, target):
        backup = path.with_name(path.name + ".before-vrcm-network-cost")
        if not backup.exists() and not backup.is_symlink():
            shutil.copy2(path, backup, follow_symlinks=False)
    fd, temporary_name = tempfile.mkstemp(prefix=".vrcm-registry-", dir=prefix)
    os.close(fd)
    temporary_registry = Path(temporary_name)
    try:
        shutil.copy2(registry, temporary_registry)
        subprocess.run([sys.executable, str(PROJECT / "scripts/regedit.py"), str(temporary_registry), KEY, "windows.networking.connectivity", "native,builtin"], check=True)
        atomic_copy(source, target)
        os.replace(temporary_registry, registry)
    finally:
        temporary_registry.unlink(missing_ok=True)
    if digest(target) != wanted:
        raise SystemExit("Installed DLL failed checksum verification")
    print("Installed the tested network-cost fix for VRChat.exe.")
    print("Original DLL and user.reg saved as *.before-vrcm-network-cost.")
    print("Launch VRChat normally through Steam for the in-game test.")

if __name__ == "__main__":
    main()
