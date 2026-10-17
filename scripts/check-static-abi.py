#!/usr/bin/env python3
"""Fail when a statically linked helper demands a newer kernel than the device.

A static glibc executable carries the kernel ABI of the toolchain that built it
in its .note.ABI-tag and aborts at start with "FATAL: kernel too old" on an
older kernel. The Docker toolchain (crosstool-NG GCC 8.3.0) emits ABI 4.19.0,
the Brick Pro kernel is 4.9.191, so a Docker-built nx_usb_guard.elf made
`nx_usb_mode.sh restore` abort and left USB without MTP.

Static = ET_EXEC with no PT_INTERP. Dynamic executables and shared objects are
skipped: they run on the device's own glibc. A static binary with no ABI note
(musl, clang) cannot hit this failure and is reported but accepted.

Usage: check-static-abi.py [--max X.Y.Z] [--quiet] PATH...   (files or directories)
Exit:  0 all fine, 1 a static helper exceeds --max, 2 usage/read error.
"""
import os
import struct
import sys

PT_INTERP, PT_NOTE = 3, 4
ET_EXEC = 2
NT_GNU_ABI_TAG = 1


def parse_version(text):
    try:
        parts = tuple(int(p) for p in text.split("."))
    except ValueError:
        raise SystemExit("invalid version: %r" % text)
    return parts + (0,) * (3 - len(parts))


def inspect(path):
    """Return None if not an ELF, else (is_static, abi_tuple_or_None)."""
    with open(path, "rb") as f:
        head = f.read(64)
        if len(head) < 52 or head[:4] != b"\x7fELF":
            return None
        is64 = head[4] == 2
        endian = "<" if head[5] == 1 else ">"
        if is64:
            e_type = struct.unpack_from(endian + "H", head, 16)[0]
            phoff = struct.unpack_from(endian + "Q", head, 32)[0]
            phentsize, phnum = struct.unpack_from(endian + "HH", head, 54)
        else:
            e_type = struct.unpack_from(endian + "H", head, 16)[0]
            phoff = struct.unpack_from(endian + "I", head, 28)[0]
            phentsize, phnum = struct.unpack_from(endian + "HH", head, 42)
        f.seek(phoff)
        table = f.read(phentsize * phnum)
        has_interp = False
        notes = []
        for i in range(phnum):
            base = i * phentsize
            if is64:
                p_type, _flags, p_off, _va, _pa, p_filesz = struct.unpack_from(
                    endian + "IIQQQQ", table, base)
            else:
                p_type, p_off, _va, _pa, p_filesz = struct.unpack_from(
                    endian + "IIIII", table, base)
            if p_type == PT_INTERP:
                has_interp = True
            elif p_type == PT_NOTE:
                notes.append((p_off, p_filesz))
        abi = None
        for off, size in notes:
            f.seek(off)
            data = f.read(size)
            pos = 0
            while pos + 12 <= len(data):
                namesz, descsz, ntype = struct.unpack_from(endian + "III", data, pos)
                name_at = pos + 12
                desc_at = name_at + ((namesz + 3) & ~3)
                if (ntype == NT_GNU_ABI_TAG and data[name_at:name_at + namesz].rstrip(b"\0") == b"GNU"
                        and descsz >= 16):
                    os_id, major, minor, patch = struct.unpack_from(endian + "IIII", data, desc_at)
                    if os_id == 0:  # Linux
                        abi = (major, minor, patch)
                pos = desc_at + ((descsz + 3) & ~3)
        return (e_type == ET_EXEC and not has_interp), abi


def iter_files(paths):
    for p in paths:
        if os.path.isdir(p):
            for root, _dirs, files in os.walk(p):
                for name in sorted(files):
                    full = os.path.join(root, name)
                    if os.path.isfile(full) and not os.path.islink(full):
                        yield full
        else:
            yield p


def main(argv):
    limit = parse_version("3.7.0")
    quiet = False
    paths = []
    args = list(argv)
    while args:
        a = args.pop(0)
        if a == "--max" and args:
            limit = parse_version(args.pop(0))
        elif a == "--quiet":
            quiet = True
        elif a.startswith("--"):
            print(__doc__, file=sys.stderr)
            return 2
        else:
            paths.append(a)
    if not paths:
        print(__doc__, file=sys.stderr)
        return 2

    bad = 0
    checked = 0
    for path in iter_files(paths):
        try:
            result = inspect(path)
        except (OSError, struct.error) as exc:
            print("ERROR %s: %s" % (path, exc), file=sys.stderr)
            return 2
        if result is None:
            continue
        is_static, abi = result
        if not is_static:
            continue
        checked += 1
        if abi is None:
            if not quiet:
                print("ok    %s static, no GNU ABI note (musl/clang)" % path)
        elif abi > limit:
            bad += 1
            print("FAIL  %s static, ABI %s > %s: aborts with 'kernel too old' on an older kernel. "
                  "Rebuild with the WSL aarch64-linux-gnu-gcc toolchain (.dev/CROSS_BUILD_WSL.md)."
                  % (path, ".".join(map(str, abi)), ".".join(map(str, limit))))
        elif not quiet:
            print("ok    %s static, ABI %s" % (path, ".".join(map(str, abi))))
    print("static ABI check: %d static helper(s), %d over %s"
          % (checked, bad, ".".join(map(str, limit))))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
