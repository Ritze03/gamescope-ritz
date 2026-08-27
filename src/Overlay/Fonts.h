// M8 part 1 (issue #13; typeface swapped from IBM Plex to Geist by issue
// #53): the settings overlay's typography system -- Geist Sans for prose,
// Geist Mono for every number/unit/state-word, per
// superdoc/planning/ui-design-guide.md's "Typography" section.
//
// A small named-style API, not a general typography engine (per the task
// brief): the design guide's "Scale observed" list names a finite set of
// roles, and Style below is exactly that list, one enumerator per role --
// nothing here is meant to grow arbitrary new sizes/weights on request.
#pragma once

#include <cstdint>

struct ImFont;

namespace gamescope::fonts
{
	// One enumerator per typography role named in
	// superdoc/planning/ui-mockup-precise-spec.md §2 ("Typography"), the
	// pixel-measured spec that supersedes the earlier descriptive design
	// guide (see that file's own provenance note). Sizes/weights below are
	// its table's own numbers; see Fonts.cpp's kSpecs for the exact figures.
	enum class Style
	{
		Title,   // Mono 600, 11px -- window title-bar text.
		         // Uppercase the string yourself (Fonts.cpp doesn't); the
		         // spec's ~.16em letter-spacing on top of that is explicitly
		         // marked "skipped" (spec §2, §13) -- ImGui has no
		         // letter-spacing primitive, and faking it badly is worse
		         // than not.
		Section, // Sans 500, 13px -- group/section names (spec §2 "Group name").
		Label,   // Sans 400, 11.5px -- parameter labels, body/prose text
		         // (spec §2 "Parameter label").
		         // This is also the atlas's default font (ImGuiIO::FontDefault),
		         // so every pre-existing ImGui::Text/Checkbox/Slider/etc.
		         // call that never explicitly pushes a Style gets Sans for
		         // free, with no per-callsite change needed.
		Value,   // Mono 500, 13px -- numeric/unit/state-word readouts
		         // (spec §2 "Value readout"), drawn as plain text
		         // (ImGui::Text/AddText), tabular by construction since Plex
		         // Mono is genuinely monospaced.
		         // Does NOT reach text baked into a stock ImGui widget's own
		         // internal draw (e.g. SliderInt's live value overlay) --
		         // ImGui draws that with whatever font is active for the
		         // whole widget, not a separately stylable run; Widgets.cpp's
		         // custom slider draws its own value/mark text instead.
		Meta,    // Mono 400, 10.5px -- units, hints, disabled/meta text,
		         // title-bar meta, dock hint line (spec §2 rows "Title-bar
		         // meta" / "Meta/status line" / "Dock hint line" share this
		         // one size).
		Hero,    // Mono 600, 18px -- large standalone numeric readouts
		         // (the FPS display's number; spec §2 "HUD FPS number").
		SegmentLabel,  // Mono 500, 11.5px -- inactive segmented-control cell text (spec §2 "Segment label").
		SegmentActive, // Mono 600, 11.5px -- active segmented-control cell text.
		ScaleMark,     // Mono 400, 9.5px -- slider min/max scale marks (spec §2 "Scale min/max").
		DockHotkey,    // Mono 500, 8px -- dock button hotkey glyph (spec §2 "Dock hotkey glyph").
	};

