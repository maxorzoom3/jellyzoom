#!/usr/bin/env python3
"""Is anything of yours in what you are about to publish?

    security-check.py                 the source tree and git history
    security-check.py --build         ... and the built title folder (dist/<TITLE_ID>)
    security-check.py --zip FILE.zip  ... and the release zip

It looks for your own values, not for guesses: the server, login and device id in ps5/src/jf/config.h,
your computer's user name and host name, and any words you list in ~/.mortalfin-private (one per line:
your server's name, your domain, a nickname). It also looks for kinds of thing that should never be in
a public repository: private network addresses in our own code, e-mail addresses, home-folder paths,
keys and tokens, and files such as config.h, logs and builds.

It prints WHAT it found and WHERE, never the value, so its output is safe to paste anywhere.
Exit status 1 if anything must be fixed (FAIL); warnings (WARN) are for you to look at.
"""
import argparse
import getpass
import hashlib
import json
import os
import re
import socket
import struct
import subprocess
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
OWN = ("examples/jellyfinwheel/", "ps5/src/", "ps5/tools/", "ps5/CMakeLists.txt", "README", "docs/", ".github/", "CHANGELOG", "VERSION")
SKIP_OWN = ("examples/jellyfinwheel/kit/", "ps5/src/jf/lib/", "ps5/src/jf/include/")  # code we did not write
FORBIDDEN_TRACKED = re.compile(r"(^|/)(config\.h|\.env[^/]*|klog[^/]*|test-run\.txt|test-results\.txt|\.release)$|\.(log|elf|bin|pem|key|tgz|zip|pkg|fself)$|^dist/|^build/|(^|/)\.deps/")
NEVER_IN_TITLE = ("test-run.txt", "test-results.txt", "screenshots", "klog", ".env", "radv-shader-cache", "config.h")
PRIVATE_IP = re.compile(r"\b(?:10\.\d{1,3}\.\d{1,3}\.\d{1,3}|192\.168\.\d{1,3}\.\d{1,3}|172\.(?:1[6-9]|2\d|3[01])\.\d{1,3}\.\d{1,3})\b")
OK_IPS = {"192.168.1.10", "192.168.1.1", "192.168.0.1"}  # the examples the program shows
EMAIL = re.compile(r"\b[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\.[A-Za-z]{2,}\b")
OK_EMAIL = re.compile(r"(noreply|example\.(com|org)|users\.noreply\.github\.com)")
HOME_PATH = re.compile(rb"/(?:home|Users)/[A-Za-z0-9_.-]+/|/mnt/data/[A-Za-z0-9_.-]+")
KEYS = re.compile(r"-----BEGIN [A-Z ]*PRIVATE KEY-----|\bghp_[A-Za-z0-9]{30,}|\bgithub_pat_[A-Za-z0-9_]{30,}|\bAKIA[0-9A-Z]{16}\b|\beyJ[A-Za-z0-9_-]{15,}\.[A-Za-z0-9_-]{15,}\.[A-Za-z0-9_-]{10,}")
CRED = re.compile(r"(?i)\b(pass(?:word|wd)?|secret|api[_-]?key|token)\b\s*[:=]\s*[\"'][^\"'\s]{4,}[\"']")

results = {"FAIL": [], "WARN": [], "ok": []}


def note(level, what, where=""):
    results[level].append(f"{what}" + (f"  [{where}]" if where else ""))


def git(*args):
    return subprocess.run(["git", "-C", str(ROOT), *args], capture_output=True, text=True).stdout


def secrets():
    """name -> value, from config.h, this computer and the private list; the values never leave this function's callers."""
    found = {}
    cfg = ROOT / "ps5/src/jf/config.h"
    if cfg.is_file():
        for name, value in re.findall(r'#\s*define\s+(JF_[A-Z_]+)\s+"([^"]*)"', cfg.read_text(errors="replace")):
            if len(value) >= 3 and name not in ("JF_CLIENT_NAME", "JF_DEVICE_NAME", "JF_CLIENT_VERSION"):
                found[f"config.h {name}"] = value
    for name, value in (("your user name", getpass.getuser()), ("this computer's name", socket.gethostname().split(".")[0])):
        if len(value) >= 3 and value.lower() not in ("root", "localhost", "user", "admin"):
            found[name] = value
    private = Path.home() / ".mortalfin-private"
    if private.is_file():
        for n, line in enumerate(private.read_text().splitlines(), 1):
            line = line.strip()
            if line and not line.startswith("#") and len(line) >= 3:
                found[f"{private.name} line {n}"] = line
    for env in filter(None, os.environ.get("MORTALFIN_PRIVATE", "").split(",")):
        if len(env) >= 3:
            found["MORTALFIN_PRIVATE"] = env.strip()
    return found


