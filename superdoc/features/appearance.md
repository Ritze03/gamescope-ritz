# Appearance — `setup.appearance`

The **Appearance** area of the **Shell** in the SETTINGS rail group (`UI/Registry.cpp`): how the overlay itself looks. It does not affect
the game's picture. Its routing is global (`global.json`), unlike [system-general.md](system-general.md), which is per profile; that is
the main thing this page records. Both were one page until 2026-11-07 (text moved verbatim).


Code: `src/Overlay/PanelConfig.cpp` (`BuildAppearanceArea`, registered from
`PanelConfig_RegisterAreas`; the Profiles area is in the same file, see
[profiles.md](profiles.md)).

#### How it works

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

#### Rows

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

## Related links

- [config-system.md](config-system.md), [profiles.md](profiles.md)
- [system-general.md](system-general.md) — the System area
- [log.md](log.md), [about.md](about.md) — the Log and About areas

