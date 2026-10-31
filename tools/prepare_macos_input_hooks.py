"""Prepare a private generated source with game-specific input wrappers."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import tempfile


HOOKS = (
    "0016DC16",
    "0016DC8B",
    "0016DC97",
    "0016DE89",
    "0016DEFA",
    "0016F121",
    "0016F126",
    "0016F148",
)


def prepare(source, expected_size, expected_sha256):
    source = source.replace(b"\r\n", b"\n")
    candidates = (source, source.replace(b"\n", b"\r\n"))
    if not any(len(data) == expected_size and hashlib.sha256(data).hexdigest() == expected_sha256
               for data in candidates):
        raise ValueError("input source does not match the generated-code fingerprint")
    for address in HOOKS:
        original = f"void sub_{address}(void)\n".encode("ascii")
        replacement = f"void sub_{address}_gen(void)\n".encode("ascii")
        if source.count(original) != 1 or replacement in source:
            raise ValueError(f"expected exactly one original definition for sub_{address}")
        source = source.replace(original, replacement, 1)
    return source


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--fingerprint", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.source.resolve() == args.output.resolve():
        parser.error("output must not replace the prepared source")
    fingerprint = json.loads(args.fingerprint.read_text(encoding="utf-8"))
    record = fingerprint["files"]["recomp_0032.c"]
    result = prepare(args.source.read_bytes(), record["size"], record["sha256"])
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(dir=args.output.parent, prefix=".bw-input-",
                                         delete=False) as output:
            temporary = Path(output.name)
            output.write(result)
        os.replace(temporary, args.output)
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


if __name__ == "__main__":
    main()
