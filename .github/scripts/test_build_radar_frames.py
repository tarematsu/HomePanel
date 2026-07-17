#!/usr/bin/env python3
"""Focused tests for the radar frame builder."""

from __future__ import annotations

import importlib.util
import sys
import unittest
import urllib.error
from pathlib import Path
from unittest.mock import patch

SCRIPT_PATH = Path(__file__).with_name("build-radar-frames.py")
SPEC = importlib.util.spec_from_file_location("build_radar_frames", SCRIPT_PATH)
if SPEC is None or SPEC.loader is None:
    raise RuntimeError(f"failed to load {SCRIPT_PATH}")
MODULE = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = MODULE
SPEC.loader.exec_module(MODULE)


class RadarTileFetchTests(unittest.TestCase):
    def test_missing_radar_tile_is_transparent(self) -> None:
        error = urllib.error.HTTPError(
            "https://example.invalid/tile.png",
            404,
            "Not Found",
            hdrs=None,
            fp=None,
        )
        with patch.object(MODULE.urllib.request, "urlopen", side_effect=error):
            image = MODULE.fetch_radar_tile("https://example.invalid/tile.png")

        self.assertEqual(image.mode, "RGBA")
        self.assertEqual(image.size, (MODULE.TILE_SIZE, MODULE.TILE_SIZE))
        self.assertIsNone(image.getbbox())

    def test_non_optional_404_remains_an_error(self) -> None:
        error = urllib.error.HTTPError(
            "https://example.invalid/target-times.json",
            404,
            "Not Found",
            hdrs=None,
            fp=None,
        )
        with patch.object(MODULE.urllib.request, "urlopen", side_effect=error):
            with self.assertRaisesRegex(RuntimeError, "HTTP Error 404"):
                MODULE.fetch_bytes("https://example.invalid/target-times.json", attempts=1)


class RadarFrameKeyTests(unittest.TestCase):
    def test_frame_key_changes_when_forecast_cycle_changes(self) -> None:
        render_variant = "gha-v4-0123456789abcdef"
        earlier = MODULE.TimeEntry(
            base_time="20260717190000",
            valid_time="20260717193000",
            valid_at=MODULE.jma_timestamp_ms("20260717193000"),
        )
        later = MODULE.TimeEntry(
            base_time="20260717190500",
            valid_time="20260717193000",
            valid_at=MODULE.jma_timestamp_ms("20260717193000"),
        )

        earlier_record = MODULE.frame_record(earlier, render_variant)
        later_record = MODULE.frame_record(later, render_variant)

        self.assertNotEqual(earlier_record["key"], later_record["key"])
        self.assertNotEqual(earlier_record["url"], later_record["url"])
        self.assertEqual(earlier_record["variant"], f"{render_variant}-{earlier.base_time}")
        self.assertEqual(later_record["variant"], f"{render_variant}-{later.base_time}")

    def test_frame_key_is_stable_for_same_forecast_cycle(self) -> None:
        render_variant = "gha-v4-0123456789abcdef"
        entry = MODULE.TimeEntry(
            base_time="20260717190000",
            valid_time="20260717193000",
            valid_at=MODULE.jma_timestamp_ms("20260717193000"),
        )

        first = MODULE.frame_record(entry, render_variant)
        second = MODULE.frame_record(entry, render_variant)

        self.assertEqual(first, second)
        self.assertRegex(first["key"], MODULE.FRAME_KEY_PATTERN)


if __name__ == "__main__":
    unittest.main()