def source_files():
    names = git("ls-files", "-co", "--exclude-standard", "-z").split("\0")
    return [n for n in names if n and (ROOT / n).is_file()]


def check_files(sec):
    files = source_files()
    tracked = set(filter(None, git("ls-files").splitlines()))
    bad = [f for f in tracked if FORBIDDEN_TRACKED.search(f) and "dav1d" not in f and not f.startswith(("external/", "vendor/"))]
    for f in bad:
        note("FAIL", "a file that must not be published is tracked by git", f)
    if not bad:
        note("ok", "no config, log, key, build or archive file is tracked")
    cfg = ROOT / "ps5/src/jf/config.h"
    ignored = subprocess.run(["git", "-C", str(ROOT), "check-ignore", "-q", "ps5/src/jf/config.h"]).returncode == 0
    if cfg.is_file() and not ignored:
        note("FAIL", "config.h is not ignored by git: add ps5/src/jf/config.h to .gitignore")
    else:
        note("ok", "config.h is ignored by git")
    if not (ROOT / "ps5/src/jf/config.h.example").is_file():
        note("WARN", "ps5/src/jf/config.h.example is missing")
    hits = 0
    for f in files:
        path = ROOT / f
        try:
            if path.stat().st_size > 8 << 20:
                continue
            data = path.read_bytes()
        except OSError:
            continue
        if b"\0" in data[:4096]:
            continue
        text = data.decode("utf-8", "replace")
        low = text.lower()
        for name, value in sec.items():
            if value.lower() in low and f != "ps5/src/jf/config.h":
                note("FAIL", f"your value ({name}) is in the source", f)
                hits += 1
        own = f.startswith(OWN) and not f.startswith(SKIP_OWN) and f != "ps5/src/jf/config.h" and f != "ps5/tools/security-check.py"
        if not own:
            continue
        for n, line in enumerate(text.splitlines(), 1):
            for m in PRIVATE_IP.finditer(line):
                if m.group(0) not in OK_IPS:
                    note("FAIL", "a private network address", f"{f}:{n}")
                    hits += 1
            for m in EMAIL.finditer(line):
                if not OK_EMAIL.search(m.group(0)):
                    note("WARN", "an e-mail address", f"{f}:{n}")
            if KEYS.search(line):
                note("FAIL", "a key or token", f"{f}:{n}")
                hits += 1
            if CRED.search(line):
                note("WARN", "something that looks like a password or token", f"{f}:{n}")
            if HOME_PATH.search(line.encode()):
                note("WARN", "a path on your computer", f"{f}:{n}")
    if not hits:
        note("ok", "none of your values, no private address and no key in %d source files" % len(files))


def check_history(sec):
    log = git("log", "--all", "--diff-filter=A", "--name-only", "--format=").splitlines()
    cfg = [f for f in set(log) if re.search(r"(^|/)(config\.h|\.env.*)$", f) and "dav1d" not in f and "external/" not in f]
    for f in cfg:
        note("FAIL", "this file was committed at some time (it stays in git history)", f)
    clean = not cfg
    for name, value in sec.items():
        n = len(git("log", "--all", "--oneline", "-S" + value, "--").splitlines())
        if n:
            note("FAIL", f"your value ({name}) appears in git history, in {n} commit(s)")
            clean = False
    if clean:
        note("ok", "git history holds none of your values and no config file")
    email = git("config", "user.email").strip()
    if email and not OK_EMAIL.search(email):
        note("WARN", "your git author e-mail will be public on every commit; GitHub's no-reply address "
                     "(Settings > Emails > 'Keep my email addresses private') avoids that", email[:2] + "***@" + email.split("@")[-1])
    authors = set(git("log", "--all", "--format=%ae").split())
    odd = [a for a in authors if not OK_EMAIL.search(a)]
    if odd:
        note("WARN", f"{len(odd)} commit author address(es) in history are real e-mail addresses (public once pushed)")


def scan_blob(label, data, sec, paths=True):
    low = data.lower()
    found = False
    for name, value in sec.items():
        if value.lower().encode() in low:
            note("FAIL", f"your value ({name}) is inside the build", label)
            found = True
    if paths:
        n = len(HOME_PATH.findall(data))
        if n:
            note("WARN", f"{n} build path(s) of your computer inside (file paths of the compiler; harmless but tidy)", label)
    return found


