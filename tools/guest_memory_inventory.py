#!/usr/bin/env python3
"""Version-checked, deliberately conservative source inventory; not a C parser."""

import argparse
import bisect
import hashlib
import json
import os
import re
import stat
import subprocess
import sys
from collections import Counter
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PINNED_TOOLKIT = "f0f8a461496a4f23d9b0aeaa284b8d70ba7ab82b"
PUBLISHED_GENERATED = "d2552e199cb8d94484ee589d151a41faf52b83c231aa7f241151d19c820689aa"
SCHEMA = 1
EXTENSIONS = {".c", ".h", ".m", ".mm", ".inc"}
EXTRA_GENERATED = {"bw_ocean_native.inc"}

SCALARS = {
    "MEM8": 1, "MEM16": 2, "MEM32": 4, "MEM64": 8,
    "SMEM8": 1, "SMEM16": 2, "SMEM32": 4, "SMEM64": 8,
    "MEMF": 4, "MEMD": 8,
}
SCALARS.update({"N" + name: width for name, width in list(SCALARS.items())})
HELPERS = {
    "PUSH32": ("stack", "write", 4), "POP32": ("stack", "read", 4),
    "XMM_MEM": ("vector", "read", 16), "XMM_STORE": ("vector", "write", 16),
    "XMM_LOAD_LOW": ("vector", "read", 8), "XMM_LOAD_HIGH": ("vector", "read", 8),
    "XMM_STORE_LOW": ("vector", "write", 8), "XMM_STORE_HIGH": ("vector", "write", 8),
    "MMX_MEM": ("vector", "read", 8), "MMX_STORE": ("vector", "write", 8),
}
BULK = {
    "memcpy": ("copy", 2), "memmove": ("copy", 2), "memset": ("fill", 2),
    "memcmp": ("compare", 2), "memchr": ("scan", 2),
    "strcpy": ("copy", None), "strncpy": ("copy", 2), "strcat": ("copy", None),
    "strncat": ("copy", 2), "strcmp": ("compare", None), "strncmp": ("compare", 2),
    "strlen": ("scan", None), "strnlen": ("scan", 1), "strchr": ("scan", None),
    "strstr": ("scan", None), "wmemcpy": ("copy", None), "wmemmove": ("copy", None),
    "wmemset": ("fill", None),
    "__builtin_memcpy": ("copy", 2), "__builtin_memmove": ("copy", 2),
    "__builtin_memset": ("fill", 2), "memcpy_s": ("copy", 3), "memmove_s": ("copy", 3),
    "strcpy_s": ("copy", None), "strncpy_s": ("copy", None),
    "RtlCopyMemory": ("copy", 2), "RtlMoveMemory": ("copy", 2),
    "RtlFillMemory": ("fill", 1), "RtlZeroMemory": ("fill", 1),
}
ANCHORS = {"g_xbox_mem_offset", "g_memory_offset", "g_memory_base",
           "xbox_GetMemoryOffset", "xbox_GetMemoryBase", "XBOX_PTR",
           "ram_ptr", "guest_ptr", "guest_mem", "guest_memory"}
MASK = re.compile(r'//[^\n]*|/\*[\s\S]*?\*/|"(?:\\[\s\S]|[^"\\])*"|'
                  r"'(?:\\[\s\S]|[^'\\])*'")
IDENT = re.compile(r"\b[A-Za-z_]\w*\b")
DEFINITION = re.compile(r"(?m)^[ \t]*#[ \t]*define[ \t]+([A-Za-z_]\w*)"
                        r"\([^\n]*?\)[ \t]*((?:[^\n]*\\\n)*[^\n]*)")
ARRAY = re.compile(r"\b[A-Za-z_]\w*(?:(?:->|\.)[A-Za-z_]\w*)*\s*\[")
DEREF = re.compile(r"\*\s*(?=\(|[A-Za-z_])")
ASSIGN = re.compile(r"^(?:<<=|>>=|\+=|-=|\*=|/=|%=|&=|\|=|\^=|=(?!=))")
TYPED_DEREF = re.compile(
    r"\*\s*\(\s*(?:(?:const|volatile|signed|unsigned)\s+)*"
    r"(u?int(8|16|32|64)_t|float|double|char)\s*\*\s*"
    r"(?:(?:const|volatile)\s*)?\)")


class InventoryError(Exception):
    pass