	// Builds the IBM Plex atlas for the *currently current* ImGui context at
	// the given effective UI scale (every Style's baseline pixel size in
	// Fonts.cpp's kSpecs, multiplied by flScale) and must be called after
	// ImGui::CreateContext() but before ImGui_ImplVulkan_Init() -- the
	// Vulkan backend uploads whatever ImGui::GetIO().Fonts holds at Init()
	// time, so the atlas has to be finished building before that call, not
	// after.
	//
	// Issue #38: no longer idempotent-per-context-only -- safe to call again
	// later for a context that already has an atlas built, to re-bake it at
	// a new flScale (a no-op if flScale matches what that context is
	// currently built at). See Fonts.cpp for the ClearFonts()-then-rebuild
	// shape -- deliberately ClearFonts(), not the full (and, on an
	// already-rendered atlas, crash-on-next-glyph) Clear() -- and for why
	// no manual texture create/destroy call is needed to pair with it
	// against this ImGui version's dynamic-texture Vulkan backend. Callers
	// other than a context's own EnsureImguiInit() should go through
	// RebuildAll() below instead of calling this directly, so every context
	// stays in sync.
	//
	// ImGui font atlases are per-IO/per-context, not shared across contexts
	// -- SettingsOverlay.cpp, FpsDisplay.cpp and Notifications.cpp each own
	// a separate ImGui context (see FpsDisplay.h's file comment for why), so
	// each calls this once at init, from its own EnsureImguiInit().
	//
	// Fallback (required, not optional): if the bundled IBM Plex data fails
	// to build into the atlas for any reason, this leaves the atlas on
	// ImGui's built-in default font and every Style resolves to that same
	// default font instead -- text still renders, just not in Plex. Never
	// leaves the atlas empty and never crashes.
	//
	// Issue #48: also resets the current context's ImGuiIO::FontGlobalScale
	// to 1.0f every time it runs. The atlas above is baked at the effective
	// scale already (flSizePixels * flScale), so it is the sole scaling
	// mechanism for this context's fonts once Load()/RebuildAll() has run;
	// a stray FontGlobalScale left set to display_scale by another call site
	// would double-apply the factor on the pushed-font draw path
	// (ImGui::Text()/TextDisabled()) without affecting explicit-size
	// AddText() calls, which is exactly issue #48's scale^2 bug. See
	// Fonts.cpp's Load() for the full rationale.
	//
	// Issue #87: bBootstrap marks the one call SettingsOverlay.cpp makes
	// before ImGui_ImplVulkan_Init(), before the persisted display_scale is
	// known (see this function's own file-header note in Fonts.cpp). That
	// bake is real but provisional -- it must never let its own scale value
	// satisfy the "already built at this scale" guard against the first
	// real, post-config-load rebuild that follows (RebuildAll(), reached
	// via Palette.cpp's EnsureThemeLoaded()). Without this, a persisted
	// display_scale of exactly 1.0 -- the same value the bootstrap bake
	// used -- made that guard false-positive and silently skip the real
	// rebuild, the only scale for which this ever mattered (every other
	// scale differs from the 1.0 bootstrap value and took the real path
	// regardless). Callers other than SettingsOverlay.cpp's own bootstrap
	// Load() should never pass true here.
	void Load( float flScale = 1.0f, bool bBootstrap = false );

	// ---------------------------------------------------------------------
	// Issue #99 -- the pixel size ImGui will ACTUALLY rasterise at.
	// ---------------------------------------------------------------------
	// ImGui 1.92's font system bakes on demand, and it bakes at INTEGER
	// pixel sizes only: ImFont::GetFontBaked() runs the requested size
	// through GetRoundedFontSize() (imgui_internal.h -- literally
	// IM_ROUND()) before it looks a bake up. ImFont::RenderText() then
	// draws the glyph quads at `scale = size / baked->Size`, so a
	// FRACTIONAL requested size does not get a fractional bake -- it gets
	// the nearest integer bake, bilinearly RESAMPLED by that ratio. That is
	// exactly the "vector font turned back into a stretched bitmap" look:
	// soft edges plus dropped/doubled stems where the resample lands.
	//
	// The pushed-font path never hits this (imgui.cpp's SetCurrentFont()
	// rounds g.FontSize itself, so scale is always 1.0). The overlay does
	// not use that path: Controls.cpp draws every string with the explicit
	// -size ImDrawList::AddText()/ImFont::CalcTextSizeA() overloads, which
	// pass the caller's unrounded float straight through.
	//
	// That made display_scale 1.0 the WORST-looking scale in the product,
	// which is why this read as a 1.0x-only bug. The type ladder is
	// authored in half-pixels (Tokens.cpp: Title 14.5, Section 13.5,
	// Value 16.5), so at 1.0x those three roles -- slab/region titles,
	// group bands and rail sections, and every numeric/state readout --
	// were resampled by 0.966/0.964/0.971. At 2.0x every role doubles to a
	// whole number and resamples by exactly 1.0, i.e. is pixel-perfect;
	// at 1.25x/1.5x the ratios land within ~1.5% of 1.0 and are far less
	// visible. Measured, not reasoned: 1.0x is 3-4% off, ~2x worse than
	// any other scale, and it is font-independent (it reproduces on
	// ImGui's own built-in default font, as the user reported).
	//
	// So: run any size destined for an explicit-size AddText()/
	// CalcTextSizeA() call through this first. Rounding the REQUEST is the
	// fix rather than rounding the token table, because the token table is
	// authored in scale-1.0 base units and has to stay fractional to keep
	// its six-step ladder (D27); it is the physical-pixel size, after the
	// display_scale multiply, that has to land on a bake.
	float RasterSize( float flSizePx );

