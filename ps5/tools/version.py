#!/usr/bin/env python3
"""The one version of the title: the file VERSION at the top of the project.

    version.py            print it (1.0.0-beta.1)
    version.py --sync     write the matching contentVersion into ps5/sce_sys/param.json
    version.py --check    exit 1 when param.json does not match VERSION
    version.py --content  print the contentVersion

VERSION is major.minor.patch, with -beta.N or -rc.N for a test build. The console's contentVersion is
MM.mmm.ppp: MM major, mmm minor, ppp = patch * 100 + the build number (beta.N: N, rc.N: 50 + N, a final
release: 99), so every release has a higher contentVersion than the one before it, and 1.0.0 is above all its betas.
"""
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
PARAM = ROOT / "ps5/sce_sys/param.json"
PATTERN = re.compile(r"^(\d+)\.(\d+)\.(\d+)(?:-(beta|rc)\.(\d+))?$")


def version():
    text = (ROOT / "VERSION").read_text().strip()
    if not PATTERN.match(text):
        raise SystemExit(f"VERSION says {text!r}: expected 1.2.3, 1.2.3-beta.1 or 1.2.3-rc.1")
    return text


def content_version(text):
    major, minor, patch, kind, number = PATTERN.match(text).groups()
    major, minor, patch = int(major), int(minor), int(patch)
    build = 99 if kind is None else (int(number) if kind == "beta" else 50 + int(number))
    if major > 99 or minor > 999 or patch > 9 or (kind == "beta" and not 1 <= build <= 49) or (kind == "rc" and not 51 <= build <= 98):
        raise SystemExit(f"VERSION {text}: out of range (major 0-99, minor 0-999, patch 0-9, beta.1-49, rc.1-48)")
    return f"{major:02d}.{minor:03d}.{patch * 100 + build:03d}"


def main():
    text = version()
    want = content_version(text)
    arg = sys.argv[1] if len(sys.argv) > 1 else ""
    if arg == "--content":
        print(want)
    elif arg in ("--sync", "--check"):
        param = PARAM.read_text()
        match = re.search(r'("contentVersion"\s*:\s*")([^"]*)(")', param)
        if not match:
            raise SystemExit("param.json has no contentVersion")
        if match.group(2) == want:
            print(f"param.json contentVersion {want} matches VERSION {text}")
        elif arg == "--check":
            raise SystemExit(f"param.json contentVersion is {match.group(2)}, VERSION {text} needs {want}: run ps5/tools/version.py --sync")
        else:
            PARAM.write_text(param[:match.start(2)] + want + param[match.end(2):])
            print(f"param.json contentVersion {match.group(2)} -> {want} (VERSION {text}); commit it before packaging")
    else:
        print(text)


if __name__ == "__main__":
    main()
