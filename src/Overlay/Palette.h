// Shared color tokens for the settings overlay's chrome (Chrome.cpp) and
// widget drawing (Widgets.cpp), plus anything else that needs "the accent"
// or one of its derived tones (FpsDisplay.cpp).
//
// Source: superdoc/planning/ui-mockup-precise-spec.md §1 ("Color tokens
// (canonical)") -- every hex value below is that file's own oklch-derived,
// pixel-sampling-verified sRGB conversion, copied here once so every caller
// references the same numbers instead of re-deriving/re-typing them (the
// prior state of this codebase had the accent hex baked separately into
// Widgets.cpp and Chrome.cpp, which is exactly how it drifted to the wrong
// value -- see the spec's gap list item 1).
//
// Internal-only header (not part of the widgets::/chrome:: public API
// surface) -- included only by .cpp files that already pull in imgui.h.
#pragma once

#include "imgui.h"

namespace gamescope::palette
{
	// ---- Accent family (oklch(L C 218) per-token, hue is live -- issue #37)
	// §1's table gives each token's own OKLCH L/C at the spec's hue (218);
	// only the hue is ever user-tunable (LiveTheme::flAccentHue below), L/C
	// stay exactly as spec'd so a hue change can't wash a token out or blow
	// it out. These used to be `constexpr` (baked for hue 218 only); they are
	// now regular globals, recomputed by UpdateAccentFamily() every time the
	// hue changes (once at process start from Chrome.cpp's
	// EnsureLiveThemeLoaded(), and again on every General-tab hue-slider
	// edit) -- every existing call site (Widgets.cpp, Chrome.cpp,
	// FpsDisplay.cpp, Notifications.cpp) keeps reading these as plain values,
	// completely unchanged. Defined in Palette.cpp, alongside the OKLCH->sRGB
	// conversion and the per-token L/C table.
	extern ImU32 kAccent;        // oklch(.74 .12 h) -- status dot, slider fill lo-end, active borders, dock top-edge base
	extern ImU32 kAccentEdge;    // oklch(.8  .12 h) -- dock active 2px top edge
	extern ImU32 kAccentGradHi;  // oklch(.78 .12 h) -- slider fill gradient end (right/handle end)
	extern ImU32 kAccentValue;   // oklch(.84 .10 h) -- numeric value readouts
	extern ImU32 kAccentText;    // oklch(.82 .10 h) -- chip text, link-style labels
	extern ImU32 kAccentSegText; // oklch(.9  .07 h) -- active segmented-control label
	extern ImU32 kAccentKnob;    // oklch(.86 .08 h) -- toggle knob, checkbox inner mark
	extern ImU32 kAccentIcon;    // oklch(.88 .07 h) -- active dock icon strokes
	extern ImU32 kAccentHandle;  // oklch(.9  .05 h) -- slider handle fill
	extern ImU32 kAccentLinkDim; // oklch(.8  .10 h) -- footer action text ("live", title-bar meta)

	// One OKLCH color (Oklab-based, D65), converted to a packed sRGB ImU32 at
	// the given alpha -- the same conversion the spec's own hex values were
	// derived from (superdoc/planning/ui-mockup-precise-spec.md §1). Exposed
	// (not Palette.cpp-local) so PanelConfig.cpp's hue slider can render a
	// live hue-ring/gradient preview using the exact same math the accent
	// family itself uses, rather than an approximate HSV gradient that would
	// visibly disagree with the real accent at some hues.
	ImU32 OklchToImU32( float flL, float flC, float flHueDegrees, float flAlpha = 1.0f );

