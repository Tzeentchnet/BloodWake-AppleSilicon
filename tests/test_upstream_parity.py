import json
from pathlib import Path
import shutil
import tempfile
import unittest

from tools import check_upstream_parity


ROOT = Path(__file__).resolve().parents[1]
MANIFEST = ROOT / "fingerprints" / "upstream_baseline.json"


class UpstreamParityTests(unittest.TestCase):
    def test_recorded_baseline(self):
        result = check_upstream_parity.check(ROOT, MANIFEST)
        self.assertTrue(result["ok"], result["errors"])
        self.assertEqual(result["registry"]["entry_count"], 42)
        self.assertEqual(len(result["classified_commits"]), 2)
        self.assertFalse(result["classified_commits"][0]["merged"])
        self.assertFalse(result["classified_commits"][1]["merged"])

    def test_unknown_settings_fix_is_rejected(self):
        with tempfile.TemporaryDirectory() as temp:
            fixture = Path(temp)
            (fixture / "src").mkdir()
            (fixture / "src" / "gpu").mkdir()
            manifest = json.loads(MANIFEST.read_text(encoding="utf-8"))
            shutil.copy2(ROOT / "src" / "bw_fixes.def", fixture / "src" / "bw_fixes.def")
            settings = (ROOT / "src" / "bw_settings.c").read_text(encoding="utf-8")
            (fixture / "src" / "bw_settings.c").write_text(
                settings + '\nstatic const char *bad_fix = "fix:+NOT_REGISTERED";\n',
                encoding="utf-8",
            )
            for source in manifest["native_fix_sources"]:
                target = fixture / source
                target.parent.mkdir(parents=True, exist_ok=True)
                if not target.exists():
                    target.write_text("/* synthetic fixture */\n", encoding="utf-8")
            (fixture / "CMakeLists.txt").write_text(
                "\n".join(manifest["native_fix_sources"]) + "\n",
                encoding="utf-8",
            )
            result = check_upstream_parity.check(fixture, MANIFEST)
            self.assertFalse(result["ok"])
            self.assertIn(
                "settings reference unregistered fixes: NOT_REGISTERED",
                result["errors"],
            )


if __name__ == "__main__":
    unittest.main()
