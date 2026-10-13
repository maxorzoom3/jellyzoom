#!/usr/bin/env python3
"""PS5 Vulkan Template - the notices a title folder carries: LEGAL.txt and licenses/.

    stage-notices.py APP_DIR VULKAN_DIR RADV_ARCHIVE

Called by ps5/tools/link-title.sh after it packages the title. It writes, beside
eboot.bin:

  LEGAL.txt                 no piracy; what the title is licensed under; trademarks
  licenses/README.txt       the same notice, then every part: its licence, copyright and source
  licenses/components.json  the same parts as data, each with the revision it was built from
                            (ps5/tools/source-bundle.py archives the source from it)
  licenses/<part>/...       the licence texts each part requires

Every text is read at the revision the title was built from (git show), not from a
working tree, so the notices match the source a release attaches. A part whose
repository has uncommitted changes is recorded as "dirty": a release refuses it.
The assets' notices stay in assets/NOTICES.txt (ps5/tools/build-assets.py).

Copyright (C) 2026 Mihawk
SPDX-License-Identifier: MIT
"""
import json
import re
import shutil
import subprocess
import sys
from pathlib import Path

PS5 = Path(__file__).resolve().parent.parent
ROOT = PS5.parent


def git(repo, *args):
    return subprocess.run(["git", "-C", str(repo), *args], capture_output=True, text=True, check=True).stdout.strip()


def revision(repo):
    """The repository's HEAD, its remote, and whether its tracked files differ from it."""
    rev = git(repo, "rev-parse", "HEAD")
    dirty = bool(git(repo, "status", "--porcelain", "--untracked-files=no"))
    remotes = git(repo, "remote").split()
    name = "origin" if "origin" in remotes else (remotes[0] if remotes else None)
    remote = git(repo, "remote", "get-url", name) if name else "(no remote yet)"
    return rev, dirty, remote.removesuffix(".git")


def pinned(script, name):
    """A revision a setup script pins ("name=<sha>")."""
    m = re.search(rf"^{name}=([0-9a-f]{{40}})", (PS5 / script).read_text(), re.M)
    return m.group(1) if m else None


def write_text(dest, repo, rev, path):
    """A file of a repository at a revision, into the licenses folder."""
    dest.parent.mkdir(parents=True, exist_ok=True)
    data = subprocess.run(["git", "-C", str(repo), "show", f"{rev}:{path}"], capture_output=True, check=True).stdout
    dest.write_bytes(data)


def write_tree(dest, repo, rev, path):
    """A folder of a repository at a revision (Mesa's licenses/)."""
    dest.mkdir(parents=True, exist_ok=True)
    tar = subprocess.run(["git", "-C", str(repo), "archive", rev, path], capture_output=True, check=True).stdout
    subprocess.run(["tar", "-x", "-C", str(dest), "--strip-components", str(len(Path(path).parts))], input=tar, check=True)


LEGAL = """{name} - legal use
{rule}

PIRACY IS NOT CONDONED. This title contains no games, no BIOS files, no
console firmware and no decryption keys, and none will ever be provided or
linked to. Use only games you own and files from hardware you own.
Downloading or sharing games, BIOS files, firmware or keys you do not own is
piracy: do not do it, and do not ask for help with it.

Licences: the title's own code and Sascha Willems' Vulkan examples are MIT.
It links the PS5 platform layer{kit}, which {are} GPL-3.0-or-later, so the
title as distributed is under GPL-3.0-or-later as a whole, and its complete
source is offered with it. Every part, its licence and its source are listed
in licenses/README.txt; the assets and theirs in assets/NOTICES.txt.

Not affiliated with or endorsed by Sony Interactive Entertainment.
"PlayStation" and "PS5" are trademarks of Sony Interactive Entertainment Inc.
Vulkan is a registered trademark of the Khronos Group Inc.; the RADV port is
not a conformant product (its conformanceVersion is 0.0.0.0).
"""


