import struct, sys, collections
for path in sys.argv[1:]:
    d = open(path,'rb').read()
    i = 0; stats = collections.Counter(); arches = collections.Counter()
    while True:
        i = d.find(b'\x50\xed\x55\xba', i)
        if i < 0: break
        magic, ver, hs, fsz = struct.unpack_from('<IHHQ', d, i)
        if ver != 1 or hs != 16 or fsz > len(d): i += 4; continue
        p = i + hs; end = p + fsz
        while p < end:
            kind, _u, ehs, psz, csz, _u2, mn, mj, arch, on, ol, flags, _z, dsz = struct.unpack_from('<HHIQIIHHIIIQQQ', d, p)
            if ehs == 0: break
            comp = bool(flags & 0x2000)
            stats[(('PTX' if kind == 1 else 'ELF' if kind == 2 else kind), comp)] += 1
            arches[arch] += 1
            p += ehs + psz
        i = end
    print(path, dict(stats), dict(arches))
