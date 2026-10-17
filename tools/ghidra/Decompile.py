# Ghidra headless script (Jython). Decompile the functions at the given
# addresses (hex args) and print the C.
from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor
args = getScriptArgs()
space = currentProgram.getAddressFactory().getDefaultAddressSpace()
fm = currentProgram.getFunctionManager(); listing = currentProgram.getListing()
di = DecompInterface(); di.openProgram(currentProgram)
for a in args:
    fa = space.getAddress(int(a, 16))
    if listing.getInstructionAt(fa) is None: disassemble(fa)
    f = fm.getFunctionAt(fa)
    if f is None: f = createFunction(fa, None)
    print("##### %s %s" % (fa, f.getName() if f else "?"))
    if f is None: continue
    r = di.decompileFunction(f, 60, ConsoleTaskMonitor())
    if r.decompileCompleted(): print(r.getDecompiledFunction().getC())
    else: print("decompile failed: " + str(r.getErrorMessage()))
