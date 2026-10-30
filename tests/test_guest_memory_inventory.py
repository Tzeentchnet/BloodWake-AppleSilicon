import contextlib
import copy
import io
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest import mock

from tools import guest_memory_inventory as inv


ROOT = Path(__file__).resolve().parents[1]


class ScannerTests(unittest.TestCase):
    def scan(self, source):
        return inv.scan_source(source.encode(), "project", "fixture.c")

    def selected(self, records, category, symbol):
        return [r for r in records if r["category"] == category and r["symbol"] == symbol]

    def test_lvalue_writes_reads_rmw_escapes_and_multiline(self):
        records = self.scan("""
// MEM8(commented) = 1;
const char *s = "MEM32(string) = 1";
MEM8(a) = 1;
(MEM16(a + (b))) += 2;
++MEM32(a);
MEMF(a)--;
v = MEMD(a);
p = &((MEM32(a)));
MUTATE(MEM8(a), 1);
MEM32(
  a + b
) = 9;
if (MEM8(a) == 1) v++;
""")
        scalar = [r for r in records if r["category"] == "scalar"]
        self.assertEqual([r["operation"] for r in scalar], [
            "write", "read_modify_write", "read_modify_write", "read_modify_write",
            "read_candidate", "address_escape", "unknown_lvalue_context", "write",
            "unknown_lvalue_context"])
        self.assertEqual([r["width_bytes"] for r in scalar], [1, 2, 4, 4, 8, 4, 1, 4, 1])
        self.assertEqual(scalar[1]["span"], "fixed_may_cross")
        self.assertEqual(scalar[7]["line"], 11)

    def test_macro_discovery_and_pointer_origins(self):
        records = self.scan("""
#define GP(a) ((void *)((uintptr_t)(a) + g_xbox_mem_offset))
#define GU32(a) (*(volatile uint32_t *)GP(a))
GU32(x) = 3;
v = GU32(y);
*(float *)((uintptr_t)x + g_xbox_mem_offset) = 1.0f;
uint8_t *p = GP(x);
p[2] = 4;
""")
        use = [r for r in self.selected(records, "scalar", "GU32")
               if r.get("context") != "macro_definition"]
        self.assertEqual([r["operation"] for r in use], ["write", "read_candidate"])
        self.assertTrue(all(r["width_bytes"] == 4 for r in use))
        self.assertTrue(self.selected(records, "pointer_origin", "GP"))
        self.assertFalse(self.selected(records, "scalar", "GP"))
        self.assertTrue(self.selected(records, "pointer_dereference", "*"))
        self.assertTrue(self.selected(records, "subscript", "p"))

    def test_bulk_vectors_atomics_stack_and_unknown(self):
        records = self.scan("""
memcpy(dst, src, 16);
memmove(dst, src, count);
strcpy(dst, src);
XMM_MEM(va); XMM_STORE(va, x); XMM_LOAD_LOW(x, va);
MMX_MEM(va); MMX_STORE(va, x);
PUSH32(esp, value); POP32(esp, value);
__atomic_fetch_add(&MEM32(va), 1, 0);
_mm_loadu_si128(p);
MYSTERY_MEM128(va);
""")
        copy = self.selected(records, "bulk_string", "memcpy")[0]
        self.assertEqual((copy["operation"], copy["width_bytes"], copy["span"]),
                         ("copy", 16, "fixed_may_cross"))
        self.assertEqual(self.selected(records, "bulk_string", "memmove")[0]["span"],
                         "dynamic_or_terminated")
        self.assertEqual(self.selected(records, "bulk_string", "strcpy")[0]["span"],
                         "dynamic_or_terminated")
        self.assertEqual(self.selected(records, "vector", "XMM_MEM")[0]["width_bytes"], 16)
        self.assertEqual(self.selected(records, "vector", "XMM_STORE")[0]["operation"], "write")
        self.assertEqual(self.selected(records, "vector", "XMM_LOAD_LOW")[0]["width_bytes"], 8)
        self.assertEqual(self.selected(records, "vector", "MMX_MEM")[0]["width_bytes"], 8)
        self.assertEqual(self.selected(records, "vector", "MMX_STORE")[0]["operation"], "write")
        self.assertEqual(self.selected(records, "stack", "PUSH32")[0]["operation"], "write")
        self.assertEqual(self.selected(records, "stack", "POP32")[0]["operation"], "read")
        self.assertEqual(self.selected(records, "atomic", "__atomic_fetch_add")[0]["operation"],
                         "unknown_atomic")
        self.assertEqual(self.selected(records, "scalar", "MEM32")[0]["operation"], "address_escape")
        self.assertTrue(self.selected(records, "vector_intrinsic", "_mm_loadu_si128"))
        self.assertTrue(self.selected(records, "unrecognized", "MYSTERY_MEM128"))

    def test_typed_pointer_dereference_direction_and_width(self):
        records = self.scan("""
*(volatile uint32_t *)((uintptr_t)base + va) = 2;
v = *(const float *)XBOX_PTR(va);
(*(uint16_t *)p) += 1;
v = *(uint8_t *)p++;
""")
        derefs = self.selected(records, "pointer_dereference", "*")
        self.assertEqual([(r["operation"], r["width_bytes"]) for r in derefs],
                         [("write", 4), ("read_candidate", 4),
                          ("read_modify_write", 2), ("unknown", 1)])

    def test_constant_span_boundaries_and_wrap(self):
        records = self.scan("""
#define STACK_ARG(n) MEM32(g_esp + (n) * 4)
v = MEM32(0xfff);
XMM_STORE(0x3ffc, x);
MEM16(0xffffffffu) = x;
v = MEM8(dynamic);
memcpy((void *)XBOX_PTR(0xfffu), XBOX_PTR(0x3000), 8);
memcpy(XBOX_PTR(0xfff) + delta, src, 8);
v = STACK_ARG(0xfff);
""")
        scalar = [r for r in records if r["category"] == "scalar" and
                  r.get("context") != "macro_definition"]
        self.assertEqual(scalar[0]["constant_span"],
                         {"crosses_4k": True, "crosses_16k": False, "wraps_32bit": False})
        self.assertTrue(self.selected(records, "vector", "XMM_STORE")[0]["constant_span"]["crosses_16k"])
        self.assertTrue(scalar[1]["constant_span"]["wraps_32bit"])
        self.assertNotIn("constant_span", scalar[2])
        copies = self.selected(records, "bulk_string", "memcpy")
        self.assertEqual(len(copies[0]["constant_operands"]), 2)
        self.assertTrue(copies[0]["constant_operands"][0]["crosses_4k"])
        self.assertFalse(copies[0]["constant_operands"][1]["crosses_4k"])
        self.assertNotIn("constant_operands", copies[1])
        self.assertNotIn("constant_span", self.selected(records, "scalar", "STACK_ARG")[-1])

    def test_native_ocean_memory_macro_lvalues(self):
        records = self.scan("NMEMF(a) = 1; v = NSMEM64(b); NMEM32(c) |= 3;")
        scalar = [r for r in records if r["category"] == "scalar"]
        self.assertEqual([(r["symbol"], r["operation"], r["width_bytes"]) for r in scalar],
                         [("NMEMF", "write", 4), ("NSMEM64", "read_candidate", 8),
                          ("NMEM32", "read_modify_write", 4)])

    def test_incomplete_call_and_lexical_false_positives_are_visible(self):
        records = self.scan("MEM32(a;\n")
        self.assertEqual(records[0]["reason"], "unbalanced_call")
        self.assertTrue(self.selected(self.scan("memcpy(host, other, n);"), "bulk_string", "memcpy"))
        self.assertFalse(self.scan("/* MEM32(a) */ // memcpy(x,y,z)\n"))

    def test_array_filter_only_skips_known_host_x87_stack_in_generated(self):
        generated = inv.scan_source(b"g_fp_stack[i] = p[i];", "generated", "g.c")
        project = inv.scan_source(b"g_fp_stack[i] = p[i];", "project", "p.c")
        self.assertFalse(self.selected(generated, "subscript", "g_fp_stack"))
        self.assertTrue(self.selected(project, "subscript", "g_fp_stack"))
        self.assertTrue(self.selected(generated, "subscript", "p"))


