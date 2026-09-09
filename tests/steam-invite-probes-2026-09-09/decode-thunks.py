#!/usr/bin/env python3
"""
decode-thunks.py -- prove the ISteamFriends slot map against the LIVE client
by READING BYTES.  Nothing is called.

Every entry in the client's SteamFriends0NN vtable is a small forwarding thunk
onto ONE shared implementation object:

    mov 0x8(%rdi),%rdi          ; hop to the inner implementation object
    [ small argument fixups ]   ; e.g. movswl %si,%esi
    mov (%rdi),%rax             ; its vtable
    jmp/call *0xNN(%rax)        ; slot NN/8 of the INNER vtable

The inner vtable is the single implementation that BOTH SteamFriends017 and
SteamFriends018 adapt, so the inner slot number is a version-independent
IDENTITY for a method.  Aligning the two public vtables by inner slot, and
then asking Valve's own Proton bridge what it calls each public slot, is a
complete cross-check of the whole map -- with no vtable call anywhere.

Inputs (all produced without calling a slot):
  slot-offsets-018-vs-017.txt          <- vtable_read_probe, a pure memory read
  slots-SteamFriends018-proton10.txt   <- names, from Proton's disassembly
  slots-SteamFriends017-proton10.txt
Reads ~/.steam/steam/linux64/steamclient.so as a FILE.
"""
import os, re, struct, sys

SO = os.path.expanduser("~/.steam/steam/linux64/steamclient.so")

def load_segments(path):
    with open(path, "rb") as f:
        hdr = f.read(64)
        e_phoff, = struct.unpack_from("<Q", hdr, 0x20)
        e_phentsize, e_phnum = struct.unpack_from("<HH", hdr, 0x36)
        f.seek(e_phoff); ph = f.read(e_phentsize * e_phnum)
    segs = []
    for i in range(e_phnum):
        o = i * e_phentsize
        if struct.unpack_from("<I", ph, o)[0] != 1: continue
        p_offset, p_vaddr, _, p_filesz = struct.unpack_from("<QQQQ", ph, o + 8)
        segs.append((p_vaddr, p_vaddr + p_filesz, p_offset))
    return segs

SEGS = load_segments(SO)
BLOB = open(SO, "rb").read()

def at(vaddr, n):
    for lo, hi, off in SEGS:
        if lo <= vaddr < hi:
            fo = off + (vaddr - lo)
            return BLOB[fo:fo + n]
    return b""

def _load_from_rax(b):
    """`mov 0xNNN(%rax),%reg` -- the string-marshalling thunks load the inner
    slot into a register first and jmp through it, so the displacement is
    still the inner slot's byte offset."""
    for i in range(len(b) - 7):
        if b[i] in (0x48, 0x4C) and b[i + 1] == 0x8B:
            modrm = b[i + 2]
            if (modrm & 0xC7) == 0x80:            # mod=10, rm=000 (%rax), disp32
                return struct.unpack_from("<I", b, i + 3)[0] // 8
    return None


def inner_slot(vaddr, depth=0):
    """The inner-vtable slot this thunk dispatches to, or None."""
    if depth > 3: return None
    b = at(vaddr, 40)
    i = 0
    while i < len(b) - 6:
        # jmp rel32 -- a thunk that shares a tail with its neighbour
        if b[i] == 0xE9:
            rel, = struct.unpack_from("<i", b, i + 1)
            return inner_slot(vaddr + i + 5 + rel, depth + 1)
        if b[i] == 0xFF:
            m = b[i + 1]
            if m == 0x20: return 0                                    # jmp  *(%rax)
            if m == 0x10: return 0                                    # call *(%rax)
            if m == 0x60: return b[i + 2] // 8                        # jmp  *d8(%rax)
            if m == 0x50: return b[i + 2] // 8                        # call *d8(%rax)
            if m == 0xA0: return struct.unpack_from("<I", b, i + 2)[0] // 8   # jmp  *d32(%rax)
            if m == 0x90: return struct.unpack_from("<I", b, i + 2)[0] // 8   # call *d32(%rax)
        i += 1
    return _load_from_rax(b)

# Each dump names its FIRST interface's slots directly (column 1); the second
# column is the other interface shifted by one, which is only a convenience.
# Reading both dumps gives every slot of both tables from a primary column.
offs = {"018": {}, "017": {}}
for fn, first in (("slot-offsets-018-vs-017.txt", "018"),
                  ("slot-offsets-017-vs-018.txt", "017")):
    for line in open(fn):
        m = re.match(r"\s*slot (\d+)\s+SteamFriends(\d+) \+0x([0-9a-f]+)", line)
        if m:
            offs[first][int(m.group(1))] = int(m.group(3), 16)

def names(path):
    d = {}
    for line in open(path):
        p = line.split()
        if len(p) == 3 and p[0].isdigit(): d[int(p[0])] = p[2]
    return d

n = {"018": names("slots-SteamFriends018-proton10.txt"),
     "017": names("slots-SteamFriends017-proton10.txt")}

inner = {v: {s: inner_slot(a) for s, a in offs[v].items()} for v in offs}

print("== ISteamFriends: public slot -> inner implementation slot, read out of the LIVE client ==")
print("   (nothing was called; these are the bytes of each vtable thunk)\n")
print("%-5s %-46s %-7s %-7s %-5s %s" % ("018", "Proton's name for 018's slot", "inner", "inner", "017", "Proton's name for that 017 slot"))

# inner slot -> 017 public slot
back17 = {}
for s, v in inner["017"].items():
    if v is None: continue
    back17.setdefault(v, [])
    back17[v].append(s)

agree = disagree = undec = 0
for s in sorted(inner["018"]):
    a = inner["018"][s]
    if a is None:
        print("%-5d %-46s %-7s %-7s %-5s %s" % (s, n["018"].get(s, "?"), "?", "-", "-", "(thunk not decodable)"))
        undec += 1; continue
    cand = back17.get(a, [])
    na0 = n["018"].get(s, "?")
    j = next((c for c in cand if n["017"].get(c) == na0), cand[0] if cand else None)
    nb = n["017"].get(j, "?") if j is not None else "(absent in 017)"
    na = n["018"].get(s, "?")
    same = (j is not None and na == nb)
    if j is None:
        verdict = "only in 018"
    elif same:
        agree += 1; verdict = ""
    else:
        disagree += 1; verdict = "  <-- NAMES DISAGREE"
    print("%-5d %-46s %-7d %-7s %-5s %s%s" % (s, na, a, a if j is not None else "-",
                                              j if j is not None else "-", nb, verdict))

print()
print("RESULT cross-version name agreement (018 vs 017, aligned by inner slot):")
print("  %d slots agree, %d disagree, %d thunks undecodable" % (agree, disagree, undec))
for want in ("InviteUserToGame", "SetListenForFriendsMessages", "GetFriendMessage",
             "GetFriendRichPresence", "SetRichPresence", "ClearRichPresence",
             "GetFriendCount", "GetFriendByIndex", "GetFriendGamePlayed", "GetPersonaName"):
    s = next((k for k, v in n["018"].items() if v == want), None)
    if s is None: continue
    print("  %-30s 018 slot %-3s inner slot %s" % (want, s, inner["018"].get(s)))
