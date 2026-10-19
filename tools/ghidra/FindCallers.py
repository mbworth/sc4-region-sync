# Ghidra headless script (Jython). For each target address (hex args), scan
# executable blocks for E8 rel32 calls landing on it and print the caller
# functions; also report data pointers to it (vtable slots).
import struct, jarray
args = getScriptArgs()
mem = currentProgram.getMemory(); listing = currentProgram.getListing()
fm = currentProgram.getFunctionManager(); space = currentProgram.getAddressFactory().getDefaultAddressSpace()

def ensure_func_containing(a):
    f = fm.getFunctionContaining(a)
    if f: return f
    if listing.getInstructionAt(a) is None:
        try: disassemble(a)
        except: pass
    return fm.getFunctionContaining(a)

for arg in args:
    target = int(arg, 16)
    print("target %08x" % target)
    for b in mem.getBlocks():
        if not b.isInitialized(): continue
        size = b.getSize(); base = b.getStart().getOffset()
        jb = jarray.zeros(size, 'b')
        try: mem.getBytes(b.getStart(), jb)
        except: continue
        buf = jb.tostring()
        if b.isExecute():
            i = 0
            while True:
                i = buf.find('\xe8', i)
                if i < 0 or i + 5 > size: break
                rel = struct.unpack_from("<i", buf, i + 1)[0]
                if (base + i + 5 + rel) & 0xffffffff == target:
                    a = space.getAddress(base + i)
                    f = ensure_func_containing(a)
                    print("  call at %s%s" % (a, (" in %s (size %d)" % (f.getEntryPoint(), f.getBody().getNumAddresses())) if f else ""))
                i += 1
        le = struct.pack("<I", target); i = 0
        while True:
            i = buf.find(le, i)
            if i < 0: break
            print("  pointer at %s (%s)" % (space.getAddress(base + i), "code" if b.isExecute() else "data"))
            i += 4
