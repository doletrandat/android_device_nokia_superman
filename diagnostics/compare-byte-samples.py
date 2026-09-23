#!/usr/bin/env python3
"""Compare BYTE_SAMPLE records with an explicitly selected extracted system tree.

Usage: compare-byte-samples.py SYSTEM_ROOT MEMDIAG_LOG [MEMDIAG_LOG ...]
SYSTEM_ROOT contains framework/, lib/, etc. Use the tested image's files.
Original capture files and reference files are opened read-only.
"""
import hashlib
import pathlib
import re
import sys


def main():
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    root = pathlib.Path(sys.argv[1]).resolve()
    pattern = re.compile(r"BYTE_SAMPLE path=(\S+) offset=(\d+) method=(read1|mmap|read2) hex=([0-9a-f]{128})(?:\s|$)")
    references = {}
    records = mismatches = errors = 0
    for name in sys.argv[2:]:
        with open(name, encoding="utf-8", errors="replace") as log:
            for line_number, line in enumerate(log, 1):
                if "BYTE_SAMPLE " not in line:
                    continue
                match = pattern.search(line)
                if not match:
                    print(f"ERROR {name}:{line_number}: malformed sample")
                    errors += 1
                    continue
                path, offset, method, hexbytes = match.groups()
                if not path.startswith("/system/"):
                    print(f"ERROR unsupported path {path}")
                    errors += 1
                    continue
                reference = (root / path[len('/system/'):]).resolve()
                if root not in reference.parents:
                    print(f"ERROR path escapes reference root: {path}")
                    errors += 1
                    continue
                try:
                    if reference not in references:
                        digest = hashlib.sha256()
                        with reference.open("rb") as stream:
                            for block in iter(lambda: stream.read(1024 * 1024), b""):
                                digest.update(block)
                        references[reference] = digest.hexdigest()
                        print(f"REFERENCE {reference} sha256={digest.hexdigest()}")
                    with reference.open("rb") as stream:
                        stream.seek(int(offset))
                        expected = stream.read(64)
                except OSError as error:
                    print(f"ERROR {reference}: {error}")
                    errors += 1
                    continue
                actual = bytes.fromhex(hexbytes)
                records += 1
                if len(expected) != 64:
                    errors += 1
                    print(f"ERROR {path} offset={offset}: short reference")
                elif actual != expected:
                    mismatches += 1
                    differences = [f"{int(offset)+i}:{a:02x}->{b:02x}" for i, (a,b) in enumerate(zip(actual, expected)) if a != b]
                    print(f"MISMATCH {name}:{line_number} {path} {method} actual->expected " + " ".join(differences))
    print(f"SUMMARY records={records} mismatched_records={mismatches} errors={errors}; sampled windows only")
    return 2 if errors or not records else 1 if mismatches else 0


if __name__ == "__main__":
    sys.exit(main())
