# Ghidra headless script (Jython). Find ASCII substrings (args, case-insensitive)
# in data blocks and list code references to the containing string.
import re, jarray, struct
args = getScriptArgs()
mem = currentProgram.getMemory(); listing = currentProgram.getListing()
fm = currentProgram.getFunctionManager(); rm = currentProgram.getReferenceManager()
space = currentProgram.getAddressFactory().getDefaultAddressSpace()

def read_str(a):
    # back up to the string start, then read forward
    s = a
    while True:
        p = s.subtract(1)
        try: c = mem.getByte(p) & 0xff
        except: break
        if c < 0x20 or c > 0x7e: break
        s = p
    out = []
    e = s
    for _ in range(200):
        try: c = mem.getByte(e) & 0xff
        except: break
        if c < 0x20 or c > 0x7e: break
        out.append(chr(c)); e = e.add(1)
    return s, "".join(out)

for needle in args:
    print("needle %r" % needle)
    pat = re.compile(re.escape(needle), re.I)
    for b in mem.getBlocks():
        if not b.isInitialized() or b.isExecute(): continue
        size = b.getSize(); chunk = 1 << 20; off = 0
        data = []
        while off < size:
            n = min(chunk, size - off)
            jb = jarray.zeros(n, 'b')
            try: mem.getBytes(b.getStart().add(off), jb)
            except: break
            data.append(jb.tostring()); off += n
        text = "".join(data)
        for m in pat.finditer(text):
            a = b.getStart().add(m.start())
            s, full = read_str(a)
            refs = []
            for r in rm.getReferencesTo(s):
                f = fm.getFunctionContaining(r.getFromAddress())
                refs.append("%s%s" % (r.getFromAddress(), (" in " + str(f.getEntryPoint())) if f else ""))
            # also raw pointer search for the string start (unanalyzed refs)
            le = struct.pack("<I", s.getOffset())
            raw = []
            for cb in mem.getBlocks():
                if not cb.isInitialized(): continue
                addr = cb.getStart()
                while True:
                    found = mem.findBytes(addr, cb.getEnd(), le, None, True, monitor)
                    if found is None: break
                    f = fm.getFunctionContaining(found)
                    raw.append("%s%s" % (found, (" in " + str(f.getEntryPoint())) if f else ""))
                    addr = found.add(1)
                    if len(raw) > 20: break
            print("  %s %r refs=%s raw=%s" % (s, full[:120], refs[:8], raw[:8]))
