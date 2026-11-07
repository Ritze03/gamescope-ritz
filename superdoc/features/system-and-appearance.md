# System and Appearance — the two small settings areas of the SETTINGS rail group

Two areas of the **Shell** in the SETTINGS rail group (`UI/Registry.cpp`): **System**
(`system.general`) holds the clipboard-sync switch and its status; **Appearance**
(`setup.appearance`) holds how the overlay itself looks. Neither affects the game's picture.
Their routing differs (System is per profile, Appearance is global), and that is the main
thing this page records.

## System — `system.general`

Code: `src/Overlay/PanelSystem.{h,cpp}`. The behaviour of clipboard sync itself is in
[clipboard-sync.md](clipboard-sync.md); this area is its switch.

### How it works

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

## Appearance — `setup.appearance`

Code: `src/Overlay/PanelConfig.cpp` (`BuildAppearanceArea`, registered from
`PanelConfig_RegisterAreas`; the Profiles area is in the same file, see
[profiles.md](profiles.md)).

### How it works

- **Always global.** Every row edits `OverlaySettings` (`ConfigSchema.h`), which exists only
  in `global.json` (see [config-system.md](config-system.md)). The area badge is fixed to
  "global only" instead of the session profile name, and there are no inherited/overridden
  markers. *Why:* UI scale and accent colour belong to the person, not to a game setup.
- **Its own cached settings.** The area keeps `s_GeneralSettings`, filled by
  `config::LoadGlobal()` (not `ResolvedSettings()`, which a profile-routed panel would use),
  refreshed on `ConfigGeneration()`. Writes go through `QueueGeneralSave()`, which first calls
  `PushLiveTheme()` and then `config::EnqueueGlobalWrite()`. *Why per-field merge matters
  here:* the Cursor area and the notification placement write the same `overlay` object from
  their own copies; whole-object writes used to put nine of this area's fields back (see the
  merge notes in config-system.md).
- **Live theme.** `PushLiveTheme()` copies scale, window opacity and accent hue into
  `palette::g_LiveTheme` (then `UpdateAccentFamily()`), notification scale and opacity into
  `Notifications::g_LiveTheme`, and backdrop blur and darkening into
  `g_BackgroundLiveTheme`, so edits show on the next frame.
- **UI scale applies on release.** The `overlay.display_scale` setter always stores the value
  at once, but while `ui::IsPointerDragActive()` it defers `ApplyDisplayScale()` (live push,
  write, and `fonts::RequestRebuild()`) to pointer release via
  `ui::controls::DeferToRelease()`. *Why:* this is the one setting that decides the geometry
  of the control editing it, so applying it mid-drag slides the track away from the pointer
  (the user: the scale "should update, when the slider is released"). Not previewing during
  the drag also removes the earlier stacked-font-scale bug class (`FontGlobalScale` multiplies
  on the atlas' *baked* scale, hence the `/ fonts::BuiltScale()` in `PushLiveTheme`). The
  re-bake itself is performed by `fonts::PumpRequestedRebuild()` at the start of the next
  frame, never mid-frame.
- **Console-thread safety.** `PushLiveTheme()` only touches `ImGui::GetIO()` when a current
  context exists, because the row setters are also reachable from the `overlay_e2_set`
  console command, which has no ImGui context.
- **Notifications rows are split across two files.** Appearance opens the *Notifications*
  group (with `overlay.notification_scale`, bound here) and
  `gamescope::Notifications::RegisterRows()` (`Notifications.cpp`) adds the rest without
  opening a second group. *Why:* `Area::Group()` is a band marker, not a lookup, so opening
  it twice would draw two identical headers; and `notification_scale` must stay bound here so
  there is a single writer of `overlay` per cached copy. See [notifications.md](notifications.md).

### Rows

| Row id | Config key (global.json `overlay`) | Default | Meaning |
| --- | --- | --- | --- |
| `overlay.accent_hue` (Theme) | `accent_hue` | 218 | Hue-only accent picker, 0-360 deg; every accent token is regenerated from it. |
| `overlay.display_scale` (Theme) | `display_scale` | 1.0 | Whole-overlay size, 0.5-2.0x, applied on release. |
| `overlay.background_blur` (Backdrop) | `background_blur` | 1.0 | Blur of the game behind the open overlay. |
| `overlay.background_darkening` (Backdrop) | `background_darkening` | 0.8 | Dimming of the game behind the overlay. |
| `overlay.window_opacity` (Transparency) | `window_opacity` | 1.0 | See-through-ness of the Shell slab and Inspector, 0.3-1. Text is never dimmed. |
| `overlay.opacity_notifications` (Transparency) | `opacity_notifications` | 0.9 | Toast opacity, 0.3-1. |
| `overlay.startup_announce` (Startup) | `startup_announce_enabled` | on | The launch toast; takes effect next start. |
| `overlay.notification_scale` (Notifications) | `notification_scale` | 1.0 | Toast size, 0.5-2.0x. |

A *Diagnostics* group adds three read-only facts: *Routing* (always "global.json"),
*Font atlas* (`fonts::BuiltScale()`), and *Config location* (`config::ConfigRoot()`).
*Why three rows, not one:* a single facts row bundling three topics read as unrelated facts
under one label; the split also keeps the row count high enough for the second column, since the column
count follows row count alone (`Layout.cpp`'s solver); merging them once dropped the area to
one column.

## Where `UpscaleFilterGate.h` belongs

`src/UpscaleFilterGate.h` (`FilterPassApplies( flScaleX, flScaleY )`) does not fit this page:
it is the compositor's rule for whether the FSR/NIS passes run on the base layer, called from
`steamcompmgr.cpp`. It belongs in [scaling-filters.md](scaling-filters.md), which currently
shows the old inline expression instead of the function (see the report to the lead).

## Related links

- [config-system.md](config-system.md), [profiles.md](profiles.md)
- [clipboard-sync.md](clipboard-sync.md), [notifications.md](notifications.md)
- [log-and-about.md](log-and-about.md) — the Log and About areas