def sha(data):
    return hashlib.sha256(data).hexdigest()


def tree_digest(files):
    digest = hashlib.sha256()
    for name, value in sorted(files.items()):
        digest.update((name + "\0" + value + "\n").encode("utf-8"))
    return digest.hexdigest()


def read_sources(root, *, skip=None):
    root = Path(root)
    if root.is_symlink():
        raise InventoryError("source tree contains a symlink: " + str(root))
    if not root.is_dir():
        raise InventoryError("missing source directory: " + str(root))
    result = {}

    def walk_error(error):
        raise error

    for parent, directories, files in os.walk(root, followlinks=False, onerror=walk_error):
        directories.sort()
        for name in sorted(directories + files):
            path = Path(parent) / name
            rel = path.relative_to(root)
            if skip and (rel == skip or skip in rel.parents):
                if name in directories:
                    directories.remove(name)
                continue
            mode = path.lstat().st_mode
            if stat.S_ISLNK(mode):
                raise InventoryError("source tree contains a symlink: " + str(path))
            if stat.S_ISREG(mode) and path.suffix.lower() in EXTENSIONS:
                result[rel.as_posix()] = path.read_bytes()
            elif not stat.S_ISDIR(mode) and not stat.S_ISREG(mode) and path.suffix.lower() in EXTENSIONS:
                raise InventoryError("source is not a regular file: " + str(path))
    return result


def generated_sources(root, manifest, toolkit_commit):
    root = Path(root)
    if manifest.get("format") != 1 or manifest.get("toolkit_commit") != toolkit_commit:
        raise InventoryError("unsupported generated fingerprint or toolkit revision")
    expected = manifest.get("files", {})
    if (len(expected) != manifest.get("file_count") or not expected or
            tree_digest({name: info["sha256"] for name, info in expected.items()})
            != manifest.get("total_sha256")):
        raise InventoryError("invalid generated fingerprint manifest")
    if not root.is_dir():
        raise InventoryError("missing generated directory: " + str(root))
    found = {}
    for path in sorted(root.iterdir()):
        if path.is_symlink() or not path.is_file():
            raise InventoryError("unexpected generated directory entry: " + path.name)
        found[path.name] = path.read_bytes()
    if set(found) - EXTRA_GENERATED != set(expected):
        raise InventoryError("generated file set differs from fingerprint")

    def matches(normalize):
        for name, info in expected.items():
            data = found[name]
            if normalize:
                data = data.replace(b"\r\n", b"\n").replace(b"\n", b"\r\n")
            if len(data) != info["size"] or sha(data) != info["sha256"]:
                return False
        return True

    if matches(False):
        mode = "exact"
    elif matches(True):
        mode = "crlf-normalized"
    else:
        raise InventoryError("generated contents differ from fingerprint")
    return found, mode