	// Re-bakes every context that has ever called Load() (i.e. every
	// context whose EnsureImguiInit() has actually run at least once this
	// process -- an unopened FPS HUD or a Notifications context that has
	// never shown a toast yet simply isn't in that set, and picks up
	// flScale on its own from whatever its own first Load() call passes)
	// at the given effective scale, temporarily making each one current in
	// turn and restoring whatever was current on entry. Called from
	// PanelConfig.cpp's General tab, debounced to fire once when the
	// Display-scale slider is released rather than on every intermediate
	// drag value -- see that file's own comment for why.
	void RebuildAll( float flScale );

	// Issue #51: applies a rebuild RebuildAll() deferred for the
	// *currently-current* context, if one is pending -- a no-op otherwise
	// (the common case, every frame but the one right after a rebuild was
	// requested). RebuildAll() always defers the rebuild of whichever
	// context is current when it's called (every real call site runs
	// mid-frame, from inside that context's own active NewFrame()/Render()
	// bracket -- rebuilding the atlas synchronously right there deletes
	// glyph state this same frame's earlier draw calls already reference,
	// which reproduced as corrupted/"awful" text; see Fonts.cpp's
	// RebuildAll() for the full write-up), so every context that ever
	// calls RebuildAll() -- today, only SettingsOverlay.cpp -- MUST call
	// this once per frame, before any widget is drawn (right after
	// EnsureImguiInit(), before ImGui::NewFrame()), or a requested rebuild
	// will simply never take effect for that context. FpsDisplay.cpp and
	// Notifications.cpp don't need to call this: RebuildAll() rebuilds
	// every *other* context immediately, since none of them are mid-frame
	// at the moment RebuildAll() runs (nothing re-enters another context's
	// frame from inside the call stack that reaches RebuildAll()).
	void ApplyPendingRebuild();

	// ---- rebuilding from a thread that is not the render thread ----------
	// Records a rebuild request; PumpRequestedRebuild() performs it, on the
	// render thread, at the start of its next frame.
	//
	// WHY THIS IS NOT JUST RebuildAll(). RebuildAll() MUTATES font state --
	// Load() calls ClearFonts(), which deletes every ImFont, ImFontBaked and
	// glyph rect a context owns. Its whole deferral scheme is built on one
	// assumption: that it is being called from the render thread, inside
	// pPrevContext's own live frame, so every OTHER context in the map is
	// safely between frames. Read ImGui's current context is a process-wide
	// global, and that assumption is exactly what a registration setter
	// breaks -- `overlay_e2_set overlay.display_scale 2.0` reaches the setter
	// on the CONSOLE thread, where the "current" context is whatever the
	// render thread is drawing with at that instant. RebuildAll() there
	// clears an atlas out from under a live draw pass, and the next glyph
	// that needs baking walks an ImFont whose Sources vector has already been
	// freed: `ImVector<ImFontConfig*>::operator[]` asserts and the whole
	// compositor aborts.
	//
	// That is not hypothetical and it is not rare once something on screen
	// needs a glyph baked at the new size -- a composite band's value string
	// reproduced it on the first frame after the request, every time.
	//
	// So: any caller that is not certain it is on the render thread inside a
	// live frame uses these two instead of RebuildAll().
	void RequestRebuild( float flScale );

