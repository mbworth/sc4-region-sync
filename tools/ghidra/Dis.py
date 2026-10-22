# Disassemble ranges: args start:end hex
args = getScriptArgs(); listing = currentProgram.getListing()
space = currentProgram.getAddressFactory().getDefaultAddressSpace()
for arg in args:
    s, e = [int(x, 16) for x in arg.split(":")]
    a = space.getAddress(s)
    if listing.getInstructionAt(a) is None: disassemble(a)
    print("---- %s" % arg)
    ins = listing.getInstructionAt(a)
    while ins is not None and ins.getAddress().getOffset() < e:
        print("  %s  %s" % (ins.getAddress(), ins)); n = ins.getNext()
        if n is None or n.getAddress() != ins.getAddress().add(ins.getLength()):
            disassemble(ins.getAddress().add(ins.getLength())); n = listing.getInstructionAt(ins.getAddress().add(ins.getLength()))
        ins = n
