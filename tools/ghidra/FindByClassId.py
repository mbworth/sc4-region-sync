# Ghidra headless script (Jython). Given 32-bit IDs (args), find code that uses
# them as immediates, the functions containing those uses, and the data
# pointers (vtable slots) to those functions; dump each such vtable.
import struct, sys
from ghidra.program.model.address import AddressSet

args = getScriptArgs()
IDS = [int(a, 16) for a in args] if args else [0xabf98cb5]
N_SLOTS = 72

mem = currentProgram.getMemory(); listing = currentProgram.getListing()
fm = currentProgram.getFunctionManager(); af = currentProgram.getAddressFactory()
space = af.getDefaultAddressSpace()

def blocks(pred): return [b for b in mem.getBlocks() if b.isInitialized() and pred(b)]
def find_bytes(data, pred=lambda b: True):
    out = []
    for b in blocks(pred):
        addr = b.getStart()
        while True:
            found = mem.findBytes(addr, b.getEnd(), data, None, True, monitor)
            if found is None: break
            out.append(found); addr = found.add(1)
    return out
def ptr_at(a):
    try: return mem.getInt(a) & 0xffffffff
    except: return None
def is_code(v):
    if v is None: return False
    b = mem.getBlock(space.getAddress(v)); return b is not None and b.isExecute()
def ensure_func(fa):
    if listing.getInstructionAt(fa) is None: disassemble(fa)
    if fm.getFunctionAt(fa) is None: createFunction(fa, None)
def ret_imm(fa):
    ensure_func(fa); func = fm.getFunctionAt(fa)
    body = func.getBody() if func else AddressSet(fa, fa.add(200))
    it = listing.getInstructions(body, True); seen = -1; n = 0
    while it.hasNext() and n < 800:
        ins = it.next(); n += 1
        if ins.getMnemonicString() == "RET":
            if ins.getNumOperands() == 0: seen = max(seen, 0)
            else:
                try: seen = max(seen, ins.getScalar(0).getUnsignedValue())
                except: pass
    return seen
def fsize(fa):
    f = fm.getFunctionAt(fa); return f.getBody().getNumAddresses() if f else -1
def dump(vt, n=N_SLOTS):
    print("=== vtable at %s" % vt)
    for i in range(n):
        v = ptr_at(vt.add(i * 4))
        if not is_code(v): print("  [%2d] end" % i); break
        fa = space.getAddress(v)
        print("  [%2d] %s ret=%-3s size=%-5d" % (i, fa, ret_imm(fa), fsize(fa)))

for cid in IDS:
    le = struct.pack("<I", cid)
    print("id %08x" % cid)
    funcs = set()
    for hit in find_bytes(le, lambda b: b.isExecute()):
        if listing.getInstructionContaining(hit) is None: disassemble(hit)
        f = fm.getFunctionContaining(hit)
        if f is None:
            # try to create a function at the nearest preceding defined start is unreliable; report raw
            print("  code use at %s (no function)" % hit); continue
        funcs.add(f.getEntryPoint()); print("  code use at %s in func %s (size %d)" % (hit, f.getEntryPoint(), fsize(f.getEntryPoint())))
    # show instructions around every code use (function or not)
    for hit in find_bytes(le, lambda b: b.isExecute()):
        ins = listing.getInstructionContaining(hit)
        if ins is None: continue
        back = ins
        for _ in range(8):
            p_ = back.getPrevious()
            if p_ is None: break
            back = p_
        cur = back; print("  --- around %s" % hit)
        for _ in range(18):
            if cur is None: break
            print("    %s  %s" % (cur.getAddress(), cur))
            cur = cur.getNext()
    for hit in find_bytes(le, lambda b: not b.isExecute()):
        print("  data use at %s" % hit)
    vts = set()
    for fa in funcs:
        for hit in find_bytes(struct.pack("<I", fa.getOffset()), lambda b: not b.isExecute()):
            a = hit
            while True:
                prev = a.subtract(4)
                if not is_code(ptr_at(prev)): break
                a = prev
            print("  func %s referenced from data %s -> vtable start %s (slot %d)" % (fa, hit, a, (hit.getOffset() - a.getOffset()) // 4))
            vts.add(a)
    for vt in sorted(vts, key=lambda x: x.getOffset()):
        dump(vt)
