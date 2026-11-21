#!/usr/bin/env python3
"""Check Git-tracked files for local artifacts and accidental credentials."""
import re
from pathlib import Path
import subprocess


def main():
    root = Path(__file__).resolve().parents[1]
    result = subprocess.run(["git", "-C", str(root), "ls-files", "-z"],
                            capture_output=True, check=True)
    paths = [Path(name) for name in result.stdout.decode().split("\0") if name]
    forbidden_roots = {"outputs", "work", "archive", "build", "dist", "releases", "prebuilt"}
    forbidden_suffixes = {".keystore", ".jks", ".p12", ".pfx", ".dmp", ".ips",
                          ".exe", ".dll", ".dylib", ".so", ".apk", ".o", ".pdb"}
    secrets = re.compile(rb"-----BEGIN (?:RSA |EC |OPENSSH )?PRIVATE KEY-----"
                         rb"|gh[pousr]_[A-Za-z0-9]{30,}|AKIA[0-9A-Z]{16}")
    problems = []
    for path in paths:
        if path.parts[0] in forbidden_roots or path.suffix.lower() in forbidden_suffixes:
            problems.append(f"Local or binary artifact: {path}")
        absolute = root / path
        if absolute.is_symlink():
            try:
                absolute.resolve(strict=True).relative_to(root)
            except (OSError, ValueError, RuntimeError):
                problems.append(f"Symlink leaves source tree or is broken: {path}")
            continue
        if absolute.stat().st_size > 50 * 1024 * 1024:
            problems.append(f"File exceeds 50 MiB: {path}")
        if secrets.search(absolute.read_bytes()):
            problems.append(f"Possible credential: {path}")
    if problems:
        raise SystemExit("\n".join(problems))
    print(f"PASS: {len(paths)} source files checked for artifacts and credentials")


if __name__ == "__main__":
    main()
