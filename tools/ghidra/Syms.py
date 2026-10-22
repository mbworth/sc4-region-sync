# For each address: containing function, every referenced symbol name (imports etc.)
args = getScriptArgs()
fm = currentProgram.getFunctionManager(); listing = currentProgram.getListing()
st = currentProgram.getSymbolTable(); mem = currentProgram.getMemory()
space = currentProgram.getAddressFactory().getDefaultAddressSpace()
for arg in args:
    a = space.getAddress(int(arg, 16)); f = fm.getFunctionContaining(a)
    if f is None:
        try:
            disassemble(a); createFunction(a, None)
        except: pass
        f = fm.getFunctionContaining(a)
    if f is None: print("==== %s nofunc" % a); continue
    names = []
    for ins in listing.getInstructions(f.getBody(), True):
        for r in ins.getReferencesFrom():
            s = st.getPrimarySymbol(r.getToAddress())
            if s and not s.getName().startswith(("FUN_","LAB_","DAT_","s_","u_")) and s.getName() not in names: names.append(s.getName())
            # indirect via pointer
            try:
                p = space.getAddress(mem.getInt(r.getToAddress()) & 0xffffffff)
                s2 = st.getPrimarySymbol(p)
                if s2 and s2.isExternal() and s2.getName() not in names: names.append(s2.getName())
            except: pass
    print("==== %s in %s: %s" % (a, f.getEntryPoint(), names[:40]))
