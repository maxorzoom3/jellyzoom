#!/usr/bin/env python3
"""Check Jellyzoom's title identity, artwork and independent console storage."""
import json
import re
import struct
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


class BrandingTests(unittest.TestCase):
    def test_title_identity(self):
        param = json.loads((ROOT / "ps5/sce_sys/param.json").read_text())
        self.assertEqual(param["titleId"], "PPSA99483")
        self.assertEqual(param["conceptId"], "99483")
        self.assertEqual(param["contentId"], "UP9000-PPSA99483_00-JELLYZOOM0000000")
        self.assertEqual(param["localizedParameters"]["en-US"]["titleName"], "Jellyzoom")
        config = (ROOT / "ps5/src/jf/jf_config.h").read_text()
        self.assertIn('#define JF_CLIENT_NAME "Jellyzoom"', config)

    def test_logo_assets(self):
        icon = (ROOT / "ps5/sce_sys/icon0.png").read_bytes()
        self.assertEqual(icon, (ROOT / "docs/media/icon.png").read_bytes())
        embedded = (ROOT / "examples/jellyfinwheel/jf_brand_png.inc").read_text()
        logo = bytes(int(value, 16) for value in re.findall(r"0x([0-9a-f]{2})", embedded))
        for png in (icon, logo):
            self.assertEqual(png[:8], b"\x89PNG\r\n\x1a\n")
            self.assertEqual(struct.unpack(">II", png[16:24]), (512, 512))
        self.assertEqual(logo[25], 6)  # RGBA, so the in-app logo has an alpha channel.

    def test_no_mortalfin_storage_paths(self):
        for directory in (ROOT / "ps5/src", ROOT / "examples/jellyfinwheel"):
            for path in directory.rglob("*"):
                if path.suffix not in {".c", ".cpp", ".h", ".hpp"}:
                    continue
                with self.subTest(path=path.relative_to(ROOT)):
                    source = path.read_text(encoding="utf-8")
                    self.assertNotIn("/data/jf-", source)
                    self.assertNotIn('"/jf-', source)

        tls = (ROOT / "ps5/src/jf/jf_tls.cpp").read_text()
        for name in ("hosts", "dns", "ca.pem", "tls-insecure"):
            self.assertIn(f'"/jellyzoom-{name}"', tls)
        identity = (ROOT / "ps5/src/jf/jf_identity.c").read_text()
        self.assertIn('"/data/jellyzoom-device-id"', identity)


if __name__ == "__main__":
    unittest.main()
