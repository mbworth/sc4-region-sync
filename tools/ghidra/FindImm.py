# Find code that pushes/moves each 32-bit constant (byte search), print containing function.
import jarray
args = getScriptArgs(); mem = currentProgram.getMemory(); fm = currentProgram.getFunctionManager()
space = currentProgram.getAddressFactory().getDefaultAddressSpace()
for arg in args:
    v = int(arg, 16)
    b = jarray.array([x - 256 if x > 127 else x for x in [(v >> (8*i)) & 0xff for i in range(4)]], 'b')
    a = currentProgram.getMinAddress(); hits = []
    while True:
        a = mem.findBytes(a, b, None, True, monitor)
        if a is None or len(hits) > 30: break
        f = fm.getFunctionContaining(a)
        if f is None:
            # try to create function around: back off
            pass
        blk = mem.getBlock(a)
        hits.append("%s[%s]%s" % (a, blk.getName() if blk else "?", (" in " + str(f.getEntryPoint())) if f else ""))
        a = a.add(1)
    print("IMM %08x: %s" % (v, hits))
