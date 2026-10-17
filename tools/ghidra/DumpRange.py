# Ghidra headless script (Jython). Dump a data range as dwords, annotating code
# pointers with ret-imm, body size and the first instructions, to lay out
# back-to-back vtables. Args: start end (hex).
import struct
from ghidra.program.model.address import AddressSet
args = getScriptArgs()
start = int(args[0], 16); end = int(args[1], 16)
mem = currentProgram.getMemory(); listing = currentProgram.getListing()
fm = currentProgram.getFunctionManager(); space = currentProgram.getAddressFactory().getDefaultAddressSpace()
def ptr_at(a):
    try: return mem.getInt(a) & 0xffffffff
    except: return None
def is_code(v):
    if v is None: return False
    b = mem.getBlock(space.getAddress(v)); return b is not None and b.isExecute()
def ensure_func(fa):
    if listing.getInstructionAt(fa) is None: disassemble(fa)
    if fm.getFunctionAt(fa) is None: createFunction(fa, None)
def info(fa):
    ensure_func(fa); f = fm.getFunctionAt(fa)
    body = f.getBody() if f else AddressSet(fa, fa.add(64))
    it = listing.getInstructions(body, True); ret = -1; n = 0; first = []
    while it.hasNext() and n < 800:
        ins = it.next(); n += 1
        if len(first) < 4: first.append(str(ins))
        if ins.getMnemonicString() == "RET":
            ret = max(ret, ins.getScalar(0).getUnsignedValue() if ins.getNumOperands() else 0)
    size = f.getBody().getNumAddresses() if f else -1
    return ret, size, " ; ".join(first)
a = space.getAddress(start); i = 0
while a.getOffset() < end:
    v = ptr_at(a)
    if is_code(v):
        fa = space.getAddress(v); ret, size, first = info(fa)
        print("%s  [%3d] %s ret=%-3s size=%-5d %s" % (a, i, fa, ret, size, first))
    else:
        print("%s  [%3d] %08x" % (a, i, v if v is not None else 0))
    a = a.add(4); i += 1
