# Nearest preceding export for offsets into a PE file. Usage:
#   python3 -I tools/pe_exports.py path/ntdll.dll:7858c,93dc9 path/user32.dll:c7d60
import struct, sys
def exports(path):
    d = open(path, 'rb').read(); pe = struct.unpack_from('<I', d, 0x3c)[0]
    nsec = struct.unpack_from('<H', d, pe + 6)[0]; optsz = struct.unpack_from('<H', d, pe + 20)[0]
    exp_rva, exp_sz = struct.unpack_from('<II', d, pe + 24 + 96)
    secs = []
    for i in range(nsec):
        o = pe + 24 + optsz + 40 * i; va, vsz, raw, rawsz = struct.unpack_from('<IIII', d, o + 8)[0:4] if False else (struct.unpack_from('<I', d, o + 12)[0], struct.unpack_from('<I', d, o + 8)[0], struct.unpack_from('<I', d, o + 20)[0], struct.unpack_from('<I', d, o + 16)[0])
        secs.append((va, vsz, raw, rawsz))
    def r2o(rva):
        for va, vsz, raw, rawsz in secs:
            if va <= rva < va + max(vsz, rawsz): return raw + rva - va
    e = r2o(exp_rva); nfunc, nnames, addrs, names, ords = struct.unpack_from('<IIIII', d, e + 20)
    out = []
    for i in range(nnames):
        nrva = struct.unpack_from('<I', d, r2o(names) + 4 * i)[0]; o = struct.unpack_from('<H', d, r2o(ords) + 2 * i)[0]
        frva = struct.unpack_from('<I', d, r2o(addrs) + 4 * o)[0]; no = r2o(nrva); nm = d[no:d.index(b'\0', no)].decode()
        out.append((frva, nm))
    return sorted(out)
for spec in sys.argv[1:]:
    path, offs = spec.split(':'); ex = exports(path)
    for off in offs.split(','):
        v = int(off, 16); best = max([x for x in ex if x[0] <= v], key=lambda x: x[0])
        print('%s+%x = %s+0x%x' % (path.split('/')[-1], v, best[1], v - best[0]))
