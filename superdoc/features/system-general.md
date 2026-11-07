# System — `system.general`

The **System** area of the **Shell** in the SETTINGS rail group (`UI/Registry.cpp`): the clipboard-sync switch and its status.
It does not affect the game's picture. Its routing is per profile, unlike [appearance.md](appearance.md), which is global; that is
the main thing this page records. Both were one page until 2026-11-07 (text moved verbatim).


Code: `src/Overlay/PanelSystem.{h,cpp}`. The behaviour of clipboard sync itself is in
[clipboard-sync.md](clipboard-sync.md); this area is its switch.

#### How it works

- **One switch, per profile.** *Clipboard sync* (`system.clipboard_sync`) is
  `config::SystemSettings::clipboard_sync` (default on), a normal profile field written
  through `config::EnqueueRoutedWrite()`. A game profile may override it.
- **Two copies of the value, kept in step.** The nested backends read the atomic
  `gamescope::g_bClipboardSyncEnabled` (`src/Clipboard/ClipboardSync.h`) on each event, as
  does the outbound gate in `steamcompmgr.cpp`. The setter stores the atomic first and then
  queues the routed write. `EnsureConfigLoaded()` reseeds the atomic from the resolved
  settings whenever `ConfigGeneration()` moves, so selecting a profile that carries a
  different value takes effect. *Why the atomic lives in `Clipboard/`:* both backends need it
  and neither should include an Overlay file.
- **Seeded before the Shell exists.** `PanelSystem_SeedFromConfig()` is called from
  `main.cpp` (two call sites, both at startup) so a persisted *off* holds
  from the first clipboard event, not from the first time the overlay is opened.
- **Status line.** `ClipboardSyncStatus()` asks `GetBackend()->GetCurrentConnector()`'s
  `GetNestedHints()` which protocol is really in use; a null result (embedded DRM mode, no
  host compositor) reads "inert: no host (embedded)". It is the same discriminator the
  broadcast path uses to decide whether a host exists.
- Rows: *Clipboard sync* (group Clipboard) and a read-only *Clipboard status* facts row
  (group Diagnostics).

## Related links

- [config-system.md](config-system.md), [profiles.md](profiles.md)
- [clipboard-sync.md](clipboard-sync.md), [notifications.md](notifications.md)
- [appearance.md](appearance.md) — the Appearance area
- [log.md](log.md), [about.md](about.md) — the Log and About areas