def main():
    app, vulkan, archive = (Path(a).resolve() for a in sys.argv[1:4])
    name = json.loads((PS5 / "sce_sys/param.json").read_text())["localizedParameters"]["en-US"]["titleName"]
    out = app / "licenses"
    shutil.rmtree(out, ignore_errors=True)
    out.mkdir(parents=True)
    parts = []

    # The title: its own code, the foundation and the libraries in its tree
    rev, dirty, remote = revision(ROOT)
    write_text(out / "title/LICENSE.md", ROOT, rev, "LICENSE.md")
    try:
        write_text(out / "title/MIT-VulkanExamples.md", ROOT, rev, "ps5/licenses/MIT-VulkanExamples.md")
    except subprocess.CalledProcessError:  # not committed yet: a development build, not a release
        shutil.copy2(PS5 / "licenses/MIT-VulkanExamples.md", out / "title/MIT-VulkanExamples.md")
    libraries = [("external/imgui/LICENSE.txt", "imgui-LICENSE.txt"), ("external/ktx/LICENSE.md", "ktx-LICENSE.md"),
                 ("external/ktx/NOTICE.md", "ktx-NOTICE.md"), ("external/tinygltf/LICENSE", "tinygltf-LICENSE"),
                 ("ps5/third_party/volk/LICENSE.md", "volk-LICENSE.md")]
    for path, dest in libraries:
        write_text(out / "title" / dest, ROOT, rev, path)
    glm = git(ROOT, "ls-tree", rev, "external/glm").split()[2]
    parts.append(dict(
        id="title", name=f"{name}: its program, the foundation (Sascha Willems' Vulkan examples and base class, the PS5 layer) "
                         "and the libraries in its tree (Dear ImGui, libktx, tinygltf with stb_image and nlohmann json, "
                         "volk, the Vulkan headers, glm)",
        licence="MIT (the title, the examples, Dear ImGui, tinygltf, volk, glm); Apache-2.0 (libktx); "
                "Apache-2.0 OR MIT (the Vulkan headers)",
        copyright=["Copyright (c) 2016-2026 Sascha Willems", "Copyright (C) 2026 Mihawk", "and each library's authors"],
        artifacts=["eboot.bin", "shaders/"], texts="licenses/title/",
        source=dict(kind="git", path=str(ROOT), remote=remote, revision=rev, dirty=dirty,
                    submodules={"external/glm": dict(remote="https://github.com/g-truc/glm", revision=glm)})))

    # The UI kit, in a title that has the module
    if (PS5 / "ui/setup-kit.sh").exists():
        kit = Path(ROOT.parent / "PS5_VKHomebrewUI")
        kit_rev = pinned("ui/setup-kit.sh", "revision")
        write_text(out / "ui-kit/LICENSE", kit, kit_rev, "LICENSE")
        parts.append(dict(
            id="ui-kit", name="the UI kit: BlackBearReloaded's ps5-homebrew-ui with my Vulkan backend (PS5_VKHomebrewUI), "
                              "and the UI module that draws with it (ps5/ui/)",
            licence="GPL-3.0-or-later", copyright=["Copyright (C) 2026 BlackBearReloaded", "Copyright (C) 2026 Mihawk"],
            artifacts=["eboot.bin", "assets/hui/"], texts="licenses/ui-kit/",
            source=dict(kind="git", path=str(kit), remote="https://github.com/mihawk-99/PS5_VKHomebrewUI", revision=kit_rev,
                        dirty=False)))

    # The payload SDK fork: its platform layer and headers
    sdk = Path(ROOT.parent / "PS5_PayloadSDK")
    sdk_rev = pinned("tools/setup-sdk.sh", "sdk_revision")
    write_text(out / "platform/GPL-3.0.txt", sdk, sdk_rev, "LICENSE")
    write_text(out / "platform/musl-regex-COPYRIGHT.txt", sdk, sdk_rev, "platform/src/regex/COPYRIGHT.musl")
    write_text(out / "platform/dlmalloc-SOURCE.txt", sdk, sdk_rev, "platform/src/dlmalloc/SOURCE")
    parts.append(dict(
        id="platform", name="the PS5 platform layer (libps5platform.a) and the SDK headers, from my payload SDK fork, "
                            "with musl's regex and dlmalloc",
        licence="GPL-3.0-or-later; musl's regex MIT; dlmalloc public domain",
        copyright=["Copyright (C) John Törnblom and the ps5-payload-dev contributors", "Copyright (C) 2026 Mihawk"],
        artifacts=["eboot.bin"], texts="licenses/platform/",
        source=dict(kind="git", path=str(sdk), remote="https://github.com/mihawk-99/PS5_PayloadSDK", revision=sdk_rev,
                    dirty=False)))

    # PS5_Vulkan: the link recipe, the CRT and its startup, the AGC imports, libc.prx
    vrev, vdirty, vremote = revision(vulkan)
    write_text(out / "ps5-vulkan/GPL-3.0.txt", vulkan, vrev, "LICENSE")
    parts.append(dict(
        id="ps5-vulkan", name="PS5_Vulkan: the RADV link recipe, the C runtime start-up, the AGC import stubs and "
                              "sce_module/libc.prx",
        licence="GPL-3.0-or-later", copyright=["Copyright (C) 2026 Mihawk"],
        artifacts=["eboot.bin", "sce_module/libc.prx"], texts="licenses/ps5-vulkan/",
        source=dict(kind="git", path=str(vulkan), remote=vremote, revision=vrev, dirty=vdirty)))

    # RADV: Mesa's Vulkan driver, from my PS5_Mesa fork, built by PS5_Vulkan
    provenance = (archive.parent.parent / "PROVENANCE.txt").read_text()
    mesa_rev = re.search(r"^revision: ([0-9a-f]{40})", provenance, re.M).group(1)
    radv_sdk = re.search(r"^sdk: ([0-9a-f]{40})", provenance, re.M)
    mesa = Path(ROOT.parent / "PS5_Mesa")
    write_text(out / "radv/Mesa-license.rst", mesa, mesa_rev, "docs/license.rst")
    write_tree(out / "radv/licenses", mesa, mesa_rev, "licenses")
    parts.append(dict(
        id="radv", name="RADV, Mesa's Vulkan driver (with ACO, NIR and Mesa's Vulkan runtime), from my PS5_Mesa fork",
        licence="MIT, with other licences stated per file (licenses/radv/Mesa-license.rst)",
        copyright=["Copyright the Mesa contributors (per-file notices in the source)"],
        artifacts=["eboot.bin"], texts="licenses/radv/",
        source=dict(kind="git", path=str(mesa), remote="https://github.com/mihawk-99/PS5_Mesa", revision=mesa_rev,
                    dirty=False, built_with_sdk=radv_sdk.group(1) if radv_sdk else None)))

    # The LLVM runtime the SDK links in
    llvm = ROOT.parent / "PS5_LLVM/llvm/LICENSE.TXT"
    if llvm.exists():
        (out / "llvm-runtime").mkdir()
        shutil.copy2(llvm, out / "llvm-runtime/LICENSE.TXT")
    parts.append(dict(
        id="llvm-runtime", name="LLVM libc++, libc++abi, libunwind and compiler-rt builtins (linked into eboot.bin)",
        licence="Apache-2.0 WITH LLVM-exception",
        copyright=["Copyright (c) 2003-2026 University of Illinois at Urbana-Champaign and the LLVM project contributors"],
        artifacts=["eboot.bin"], texts="licenses/llvm-runtime/",
        source=dict(kind="fixed", revision="ps5-payload-dev SDK v0.42 release archives",
                    url="https://github.com/ps5-payload-dev/sdk/releases/tag/v0.42")))

    # The https tunnel (ps5/src/jf/jf_tls.cpp): Mbed TLS, and Mozilla's root certificates
    def licence_text(dest, path):
        dest.parent.mkdir(parents=True, exist_ok=True)
        try:
            write_text(dest, ROOT, rev, path)
        except subprocess.CalledProcessError:  # not committed yet: a development build, not a release
            shutil.copy2(ROOT / path, dest)

    licence_text(out / "mbedtls/LICENSE.txt", "ps5/licenses/mbedtls-LICENSE.txt")
    parts.append(dict(
        id="mbedtls", name="Mbed TLS 3.6.7, the TLS client behind https (built by ps5/tools/build-mbedtls.sh)",
        licence="Apache-2.0 (Mbed TLS is Apache-2.0 OR GPL-2.0-or-later; it is used here under Apache-2.0)",
        copyright=["Copyright The Mbed TLS Contributors"],
        artifacts=["eboot.bin"], texts="licenses/mbedtls/",
        source=dict(kind="fixed", revision="Mbed TLS 3.6.7 release tarball, sha256 "
                                           "a7e8bcbec0e6f761b4af24f25677626b35f762f68eef79c08677a363212d11f6",
                    url="https://github.com/Mbed-TLS/mbedtls/releases/tag/mbedtls-3.6.7")))
    licence_text(out / "mozilla-ca/MPL-2.0.txt", "ps5/licenses/MPL-2.0.txt")
    parts.append(dict(
        id="mozilla-ca", name="Mozilla's list of trusted root certificates (data, from the certifi 2026.7.22 copy), "
                              "embedded as ps5/src/jf/jf_ca_bundle.inc",
        licence="MPL-2.0", copyright=["Copyright the Mozilla Foundation and the contributors to its root programme"],
        artifacts=["eboot.bin"], texts="licenses/mozilla-ca/",
        source=dict(kind="fixed", revision="certifi 2026.7.22 (cacert.pem); the embedded copy is in the title's source",
                    url="https://github.com/certifi/python-certifi")))

    (out / "components.json").write_text(json.dumps(parts, indent=1, ensure_ascii=False) + "\n")
    has_kit = any(p["id"] == "ui-kit" for p in parts)
    legal = LEGAL.format(name=name, rule="=" * (len(name) + 12), kit=" and the UI kit" if has_kit else "",
                         are="are" if has_kit else "is")
    (app / "LEGAL.txt").write_text(legal)
    lines = [legal, "", "The parts of this title", "=======================", ""]
    for p in parts:
        src = p["source"]
        where = f'{src["remote"]} at {src["revision"]}' if src["kind"] == "git" else f'{src["revision"]}: {src["url"]}'
        lines += [p["name"], f'  licence:   {p["licence"]}', f'  copyright: {"; ".join(p["copyright"])}',
                  f'  in:        {", ".join(p["artifacts"])}', f'  texts:     {p["texts"]}', f'  source:    {where}', ""]
    lines += ["The assets: assets/NOTICES.txt, with their licence texts in assets/LICENSES/."]
    (out / "README.txt").write_text("\n".join(lines) + "\n")
    dirty = [p["id"] for p in parts if p["source"].get("dirty")]
    print(f"==> notices: LEGAL.txt, licenses/ ({len(parts)} parts{'; dirty: ' + ', '.join(dirty) if dirty else ''})")


if __name__ == "__main__":
    main()
