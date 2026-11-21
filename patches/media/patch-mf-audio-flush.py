"""Apply the Wine audio flush guard to the exact Voile v26.3.0-24 DLL."""
import argparse
import hashlib
import os
from pathlib import Path
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument("--stock", action="store_true", help="Restore the original DLL in the experimental runtime.")
parser.add_argument("--prefix", type=Path, default=Path("steam-test/home/prefix"))
args = parser.parse_args()
root = Path(__file__).resolve().parent
stock = Path.home() / "Library/Application Support/com.zagrios.obelisk/Voile/v26.3.0-24/linux/wine/lib/wine/x86_64-windows/mf.dll"
original = stock.read_bytes()
expected = "b3079c303a51fb4d8e68404c0fc4c4bf892f0c22980cce7d3d84ac1d19a43700"
if hashlib.sha256(original).hexdigest() != expected:
    raise SystemExit("Unexpected mf.dll build. No files changed.")

start, end = 0x19794, 0x197c5
assert original[start:start + 8] == bytes.fromhex("48 8b 01 ff 50 60 89 c7")
guard = bytes.fromhex(
    "e3 0a "                  # jrcxz no_client
    "48 8b 01 ff 50 60 "      # IAudioClient::Reset, unchanged for a valid client
    "89 c7 eb 25 "            # retain HRESULT and jump to LeaveCriticalSection
    "f6 87 ec 00 00 00 01 "   # no_client: test SAR_SHUT_DOWN
    "bf 38 4a 0d c0 "         # MF_E_STREAMSINK_REMOVED if shut down
    "75 17 "
    "31 ff eb 13"             # otherwise S_OK, then common cleanup
)
assert len(guard) <= end - start
patched = original[:start] + guard.ljust(end - start, b"\x90") + original[end:]
targets = [
    root / "steam-test/linux/wine/lib/wine/x86_64-windows/mf.dll",
    root / args.prefix / "drive_c/windows/system32/mf.dll",
]
for target in targets:
    if target.is_symlink() or target.read_bytes() not in (original, patched):
        raise SystemExit(f"Unexpected test DLL: {target}. No files changed.")
selected = original if args.stock else patched
for target in targets:
    if target.read_bytes() == selected:
        continue
    fd, staging = tempfile.mkstemp(dir=target.parent, prefix=".mf-flush-")
    try:
        with os.fdopen(fd, "wb") as file:
            file.write(selected)
        os.chmod(staging, target.stat().st_mode & 0o777)
        os.replace(staging, target)
    finally:
        Path(staging).unlink(missing_ok=True)
print("Experimental mf.dll:", "stock" if args.stock else "audio guard", hashlib.sha256(selected).hexdigest())
