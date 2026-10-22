# Prints each thread of an x86 minidump with the module+offset of every code pointer on
# its stack (innermost first). Usage: python3 -I tools/minidump_stack.py dump.dmp
# Pair with tools/pe_exports.py to turn system DLL offsets into export names.
import struct, sys
d = open(sys.argv[1], 'rb').read()
sig, ver, nstreams, rva = struct.unpack_from('<4sIII', d, 0)
streams = {}
for i in range(nstreams):
    t, sz, r = struct.unpack_from('<III', d, rva + 12 * i); streams[t] = (sz, r)
# modules
mods = []
sz, r = streams[4]; n = struct.unpack_from('<I', d, r)[0]
for i in range(n):
    base, size, _, _, nameRva = struct.unpack_from('<QIIII', d, r + 4 + 108 * i)
    ln = struct.unpack_from('<I', d, nameRva)[0]; name = d[nameRva + 4: nameRva + 4 + ln].decode('utf-16le')
    mods.append((base, size, name.split('\\')[-1]))
def mod(a):
    for b, s, n in mods:
        if b <= a < b + s: return n, a - b
    return None
# memory ranges (for reading stacks)
mem = []
sz, r = streams[5]; n = struct.unpack_from('<I', d, r)[0]
for i in range(n):
    start, dsz, drva = struct.unpack_from('<QII', d, r + 4 + 16 * i); mem.append((start, dsz, drva))
def read(a, ln):
    for s, z, rv in mem:
        if s <= a and a + ln <= s + z: return d[rv + (a - s): rv + (a - s) + ln]
    return None
sz, r = streams[3]; n = struct.unpack_from('<I', d, r)[0]
for i in range(n):
    tid, susp, prio, cls, teb, sstart, ssz, srva, csz, crva = struct.unpack_from('<IIIIQQIIII', d, r + 4 + 48 * i)
    eip = struct.unpack_from('<I', d, crva + 0xB8)[0]; esp = struct.unpack_from('<I', d, crva + 0xC4)[0]
    stack = d[srva: srva + ssz]
    frames = []
    for off in range(0, len(stack) - 3, 4):
        v = struct.unpack_from('<I', stack, off)[0]; m = mod(v)
        if m and m[0].lower() not in ('ntdll.dll', 'kernelbase.dll', 'kernel32.dll') or (m and len(frames) < 3):
            frames.append('%s+%x' % m if m[0] != 'SimCity 4.exe' else 'exe:%08x' % v)
    print('thread %d susp=%d eip=%s esp=%08x stack=%d bytes' % (tid, susp, mod(eip) or hex(eip), esp, ssz))
    print('   ', ' '.join(frames[:28]))
