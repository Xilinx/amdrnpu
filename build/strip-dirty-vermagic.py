#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""
Strip a `-dirty` suffix from a kernel module's vermagic string in place.

The .ko was built from a kernel git tree that scripts/setlocalversion deems
"dirty" (uncommitted changes), but the running kernel on the target board
was built from the same SHA in a clean state. The kernel ABI is identical,
only the cosmetic `-dirty` suffix differs. Patching it lets `insmod` accept
the module without needing `modprobe --force-vermagic` (which requires
installing the module under /lib/modules/$(uname -r)/ first).

The .modinfo section is a sequence of NUL-terminated `key=value` strings.
We replace `vermagic=...-dirty SMP ...` with `vermagic=... SMP ...` and
right-pad the freed bytes with NULs to keep the section byte length, so
no ELF section offsets need to be touched. The NUL gap is parsed by the
kernel as empty modinfo entries and harmlessly skipped.
"""
import sys
import argparse


def patch_vermagic(path: str, dry_run: bool = False) -> int:
    with open(path, "rb") as f:
        data = bytearray(f.read())

    key = b"vermagic="
    start = data.find(key)
    if start < 0:
        print(f"{path}: no vermagic= entry found", file=sys.stderr)
        return 2

    end = data.find(b"\x00", start)
    if end < 0:
        print(f"{path}: vermagic entry not NUL-terminated", file=sys.stderr)
        return 2

    original_len = end - start  # length of "vermagic=...." excluding the NUL
    original = bytes(data[start:end])
    print(f"{path}: vermagic at offset {start} ({original_len} bytes)")
    print(f"  before: {original.decode()}")

    needle = b"-dirty"
    didx = original.find(needle)
    if didx < 0:
        print(f"  no -dirty suffix found; nothing to do")
        return 0

    new_value = original[:didx] + original[didx + len(needle):]
    print(f"  after:  {new_value.decode()}")

    if dry_run:
        print("  (dry-run, file not modified)")
        return 0

    # Overwrite in-place; pad freed tail bytes with NULs so the section
    # byte length is preserved.
    data[start:end] = new_value + b"\x00" * len(needle)

    with open(path, "wb") as f:
        f.write(data)
    print(f"  patched {len(needle)} bytes (NUL-padded)")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(
        description="Strip -dirty from a kernel module's vermagic in place"
    )
    ap.add_argument("module", help="path to the .ko file")
    ap.add_argument("-n", "--dry-run", action="store_true", help="show changes, don't write")
    args = ap.parse_args()
    return patch_vermagic(args.module, args.dry_run)


if __name__ == "__main__":
    sys.exit(main())
