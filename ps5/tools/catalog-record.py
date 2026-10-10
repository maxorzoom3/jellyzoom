#!/usr/bin/env python3
"""The Homebrew Store (ps5-homebrew-catalog) record for a release, written to dist/release-<version>/<TITLEID>.json.

    catalog-record.py 1.0.0-beta.1 [--repo OWNER/NAME] [--author NAME]

The zip must already be attached to the GitHub release of the same tag under the same name,
because the record names it by address and pins it by sha256.
"""
import argparse
import hashlib
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("version")
    ap.add_argument("--repo", default="mortaljinx/mortalfin")
    ap.add_argument("--author", default="mortaljinx")
    args = ap.parse_args()
    param = json.loads((ROOT / "ps5/sce_sys/param.json").read_text())
    title_id = param["titleId"]
    name = param["localizedParameters"]["en-US"]["titleName"]
    zip_name = f"{name}-{args.version}.zip"
    zip_path = ROOT / "dist" / f"release-{args.version}" / zip_name
    digest = hashlib.sha256(zip_path.read_bytes()).hexdigest()
    record = {
        "titleid": title_id,
        "name": name,
        "kind": "app",
        "description": "A native Jellyfin client for the PS5 with a cover-wheel interface, HDR and surround sound passthrough. Beta.",
        "license": "GPL-3.0",
        "author": args.author,
        "version": args.version,
        "source_repo": f"https://github.com/{args.repo}",
        "artifact_url": f"https://github.com/{args.repo}/releases/download/{args.version}/{zip_name}",
        "sha256": digest,
        "icon_url": f"https://raw.githubusercontent.com/{args.repo}/{args.version}/ps5/sce_sys/icon0.png",
    }
    out = zip_path.parent / f"{title_id}.json"
    out.write_text(json.dumps(record, indent=2) + "\n")
    print(f"==> {out.relative_to(ROOT)} (sha256 {digest[:16]}...)")


if __name__ == "__main__":
    main()
