import os
from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
HOOKS = ROOT / "tools" / "git-hooks"


class CommitGuardTests(unittest.TestCase):
    def test_git_runs_guard_and_blocks_disc_images(self):
        self.assertTrue(os.access(HOOKS / "pre-commit", os.X_OK))
        with tempfile.TemporaryDirectory() as temp:
            repo = Path(temp)

            def git(*args):
                return subprocess.run(
                    ["git", "-C", str(repo), *args],
                    capture_output=True, text=True, check=True,
                )

            git("init", "--quiet")
            git("config", "core.hooksPath", str(HOOKS))
            git("config", "user.name", "Guard Test")
            git("config", "user.email", "guard-test@example.invalid")
            (repo / "safe.txt").write_text("safe test fixture\n", encoding="utf-8")
            git("add", "safe.txt")
            subprocess.run([str(HOOKS / "pre-commit")], cwd=repo, check=True)

            (repo / "dummy.iso").write_bytes(b"synthetic test fixture")
            git("add", "dummy.iso")
            (repo / "build").mkdir()
            (repo / "build" / "disassembly.json").write_text("synthetic test fixture", encoding="utf-8")
            git("add", "build/disassembly.json")
            commit = subprocess.run(
                ["git", "-C", str(repo), "commit", "-m", "Test guard",
                 "-m", "Co-authored-by: Copilot <223556219+Copilot@users.noreply.github.com>"],
                capture_output=True, text=True,
            )
            self.assertNotEqual(commit.returncode, 0)
            self.assertIn("pre-commit: BLOCKED dummy.iso", commit.stderr)
            self.assertIn("pre-commit: BLOCKED build/disassembly.json", commit.stderr)


if __name__ == "__main__":
    unittest.main()
