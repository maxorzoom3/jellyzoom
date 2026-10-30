#!/usr/bin/env python3
"""Verify how release notices identify GLM source."""
import importlib.util
import unittest
from pathlib import Path
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("stage_notices", ROOT / "ps5/tools/stage-notices.py")
stage_notices = importlib.util.module_from_spec(spec)
spec.loader.exec_module(stage_notices)


class ReleaseProvenanceTests(unittest.TestCase):
    def test_vendored_glm_is_part_of_title_source(self):
        with patch.object(stage_notices, "git", return_value="040000 tree abc123\texternal/glm"):
            self.assertEqual(stage_notices.title_submodules("HEAD"), {})

    def test_glm_submodule_is_archived_separately(self):
        with patch.object(stage_notices, "git", return_value="160000 commit abc123\texternal/glm"):
            self.assertEqual(stage_notices.title_submodules("HEAD"), {
                "external/glm": {"remote": "https://github.com/g-truc/glm", "revision": "abc123"}
            })

    def test_missing_or_unexpected_glm_fails(self):
        for entry in ("", "100644 blob abc123\texternal/glm"):
            with self.subTest(entry=entry), patch.object(stage_notices, "git", return_value=entry):
                with self.assertRaises(ValueError):
                    stage_notices.title_submodules("HEAD")

    def test_actual_checkout_vendors_glm(self):
        self.assertEqual(stage_notices.title_submodules("HEAD"), {})


if __name__ == "__main__":
    unittest.main()
