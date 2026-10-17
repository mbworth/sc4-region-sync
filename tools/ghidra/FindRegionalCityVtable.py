# Ghidra headless script (Jython). Two strategies to find cSC4RegionalCity's
# vtable, then dump slots with each callee's stack cleanup (ret imm16 = total
# argument bytes for thiscall) and body size.
#   analyzeHeadless <proj> SC4 -process SimCity4.exe -noanalysis \
#       -scriptPath tools/ghidra -postScript FindRegionalCityVtable.py
from ghidra.program.model.address import AddressSet
import struct

N_SLOTS = 72
NAME = "cSC4RegionalCity"

mem = currentProgram.getMemory()
listing = currentProgram.getListing()
fm = currentProgram.getFunctionManager()
af = currentProgram.getAddressFactory()
space = af.getDefaultAddressSpace()

def blocks(pred):
    return [b for b in mem.getBlocks() if b.isInitialized() and pred(b)]

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
    b = mem.getBlock(space.getAddress(v))
    return b is not None and b.isExecute()

def ensure_func(fa):
    if listing.getInstructionAt(fa) is None:
        disassemble(fa)
    if fm.getFunctionAt(fa) is None:
        createFunction(fa, None)

def ret_imm(fa):
    ensure_func(fa)
    func = fm.getFunctionAt(fa)
    body = func.getBody() if func else AddressSet(fa, fa.add(200))
    it = listing.getInstructions(body, True); seen = -1; n = 0
    while it.hasNext() and n < 600:
        ins = it.next(); n += 1
        if ins.getMnemonicString() == "RET":
            if ins.getNumOperands() == 0: seen = max(seen, 0)
            else:
                try: seen = max(seen, ins.getScalar(0).getUnsignedValue())
                except: pass
    return seen

def fsize(fa):
    f = fm.getFunctionAt(fa)
    return f.getBody().getNumAddresses() if f else -1

def dump(vt, n=N_SLOTS):
    print("=== vtable at %s" % vt)
    for i in range(n):
        v = ptr_at(vt.add(i * 4))
        if not is_code(v):
            print("  [%2d] end" % i); break
        fa = space.getAddress(v)
        print("  [%2d] %s ret=%-3s size=%-5d" % (i, fa, ret_imm(fa), fsize(fa)))

# --- strategy A: raw pointers to the class-name string
for s in find_bytes(NAME + "\x00"):
    print("string at %s" % s)
    le = struct.pack("<I", s.getOffset())
    for hit in find_bytes(le):
        f = fm.getFunctionContaining(hit)
        print("  pointer to string at %s  (%s)" % (hit, f.getName() if f else "data"))

# --- strategy A2: dump data around the pointer to the string
for s_ in find_bytes(NAME + "\x00"):
    le = struct.pack("<I", s_.getOffset())
    for hit in find_bytes(le):
        print("context around %s:" % hit)
        for off in range(-48, 52, 4):
            a = hit.add(off); v = ptr_at(a)
            tag = ""
            if is_code(v): tag = "CODE"
            elif v is not None and mem.getBlock(space.getAddress(v)) is not None: tag = "DATA"
            print("  %s: %08x %s" % (a, v if v is not None else 0, tag))

# --- strategy B: enumerate vtables in non-exec blocks: runs of >=66 code ptrs
cands = []
for b in blocks(lambda b: not b.isExecute()):
    a = b.getStart(); end = b.getEnd()
    run_start = None; run = 0
    while a.compareTo(end) < 0:
        v = ptr_at(a)
        if is_code(v):
            if run == 0: run_start = a
            run += 1
        else:
            if run >= 66: cands.append((run_start, run))
            run = 0
        a = a.add(4)
print("vtable-like runs (>=66 code ptrs): %d" % len(cands))

# Score against what the game proved correct: slot 5 GetPosition(int&,int&) ret=8,
# slot 9 GetPopulation() ret=0, slot 33 GetCityName(cIGZString&) ret=4,
# slot 43 GetEstablished() ret=0, slot 50 GetBudget() ret=0, slot 58 GetProduced(int) ret=4.
want = {0: 8, 1: 0, 2: 0, 5: 8, 7: 8, 9: 0, 10: 0, 11: 0, 16: 4, 26: 4, 33: 4, 43: 0, 50: 0, 52: 0, 54: 4, 58: 4, 62: 4, 64: 4}
scored = []
for (vt, run) in cands:
    score = 0
    for slot, r in want.items():
        if slot >= run: continue
        fa = space.getAddress(ptr_at(vt.add(slot * 4)))
        if ret_imm(fa) == r: score += 1
    scored.append((score, vt, run))
scored.sort(key=lambda t: -t[0])
for score, vt, run in scored[:8]:
    print("candidate %s run=%d score=%d/%d" % (vt, run, score, len(want)))
for score, vt, run in scored[:2]:
    dump(vt)
