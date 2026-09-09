#!/usr/bin/env python3
"""
extract-vtable-slots.py -- read ISteamFriends' vtable slot numbers out of
Valve's own Proton bridge, WITHOUT calling anything.

Proton's lsteamclient.so is Valve-authored glue that calls the SAME Linux
steamclient.so interfaces this fork binds to.  Every method gets a wrapper
named ISteam<Iface>_<Version>_<Method>, and each wrapper's body is literally

    mov (%rdi),%rdi          ; params->linux_side
    mov (%rdi),%rax          ; the vtable
    call *0xNN(%rax)         ; <-- the slot, in bytes

So `0xNN / 8` IS the slot index, stated by Valve's own compiler for that exact
interface version string.  This script disassembles every wrapper and prints
the map.  It reads a file; it loads no library and calls no Steam.

usage: extract-vtable-slots.py <lsteamclient.so> <InterfaceVersion e.g. SteamFriends018>
"""
import re, subprocess, sys

lib, iface = sys.argv[1], sys.argv[2]

# symbol table -> {name: addr} for the wrappers of this interface version
syms = subprocess.run(["nm", "--defined-only", lib], capture_output=True, text=True).stdout
pat = re.compile(r"^([0-9a-f]+)\s+\S+\s+(?:cpp|win)?ISteam\w*_%s_(\w+)$" % re.escape(iface))
funcs = {}
for line in syms.splitlines():
    m = pat.match(line.strip())
    if m:
        funcs[m.group(2)] = int(m.group(1), 16)

if not funcs:
    sys.exit("no wrappers for %s in %s" % (iface, lib))

# one objdump over the whole .text, then walk it once
dis = subprocess.run(["objdump", "-d", lib], capture_output=True, text=True).stdout
cur = None
out = {}
# any indirect call through a register: `call *0xNN(%reg)` or `call *(%reg)`.
# Each wrapper contains exactly one, and it is the vtable dispatch.
callpat = re.compile(r"call\s+\*(?:(0x[0-9a-f]+))?\(%[re][a-z0-9]+\)")
hdr = re.compile(r"^[0-9a-f]+ <(\S+)>:")
for line in dis.splitlines():
    h = hdr.match(line)
    if h:
        cur = h.group(1)
        continue
    if cur is None:
        continue
    name = cur
    prefix = "ISteam"
    # skip the wow64_ twins: same body, would just duplicate
    if name.startswith("wow64_"):
        continue
    m = re.match(r"(?:cpp)?ISteam\w*_%s_(\w+)$" % re.escape(iface), name)
    if not m:
        continue
    c = callpat.search(line)
    if c and m.group(1) not in out:
        off = int(c.group(1), 16) if c.group(1) else 0
        out[m.group(1)] = off

rows = sorted(out.items(), key=lambda kv: kv[1])
print("# %s -- %d methods, from %s" % (iface, len(rows), lib))
print("# slot  byte-offset  method")
for name, off in rows:
    assert off % 8 == 0, (name, off)
    print("%5d  0x%-6x     %s" % (off // 8, off, name))
missing = sorted(set(funcs) - set(out))
if missing:
    print("# no vtable call found in: %s" % ", ".join(missing))
