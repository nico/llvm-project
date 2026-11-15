#!/usr/bin/env python3
"""Writes a minimal arm64 Mach-O executable with a debug map, as the linker
would leave it, for dsymutil2 tests. It has no contents besides the symbol
table: dsymutil2 reads nothing else from the binary.

usage: make-binary.py OUTPUT SPEC...

  object:PATH             starts the debug map entry of an object file
  fn:NAME:ADDRESS:SIZE    a function of that object, linked at ADDRESS
  global:NAME:ADDRESS     a global variable
  static:NAME:ADDRESS     a static variable

__TEXT,__text is at 0x1000 and __DATA,__data at 0x4000, 0x1000 bytes each.
"""

import struct
import sys

N_GSYM, N_FUN, N_STSYM, N_SO, N_OSO = 0x20, 0x24, 0x26, 0x64, 0x66
N_SECT_EXT = 0x0F
TEXT, DATA = 1, 2


def main():
    output, specs = sys.argv[1], sys.argv[2:]
    strings = bytearray(b"\0")
    symbols = bytearray()

    def add(name, kind, section, desc, value):
        nonlocal symbols
        index = 0
        if name:
            index = len(strings)
            strings.extend(name.encode() + b"\0")
        symbols += struct.pack("<IBBHQ", index, kind, section, desc, value)

    regular = []
    in_object = False
    for spec in specs:
        kind, *args = spec.split(":")
        if kind == "object":
            if in_object:
                add("", N_SO, TEXT, 0, 0)
            add("source.cc", N_SO, 0, 0, 0)
            add(args[0], N_OSO, 0, 1, 0)
            in_object = True
        elif kind == "fn":
            add(args[0], N_FUN, TEXT, 0, int(args[1], 0))
            add("", N_FUN, 0, 0, int(args[2], 0))
            regular.append((args[0], TEXT, int(args[1], 0)))
        elif kind == "global":
            add(args[0], N_GSYM, 0, 0, 0)
            regular.append((args[0], DATA, int(args[1], 0)))
        elif kind == "static":
            add(args[0], N_STSYM, DATA, 0, int(args[1], 0))
        else:
            sys.exit("unknown spec " + spec)
    if in_object:
        add("", N_SO, TEXT, 0, 0)
    for name, section, value in regular:
        add(name, N_SECT_EXT, section, 0, value)

    def segment(name, vmaddr, vmsize, fileoff, filesize, prot, sections):
        size = 72 + 80 * len(sections)
        command = struct.pack("<II16sQQQQiiII", 0x19, size, name, vmaddr,
                              vmsize, fileoff, filesize, prot, prot,
                              len(sections), 0)
        for sectname, addr, flags in sections:
            command += struct.pack("<16s16sQQIIIIIIII", sectname, name, addr,
                                   0x1000, 0, 0, 0, 0, flags, 0, 0, 0)
        return command

    num_symbols = len(symbols) // 16
    commands_size = (72 + 80) * 2 + 72 + 24 + 24
    symoff = 32 + commands_size
    stroff = symoff + len(symbols)
    S_ZEROFILL = 1
    commands = [
        segment(b"__TEXT", 0, 0x4000, 0, 0, 5,
                [(b"__text", 0x1000, 0x80000400 | S_ZEROFILL)]),
        segment(b"__DATA", 0x4000, 0x4000, 0, 0, 3,
                [(b"__data", 0x4000, S_ZEROFILL)]),
        segment(b"__LINKEDIT", 0x8000, 0x4000, symoff,
                len(symbols) + len(strings), 1, []),
        struct.pack("<IIIIII", 0x2, 24, symoff, num_symbols, stroff,
                    len(strings)),
        struct.pack("<II16s", 0x1B, 24, bytes(range(0x10, 0x20))),
    ]
    assert sum(len(c) for c in commands) == commands_size
    header = struct.pack("<IIIIIIII", 0xFEEDFACF, 0x0100000C, 0, 2,
                         len(commands), commands_size, 0, 0)
    with open(output, "wb") as f:
        f.write(header + b"".join(commands) + symbols + strings)


if __name__ == "__main__":
    main()