def verify_toolkit(root, expected):
    root = Path(root).resolve()

    def git(*args):
        result = subprocess.run(["git", "-C", str(root), *args], text=True,
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        if result.returncode:
            raise InventoryError("toolkit must be a clean pinned git checkout")
        return result.stdout.strip()

    if Path(git("rev-parse", "--show-toplevel")).resolve() != root:
        raise InventoryError("toolkit must be the checkout root")
    if git("rev-parse", "HEAD") != expected:
        raise InventoryError("toolkit revision does not match the pin")
    if git("status", "--porcelain", "--untracked-files=all"):
        raise InventoryError("toolkit checkout is dirty")
    return expected


def verify_toolkit_sources(root, sources):
    result = subprocess.run(["git", "-C", str(root), "ls-tree", "-r", "-z", "HEAD", "--", "src"],
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if result.returncode:
        raise InventoryError("cannot enumerate pinned toolkit source blobs")
    expected = {}
    for entry in result.stdout.split(b"\0"):
        if not entry:
            continue
        meta, name = entry.split(b"\t", 1)
        mode, kind, object_id = meta.decode("ascii").split()
        rel = name.decode("utf-8").removeprefix("src/")
        if Path(rel).suffix.lower() not in EXTENSIONS:
            continue
        if mode not in ("100644", "100755") or kind != "blob":
            raise InventoryError("unsupported pinned toolkit source entry: " + rel)
        expected[rel] = object_id
    if set(expected) != set(sources):
        raise InventoryError("toolkit source file set differs from pinned checkout")
    for name, data in sources.items():
        blob = b"blob " + str(len(data)).encode("ascii") + b"\0" + data
        if hashlib.sha1(blob).hexdigest() != expected[name]:
            raise InventoryError("toolkit source bytes differ from pinned blob: " + name)


def mask_noncode(text):
    return MASK.sub(lambda m: re.sub(r"[^\n]", " ", m.group()), text)


def closing(text, opening):
    depth = 0
    for i in range(opening, len(text)):
        if text[i] == "(":
            depth += 1
        elif text[i] == ")":
            depth -= 1
            if not depth:
                return i
    return None


def arguments(text, start, end):
    parts, depth, begin = [], 0, start + 1
    for i in range(start + 1, end):
        if text[i] in "([{":
            depth += 1
        elif text[i] in ")]}":
            depth -= 1
        elif text[i] == "," and depth == 0:
            parts.append(text[begin:i].strip())
            begin = i + 1
    return parts + [text[begin:end].strip()]


def integer_literal(expression):
    match = re.fullmatch(r"\s*(0[xX][0-9a-fA-F]+|[0-9]+)[uUlL]*\s*", expression)
    if not match:
        return None
    digits = match.group(1)
    try:
        return int(digits, 16 if digits.lower().startswith("0x") else
                   8 if len(digits) > 1 and digits.startswith("0") else 10)
    except ValueError:
        return None


def constant_span(expression, width):
    """Simple constant guest VA only; do not emit source expressions or addresses."""
    address = integer_literal(expression)
    if address is None or width is None or address > 0xffffffff:
        return None
    last = address + max(0, width - 1)
    return {"crosses_4k": width > 0 and address // 4096 != last // 4096,
            "crosses_16k": width > 0 and address // 16384 != last // 16384,
            "wraps_32bit": last > 0xffffffff}


def bulk_constant_spans(symbol, args, width):
    indices = {
        "memcpy": (0, 1), "memmove": (0, 1), "memcmp": (0, 1),
        "memset": (0,), "memchr": (0,),
        "__builtin_memcpy": (0, 1), "__builtin_memmove": (0, 1),
        "__builtin_memset": (0,), "memcpy_s": (0, 2), "memmove_s": (0, 2),
        "RtlCopyMemory": (0, 1), "RtlMoveMemory": (0, 1),
        "RtlFillMemory": (0,), "RtlZeroMemory": (0,),
    }.get(symbol, ())
    spans = []
    for index in indices:
        if index >= len(args):
            continue
        match = re.fullmatch(
            r"\s*(?:\(\s*(?:(?:const|volatile)\s+)?[A-Za-z_]\w*\s*\*\s*\)\s*)?"
            r"(?:XBOX_PTR|BW_GUEST|GUEST|GP|HP|XBOX_TO_NATIVE)\s*\(\s*"
            r"(0[xX][0-9a-fA-F]+|[0-9]+)[uUlL]*\s*\)\s*", args[index])
        if match:
            span = constant_span(match.group(1), width)
            if span is not None:
                spans.append({"argument_index": index, **span})
    return spans


def macro_use(text, start, end):
    # Parentheses enclosing only this lvalue do not change its assignment role.
    left, right = start, end + 1
    while True:
        before = left - 1
        while before >= 0 and text[before].isspace():
            before -= 1
        after = right
        while after < len(text) and text[after].isspace():
            after += 1
        prior = before - 1
        while prior >= 0 and text[prior].isspace():
            prior -= 1
        is_call = prior >= 0 and (text[prior].isalnum() or text[prior] in "_)]")
        if (before >= 0 and text[before] == "(" and after < len(text) and
                text[after] == ")" and not is_call):
            left, right = before, after + 1
        else:
            break
    prefix = text[max(0, left - 40):left].rstrip()
    while right < len(text) and text[right].isspace():
        right += 1
    suffix = text[right:right + 4]
    if prefix.endswith("&"):
        return "address_escape"
    if prefix.endswith(("++", "--")) or suffix.startswith(("++", "--")):
        return "read_modify_write"
    assignment = ASSIGN.match(suffix)
    if assignment:
        return "write" if assignment.group() == "=" else "read_modify_write"
    if suffix.startswith((".", "->", "[")):
        return "unknown"
    # A C macro may mutate a by-name lvalue argument. Do not call it a read.
    fragment = text[max(0, left - 256):left]
    fragment = re.split(r"[;{}]", fragment)[-1]
    depth = 0
    for i in range(len(fragment) - 1, -1, -1):
        if fragment[i] == ")":
            depth += 1
        elif fragment[i] == "(":
            if depth:
                depth -= 1
            elif re.search(r"\b[A-Za-z_]\w*\s*$", fragment[:i]):
                return "unknown_lvalue_context"
    return "read_candidate"


def typed_dereference(code, start):
    cast = TYPED_DEREF.match(code, start)
    if not cast:
        return "unknown", None
    kind, bits = cast.group(1), cast.group(2)
    width = {"float": 4, "double": 8, "char": 1}.get(kind)
    if width is None:
        width = int(bits) // 8
    pos = cast.end()
    while pos < len(code) and code[pos].isspace():
        pos += 1
    if pos >= len(code):
        return "unknown", width
    if code[pos] == "(":
        end = closing(code, pos)
    else:
        name = IDENT.match(code, pos)
        if not name:
            return "unknown", width
        end = name.end() - 1
        next_pos = name.end()
        while next_pos < len(code) and code[next_pos].isspace():
            next_pos += 1
        if next_pos < len(code) and code[next_pos] == "(":
            end = closing(code, next_pos)
    if end is None:
        return "unknown", width
    # In *p++, ++ may update p rather than the referenced value.
    following = code[end + 1:end + 4].lstrip()
    if following.startswith(("++", "--")):
        return "unknown", width
    return macro_use(code, start, end), width


def definitions(code):
    """Discover file-local macro wrappers rooted in known guest pointer names."""
    defs = list(DEFINITION.finditer(code))
    known = set(ANCHORS) | set(SCALARS)
    scalar_widths = dict(SCALARS)
    result = {}
    while True:
        changed = False
        for match in defs:
            name, body = match.group(1), match.group(2)
            if name in result or not (set(IDENT.findall(body)) & known):
                continue
            width = None
            typ = re.search(r"\*\s*\(\s*(?:(?:const|volatile|signed|unsigned)\s+)*"
                            r"(u?int(8|16|32|64)_t|float|double)\s*\*", body)
            if typ:
                width = {"float": 4, "double": 8}.get(typ.group(1))
                if width is None:
                    width = int(typ.group(2)) // 8
            elif not re.search(r"&\s*\b", body):
                inherited = [scalar_widths[token] for token in IDENT.findall(body)
                             if token in scalar_widths]
                if inherited:
                    width = inherited[0] if all(n == inherited[0] for n in inherited) else None
            kind = "scalar" if width else "pointer"
            result[name] = (kind, width)
            if width:
                scalar_widths[name] = width
            known.add(name)
            changed = True
        if not changed:
            return result


def scan_source(data, domain, name):
    try:
        code = mask_noncode(data.decode("utf-8"))
    except UnicodeDecodeError as error:
        raise InventoryError("non-UTF-8 source: " + domain + "/" + name) from error
    newlines = [-1] + [m.start() for m in re.finditer("\n", code)]
    macros = definitions(code)
    definition_lines = set()
    for match in DEFINITION.finditer(code):
        first = bisect.bisect_right(newlines, match.start())
        last = bisect.bisect_right(newlines, match.end() - 1)
        definition_lines.update(range(first, last + 1))
    records = []

    def record(pos, category, symbol, operation="unknown", width=None, span="unknown", **extra):
        line = bisect.bisect_right(newlines, pos)
        item = {"domain": domain, "path": name, "line": line,
                "column": pos - newlines[line - 1], "category": category,
                "symbol": symbol, "operation": operation, "width_bytes": width,
                "span": span, "certainty": "candidate"}
        if line in definition_lines:
            item["context"] = "macro_definition"
        item.update(extra)
        records.append(item)

    for match in IDENT.finditer(code):
        symbol = match.group()
        start, end = match.span()
        after = end
        while after < len(code) and code[after].isspace():
            after += 1
        call = after < len(code) and code[after] == "("
        scalar = symbol in SCALARS or (symbol in macros and macros[symbol][0] == "scalar")
        pointer = symbol in ANCHORS or (symbol in macros and macros[symbol][0] == "pointer")
        atomic = symbol.startswith(("atomic_", "__atomic_", "__sync_", "qatomic_",
                                    "Interlocked", "_Interlocked"))
        vector = symbol.startswith(("_mm_load", "_mm_store", "vld1", "vst1"))
        unknown_helper = (call and not scalar and not pointer and symbol not in HELPERS
                          and re.search(r"(?:^|_)(?:S?MEM|GUEST)(?:\w*)$", symbol))
        if not (scalar or pointer or symbol in HELPERS or symbol in BULK or atomic or vector or unknown_helper):
            continue
        if not call and not (pointer and symbol in ANCHORS):
            continue
        close = closing(code, after) if call else None
        if call and close is None:
            record(start, "unrecognized", symbol, reason="unbalanced_call")
            continue
        if scalar:
            width = SCALARS.get(symbol, macros.get(symbol, (None, None))[1])
            args = arguments(code, after, close)
            span = constant_span(args[0], width) if symbol in SCALARS and args else None
            record(start, "scalar", symbol, macro_use(code, start, close), width,
                   "fixed_may_cross" if width and width > 1 else "single_byte_or_unknown",
                   **({"constant_span": span} if span is not None else {}))
        elif symbol in HELPERS:
            category, operation, width = HELPERS[symbol]
            args = arguments(code, after, close)
            arg_index = 1 if symbol in ("XMM_LOAD_LOW", "XMM_LOAD_HIGH") else 0
            span = (constant_span(args[arg_index], width) if category == "vector" and
                    len(args) > arg_index else None)
            record(start, category, symbol, operation, width, "fixed_may_cross",
                   **({"constant_span": span} if span is not None else {}))
        elif symbol in BULK:
            op, length_index = BULK[symbol]
            args = arguments(code, after, close)
            width = None
            if length_index is not None and len(args) > length_index:
                width = integer_literal(args[length_index])
            spans = bulk_constant_spans(symbol, args, width)
            record(start, "bulk_string", symbol, op, width,
                   "fixed_may_cross" if width is not None else "dynamic_or_terminated",
                   **({"constant_operands": spans} if spans else {}))
        elif atomic:
            record(start, "atomic", symbol, "unknown_atomic", span="unknown_may_cross")
        elif vector:
            record(start, "vector_intrinsic", symbol,
                   "read_candidate" if "load" in symbol or symbol.startswith("vld") else "write_candidate",
                   span="unknown_may_cross")
        elif pointer:
            record(start, "pointer_origin", symbol, "address_or_unknown")
        elif unknown_helper:
            record(start, "unrecognized", symbol, reason="guest_like_helper")

    for match in DEREF.finditer(code):
        cursor = match.start() - 1
        while cursor >= 0 and code[cursor].isspace():
            cursor -= 1
        if cursor >= 0 and (code[cursor].isalnum() or code[cursor] in "_)]"):
            if not re.search(r"\breturn$", code[max(0, cursor - 10):cursor + 1]):
                continue
        operation, width = typed_dereference(code, match.start())
        record(match.start(), "pointer_dereference", "*", operation, width,
               "fixed_may_cross" if width and width > 1 else "unknown_may_cross")
    for match in ARRAY.finditer(code):
        symbol = re.sub(r"\s*\[$", "", match.group())
        if domain == "generated" and symbol == "g_fp_stack":
            continue
        record(match.start(), "subscript", symbol, span="unknown_may_cross")
    records.sort(key=lambda item: (item["line"], item["column"], item["category"], item["symbol"]))
    return records


def inventory(project, generated, toolkit, manifest, expected_commit, expected_project=None,
              derived=None):
    project, generated, toolkit = map(lambda p: Path(p).resolve(), (project, generated, toolkit))
    verify_toolkit(toolkit, expected_commit)
    gen, normalization = generated_sources(generated, manifest, expected_commit)
    project_sources = read_sources(project / "src", skip=Path("recomp/gen"))
    toolkit_sources = read_sources(toolkit / "src")
    if not project_sources or not toolkit_sources:
        raise InventoryError("project and toolkit source trees must not be empty")
    verify_toolkit_sources(toolkit, toolkit_sources)
    derived_sources = {}
    derived_paths = {}
    for path in derived or ():
        path = Path(path)
        if path.is_symlink() or not path.is_file() or path.suffix.lower() not in EXTENSIONS:
            raise InventoryError("derived input must be a regular source file: " + str(path))
        if path.name in derived_sources or path.name in gen:
            raise InventoryError("ambiguous duplicate derived filename: " + path.name)
        derived_paths[path.name] = path
        derived_sources[path.name] = path.read_bytes()
    inputs = {"generated": gen, "derived": derived_sources,
              "project": project_sources, "toolkit": toolkit_sources}
    project_digest = tree_digest({p: sha(data) for p, data in project_sources.items()})
    if expected_project and project_digest != expected_project:
        raise InventoryError("project source digest differs from expected version")
    files, records = [], []
    for domain, sources in inputs.items():
        for name, data in sorted(sources.items()):
            files.append({"domain": domain, "path": name, "sha256": sha(data),
                          "verification": ("unverified_build_artifact" if domain == "derived" or
                                           (domain == "generated" and name in EXTRA_GENERATED) else
                                           "fingerprint" if domain == "generated" else
                                           "pinned_checkout" if domain == "toolkit" else
                                           "expected_digest" if expected_project else "snapshot_only")})
            if Path(name).suffix.lower() in EXTENSIONS:
                records.extend(scan_source(data, domain, name))
    # A report must describe one stable source snapshot, not a mixture from a live edit.
    if (gen != generated_sources(generated, manifest, expected_commit)[0] or
            any(path.is_symlink() or not path.is_file() or path.read_bytes() != derived_sources[name]
                for name, path in derived_paths.items()) or
            project_sources != read_sources(project / "src", skip=Path("recomp/gen")) or
            toolkit_sources != read_sources(toolkit / "src")):
        raise InventoryError("source inputs changed during inventory")
    verify_toolkit(toolkit, expected_commit)
    counts = Counter(item["domain"] + "/" + item["category"] + "/" + item["operation"] for item in records)
    return {
        "schema_version": SCHEMA, "toolkit_commit": expected_commit,
        "generated_fingerprint": manifest["total_sha256"], "generated_comparison": normalization,
        "derived_artifacts": ("provided" if derived_sources or set(gen) & EXTRA_GENERATED
                              else "not_provided"),
        "project_source_digest": project_digest,
        "project_version_check": "expected_digest" if expected_project else "snapshot_only",
        "limitations": [
            "Lexical candidates, not an exhaustive C or data-flow analysis; false positives and missed accesses are expected.",
            "Operation labels are syntactic hints; macros, aliasing and conditional compilation are not resolved.",
            "Width is a source-level hint, not proof of actual transaction boundaries or alignment.",
            "Bulk, atomic, intrinsic, pointer and subscript candidates may refer solely to host memory.",
            "Generated build artifacts listed as unverified are not covered by the published fingerprint.",
        ],
        "files": files, "counts": dict(sorted(counts.items())), "sites": records,
    }


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--gen", type=Path, required=True, help="prepared generated directory")
    parser.add_argument("--toolkit", type=Path, required=True, help="clean pinned toolkit git checkout")
    parser.add_argument("--derived", action="append", type=Path, default=[],
                        help="additional generated build source to snapshot (repeatable, unverified)")
    parser.add_argument("--expect-project-sha256", help="require the previously recorded project source digest")
    args = parser.parse_args(argv)
    if args.expect_project_sha256 and not re.fullmatch(r"[0-9a-f]{64}", args.expect_project_sha256):
        parser.error("--expect-project-sha256 must be 64 lowercase hex characters")
    try:
        manifest_path = ROOT / "fingerprints" / "generated.json"
        manifest_bytes = manifest_path.read_bytes()
        manifest = json.loads(manifest_bytes)
        if manifest.get("total_sha256") != PUBLISHED_GENERATED:
            raise InventoryError("generated fingerprint is not the published version")
        tool_bytes = Path(__file__).read_bytes()
        result = inventory(ROOT, args.gen, args.toolkit, manifest, PINNED_TOOLKIT,
                           args.expect_project_sha256, args.derived)
        if manifest_path.read_bytes() != manifest_bytes or Path(__file__).read_bytes() != tool_bytes:
            raise InventoryError("inventory tool or fingerprint changed during scan")
        result["fingerprint_manifest_sha256"] = sha(manifest_bytes)
        result["tool_sha256"] = sha(tool_bytes)
        json.dump(result, sys.stdout, sort_keys=True, separators=(",", ":"))
        sys.stdout.write("\n")
        return 0
    except (InventoryError, OSError, ValueError, KeyError, subprocess.SubprocessError) as error:
        print("guest-memory inventory: " + str(error), file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
