#!/bin/bash
cd "$(dirname "$0")"
echo "# Cross-build agreement on SteamFriends018's vtable slots"
echo "# Each row is an INDEPENDENT build of Proton's lsteamclient (Valve / GE / CachyOS / tkg)."
echo "# Nothing is called: these numbers come out of each binary's disassembly."
printf '%-58s %-8s %-4s %-4s %-4s %-5s %-5s %-7s %-5s\n' build methods GPName GPState GFCount GFByIdx GFGame INVITE SetLstn
for L in \
 "/home/mo/Schinken/Linux/Steam/steamapps/common/Proton 7.0/dist/lib64/wine/x86_64-unix/lsteamclient.dll.so" \
 "/home/mo/Schinken/Linux/Steam/steamapps/common/Proton 9.0 (Beta)/files/lib64/wine/x86_64-unix/lsteamclient.so" \
 "/home/mo/Schinken/Linux/Steam/steamapps/common/Proton 10.0/files/lib/wine/x86_64-unix/lsteamclient.so" \
 "/home/mo/Schinken/Linux/Steam/steamapps/common/Proton 11.0/files/lib/wine/x86_64-unix/lsteamclient.so" \
 "/home/mo/Schinken/Linux/Steam/steamapps/common/Proton - Experimental/files/lib/wine/x86_64-unix/lsteamclient.so" \
 "/home/mo/Schinken/Linux/Steam/steamapps/common/Proton Hotfix/files/lib/wine/x86_64-unix/lsteamclient.so" \
 "/home/mo/Schinken/fake_home/mo/.local/share/Steam/compatibilitytools.d/GE-Proton9-20/files/lib64/wine/x86_64-unix/lsteamclient.so" \
 "/home/mo/Schinken/fake_home/mo/.local/share/Steam/compatibilitytools.d/GE-Proton10-28/files/lib/wine/x86_64-unix/lsteamclient.so" \
 "/home/mo/Schinken/fake_home/mo/.local/share/Steam/compatibilitytools.d/proton-cachyos-11.0-20260506-slr-x86_64_v3/files/lib/wine/x86_64-unix/lsteamclient.so" \
 "/home/mo/Schinken/fake_home/mo/.local/share/Steam/compatibilitytools.d/proton_tkg_experimental.bleeding.edge.10.0.338968.20260404/files/lib64/wine/x86_64-unix/lsteamclient.dll.so" ; do
  short=$(echo "$L" | sed 's#^/home/mo/Schinken/##;s#Linux/Steam/steamapps/common/##;s#fake_home/mo/.local/share/Steam/compatibilitytools.d/##;s#/files.*##;s#/dist.*##')
  if [ ! -f "$L" ]; then printf '%-58s MISSING\n' "$short"; continue; fi
  python3 extract-vtable-slots.py "$L" SteamFriends018 > /tmp/m.$$ 2>/dev/null
  get() { awk -v M="$1" '$3==M{print $1}' /tmp/m.$$; }
  printf '%-58s %-8s %-6s %-7s %-7s %-7s %-6s %-7s %-5s\n' "$short" \
    "$(head -1 /tmp/m.$$ | grep -oE '[0-9]+ methods' | cut -d' ' -f1)" \
    "$(get GetPersonaName)" "$(get GetPersonaState)" "$(get GetFriendCount)" \
    "$(get GetFriendByIndex)" "$(get GetFriendGamePlayed)" "$(get InviteUserToGame)" \
    "$(get SetListenForFriendsMessages)"
  rm -f /tmp/m.$$
done
