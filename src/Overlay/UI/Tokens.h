// E2 design tokens -- the geometry, spacing and type half.
//
// Source of truth: superdoc/planning/redesign/round-2/e2-inspector-plus/SPEC.md
// (§2.5 spacing, §3.0 the one control height, §7.6 type roles) and its
// companion mockup index.html, whose `:root` block is the same table in CSS
// custom properties. Where the two disagree the mockup wins, per the design
// brief; every such call is recorded in a comment at the value.
//
// Two rules this header exists to enforce:
//
//   1. NO MAGIC NUMBERS AT CALL SITES. A number that means the same thing in
//      two places is one token here. Nothing in UI/ may spell a pixel value
//      inline; Lane.cpp, Row.cpp and Controls.cpp read everything from below.
//
//   2. A DERIVED VALUE IS DERIVED, NOT RESTATED. Where SPEC.md prints both a
//      quantity and its derivation (the switch knob is `swH - 4`, its travel
//      is `swW - swK - 4`), only the derivation appears here. That is the
//      same discipline the slider handle uses at the other end of the kit
//      (Controls.cpp's SliderTrack()): one variable, never two kept in step.
//
// Deliberately free of imgui.h and of Overlay/Palette.h, so the pure
// arithmetic built on it (Lane.cpp) is unit-testable without an ImGui context
// or the overlay's live-theme globals. Colour roles -- which do need both --
// live next door in UI/Colors.h.
#pragma once

#include <algorithm>
#include <cmath>

namespace gamescope::ui
{
	// ---- display_scale ----------------------------------------------------
	// The kit reads the user's OverlaySettings::display_scale through this one
	// accessor rather than calling gamescope::palette::DisplayScale() from
	// every file.
	//
	// Why the indirection: palette::DisplayScale() reads palette::g_LiveTheme,
	// which is *defined in Chrome.cpp*. Binding the kit to it directly would
	// drag the whole legacy chrome translation unit into anything that wants a
	// row height -- including the unit tests. The shell (P2) pushes the live
	// value in once per frame with SetScale(); everything downstream reads
	// Scale(). One reader of display_scale in the whole kit, and a scale a test
	// can set.
	void  SetScale( float flScale );
	float Scale();

	// Base units -> physical pixels. "Base" means px at display_scale 1.0,
	// which is the unit SPEC.md and index.html are both authored in.
	inline float Px( float flBase ) { return flBase * Scale(); }

	// SPEC §8.3: "Every hairline is max(1, floor(1 x scale))" -- 1px from 0.5x
	// to 1.99x, 2px at 2.0x, so a rule never vanishes at 0.5x nor thickens into
	// a border at 2.0x. Never Px(1).
	inline float Hairline() { return std::max( 1.0f, std::floor( Scale() ) ); }

	namespace tok
	{
		// ---- The one control height (SPEC §3.0) ---------------------------
		// "Every control's HIT BOX is exactly --H tall. A control with a box
		// border fills it; a control whose graphic is deliberately shorter --
		// switch, slider, meter -- is centred in it."
		inline constexpr float kControlH = 28.0f;

		// The one sheet row height. RowTall is gone; composites are n x this
		// (SPEC §4.2 clause 1), and Inspector rows are this too (SPEC §5.3).
		inline constexpr float kRowH = 44.0f;

		// ---- Switch (SPEC §3.1) -------------------------------------------
		// B's 30x15/11 geometry uplifted ~25% and snapped to the 4-unit grid.
		// Only the track's two dimensions are stated; knob and travel are
		// derived, because SPEC.md derives them ("swH - 4", "swW - swK - 4")
		// and a restated derived value is a desync waiting to happen.
		inline constexpr float kSwitchW     = 40.0f;
		inline constexpr float kSwitchH     = 20.0f;
		inline constexpr float kSwitchInset = 2.0f;                                    // 1px border + 1px gap, per side
		inline constexpr float kSwitchKnob  = kSwitchH - 2.0f * kSwitchInset;          // 16
		inline constexpr float kSwitchTrvl  = kSwitchW - kSwitchKnob - 2.0f * kSwitchInset; // 20

		// ---- Slider / hue / meter (SPEC §3.0, §3.4, §3.8) ------------------
		inline constexpr float kTrack       = 8.0f;   // track thickness
		inline constexpr float kTrackRound  = kTrack * 0.5f;
		inline constexpr float kHandleW     = 8.0f;
		inline constexpr float kHandleH     = 20.0f;
		inline constexpr float kHandleRound = 1.0f;
		inline constexpr float kHandleHalo  = 2.0f;   // B's "0 0 0 2px accent@18%"
		inline constexpr float kMeterSegs   = 20.0f;  // SPEC §3.8: 20 segments
		inline constexpr float kMeterGap    = 1.5f;