	// Performs a pending RequestRebuild(), if any. Call once per frame from
	// the render thread, immediately before ApplyPendingRebuild(), so the
	// rebuild it schedules for this context lands on this same frame.
	void PumpRequestedRebuild();

	// Returns the ImFont* for a role on the currently-current context. Once
	// Load() has run for that context (as SettingsOverlay.cpp/FpsDisplay.cpp's
	// EnsureImguiInit() always does before any drawing) this is never null:
	// a real Plex face after a successful Load(), or ImGui's own built-in
	// default font for every Style after a failed/fallback Load(). Calling
	// this before Load() has ever run for the current context is a misuse
	// this API doesn't promise to handle gracefully beyond best-effort
	// (falls back to ImGui::GetIO().FontDefault, which can itself be null).
	//
	// How a new panel (e.g. M5's Audio panel, M7's Config panel) should use
	// this: call gamescope::fonts::Get(Style::Value) (or whichever role
	// fits) and wrap just the text/number in ImGui::PushFont(...)/PopFont()
	// -- see FpsDisplay.cpp's DrawReadout() and SettingsOverlay.cpp's
	// "Layer alpha" line for the pattern. Everything else needs no change:
	// Style::Label is already the atlas's default font, so ordinary
	// ImGui::Text/Checkbox/SliderFloat/etc. calls pick it up automatically.
	ImFont *Get( Style style );

	// Issue #54: the effective scale the *currently-current* context's atlas
	// is baked at right now (Fonts.cpp's Load()'s own flBuiltScale), or 1.0f
	// if Load() has never run for this context yet (nothing baked in to
	// divide against -- same "never built" sentinel Load()'s FontSet uses).
	// PanelConfig.cpp's PushLiveTheme() divides its per-tick FontGlobalScale
	// preview by this so the preview tracks the atlas's *actual* current
	// baseline instead of assuming it's always the compiled-in 1.0x -- see
	// that call site's comment, and Load()'s own comment on how
	// FontGlobalScale folds on top of whatever scale is already baked in.
	float BuiltScale();

	// ---- the baked range, and a way to ask whether text fits in it -------
	// The atlas bakes Basic Latin + Latin-1 Supplement (U+0020..U+00FF).
	// Anything outside it renders as a fallback box.
	//
	// D18: this was found the expensive way -- "inspector ›" shipped with a
	// box in it, in the one region a user only visits after hiding the
	// Inspector, which is exactly where nobody looks. A box glyph in a rarely
	// visited corner is the sort of thing that ships, so the range stops
	// being a comment in Fonts.cpp and becomes something callable.
	//
	// The consumer is `overlay_e2_glyphs`, which sweeps the LIVE registry --
	// every area title, setting name, help sentence, option label and unit --
	// because those are declarations in a dozen files that no unit-test
	// fixture sees.
	inline constexpr uint32_t kBakedFirst = 0x0020;
	inline constexpr uint32_t kBakedLast  = 0x00FF;

	// Returns the first code point in `pszUtf8` that the atlas cannot draw,
	// or 0 when every character is inside the baked range. Pure: no atlas, no
	// ImGui context, no font is consulted, so it is unit-testable and safe to
	// call from the console thread.
	//
	// Malformed UTF-8 is REPORTED rather than skipped -- a truncated sequence
	// is a bug in whatever produced the string, and silently ignoring it is
	// how a mojibake label survives review.
	uint32_t FirstUnbakedCodepoint( const char *pszUtf8 );
}
