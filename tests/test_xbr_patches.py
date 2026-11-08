import importlib.util
from pathlib import Path
import subprocess
import sys
import types
import unittest
from unittest import mock


ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location(
    "bloodwake_xbr_patches", ROOT / "tools" / "xbr_patches.py"
)
xbr_patches = importlib.util.module_from_spec(spec)
spec.loader.exec_module(xbr_patches)


class XbrPatchTests(unittest.TestCase):
    def test_v017_keeps_only_fcmov_and_narrow_division_fixes(self):
        self.assertEqual([fix["id"] for fix in xbr_patches.FIXES], ["TK-2", "TK-33"])
        self.assertEqual(
            set(xbr_patches._TEMPLATES),
            {("div", 1), ("div", 2), ("idiv", 1), ("idiv", 2)},
        )

    def test_narrow_division_emission_and_upstream_multiply_fallback(self):
        lifter = types.ModuleType("tools.recomp.lifter")
        lifter._operand_width = lambda operand: operand.width
        lifter._fmt_operand_read = lambda operand: operand.source
        recomp = types.ModuleType("tools.recomp")
        recomp.lifter = lifter
        tools = types.ModuleType("tools")
        tools.recomp = recomp
        modules = {
            "tools": tools,
            "tools.recomp": recomp,
            "tools.recomp.lifter": lifter,
        }
        byte = types.SimpleNamespace(width=1, source="LO8(ebx)")
        word = types.SimpleNamespace(width=2, source="MEM16(ecx)")
        with mock.patch.dict(sys.modules, modules):
            div8 = "\n".join(xbr_patches._narrow("div", [byte]))
            idiv16 = "\n".join(xbr_patches._narrow("idiv", [word]))
            self.assertIsNone(xbr_patches._narrow("mul", [word]))
            self.assertIsNone(xbr_patches._narrow("imul", [byte]))
        self.assertIn("eax & 0xFFFFu", div8)
        self.assertIn("AL = AX / src", div8)
        self.assertIn("(int32_t)(((edx & 0xFFFFu) << 16)", idiv16)
        self.assertIn("AX = DX:AX / src", idiv16)

    def test_fcmov_is_presented_to_upstream_carry_scan_as_cmov(self):
        class Translator:
            @staticmethod
            def _function_needs_cf(instructions):
                return any(insn.mnemonic.startswith("cmov") for insn in instructions)

        translator = types.ModuleType("tools.recomp.translator")
        translator.FunctionTranslator = Translator
        recomp = types.ModuleType("tools.recomp")
        recomp.translator = translator
        tools = types.ModuleType("tools")
        tools.recomp = recomp
        modules = {
            "tools": tools,
            "tools.recomp": recomp,
            "tools.recomp.translator": translator,
        }
        original = Translator.__dict__["_function_needs_cf"].__func__
        instruction = types.SimpleNamespace(mnemonic="fcmovnb", op_str="st, st(1)")
        with mock.patch.dict(sys.modules, modules):
            xbr_patches._tk2_apply(original)
            self.assertTrue(Translator._function_needs_cf([instruction]))

    @unittest.skipUnless((ROOT / "build" / "xboxrecomp" / ".git").exists(),
                         "pinned xboxrecomp checkout is unavailable")
    def test_guards_apply_to_exact_v017_checkout(self):
        toolkit = ROOT / "build" / "xboxrecomp"
        head = subprocess.run(
            ["git", "-C", str(toolkit), "rev-parse", "HEAD"],
            check=True, capture_output=True, text=True,
        ).stdout.strip()
        self.assertEqual(head, "343b6d8901723b37ce568b2418c5df4e1fe1d223")
        result = subprocess.run(
            [sys.executable, str(ROOT / "tools" / "xbr_patches.py"),
             "--toolkit", str(toolkit), "list"],
            check=True, capture_output=True, text=True,
        )
        self.assertIn("applied TK-2", result.stdout)
        self.assertIn("applied TK-33", result.stdout)
        self.assertNotIn("TK-33b", result.stdout)


if __name__ == "__main__":
    unittest.main()