		// ---- Stepper (SPEC §3.5) ------------------------------------------
		// Borderless "- +" glyphs; the number lives in the value column.
		inline constexpr float kStepperGlyphW = 18.0f;
		inline constexpr float kStepperGap    = 8.0f;
		inline constexpr float kStepperW      = kStepperGlyphW * 2.0f + kStepperGap;   // 44

		// ---- Row grammar columns (SPEC §2.1) ------------------------------
		inline constexpr float kAffordanceW = 28.0f;  // the affordance column
		inline constexpr float kRowPadLeft  = 12.0f;  // the row's own left padding
		inline constexpr float kLaneFrac    = 0.46f;  // Lw = 0.46 x W ...
		inline constexpr float kLaneMin     = 200.0f; // ... clamped to [200, 420] base
		inline constexpr float kLaneMax     = 420.0f; //     (see Lane.h for the arithmetic)
		inline constexpr float kValueMaxFrac= 0.60f;  // CSS `.val{max-width:60%}` (SPEC §2.3)

		// ---- Spacing scale (SPEC §2.5) ------------------------------------
		// "chosen by the helper, never typed by a caller".
		inline constexpr float kXS  = 4.0f;
		inline constexpr float kS   = 8.0f;
		inline constexpr float kM   = 12.0f;
		inline constexpr float kL   = 16.0f;
		inline constexpr float kXL  = 24.0f;
		inline constexpr float kXXL = 32.0f;

		// Gaps that are their own meaning rather than a step on the scale.
		inline constexpr float kGapAtom  = 9.0f;  // between control atoms (B's .ctl gap)
		inline constexpr float kGapSeg   = 3.0f;  // inside a segmented group / chip bank
		inline constexpr float kGapLabel = 10.0f; // label <-> value inside the label zone

		// ---- Region padding (index.html --pad / --ipad / --icon) ----------
		inline constexpr float kSheetPad     = kXL;   // 24
		inline constexpr float kInspectorPad = kL;    // 16
		inline constexpr float kIconBox      = 24.0f;

		// ---- Group band rhythm (SPEC §2.5) --------------------------------
		inline constexpr float kGroupSpaceAbove = kL;  // 16
		inline constexpr float kGroupSpaceBelow = kS;  // 8

		// ---- Control-specific padding (index.html .seg/.bank/.verb) -------
		inline constexpr float kSegPadX  = 9.0f;
		inline constexpr float kBankPadX = 8.0f;
		inline constexpr float kVerbPadX = 9.0f;
		inline constexpr float kSelfPadX = 6.0f;  // dropdown / text field inner padding

		// ---- Composite bands (SPEC §4.2) ----------------------------------
		inline constexpr int   kBandMinLines = 2;
		inline constexpr int   kBandMaxLines = 3;
		inline constexpr float kAnchorCell   = kControlH;  // the 3x3 grid sits on the control module
	}

	// ---- Type roles (SPEC §7.6) ------------------------------------------
	// Six roles, down from E's seven and today's ten. The helper picks the
	// family per role so "a caller cannot put a number in Sans".
	enum class Family : unsigned char { Sans, Mono };

	enum class TypeRole : unsigned char
	{
		Title,    // Mono 600 11 UPPER -- slab title, region titles
		Section,  // Mono 500 10.5 UPPER -- group bands, rail sections, mode strip
		Label,    // Sans 400 14 -- row labels, list primaries
		Body,     // Sans 400 14 -- Inspector prose only
		Value,    // Mono 500 15 -- every numeric or state readout
		Meta,     // Mono 400 11.5 -- units, marks, line numbers, chips
		Count,
	};

	struct TypeSpec
	{
		Family eFamily;
		int    nWeight;      // 400 / 500 / 600
		float  flSizeBase;   // base units
		bool   bUpper;
		float  flTracking;   // letter-spacing, em
	};

	// The table itself. Pure -- no font atlas is consulted.
	const TypeSpec &Type( TypeRole eRole );

	// The role's size in physical pixels at the current display_scale.
	inline float TypeSizePx( TypeRole eRole ) { return Px( Type( eRole ).flSizeBase ); }

	// ---- Motion (SPEC §8.4) ----------------------------------------------
	// Three durations, one easing, lerped against io.DeltaTime.
	namespace tok
	{
		inline constexpr float kDurState   = 0.090f; // hover, knob travel, segment fill, focus ring
		inline constexpr float kDurRegion  = 0.160f; // host change, rail collapse, inline expansion
		inline constexpr float kDurSurface = 0.240f; // overlay open / close
	}

	// SPEC §8.4's single easing: 1 - (1 - t)^3.
	inline float Ease( float t )
	{
		t = std::clamp( t, 0.0f, 1.0f );
		const float u = 1.0f - t;
		return 1.0f - u * u * u;
	}

	// Frame-rate independent approach used by every animated token in the kit:
	// pass io.DeltaTime and one of the kDur* constants above.
	float Approach( float flCurrent, float flTarget, float flDurationSeconds, float flDeltaTime );
}
