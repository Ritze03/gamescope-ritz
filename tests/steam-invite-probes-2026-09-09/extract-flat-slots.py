#!/usr/bin/env python3
"""
extract-flat-slots.py -- read ISteamFriends' slot numbers out of a game's own
shipped libsteam_api.so.  Nothing is loaded and nothing is called.

Valve's redistributable exports the flat API, and every flat wrapper is

    mov (%rdi),%rax        ; the interface's vtable
    jmp *0xNN(%rax)        ; the slot

so 0xNN/8 is the slot index, as Valve's SDK compiled it for whichever
interface version string that copy binds.

usage: extract-flat-slots.py <libsteam_api.so> [ISteamFriends]
"""
import re, subprocess, sys

lib = sys.argv[1]
iface = sys.argv[2] if len(sys.argv) > 2 else "ISteamFriends"

syms = subprocess.run(["nm", "-D", "--defined-only", lib], capture_output=True, text=True).stdout
want = {}
for line in syms.splitlines():
    m = re.match(r"^([0-9a-f]+)\s+\S+\s+SteamAPI_%s_(\w+)$" % re.escape(iface), line.strip())
    if m:
        want[m.group(2)] = int(m.group(1), 16)

dis = subprocess.run(["objdump", "-d", lib], capture_output=True, text=True).stdout
cur, out = None, {}
hdr = re.compile(r"^[0-9a-f]+ <(\S+)>:")
cal = re.compile(r"jmp\s+\*(?:(0x[0-9a-f]+))?\(%rax\)")
for line in dis.splitlines():
    h = hdr.match(line)
    if h:
        cur = h.group(1); continue
    if not cur or not cur.startswith("SteamAPI_%s_" % iface):
        continue
    name = cur[len("SteamAPI_%s_" % iface):]
    if name in out:
        continue
    c = cal.search(line)
    if c:
        out[name] = (int(c.group(1), 16) if c.group(1) else 0)

ver = subprocess.run(["strings", "-n", "10", lib], capture_output=True, text=True).stdout
vers = sorted(set(re.findall(r"^(SteamFriends\d{3})$", ver, re.M)))
print("# %s from %s" % (iface, lib))
print("# interface version strings inside it: %s" % (" ".join(vers) or "?"))
print("# %d flat wrappers, %d decoded" % (len(want), len(out)))
print("# slot  method")
for name, off in sorted(out.items(), key=lambda kv: kv[1]):
    print("%5d  %s" % (off // 8, name))
missing = sorted(set(want) - set(out))
if missing:
    print("# not decoded: %s" % ", ".join(missing))