def check_folder(folder, sec):
    if not folder.is_dir():
        note("FAIL", f"{folder.relative_to(ROOT)} does not exist: run ps5/tools/release.sh")
        return
    leaked = False
    for p in sorted(folder.rglob("*")):
        if p.name in NEVER_IN_TITLE:
            note("FAIL", "a file a release never carries", str(p.relative_to(folder)))
        if p.is_file() and p.stat().st_size < 600 << 20:
            leaked |= scan_blob(str(p.relative_to(folder)), p.read_bytes(), sec)
    if not leaked:
        note("ok", f"none of your values is inside {folder.name}/")
    param = json.loads((folder / "sce_sys/param.json").read_text())
    if not re.fullmatch(r"[A-Z]{4}\d{5}", param["titleId"]):
        note("FAIL", "titleId is not four capital letters and five digits")
    if not re.fullmatch(r"\d{2}\.\d{3}\.\d{3}", param.get("contentVersion", "")):
        note("FAIL", "contentVersion is not in the form 01.000.001")
    icon = folder / "sce_sys/icon0.png"
    if not icon.is_file():
        note("FAIL", "sce_sys/icon0.png is missing")
    else:
        head = icon.read_bytes()[:32]
        w, h = struct.unpack(">II", head[16:24]) if head[:8] == b"\x89PNG\r\n\x1a\n" else (0, 0)
        if w != h or w < 256:
            note("FAIL", f"icon0.png must be square and at least 256 pixels; it is {w}x{h}")
        elif icon.stat().st_size > 2 << 20:
            note("FAIL", "icon0.png is over 2 MiB")
        else:
            note("ok", f"icon0.png is {w}x{h}")
    if not (folder / "eboot.bin").is_file():
        note("FAIL", "eboot.bin is missing")


def check_zip(zpath, sec):
    if not zpath.is_file():
        note("FAIL", f"{zpath} does not exist")
        return
    leaked = False
    with zipfile.ZipFile(zpath) as z:
        names = z.namelist()
        tops = {n.split("/")[0] for n in names}
        if len(tops) != 1:
            note("FAIL", "the zip must hold one folder (the title ID) at its top level; it holds " + str(len(tops)))
        top = next(iter(tops))
        for need in (f"{top}/eboot.bin", f"{top}/sce_sys/param.json"):
            if need not in names:
                note("FAIL", f"the zip lacks {need}")
        for n in names:
            if Path(n).name in NEVER_IN_TITLE:
                note("FAIL", "a file a release never carries is in the zip", n)
            if not n.endswith("/") and z.getinfo(n).file_size < 600 << 20:
                leaked |= scan_blob(n, z.read(n), sec)
        note("ok" if not leaked else "FAIL", "none of your values is inside the zip" if not leaked else "see above")
    h = hashlib.sha256(zpath.read_bytes()).hexdigest()
    note("ok", f"zip {zpath.name}: {zpath.stat().st_size / 1e6:.1f} MB, sha256 {h}")


def check_release_files():
    for need in ("README.md", "LICENSE.md", "VERSION", ".gitignore", "CHANGELOG.md"):
        if not (ROOT / need).is_file():
            note("FAIL", f"{need} is missing")
    lic = ROOT / "LICENSE.md"
    if lic.is_file() and "GNU GENERAL PUBLIC LICENSE" not in lic.read_text(errors="replace")[:400]:
        note("WARN", "LICENSE.md is not the GPL-3.0 text, so GitHub will show a different licence than the title's")
    r = subprocess.run([sys.executable, str(ROOT / "ps5/tools/version.py"), "--check"], capture_output=True, text=True)
    note("ok" if r.returncode == 0 else "FAIL", (r.stdout or r.stderr).strip().splitlines()[-1] if (r.stdout or r.stderr).strip() else "version check")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", action="store_true")
    ap.add_argument("--zip")
    args = ap.parse_args()
    sec = secrets()
    print(f"checking against {len(sec)} private value(s): " + ", ".join(sorted({k.split(' line')[0] for k in sec})) + "\n")
    check_release_files()
    check_files(sec)
    check_history(sec)
    if args.build or args.zip:
        title = json.loads((ROOT / "ps5/sce_sys/param.json").read_text())["titleId"]
        check_folder(ROOT / "dist" / title, sec)
    if args.zip:
        check_zip(Path(args.zip), sec)
    for line in results["ok"]:
        print("  ok    " + line)
    for line in results["WARN"]:
        print("  WARN  " + line)
    for line in results["FAIL"]:
        print("  FAIL  " + line)
    print(f"\n{len(results['FAIL'])} to fix, {len(results['WARN'])} to look at")
    sys.exit(1 if results["FAIL"] else 0)


if __name__ == "__main__":
    main()
