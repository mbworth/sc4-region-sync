# Ghidra headless script (Jython). For each hex address: containing function,
# strings it references, direct callers; for a data address: RTTI class name
# if it is a vtable. Used to read crash-report stacks.
from ghidra.program.model.symbol import RefType
args = getScriptArgs()
mem = currentProgram.getMemory(); listing = currentProgram.getListing()
fm = currentProgram.getFunctionManager(); rm = currentProgram.getReferenceManager()
space = currentProgram.getAddressFactory().getDefaultAddressSpace()
def A(v): return space.getAddress(v)
def rd(a):
    try: return mem.getInt(a) & 0xffffffff
    except: return None
def cstr(a, n=80):
    s = ""
    try:
        for i in range(n):
            c = mem.getByte(a.add(i)) & 0xff
            if c == 0: break
            if c < 32 or c > 126: return None
            s += chr(c)
    except: return None
    return s if len(s) >= 4 else None
def rtti_name(vt):
    col = rd(A(vt - 4))
    if col is None: return None
    td = rd(A(col + 12))
    if td is None: return None
    return cstr(A(td + 8))
def func_at(v):
    a = A(v); f = fm.getFunctionContaining(a)
    if f is None:
        if listing.getInstructionAt(a) is None:
            try: disassemble(a)
            except: pass
        try: createFunction(a, None)
        except: pass
        f = fm.getFunctionContaining(a)
    return f
for arg in args:
    v = int(arg, 16); a = A(v); b = mem.getBlock(a)
    print("==== %08x block=%s" % (v, b.getName() if b else None))
    if b is None: continue
    if not b.isExecute():
        n = rtti_name(v)
        print("  data; rtti=%s first dwords: %s" % (n, " ".join("%08x" % (rd(A(v + 4 * i)) or 0) for i in range(4))))
        for i in range(4):
            p = rd(A(v + 4 * i)); f = fm.getFunctionContaining(A(p)) if p else None
            if f: print("    slot%d -> %s" % (i, f.getName()))
        continue
    f = func_at(v)
    if f is None: print("  no function"); continue
    print("  func %s entry=%s size=%d" % (f.getName(), f.getEntryPoint(), f.getBody().getNumAddresses()))
    strs = []; vts = []
    it = listing.getInstructions(f.getBody(), True)
    for ins in it:
        for r in ins.getReferencesFrom():
            t = r.getToAddress()
            if t.getOffset() < 0x400000 or t.getOffset() > 0xB00000: continue
            if r.getReferenceType().isData():
                s = cstr(t)
                if s and s not in strs: strs.append(s)
                if r.getReferenceType() != RefType.READ:
                    n = rtti_name(t.getOffset())
                    if n and n not in vts: vts.append(n)
            elif r.getReferenceType().isCall():
                cf = fm.getFunctionContaining(t)
                if cf and not cf.getName().startswith("FUN_") and cf.getName() not in strs: strs.append("call:" + cf.getName())
    print("  strings/calls: %s" % (strs[:25],))
    if vts: print("  vtables stored: %s" % (vts[:10],))
    callers = []
    for r in rm.getReferencesTo(f.getEntryPoint()):
        if r.getReferenceType().isCall():
            cf = fm.getFunctionContaining(r.getFromAddress())
            if cf and cf.getName() not in callers: callers.append(cf.getName())
    print("  callers: %s" % (callers[:12],))
