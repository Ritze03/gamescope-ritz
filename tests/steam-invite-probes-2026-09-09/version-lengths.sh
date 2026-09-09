#!/bin/bash
cd "$(dirname "$0")"
P="/home/mo/Schinken/Linux/Steam/steamapps/common/Proton 10.0/files/lib/wine/x86_64-unix/lsteamclient.so"
echo "# ISteamFriends vtable length, LIVE CLIENT (read out of memory, nothing called)"
echo "# vs the method count in Valve's own Proton bridge for the same version string."
printf '%-18s %-14s %-14s %s\n' version live-vtable proton-methods agree
for v in SteamFriends013 SteamFriends014 SteamFriends015 SteamFriends016 SteamFriends017 SteamFriends018; do
  live=$(./vtable_read_probe "$v" "$v" 2>/dev/null | awk '/vtable length/{print $NF; exit}')
  pm=$(python3 extract-vtable-slots.py "$P" "$v" 2>/dev/null | head -1 | grep -oE '[0-9]+ methods' | cut -d' ' -f1)
  [ -z "$pm" ] && pm="(not in bridge)"
  if [ "$live" = "$pm" ]; then a=YES; elif [ "$pm" = "(not in bridge)" ]; then a="-"; else a="NO"; fi
  printf '%-18s %-14s %-14s %s\n' "$v" "$live" "$pm" "$a"
done