	// The exact inverse of the above, ignoring alpha. Added for E2's Colour
	// override composite (SPEC §4.4): that control edits L, C and H, but the
	// config format it is bound to is, and stays, a packed sRGB integer --
	// so the round trip has to happen somewhere, and it belongs next to the
	// forward conversion rather than in a panel.
	//
	// NOT round-trip-exact in general, and deliberately not pretending to
	// be: OklchToImU32() CLAMPS out-of-gamut channels (see its own comment),
	// so any OKLCH triple outside sRGB maps to a colour whose inverse is a
	// different triple. Inside the gamut it round-trips to within the 8-bit
	// quantisation the packed format imposes anyway, which is what the
	// control needs -- the stored integer, not the triple, is the value of
	// record.
	void ImU32ToOklch( ImU32 col, float *pflL, float *pflC, float *pflHueDegrees );

	// Recomputes every kAccent* token above from LiveTheme::flAccentHue --
	// call after changing that field. Idempotent; cheap enough (10 OKLCH
	// conversions) to call on every hue-slider edit, same as every other
	// General-tab control's "write straight into g_LiveTheme on every edit"
	// pattern.
	void UpdateAccentFamily();

	// ---- Neutrals (alpha layers -- alpha is load-bearing) -----------------
	constexpr ImU32 kSurfaceRgb    = IM_COL32( 0x09, 0x0A, 0x0C, 255 ); // window/panel/dock base color, .88/.86 alpha applied by callers
	constexpr ImU32 kTextRgb       = IM_COL32( 0xEF, 0xF5, 0xFB, 255 ); // near-white text base

	// Accent at an arbitrary alpha (0-1), as ImU32 -- covers the many
	// alpha steps §1 lists (.13 dock fill, .16 chip fill, .2 checkbox
	// fill, .24 active segment fill, .3 toggle track, .42 focused border,
	// .5 dock active border, .6 active segment border, .65 toggle border,
	// .8 active-group left edge / handle glow) without a named constant
	// per step. No longer `constexpr` (issue #37): the base RGB is now
	// hue-live, tracking kAccent's own bytes (kept in sync by
	// UpdateAccentFamily() below, not recomputed per call).
	ImU32 Accent( float flAlpha );

	constexpr ImU32 White( float flAlpha )
	{
		return IM_COL32( 255, 255, 255, (int)( flAlpha * 255.0f + 0.5f ) );
	}

	// kTextRgb at an arbitrary alpha -- the near-white text base (§1: "text
	// tints ... implement as one base #EFF5FB").
	constexpr ImU32 Text( float flAlpha )
	{
		return IM_COL32( 0xEF, 0xF5, 0xFB, (int)( flAlpha * 255.0f + 0.5f ) );
	}

	constexpr ImU32 Black( float flAlpha )
	{
		return IM_COL32( 0, 0, 0, (int)( flAlpha * 255.0f + 0.5f ) );
	}

	// ---- Low-alpha "dark grey" text/rail tier (issue #62) -------------------
	// Three complaints in a row have landed on the same shape: some near-
	// white element sits at the original spec's own meta/rail alpha (§1's
	// 9-36% band) and reads as too dark to comfortably see against the
	// panel's dark surface -- #44's dock hint, an earlier bottom-bar hint,
	// and now the slider's own scale-marks/rail. Each prior fix bumped one
	// call site's own alpha literal by hand (see #44's own commit), which
	// fixes that one site and leaves the next occurrence exactly where it
	// was -- the third instance of the same report. Named centrally here
	// instead, so a call site reads "the readable dark-grey tier" once and
	// any future brightness tweak is one edit, not a hunt through every
	// panel's own literals.
	//
	// Deliberately NOT a change to White()/Text() themselves: those still
	// mean exactly what their alpha argument says, and plenty of *other*
	// low-alpha uses (group-block fills at 2-3%, hairline borders at 6-10%,
	// disabled-state dimming at 30-45%) are low on purpose, not a
	// readability bug -- blanket-brightening the whole White()/Text() range
	// would wash those out along with fixing the actual complaint. Only the
	// specific tiers named below move.
	constexpr float kRailAlpha     = 0.16f; // was 0.09 (old spec §7 "track #FFFFFF @ 9%") -- slider track/rail, White()
	constexpr float kMarkAlpha     = 0.38f; // was 0.26 (old spec §7 "meta-faint 26%") -- slider min/max scale marks, White()
	constexpr float kMetaTextAlpha = 0.44f; // was 0.34 (old spec §1 "meta 30-36%") -- read-only system-measured text (ReadoutStrip), White()