class VersionTests(unittest.TestCase):
    def setUp(self):
        build = ROOT / "build"
        build.mkdir(exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(prefix="guest-inventory-test-", dir=build)
        self.addCleanup(self.temp.cleanup)
        root = Path(self.temp.name)
        self.project, self.gen, self.toolkit = (root / "project", root / "gen", root / "toolkit")
        (self.project / "src").mkdir(parents=True)
        self.gen.mkdir()
        (self.toolkit / "src").mkdir(parents=True)
        (self.project / "src" / "manual.c").write_text("MEM8(a) = 1;\n")
        (self.toolkit / "src" / "runtime.c").write_text("memcpy(a, b, n);\n")
        self.git("init", "-q")
        self.git("add", "src/runtime.c")
        self.git("-c", "user.name=Test", "-c", "user.email=test@example.invalid",
                 "commit", "-qm",
                 "synthetic fixture\n\nCo-authored-by: Copilot <223556219+Copilot@users.noreply.github.com>")
        self.commit = self.git("rev-parse", "HEAD")
        content = b"MEM32(a) = 1;\r\n"
        (self.gen / "recomp_0.c").write_bytes(content.replace(b"\r\n", b"\n"))
        files = {"recomp_0.c": {"size": len(content), "sha256": inv.sha(content)}}
        self.manifest = {"format": 1, "toolkit_commit": self.commit, "files": files,
                         "file_count": 1,
                         "total_sha256": inv.tree_digest({k: v["sha256"] for k, v in files.items()})}

    def git(self, *args):
        return subprocess.check_output(["git", "-C", str(self.toolkit), *args], text=True).strip()

    def run_inventory(self, expected=None, derived=None):
        return inv.inventory(self.project, self.gen, self.toolkit, self.manifest,
                             self.commit, expected, derived)

    def test_repeatable_fingerprinted_inventory_and_checked_project_version(self):
        one = self.run_inventory()
        two = self.run_inventory()
        self.assertEqual(one, two)
        self.assertEqual(one["generated_comparison"], "crlf-normalized")
        self.assertEqual(one["derived_artifacts"], "not_provided")
        self.assertEqual(one["project_version_check"], "snapshot_only")
        self.assertEqual({r["domain"] for r in one["sites"]}, {"generated", "project", "toolkit"})
        checked = self.run_inventory(one["project_source_digest"])
        self.assertEqual(checked["project_version_check"], "expected_digest")
        (self.project / "src" / "manual.c").write_text("MEM16(a) = 2;\n")
        with self.assertRaisesRegex(inv.InventoryError, "project source digest"):
            self.run_inventory(one["project_source_digest"])

    def test_explicit_derived_source_is_snapshotted_but_not_verified(self):
        derived = self.project / "ocean.inc"
        derived.write_text("NMEMF(a) = 1;\n")
        report = self.run_inventory(derived=[derived])
        self.assertEqual(report["derived_artifacts"], "provided")
        item = next(f for f in report["files"] if f["domain"] == "derived")
        self.assertEqual(item["verification"], "unverified_build_artifact")
        self.assertTrue(any(r["domain"] == "derived" and r["symbol"] == "NMEMF" and
                            r["operation"] == "write" for r in report["sites"]))
        with self.assertRaisesRegex(inv.InventoryError, "duplicate"):
            self.run_inventory(derived=[derived, derived])

    def test_generated_modified_missing_extra_and_unverified_build_artifact(self):
        (self.gen / "bw_ocean_native.inc").write_text("MEM8(a) = 1;\n")
        report = self.run_inventory()
        extra = next(f for f in report["files"] if f["path"] == "bw_ocean_native.inc")
        self.assertEqual(extra["verification"], "unverified_build_artifact")
        (self.gen / "extra.c").write_text("int x;\n")
        with self.assertRaisesRegex(inv.InventoryError, "file set"):
            self.run_inventory()
        (self.gen / "extra.c").unlink()
        (self.gen / "recomp_0.c").write_text("MEM16(a) = 2;\n")
        with self.assertRaisesRegex(inv.InventoryError, "contents differ"):
            self.run_inventory()
        (self.gen / "recomp_0.c").unlink()
        with self.assertRaisesRegex(inv.InventoryError, "file set"):
            self.run_inventory()

    def test_bad_manifest_and_toolkit_revision_or_dirty_tree(self):
        bad = copy.deepcopy(self.manifest)
        bad["total_sha256"] = "0" * 64
        with self.assertRaisesRegex(inv.InventoryError, "invalid generated"):
            inv.generated_sources(self.gen, bad, self.commit)
        with self.assertRaisesRegex(inv.InventoryError, "revision"):
            inv.verify_toolkit(self.toolkit, "0" * 40)
        (self.toolkit / "src" / "runtime.c").write_text("changed\n")
        with self.assertRaisesRegex(inv.InventoryError, "dirty"):
            self.run_inventory()
        (self.toolkit / "src" / "runtime.c").write_text("memcpy(a, b, n);\n")
        (self.toolkit / "src" / "extra.c").write_text("untracked\n")
        with self.assertRaisesRegex(inv.InventoryError, "dirty"):
            self.run_inventory()

    def test_detects_changes_during_scan(self):
        original = inv.scan_source
        changed = False

        def mutate(data, domain, name):
            nonlocal changed
            if not changed:
                (self.project / "src" / "manual.c").write_text("changed\n")
                changed = True
            return original(data, domain, name)

        with mock.patch.object(inv, "scan_source", side_effect=mutate):
            with self.assertRaisesRegex(inv.InventoryError, "changed during"):
                self.run_inventory()

    def test_pinned_blobs_catch_ignored_or_hidden_changes(self):
        self.git("update-index", "--assume-unchanged", "src/runtime.c")
        (self.toolkit / "src" / "runtime.c").write_text("changed behind git status\n")
        with self.assertRaisesRegex(inv.InventoryError, "source bytes"):
            self.run_inventory()
        self.git("update-index", "--no-assume-unchanged", "src/runtime.c")
        (self.toolkit / "src" / "runtime.c").write_text("memcpy(a, b, n);\n")
        (self.toolkit / ".git" / "info" / "exclude").write_text("ignored.c\n")
        (self.toolkit / "src" / "ignored.c").write_text("int ignored;\n")
        with self.assertRaisesRegex(inv.InventoryError, "source file set"):
            self.run_inventory()

    def test_cli_fails_without_emitting_partial_json(self):
        out, err = io.StringIO(), io.StringIO()
        with mock.patch.object(inv, "ROOT", self.project), \
             mock.patch.object(inv, "PUBLISHED_GENERATED", self.manifest["total_sha256"]), \
             contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            (self.project / "fingerprints").mkdir()
            (self.project / "fingerprints" / "generated.json").write_text(json.dumps(self.manifest))
            code = inv.main(["--gen", str(self.gen), "--toolkit", str(self.toolkit)])
        self.assertEqual(code, 1)
        self.assertEqual(out.getvalue(), "")
        self.assertIn("revision", err.getvalue())

    def test_cli_accepts_checked_project_and_derived_source(self):
        baseline = self.run_inventory()["project_source_digest"]
        derived = self.project / "ocean.inc"
        derived.write_text("NMEM32(a) = 1;\n")
        (self.project / "fingerprints").mkdir()
        (self.project / "fingerprints" / "generated.json").write_text(json.dumps(self.manifest))
        out, err = io.StringIO(), io.StringIO()
        with mock.patch.object(inv, "ROOT", self.project), \
             mock.patch.object(inv, "PUBLISHED_GENERATED", self.manifest["total_sha256"]), \
             mock.patch.object(inv, "PINNED_TOOLKIT", self.commit), \
             contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = inv.main(["--gen", str(self.gen), "--toolkit", str(self.toolkit),
                             "--derived", str(derived), "--expect-project-sha256", baseline])
        report = json.loads(out.getvalue())
        self.assertEqual(code, 0)
        self.assertEqual(err.getvalue(), "")
        self.assertEqual(report["project_version_check"], "expected_digest")
        self.assertEqual(report["derived_artifacts"], "provided")
        self.assertIn("tool_sha256", report)

    @unittest.skipUnless(hasattr(os, "symlink"), "symlinks unavailable")
    def test_rejects_source_symlinks(self):
        os.symlink(self.project / "src" / "manual.c", self.project / "src" / "alias.c")
        with self.assertRaisesRegex(inv.InventoryError, "symlink"):
            self.run_inventory()

    @unittest.skipUnless(hasattr(os, "symlink"), "symlinks unavailable")
    def test_rejects_source_directory_symlinks(self):
        os.symlink(self.project / "src", self.project / "src" / "alias",
                   target_is_directory=True)
        with self.assertRaisesRegex(inv.InventoryError, "symlink"):
            self.run_inventory()

    @unittest.skipUnless(hasattr(os, "symlink"), "symlinks unavailable")
    def test_rejects_symlink_as_source_root(self):
        source = self.project / "src"
        target = self.project / "actual-src"
        source.rename(target)
        os.symlink(target, source, target_is_directory=True)
        with self.assertRaisesRegex(inv.InventoryError, "symlink"):
            self.run_inventory()

    @unittest.skipUnless(hasattr(os, "mkfifo"), "FIFOs unavailable")
    def test_rejects_nonregular_source_files(self):
        os.mkfifo(self.project / "src" / "pipe.c")
        with self.assertRaisesRegex(inv.InventoryError, "not a regular file"):
            self.run_inventory()

    def test_source_traversal_errors_are_not_silently_ignored(self):
        def inaccessible(_root, *, followlinks, onerror):
            self.assertFalse(followlinks)
            onerror(PermissionError("source directory inaccessible"))
            return iter(())

        with mock.patch.object(inv.os, "walk", side_effect=inaccessible):
            with self.assertRaisesRegex(PermissionError, "source directory inaccessible"):
                inv.read_sources(self.project / "src")


if __name__ == "__main__":
    unittest.main()
