"""Read-only USB inspection of a Brick Pro Android boot/kernel snapshot.

Never opens a device, executes firmware, or patches an image. The caller must
also supply the running kernel's kallsyms and config snapshots. Output is
disassembly/evidence, not a claim that a reference BSP matches the device.
"""

import argparse
import gzip
import hashlib
import re
import struct
from pathlib import Path

from capstone import CS_ARCH_ARM64, CS_MODE_LITTLE_ENDIAN, Cs


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("boot", type=Path)
    parser.add_argument("symbols", type=Path)
    parser.add_argument("config", type=Path)
    parser.add_argument("--base", type=lambda value: int(value, 0), required=True)
    parser.add_argument("--live-code", type=Path, help="156-byte read-only snapshot of usb_gadget_ep_match_desc")
    parser.add_argument("--live-endpoints", type=Path, help="Read-only snapshot of the seven sunxi endpoint structs")
    parser.add_argument("--gadget-base", type=lambda value: int(value, 0), help="Gadget address identified from sunxi_udc_probe, never guessed")
    parser.add_argument("--live-gadget-ops", type=Path, help="8-byte live gadget operations pointer")
    parser.add_argument("--live-match-ep", type=Path, help="8-byte live match_ep callback pointer")
    parser.add_argument("--verbose", action="store_true", help="Include initial table bytes")
    parser.add_argument(
        "--functions",
        default="usb_gadget_ep_match_desc|usb_ep_autoconfig_ss|sunxi_udc_probe|sunxi_udc_reinit|__ffs_func_bind_do_descs",
    )
    args = parser.parse_args()
    boot = args.boot.read_bytes()
    if boot[:8] != b"ANDROID!":
        raise SystemExit("Expected an Android boot image")
    kernel_size, load_address = struct.unpack_from("<II", boot, 8)
    page_size = struct.unpack_from("<I", boot, 36)[0]
    if page_size not in (2048, 4096, 8192, 16384) or kernel_size > len(boot) - page_size:
        raise SystemExit("Invalid boot page/kernel bounds")
    image = boot[page_size : page_size + kernel_size]
    if image[56:60] != b"ARM\x64":
        raise SystemExit("Expected an uncompressed AArch64 Linux Image")
    config_begin = image.find(b"IKCFG_ST")
    config_end = image.find(b"IKCFG_ED", config_begin + 8)
    if config_begin < 0 or config_end < 0:
        raise SystemExit("Embedded kernel config not found")
    embedded_config = gzip.decompress(image[config_begin + 8 : config_end])
    live_config = gzip.decompress(args.config.read_bytes())
    if embedded_config != live_config:
        raise SystemExit("Boot image config does not match the running kernel")
    print("boot_sha256", hashlib.sha256(boot).hexdigest())
    print("image_sha256", hashlib.sha256(image).hexdigest())
    print(f"image_size={len(image)} page_size={page_size} load_address={load_address:#x} base={args.base:#x}")
    print("embedded_config_matches_running=true")
    symbols = []
    for line in args.symbols.read_text(encoding="ascii").splitlines():
        fields = line.split()
        if len(fields) >= 3 and fields[1] in ("t", "T", "r", "R", "d", "D"):
            address = int(fields[0], 16)
            if args.base <= address < args.base + len(image):
                symbols.append((address, fields[1], fields[2]))
    symbols.sort()
    names = {address: name for address, kind, name in symbols}
    if not any(address - args.base == 0x800 and name == "_stext" for address, kind, name in symbols):
        raise SystemExit("Unexpected running _stext mapping; refuse guessed image offsets")

    def at(address, size):
        offset = address - args.base
        if 0 <= offset and offset + size <= len(image):
            return image[offset : offset + size]
        return b""

    def describe(address):
        if address in names:
            return names[address]
        payload = at(address, 160)
        end = payload.find(b"\0")
        if end > 0 and all(value in (9, 10, 13) or 32 <= value < 127 for value in payload[:end]):
            return repr(payload[:end].decode("ascii"))
        return ""

    if args.live_code:
        match_address = next(address for address, kind, name in symbols if name == "usb_gadget_ep_match_desc")
        live_code = args.live_code.read_bytes()
        if len(live_code) != 156 or live_code != at(match_address, 156):
            raise SystemExit("Live endpoint matching code does not match the boot image")
        print("live_ep_match_code_matches_boot=true")
    if args.gadget_base:
        gadget_ops = struct.unpack_from("<Q", at(args.gadget_base + 0x28, 8))[0]
        if not args.base <= gadget_ops < args.base + len(image):
            raise SystemExit("Invalid initial gadget operations pointer")
        match_ep = struct.unpack_from("<Q", at(gadget_ops + 0x50, 8))[0]
        print(f"initial_gadget_ops={gadget_ops:#x} match_ep_override={match_ep:#x}")
        if args.live_gadget_ops and args.live_match_ep:
            if args.live_gadget_ops.read_bytes() != struct.pack("<Q", gadget_ops) or args.live_match_ep.read_bytes() != struct.pack("<Q", match_ep):
                raise SystemExit("Live gadget operations/callback differ from the boot image")
            print("live_gadget_ops_and_match_ep_match_boot=true")
        elif args.live_gadget_ops or args.live_match_ep:
            raise SystemExit("Require both bounded pointer snapshots")
    elif args.live_gadget_ops or args.live_match_ep:
        raise SystemExit("Require an independently identified gadget base")

    decoder = Cs(CS_ARCH_ARM64, CS_MODE_LITTLE_ENDIAN)
    for index, (address, kind, name) in enumerate(symbols):
        if kind not in ("t", "T") or not re.fullmatch(args.functions, name):
            continue
        end = next((entry[0] for entry in symbols[index + 1 :] if entry[0] > address), args.base + len(image))
        if end - address > 16384:
            raise SystemExit("Refuse an unbounded function disassembly")
        code = at(address, end - address)
        print(f"\n{name}: {address:#x}, {len(code)} bytes, sha256={hashlib.sha256(code).hexdigest()}")
        constants = {}
        for instruction in decoder.disasm(code, address):
            mnemonic, operands = instruction.mnemonic, instruction.op_str
            annotation = ""
            if mnemonic.startswith("b") or mnemonic in ("cbz", "cbnz", "tbz", "tbnz"):
                constants.clear()
            if mnemonic in ("b", "bl") and operands.startswith("#0x"):
                annotation = describe(int(operands[1:], 16))
            if mnemonic in ("adr", "adrp"):
                match = re.fullmatch(r"(x\d+), #(0x[0-9a-f]+)", operands)
                if match:
                    constants[match[1]] = int(match[2], 16)
                    annotation = describe(constants[match[1]])
            elif mnemonic == "add":
                match = re.fullmatch(r"(x\d+), (x\d+), #(0x[0-9a-f]+|\d+)", operands)
                if match and match[2] in constants:
                    constants[match[1]] = constants[match[2]] + int(match[3], 0)
                    annotation = describe(constants[match[1]])
            elif mnemonic in ("ldr", "ldrb", "ldrh", "ldur", "mov", "ldp"):
                register = operands.split(",", 1)[0]
                constants.pop(register, None)
                if register.startswith("w"):
                    constants.pop("x" + register[1:], None)
            print(f"{instruction.address:016x}: {mnemonic:8s} {operands}" + (f" ; {annotation}" if annotation else ""))

    print("\nEndpoint name locations/pointer references (initial image, not live allocation):")
    endpoints = []
    for match in re.finditer(rb"ep[0-9](?:in|out)?[-_](?:bulk|int|iso)\0", image):
        address = args.base + match.start()
        name = match[0][:-1].decode("ascii")
        needle = struct.pack("<Q", address)
        refs = [args.base + offset.start() for offset in re.finditer(re.escape(needle), image)]
        print(f"{name}: {address:#x}, refs=" + ",".join(hex(ref) for ref in refs))
        for ref in refs:
            if args.verbose:
                print(f"  initial_data@{ref:#x} (+0..95)=" + at(ref, 96).hex(" "))
            # The other reference is a FIFO table, not struct usb_ep. Its
            # second word is not a pointer to an operations table in Image.
            ops = struct.unpack_from("<Q", at(ref + 8, 8))[0]
            if args.base <= ops < args.base + len(image):
                endpoints.append((ref - 8, name))

    if args.live_endpoints:
        if not args.live_code:
            raise SystemExit("Require a verified live code snapshot before decoding live structs")
        if hashlib.sha256(args.live_code.read_bytes()).hexdigest() != "33734cf92b9578c4fcdee8492780eec195b9aec1f3a8719699d7e6dc7819cd15":
            raise SystemExit("Struct offsets are verified only for this endpoint matcher checksum")
        endpoints.sort()
        if len(endpoints) != 6 or any(
            second[0] - first[0] != 0x88 for first, second in zip(endpoints, endpoints[1:])
        ):
            raise SystemExit("Unexpected sunxi endpoint layout; refuse guessed offsets")
        ep0_address = endpoints[0][0] - 0x88
        ep0_name = struct.unpack_from("<Q", at(ep0_address + 8, 8))[0]
        if at(ep0_name, 4) != b"ep0\0":
            raise SystemExit("Unexpected ep0 pointer")
        endpoints.insert(0, (ep0_address, "ep0"))
        live = args.live_endpoints.read_bytes()
        if len(live) != 6 * 0x88 + 0x48:
            raise SystemExit("Unexpected bounded endpoint snapshot size")
        if args.gadget_base:
            head = args.gadget_base + 0x38
            nodes = {}
            for address, name in endpoints[1:]:
                offset = address - ep0_address
                nodes[address + 0x18] = struct.unpack_from("<QQ", live, offset + 0x18)
            first = [node for node, (following, previous) in nodes.items() if previous == head]
            if len(first) != 1:
                raise SystemExit("Cannot identify the live endpoint list start")
            seen, node, previous = set(), first[0], head
            while node != head:
                if node not in nodes or node in seen or nodes[node][1] != previous:
                    raise SystemExit("Live endpoint list is incomplete/changed; refuse a guessed endpoint count")
                seen.add(node)
                previous, node = node, nodes[node][0]
            if len(seen) != 6:
                raise SystemExit("Unexpected live allocation endpoint count")
            print("live_ep_list_allocation_count=6 (ep0 separate)")
        print(f"\nLive endpoints, usb_ep base={ep0_address:#x}, stride=0x88:")
        for address, name in endpoints:
            offset = address - ep0_address
            initial = at(address, 0x48)
            current = live[offset : offset + 0x48]
            if current[8:24] != initial[8:24] or current[0x28:0x2C] != initial[0x28:0x2C]:
                raise SystemExit("Live endpoint name/ops/capabilities changed; refuse stale image assumptions")
            caps, claimed = current[0x28], current[0x2C]
            if claimed not in (0, 1):
                raise SystemExit("Invalid claimed flag")
            types = [label for bit, label in enumerate(("control", "iso", "bulk", "interrupt")) if caps & (1 << bit)]
            directions = [label for bit, label in ((4, "IN"), (5, "OUT")) if caps & (1 << bit)]
            limit = struct.unpack_from("<H", current, 0x30)[0]
            driver_data = struct.unpack_from("<Q", current, 0)[0]
            descriptor = struct.unpack_from("<Q", current, 0x38)[0]
            print(f"{name}: caps={caps:#04x} types={'|'.join(types)} dir={'|'.join(directions)} maxpacket_limit={limit} claimed={claimed} driver_data_present={bool(driver_data)} descriptor_present={bool(descriptor)} assigned_address={current[0x35]:#04x}")
        print("claimed is an autoconfiguration flag, not a reliable free/in-use indicator after composite bind/reset.")


if __name__ == "__main__":
    main()
