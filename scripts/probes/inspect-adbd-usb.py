"""Read-only disassembly of USB ownership in the Brick Pro's stock adbd.

Use the already installed capstone/pyelftools through PYTHONPATH. This never
executes or patches the firmware binary; redirected stdout is evidence only.
"""

import argparse
import hashlib
import io
import re
from pathlib import Path

from capstone import CS_ARCH_ARM64, CS_MODE_LITTLE_ENDIAN, Cs
from elftools.elf.elffile import ELFFile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    parser.add_argument("--functions", default="usb_ffs_open_thread|usb_ffs_kick")
    args = parser.parse_args()
    payload = args.binary.read_bytes()
    elf = ELFFile(io.BytesIO(payload))
    if elf["e_machine"] != "EM_AARCH64":
        raise SystemExit("Expected AArch64 ELF")
    symbols = list(elf.get_section_by_name(".symtab").iter_symbols())
    names = {symbol["st_value"]: symbol.name for symbol in symbols if symbol.name}
    dynsym = elf.get_section_by_name(".dynsym")
    plt = elf.get_section_by_name(".plt")
    relocs = elf.get_section_by_name(".rela.plt")
    if plt and relocs:
        for index, reloc in enumerate(relocs.iter_relocations()):
            names[plt["sh_addr"] + 32 + index * 16] = (
                dynsym.get_symbol(reloc["r_info_sym"]).name + "@plt"
            )
    sections = [section for section in elf.iter_sections() if section["sh_flags"] & 2]

    def at_address(address, size):
        for section in sections:
            if section["sh_addr"] <= address < section["sh_addr"] + section["sh_size"]:
                offset = address - section["sh_addr"]
                return section.data()[offset : offset + size]
        return b""

    def describe_address(address):
        data = at_address(address, 160)
        terminator = data.find(b"\0")
        if terminator > 0:
            string = data[:terminator]
            if all(byte in (9, 10, 13) or 32 <= byte < 127 for byte in string):
                return repr(string.decode("ascii"))
        return names.get(address, "")

    print("SHA256", hashlib.sha256(payload).hexdigest())
    decoder = Cs(CS_ARCH_ARM64, CS_MODE_LITTLE_ENDIAN)
    for symbol in symbols:
        if symbol["st_info"]["type"] != "STT_FUNC" or not re.fullmatch(args.functions, symbol.name):
            continue
        address, size = symbol["st_value"], symbol["st_size"]
        print(f"\n{symbol.name}: {address:#x}, {size} bytes")
        page_registers = {}
        for instruction in decoder.disasm(at_address(address, size), address):
            annotation = ""
            mnemonic, operands = instruction.mnemonic, instruction.op_str
            # Constants are only valid in this straight-line block. Calls and
            # branches clobber/merge registers; do not label stale addresses.
            if mnemonic.startswith("b") or mnemonic in ("cbz", "cbnz", "tbz", "tbnz"):
                page_registers.clear()
            if mnemonic in ("b", "bl") and operands.startswith("#0x"):
                annotation = names.get(int(operands[1:], 16), "")
            if mnemonic in ("adrp", "adr"):
                match = re.fullmatch(r"(x\d+), #(0x[0-9a-f]+)", operands)
                if match:
                    register, immediate = match.groups()
                    page_registers[register] = int(immediate, 16)
                    annotation = describe_address(page_registers[register])
            elif mnemonic == "add":
                match = re.fullmatch(r"(x\d+), (x\d+), #(0x[0-9a-f]+|\d+)", operands)
                if match and match[2] in page_registers:
                    destination, source, immediate = match.groups()
                    page_registers[destination] = page_registers[source] + int(immediate, 0)
                    annotation = describe_address(page_registers[destination])
            elif mnemonic in ("ldr", "ldrb", "ldrh", "ldur", "mov", "ldp"):
                destination = operands.split(",", 1)[0]
                page_registers.pop(destination, None)
                if destination.startswith("w"):
                    page_registers.pop("x" + destination[1:], None)
            print(
                f"{instruction.address:08x}: {mnemonic:8s} {operands}"
                + (f" ; {annotation}" if annotation else "")
            )


if __name__ == "__main__":
    main()
