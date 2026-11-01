"""Minimal x86 minidump stack walker (raw scan).

Recovers: module list, exception record + CONTEXT, crashing thread stack bytes.
Then scans the stack for DWORDs that land inside a loaded module and prints
them as module+RVA so we can read the call chain without a PDB.
"""
import struct, sys

def u32(b, o): return struct.unpack_from('<I', b, o)[0]
def u64(b, o): return struct.unpack_from('<Q', b, o)[0]

def main(path):
    d = open(path, 'rb').read()
    assert d[:4] == b'MDMP', 'not a minidump'
    nstreams = u32(d, 8)
    dir_rva = u32(d, 12)
    streams = {}
    for i in range(nstreams):
        o = dir_rva + i * 12
        stype = u32(d, o); dsize = u32(d, o + 4); srva = u32(d, o + 8)
        streams[stype] = (dsize, srva)

    mods = []
    if 4 in streams:
        _, rva = streams[4]
        n = u32(d, rva); o = rva + 4
        MODSZ = 108
        for i in range(n):
            base = u64(d, o); size = u32(d, o + 8); name_rva = u32(d, o + 20)
            nlen = u32(d, name_rva)
            name = d[name_rva + 4: name_rva + 4 + nlen].decode('utf-16-le', 'replace')
            mods.append((base, size, name.split('\\')[-1]))
            o += MODSZ
    mods.sort()

    def sym(addr):
        for base, size, name in mods:
            if base <= addr < base + size:
                return f'{name}+0x{addr-base:X}'
        return None

    print('=== modules ===')
    for base, size, name in mods:
        if any(k in name.lower() for k in ('netplay', 'efz', 'sdl', 'ntdll', 'kernel', 'user32', 'dinput', 'ddraw')):
            print(f'  {base:08X}-{base+size:08X}  {name}')

    exc_tid = None
    if 6 in streams:
        _, rva = streams[6]
        exc_tid = u32(d, rva)
        er = rva + 8
        code = u32(d, er); addr = u64(d, er + 16)
        ctx_size = u32(d, er + 16 + 8 + 4 + 4 + 15 * 8)
        ctx_rva = u32(d, er + 16 + 8 + 4 + 4 + 15 * 8 + 4)
        print(f'\n=== exception ===\n  code=0x{code:08X} addr=0x{addr:08X} tid={exc_tid} ctx@{ctx_rva} size={ctx_size}')
        # x86 CONTEXT: Eip at offset 0xB8, Esp 0xC4, Ebp 0xB4
        if ctx_size >= 0xCC:
            eip = u32(d, ctx_rva + 0xB8); esp = u32(d, ctx_rva + 0xC4); ebp = u32(d, ctx_rva + 0xB4)
            esi = u32(d, ctx_rva + 0xA0); edi = u32(d, ctx_rva + 0x9C); ebx = u32(d, ctx_rva + 0xA4)
            print(f'  EIP={eip:08X} ESP={esp:08X} EBP={ebp:08X} ESI={esi:08X} EDI={edi:08X} EBX={ebx:08X}')
            print(f'  EIP -> {sym(eip)}   ESI -> {sym(esi)}')

    stack_lo = stack_bytes = None
    if 3 in streams:
        _, rva = streams[3]
        n = u32(d, rva); o = rva + 4
        THSZ = 48
        for i in range(n):
            tid = u32(d, o)
            stack_start = u64(d, o + 24)
            mem_size = u32(d, o + 32); mem_rva = u32(d, o + 36)
            if tid == exc_tid:
                stack_lo = stack_start
                stack_bytes = d[mem_rva: mem_rva + mem_size]
            o += THSZ

    if stack_bytes is None:
        print('\n(no stack memory for crashing thread)')
        return

    # Classify an address as plausible CODE (EFZ .text ends ~0x477D82; netplay.dll code).
    def code_sym(addr):
        if 0x401000 <= addr < 0x478000:
            return f'EFZ.EXE+0x{addr-0x400000:X} (.text)'
        for base, size, name in mods:
            if 'netplay.dll' == name and base <= addr < base + size:
                return f'netplay.dll+0x{addr-base:X}'
        return None

    esp = u32(d, ctx_rva + 0xC4)
    print(f'\n=== raw words at ESP=0x{esp:08X} (top of faulting frame) ===')
    rs = esp - stack_lo
    for i in range(rs, min(rs + 0x60, len(stack_bytes) - 3), 4):
        val = u32(stack_bytes, i)
        print(f'  [{stack_lo + i:08X}] (+0x{stack_lo + i - esp:02X}) = {val:08X}  {sym(val) or ""}')

    print(f'\n=== live stack window from ESP=0x{esp:08X} (return addresses only) ===')
    start = esp - stack_lo
    for i in range(max(0, start), len(stack_bytes) - 3, 4):
        val = u32(stack_bytes, i)
        s = code_sym(val)
        if s:
            print(f'  [{stack_lo + i:08X}] (+0x{stack_lo + i - esp:04X}) = {val:08X}  {s}')

if __name__ == '__main__':
    main(sys.argv[1])
