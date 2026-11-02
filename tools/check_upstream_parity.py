#!/usr/bin/env python3
"""Check the recorded Blood Wake upstream/fix baseline without network access."""

import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_MANIFEST = ROOT / "fingerprints" / "upstream_baseline.json"


def _sha256(data):
    return hashlib.sha256(data).hexdigest()


def _git(root, *args):
    return subprocess.run(
        ["git", "-C", str(root), *args],
        capture_output=True,
        check=False,
    )


def _registry_entries(text):
    pattern = re.compile(
        r"^BW_FIX\(\s*([A-Z0-9_]+)\s*,\s*([^,]+)\s*,\s*([01])\s*,\s*"
        r"(BW_FIXKIND_[A-Z]+)\s*,",
        re.MULTILINE,
    )
    return [
        {"name": name, "editions": editions.strip(), "default": int(default), "kind": kind}
        for name, editions, default, kind in pattern.findall(text)
    ]


def _without_c_comments(text):
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.DOTALL)
    return re.sub(r"//[^\n]*", "", text)


def check(root=ROOT, manifest_path=DEFAULT_MANIFEST, require_upstream=False):
    root = Path(root)
    manifest_path = Path(manifest_path)
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    upstream = manifest["upstream"]
    errors = []
    warnings = []

    registry_path = root / upstream["fix_registry_path"]
    registry_bytes = registry_path.read_bytes()
    registry_text = registry_bytes.decode("utf-8")
    entries = _registry_entries(registry_text)
    names = [entry["name"] for entry in entries]
    duplicate_names = sorted({name for name in names if names.count(name) > 1})

    if _sha256(registry_bytes) != upstream["fix_registry_sha256"]:
        errors.append(
            f"{upstream['fix_registry_path']} does not match the recorded upstream hash"
        )
    if len(entries) != upstream["fix_count"]:
        errors.append(
            f"fix registry has {len(entries)} entries; expected {upstream['fix_count']}"
        )
    if duplicate_names:
        errors.append(f"duplicate fix IDs: {', '.join(duplicate_names)}")

    settings_text = (root / "src" / "bw_settings.c").read_text(encoding="utf-8")
    setting_fixes = sorted(
        set(re.findall(r"fix:[+-]([A-Z0-9_]+)", _without_c_comments(settings_text)))
    )
    unknown_setting_fixes = sorted(set(setting_fixes) - set(names))
    if unknown_setting_fixes:
        errors.append(
            "settings reference unregistered fixes: " + ", ".join(unknown_setting_fixes)
        )

    cmake_text = (root / "CMakeLists.txt").read_text(encoding="utf-8")
    missing_native_sources = [
        source
        for source in manifest["native_fix_sources"]
        if source not in cmake_text or not (root / source).is_file()
    ]
    if missing_native_sources:
        errors.append(
            "native fix sources missing or not wired: " + ", ".join(missing_native_sources)
        )

    upstream_commit = upstream["commit"]
    has_upstream = _git(root, "cat-file", "-e", f"{upstream_commit}^{{commit}}").returncode == 0
    upstream_registry_sha256 = None
    ahead = None
    behind = None
    merge_base = None
    if has_upstream:
        shown = _git(root, "show", f"{upstream_commit}:{upstream['fix_registry_path']}")
        if shown.returncode != 0:
            errors.append("recorded upstream commit does not contain the fix registry")
        else:
            upstream_registry_sha256 = _sha256(shown.stdout)
            if upstream_registry_sha256 != upstream["fix_registry_sha256"]:
                errors.append("recorded upstream commit's fix registry differs from the manifest")
            if shown.stdout != registry_bytes:
                errors.append("local fix registry differs from the recorded upstream commit")

        divergence = _git(root, "rev-list", "--left-right", "--count", f"HEAD...{upstream_commit}")
        if divergence.returncode == 0:
            fields = divergence.stdout.decode("ascii", errors="replace").split()
            if len(fields) == 2:
                ahead, behind = (int(fields[0]), int(fields[1]))
        merged = _git(root, "merge-base", "HEAD", upstream_commit)
        if merged.returncode == 0:
            merge_base = merged.stdout.decode("ascii", errors="replace").strip()
    else:
        message = (
            f"recorded upstream commit {upstream_commit} is not in the local object database"
        )
        if require_upstream:
            errors.append(message)
        else:
            warnings.append(message)

    commit_status = []
    for item in manifest["classified_unmerged_commits"]:
        commit = item["commit"]
        available = _git(root, "cat-file", "-e", f"{commit}^{{commit}}").returncode == 0
        merged = None
        if available:
            merged = _git(root, "merge-base", "--is-ancestor", commit, "HEAD").returncode == 0
        commit_status.append({**item, "available": available, "merged": merged})

    kind_counts = {}
    for entry in entries:
        kind_counts[entry["kind"]] = kind_counts.get(entry["kind"], 0) + 1

    return {
        "ok": not errors,
        "errors": errors,
        "warnings": warnings,
        "registry": {
            "path": upstream["fix_registry_path"],
            "sha256": _sha256(registry_bytes),
            "entry_count": len(entries),
            "kind_counts": kind_counts,
        },
        "settings_fix_references": setting_fixes,
        "native_fix_sources": manifest["native_fix_sources"],
        "upstream": {
            "commit": upstream_commit,
            "available": has_upstream,
            "registry_sha256": upstream_registry_sha256,
            "ahead": ahead,
            "behind": behind,
            "merge_base": merge_base,
        },
        "classified_commits": commit_status,
    }


def _print_human(result):
    registry = result["registry"]
    if result["ok"]:
        print(
            f"[OK] {registry['entry_count']} fixes match "
            f"{registry['sha256'][:12]} ({registry['path']})"
        )
        print(
            f"[OK] {len(result['settings_fix_references'])} settings fix references "
            "resolve to the registry"
        )
        print(f"[OK] {len(result['native_fix_sources'])} native fix sources are wired")
    upstream = result["upstream"]
    if upstream["available"]:
        print(
            f"[OK] local upstream {upstream['commit'][:12]}: "
            f"HEAD is {upstream['ahead']} ahead, {upstream['behind']} behind; "
            f"merge base {upstream['merge_base'][:12]}"
        )
    for item in result["classified_commits"]:
        state = "merged" if item["merged"] else "not merged"
        if item["merged"] is None:
            state = "not available locally"
        print(f"[INFO] {item['commit'][:12]} {state}: {item['classification']}")
    for warning in result["warnings"]:
        print(f"[WARN] {warning}", file=sys.stderr)
    for error in result["errors"]:
        print(f"[FAIL] {error}", file=sys.stderr)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=ROOT)
    parser.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST)
    parser.add_argument(
        "--require-upstream",
        action="store_true",
        help="fail when the recorded upstream commit is unavailable locally",
    )
    parser.add_argument("--json", action="store_true", help="print the full report as JSON")
    args = parser.parse_args(argv)
    result = check(args.root, args.manifest, args.require_upstream)
    if args.json:
        print(json.dumps(result, indent=2, sort_keys=True))
    else:
        _print_human(result)
    return 0 if result["ok"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