	// surface (§1: rgba(9,10,12,.88)) as an ImVec4, for ImGuiStyle.Colors[]
	// slots that want a float color rather than a packed ImU32.
	inline ImVec4 SurfaceVec4( float flAlpha = 0.88f )
	{
		return ImVec4( 0x09 / 255.0f, 0x0A / 255.0f, 0x0C / 255.0f, flAlpha );
	}

	// Any packed ImU32 as an ImVec4, optionally overriding its alpha -- for
	// ImGuiStyle.Colors[]/PushStyleColor() slots that want a float color.
	inline ImVec4 ToVec4( ImU32 col, float flAlphaOverride = -1.0f )
	{
		ImVec4 v = ImGui::ColorConvertU32ToFloat4( col );
		if ( flAlphaOverride >= 0.0f )
			v.w = flAlphaOverride;
		return v;
	}

	// ---- Live-tunable overlay theme (window-chrome overhaul's General tab,
	// Config/ConfigSchema.h's OverlaySettings) -------------------------------
	// Everything else in this header is a compile-time constant matching the
	// spec exactly; these are the exception -- General-tab settings that
	// must take effect the instant the user moves a slider, with no restart
	// (the task's own requirement). Chrome.cpp is the sole owner: its
	// EnsureLiveThemeLoaded() seeds this once from global.json at first use,
	// and PanelConfig.cpp's General tab writes straight into it on every edit
	// (see that file's DrawGeneralTab()) -- every other reader (here,
	// Widgets.cpp, DrawDock()) only ever reads. Defined in Chrome.cpp.
	struct LiveTheme
	{
		float flDockScale          = 1.0f;  // OverlaySettings::dock_scale
		float flDisplayScale       = 1.0f;  // OverlaySettings::display_scale -- see ConfigSchema.h's ceiling note
		float flWindowAlphaFocused   = 1.0f; // OverlaySettings::opacity_windows_focused
		float flWindowAlphaUnfocused = 0.9f; // OverlaySettings::opacity_windows_unfocused
		float flDockAlpha          = 0.86f; // OverlaySettings::opacity_dock
		// OverlaySettings::accent_hue, degrees, OKLCH hue -- issue #37.
		// Default 218 reproduces today's #36BDDD family exactly (see
		// Palette.cpp's per-token L/C table). Setting this alone does
		// nothing to the drawn colors on its own -- always follow with
		// UpdateAccentFamily() (PanelConfig.cpp's DrawGeneralTab() and
		// Chrome.cpp's EnsureLiveThemeLoaded() both do) so kAccent* actually
		// picks up the new hue.
		float flAccentHue         = 218.0f;
	};
	extern LiveTheme g_LiveTheme;

	// Issue #23 half two: the effective control-geometry scale factor --
	// OverlaySettings::display_scale (0.5..2.0, ConfigSchema.h), read fresh
	// every call rather than cached, same live-tunable contract as every
	// other g_LiveTheme field. Widgets.cpp/Chrome.cpp multiply their own
	// baseline pixel constants by this, mirroring the exact pattern
	// Chrome.cpp's DrawDock()/DrawDockButton() already use for flDockScale
	// (kButtonSize = 54.0f * flDockScale, etc.) -- display_scale used to
	// drive ImGuiIO::FontGlobalScale (and the font atlas rebuild, #38) only;
	// this closes the gap #24 found, where every hand-drawn widget/chrome
	// pixel constant ignored it outright while text scaled around them.
	inline float DisplayScale() { return g_LiveTheme.flDisplayScale; }
}
