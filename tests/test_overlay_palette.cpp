// Unit tests for the command palette's pure half -- matching, ranking,
// the query field's text accumulator, and the shared arrow-key adjuster.
//
// WHY THIS FILE HAS NO WINDOW IN IT. The palette's two interesting halves are
// a scorer and a sort, and neither needs a frame. Keeping them in
// CommandPalette.cpp behind a signature that takes strings and a Registry is
// what lets every ranking rule below be asserted without a compositor, a font
// atlas or an ImGui context. The drawing half lives in Shell.cpp with every
// other pixel and is verified by screenshot instead.
//
// Source of truth for the behaviour asserted here:
//   superdoc/planning/redesign/round-2/e2-inspector-plus/SPEC.md  (§5.2, §8.2, §2.6)
//   superdoc/planning/redesign/round-2/e2-inspector-plus/API.md   (§10)
//   superdoc/planning/redesign/round-2/e2-inspector-plus/index.html  (the tiebreaker: score())
#include <catch2/catch_test_macros.hpp>

#include "Overlay/UI/CommandPalette.h"
#include "Overlay/UI/Registry.h"

#include <algorithm>
#include <string>

using namespace gamescope;

namespace
{
	// A small registry shaped like the real one: two areas, a row with
	// params, a read-only row, and a choice -- enough for every ranking band
	// and every adjust path to be exercised.
	struct Fixture
	{
		ui::Registry reg;

		bool  bTearing  = false;
		float flSharp   = 5.0f;
		bool  bDenoise  = false;
		int   nScaler   = 0;
		int   nFpsLimit = 60;

		static constexpr ui::Option kScalers[] = {
			{ 0, "auto" }, { 1, "integer" }, { 2, "fit" }, { 3, "stretch" },
		};

		Fixture()
		{
			ui::Area &disp = reg.Add( "display.upscaling", "Upscaling", ui::Section::Display );

			disp.Slider( "display.filter.sharpness", "Sharpness", ui::Bind( &flSharp ) )
				.Range( 0.0f, 20.0f ).Step( 1.0f ).Help( "Strength of the sharpening pass." )
				.Keywords( "rcas crisp detail" )
				.Param( "rcas_denoise", "RCAS denoise", ui::Bind( &bDenoise ) )
					.Help( "Suppresses grain introduced by sharpening." )
					.Keywords( "grain noise" );

			disp.Choice( "display.filter.scaler", "Scaler", ui::Bind( &nScaler ), kScalers, 4 )
				.Help( "How the image is fitted to the output." )
				.Keywords( "aspect fit integer" );

			disp.Facts( "display.path", "Effective path",
				[] { return std::string( "FSR - STRETCH" ); } )
				.Help( "What the scaling pipeline currently resolves to." );

			ui::Area &sys = reg.Add( "system.monitor", "Monitor", ui::Section::System );
			sys.Switch( "monitor.tearing", "Allow tearing", ui::Bind( &bTearing ) )
				.Help( "Lets a frame scan out before the next vblank." )
				.Keywords( "vsync immediate flip" );
			sys.Stepper( "monitor.fps_limit", "FPS limit", ui::Bind( &nFpsLimit ) )
				.Range( 0, 1000 ).Step( 5 ).Help( "Caps presentation rate." );
		}

		// Index of `sId` in a query's results, or -1.
		int IndexOf( std::string_view sQuery, const char *pszId )
		{
			const std::vector<ui::PaletteItem> hits = ui::Build( reg, sQuery );
			for ( size_t i = 0; i < hits.size(); i++ )
			{
				if ( hits[ i ].sId == pszId )
					return (int)i;
			}
			return -1;
		}
	};

	constexpr ui::Option Fixture::kScalers[];
}

// =========================================================================
//  Browsing is search with an empty query
// =========================================================================
TEST_CASE( "palette: an empty query lists every entry AND every param", "[overlay_palette]" )
{
	// SPEC §5.2's searchability guarantee, and the design's "browsing is
	// search with an empty query where that holds -- one code path, not two".
	// If this ever needs a second branch, the property has been lost.
	Fixture f;
	const std::vector<ui::PaletteItem> all = ui::Build( f.reg, "" );

	// 4 searchable entries + 1 param. "display.path" is a Facts row -- Issue
	// #91 excludes read-only entries (and their params) from the palette
	// entirely, since a launcher jump has nothing there to act on.
	REQUIRE( all.size() == 5 );
	REQUIRE( f.IndexOf( "", "display.filter.sharpness" ) >= 0 );
	REQUIRE( f.IndexOf( "", "display.filter.sharpness.rcas_denoise" ) >= 0 );
	REQUIRE( f.IndexOf( "", "monitor.fps_limit" ) >= 0 );
	REQUIRE( f.IndexOf( "", "display.path" ) == -1 );
}

TEST_CASE( "palette: an empty query keeps registration order", "[overlay_palette]" )
{
	// The stable sort is load-bearing: with every item at the same score, the
	// order the rail and the sheets already show is the order the palette
	// shows. An unstable sort would make the browse list a different product
	// from the sheet for no reason.
	// display.path is a read-only Facts row, excluded from the palette
	// entirely (Issue #91) -- so it does not occupy a slot in this order.
	Fixture f;
	const std::vector<ui::PaletteItem> all = ui::Build( f.reg, "" );
	REQUIRE( all[ 0 ].sId == "display.filter.sharpness" );
	REQUIRE( all[ 1 ].sId == "display.filter.sharpness.rcas_denoise" );
	REQUIRE( all[ 2 ].sId == "display.filter.scaler" );
	REQUIRE( all[ 3 ].sId == "monitor.tearing" );
	REQUIRE( all[ 4 ].sId == "monitor.fps_limit" );
}

// =========================================================================
//  Ranking
// =========================================================================
TEST_CASE( "palette: the five score bands rank in the documented order", "[overlay_palette]" )
{
	// index.html's score() is the tiebreaker for these, and this asserts the
	// bands rather than the numbers so a re-tune has to state its intent.
	REQUIRE( ui::Score( "sharpness", "display.filter.sharpness", "sharpness display.filter.sharpness rcas", "sharp" )
	         == ui::kScoreExact );
	REQUIRE( ui::Score( "rcas denoise", "display.filter.sharpness.rcas_denoise", "rcas denoise grain", "denoise" )
	         == ui::kScoreTitle );
	REQUIRE( ui::Score( "scaler", "display.filter.scaler", "scaler display.filter.scaler aspect", "display" )
	         == ui::kScoreId );
	REQUIRE( ui::Score( "allow tearing", "monitor.tearing", "allow tearing monitor.tearing vsync", "vsync" )
	         == ui::kScoreKeyword );
	REQUIRE( ui::Score( "sharpness", "display.filter.sharpness", "sharpness display.filter.sharpness rcas", "shrp" )
	         == ui::kScoreFuzzy );
	REQUIRE( ui::Score( "sharpness", "display.filter.sharpness", "sharpness display.filter.sharpness rcas", "zzq" )
	         == ui::kScoreNoMatch );
}

TEST_CASE( "palette: a title prefix outranks a keyword hit", "[overlay_palette]" )
{
	Fixture f;
	// "scaler" is Scaler's title and also sits in Sharpness's neighbourhood
	// only through the blob; the title-prefix hit must come first.
	REQUIRE( f.IndexOf( "scaler", "display.filter.scaler" ) == 0 );
}

TEST_CASE( "palette: a param is found by its own name", "[overlay_palette]" )
{
	// SPEC §5.2: "Ctrl+K -> 'denoise' finds display.filter.sharpness.rcas_denoise".
	// This is the anti-junk-drawer law's credibility test -- if a param is
	// not findable, depth really is the same as hidden.
	Fixture f;
	REQUIRE( f.IndexOf( "denoise", "display.filter.sharpness.rcas_denoise" ) == 0 );
}

TEST_CASE( "palette: a param is found through its PARENT's name", "[overlay_palette]" )
{
	// A user who remembers where a setting lives rather than what it is
	// called still reaches it.
	Fixture f;
	REQUIRE( f.IndexOf( "sharpness", "display.filter.sharpness.rcas_denoise" ) >= 0 );
}

TEST_CASE( "palette: a param row is labelled with its parent and flagged", "[overlay_palette]" )
{
	Fixture f;
	const std::vector<ui::PaletteItem> hits = ui::Build( f.reg, "denoise" );
	REQUIRE( hits.size() >= 1 );
	REQUIRE( hits[ 0 ].bParam );
	REQUIRE( hits[ 0 ].sLabel.find( "Sharpness" ) != std::string::npos );
	REQUIRE( hits[ 0 ].sLabel.find( "RCAS denoise" ) != std::string::npos );
}

TEST_CASE( "palette: the path column is the CONFIG KEY, never the area id", "[overlay_palette]" )
{
	// D5 / SPEC §2.6. `system.monitor` holds `monitor.*` keys, so showing the
	// area id here would print a prefix that does not exist on disk. The key
	// is what a reviewer checks against global.json.
	Fixture f;
	const std::vector<ui::PaletteItem> hits = ui::Build( f.reg, "tearing" );
	REQUIRE( hits.size() >= 1 );
	REQUIRE( hits[ 0 ].sPath == "monitor.tearing" );
	REQUIRE( hits[ 0 ].sPath.rfind( "system.", 0 ) != 0 );
}

TEST_CASE( "palette: matching is case-insensitive", "[overlay_palette]" )
{
	Fixture f;
	REQUIRE( f.IndexOf( "SHARP", "display.filter.sharpness" ) == 0 );
	REQUIRE( f.IndexOf( "ShArP", "display.filter.sharpness" ) == 0 );
}

// =========================================================================
//  The query field's accumulator (see Shell.cpp's PaletteConsumeInput)
// =========================================================================
TEST_CASE( "palette: the query accumulator round-trips UTF-8", "[overlay_palette]" )
{
	// The field is hand-rolled on io.InputQueueCharacters rather than
	// ImGui::InputText -- see CommandPalette.h for why -- so the encoder and
	// the backspace are OURS and have to be tested.
	std::string s;
	ui::AppendUtf8( s, 'a' );
	ui::AppendUtf8( s, 0x00E4 );   // a-umlaut, 2 bytes
	ui::AppendUtf8( s, 0x20AC );   // euro sign, 3 bytes
	REQUIRE( s == "a\xC3\xA4\xE2\x82\xAC" );
}

TEST_CASE( "palette: backspace deletes one CHARACTER, not one byte", "[overlay_palette]" )
{
	// A byte-wise backspace leaves a partial sequence that renders as a
	// replacement glyph and matches nothing -- an invisible dead end.
	std::string s = "a\xC3\xA4\xE2\x82\xAC";
	ui::PopUtf8( s );
	REQUIRE( s == "a\xC3\xA4" );
	ui::PopUtf8( s );
	REQUIRE( s == "a" );
	ui::PopUtf8( s );
	REQUIRE( s.empty() );
	ui::PopUtf8( s );          // must not underflow
	REQUIRE( s.empty() );
}

TEST_CASE( "palette: Ctrl+W deletes the trailing word", "[overlay_palette]" )
{
	std::string s = "display shar";
	ui::PopWord( s );
	REQUIRE( s == "display " );
	ui::PopWord( s );
	REQUIRE( s.empty() );
}

// =========================================================================
//  Adjust in place (SPEC §8.2's arrow keys, shared with the Sheet)
// =========================================================================
TEST_CASE( "palette: arrows step a slider by its declared step and clamp", "[overlay_palette]" )
{
	Fixture f;
	const ui::Entry *pE = f.reg.FindEntry( "display.filter.sharpness" );
	REQUIRE( pE != nullptr );

	REQUIRE( ui::AdjustValue( ui::Adjustable::Of( *pE ), +1, false ) );
	REQUIRE( f.flSharp == 6.0f );
	REQUIRE( ui::AdjustValue( ui::Adjustable::Of( *pE ), -1, false ) );
	REQUIRE( f.flSharp == 5.0f );

	// Shift is SPEC §3.4's x0.1 fine step.
	REQUIRE( ui::AdjustValue( ui::Adjustable::Of( *pE ), +1, true ) );
	REQUIRE( f.flSharp > 5.0f );
	REQUIRE( f.flSharp < 5.5f );

	// Clamps at the declared range, and reports "nothing changed" there so a
	// held key does not repaint forever.
	f.flSharp = 20.0f;
	REQUIRE_FALSE( ui::AdjustValue( ui::Adjustable::Of( *pE ), +1, false ) );
	REQUIRE( f.flSharp == 20.0f );
}

// Issue #101. Reported on `image.shaders.vibrancy.strength` (range -1..1,
// step 0.05, default 0): stepping the launcher's in-place adjuster down and
// back up left "-1.565e-07" on screen instead of "0" -- ordinary binary
// floating-point error accumulating over repeated +/- 0.05f, because
// AdjustValue() only ever added the step to the current value. Shaped like
// that real registration on purpose, not the Fixture's integer-step slider,
// since an exact step never drifts and would not have caught this.
TEST_CASE( "palette: stepping a float param down and back up returns exactly to its start", "[overlay_palette]" )
{
	ui::Registry reg;
	ui::Area &a = reg.Add( "test.vibrancy", "Vibrancy", ui::Section::Display );

	float flStrength = 0.0f;
	a.Slider( "test.vibrancy.strength", "Strength", ui::Bind( &flStrength ) )
		.Help( "x" )
		.Range( -1.0f, 1.0f )
		.Step( 0.05f )
		.Default( 0.0f );

	const ui::Entry *pE = reg.FindEntry( "test.vibrancy.strength" );
	REQUIRE( pE != nullptr );

	for ( int i = 0; i < 20; i++ )
		REQUIRE( ui::AdjustValue( ui::Adjustable::Of( *pE ), -1, false ) );
	for ( int i = 0; i < 20; i++ )
		REQUIRE( ui::AdjustValue( ui::Adjustable::Of( *pE ), +1, false ) );

	// Bit-exact, not "close to zero": the whole point is that the residue is
	// gone, not merely small, and this is also what ValueToString() needs to
	// print a plain "0" rather than scientific notation.
	REQUIRE( flStrength == 0.0f );

	// A held Shift (SPEC 3.4's x0.1 fine step) drifts on its own finer grid
	// exactly the same way, and must also land back on the start.
	for ( int i = 0; i < 15; i++ )
		REQUIRE( ui::AdjustValue( ui::Adjustable::Of( *pE ), +1, true ) );
	for ( int i = 0; i < 15; i++ )
		REQUIRE( ui::AdjustValue( ui::Adjustable::Of( *pE ), -1, true ) );
	REQUIRE( flStrength == 0.0f );
}

TEST_CASE( "palette: a switch takes its value from the DIRECTION, not a toggle", "[overlay_palette]" )
{
	// Holding Right down a list of switches must end with them all on. A
	// toggle would oscillate, which makes the key's meaning depend on the
	// value it is about to change.
	Fixture f;
	const ui::Entry *pE = f.reg.FindEntry( "monitor.tearing" );
	REQUIRE( pE != nullptr );

	REQUIRE( ui::AdjustValue( ui::Adjustable::Of( *pE ), +1, false ) );
	REQUIRE( f.bTearing );
	REQUIRE_FALSE( ui::AdjustValue( ui::Adjustable::Of( *pE ), +1, false ) );  // already on
	REQUIRE( f.bTearing );
	REQUIRE( ui::AdjustValue( ui::Adjustable::Of( *pE ), -1, false ) );
	REQUIRE_FALSE( f.bTearing );
}

TEST_CASE( "palette: a choice steps its options and stops at both ends", "[overlay_palette]" )
{
	Fixture f;
	const ui::Entry *pE = f.reg.FindEntry( "display.filter.scaler" );
	REQUIRE( pE != nullptr );

	REQUIRE( f.nScaler == 0 );
	REQUIRE_FALSE( ui::AdjustValue( ui::Adjustable::Of( *pE ), -1, false ) );  // already first
	REQUIRE( ui::AdjustValue( ui::Adjustable::Of( *pE ), +1, false ) );
	REQUIRE( f.nScaler == 1 );

	f.nScaler = 3;
	REQUIRE_FALSE( ui::AdjustValue( ui::Adjustable::Of( *pE ), +1, false ) );  // already last
	REQUIRE( f.nScaler == 3 );
}

TEST_CASE( "palette: an arrow refuses a Color composite but still steps a Hue", "[overlay_palette]" )
{
	// A Color composite's value is a PACKED 0xRRGGBB int, so the generic
	// integer step moved it by one -- nudging the blue channel's least
	// significant bit. It "worked" in the sense that the binding changed,
	// which is why the sweep that wrote two values and read them back
	// scored it as adjustable; it is nonetheless meaningless, because a
	// colour has no direction "right" could mean.
	//
	// The row keeps its real route: the Inspector's OKLCH body.
	int nPacked = 0x7BD3F0;
	int nHue    = 200;

	ui::Registry reg;
	ui::Area &a = reg.Add( "setup.appearance", "Appearance", ui::Section::Setup );

	a.Composite( "overlay.accent_color", "Accent colour", ui::CompositeKind::Color,
	             ui::Bind( &nPacked ) )
		.Help( "The accent colour, edited as OKLCH." );

	a.Composite( "overlay.accent_hue", "Accent hue", ui::CompositeKind::Hue,
	             ui::Bind( &nHue ) )
		.Range( 0.0f, 360.0f ).Step( 1.0f )
		.Help( "The accent hue in degrees." );

	const ui::Entry *pColor = reg.FindEntry( "overlay.accent_color" );
	const ui::Entry *pHue   = reg.FindEntry( "overlay.accent_hue" );
	REQUIRE( pColor != nullptr );
	REQUIRE( pHue != nullptr );

	// Refused, and -- the half that actually matters -- the binding is not
	// written. A refusal that still moved the value would be worse than the
	// bug it replaces.
	REQUIRE_FALSE( ui::AdjustValue( ui::Adjustable::Of( *pColor ), +1, false ) );
	REQUIRE( nPacked == 0x7BD3F0 );
	REQUIRE_FALSE( ui::AdjustValue( ui::Adjustable::Of( *pColor ), -1, false ) );
	REQUIRE( nPacked == 0x7BD3F0 );

	// The refusal is specific to Color. An Anchor's axes and a Hue's degrees
	// are genuinely ordered, so blanket-refusing every composite would break
	// two working controls to fix one broken one.
	REQUIRE( ui::AdjustValue( ui::Adjustable::Of( *pHue ), +1, false ) );
	REQUIRE( nHue == 201 );
}

TEST_CASE( "palette: a stepper honours an integer step", "[overlay_palette]" )
{
	Fixture f;
	const ui::Entry *pE = f.reg.FindEntry( "monitor.fps_limit" );
	REQUIRE( pE != nullptr );
	REQUIRE( ui::AdjustValue( ui::Adjustable::Of( *pE ), +1, false ) );
	REQUIRE( f.nFpsLimit == 65 );
}

TEST_CASE( "palette: a read-only kind refuses to be adjusted", "[overlay_palette]" )
{
	// SPEC §5.2 clause 4's read-only-by-type, reaching the keyboard: an
	// arrow key on a Facts row must not invent a write path around it.
	Fixture f;
	const ui::Entry *pE = f.reg.FindEntry( "display.path" );
	REQUIRE( pE != nullptr );
	REQUIRE_FALSE( ui::AdjustValue( ui::Adjustable::Of( *pE ), +1, false ) );
}

TEST_CASE( "palette: a param adjusts through the same one function", "[overlay_palette]" )
{
	// The palette's whole in-place promise rests on a Param being no
	// different from a row here.
	Fixture f;
	const ui::Parameter *pP = f.reg.FindParam( "display.filter.sharpness.rcas_denoise" );
	REQUIRE( pP != nullptr );
	REQUIRE( ui::AdjustValue( ui::Adjustable::Of( *pP ), +1, false ) );
	REQUIRE( f.bDenoise );
}

// =========================================================================
//  D25 -- "can this be adjusted in place, or does it need the full overlay?"
// =========================================================================
// The launcher asks this of every result it draws: an adjustable row gets
// chevrons and the "left/right adjust in place" legend, a non-adjustable one
// gets `open` and "Enter open in the full overlay". Getting it wrong is a
// visible lie on the row, so the predicate is pinned here rather than left to
// agree with AdjustValue() by inspection.
TEST_CASE( "launcher: CanAdjust follows the KIND, not the current value", "[overlay_palette]" )
{
	Fixture f;

	// Ordered kinds: yes, regardless of where the value happens to sit.
	const ui::Entry *pSlider  = f.reg.FindEntry( "display.filter.sharpness" );
	const ui::Entry *pChoice  = f.reg.FindEntry( "display.filter.scaler" );
	const ui::Entry *pSwitch  = f.reg.FindEntry( "monitor.tearing" );
	const ui::Entry *pStepper = f.reg.FindEntry( "monitor.fps_limit" );
	// Read-only by type: no.
	const ui::Entry *pFacts   = f.reg.FindEntry( "display.path" );
	REQUIRE( pSlider  != nullptr );
	REQUIRE( pChoice  != nullptr );
	REQUIRE( pSwitch  != nullptr );
	REQUIRE( pStepper != nullptr );
	REQUIRE( pFacts   != nullptr );

	REQUIRE( ui::CanAdjust( ui::Adjustable::Of( *pSlider  ) ) );
	REQUIRE( ui::CanAdjust( ui::Adjustable::Of( *pChoice  ) ) );
	REQUIRE( ui::CanAdjust( ui::Adjustable::Of( *pSwitch  ) ) );
	REQUIRE( ui::CanAdjust( ui::Adjustable::Of( *pStepper ) ) );
	REQUIRE_FALSE( ui::CanAdjust( ui::Adjustable::Of( *pFacts ) ) );

	// A Param is adjustable on exactly the same terms as a row -- the
	// launcher must not offer chevrons on one and `open` on the other.
	const ui::Parameter *pParam = f.reg.FindParam( "display.filter.sharpness.rcas_denoise" );
	REQUIRE( pParam != nullptr );
	REQUIRE( ui::CanAdjust( ui::Adjustable::Of( *pParam ) ) );

	// THE POINT OF THE PREDICATE. A value sitting on an end stop makes
	// AdjustValue() return false in that direction, and the row is still
	// adjustable -- the other arrow moves it. A launcher that decided
	// adjustability by trying the step would mislabel exactly the rows a
	// user is most likely to be looking at: a maxed slider, a switch already
	// on, a choice on its last option.
	f.flSharp = 20.0f;   // the slider's ceiling
	f.bTearing = true;   // the switch is already what Right would set it to
	f.nScaler = 3;       // the last option
	REQUIRE_FALSE( ui::AdjustValue( ui::Adjustable::Of( *pSlider ), +1, false ) );
	REQUIRE_FALSE( ui::AdjustValue( ui::Adjustable::Of( *pSwitch ), +1, false ) );
	REQUIRE_FALSE( ui::AdjustValue( ui::Adjustable::Of( *pChoice ), +1, false ) );
	REQUIRE( ui::CanAdjust( ui::Adjustable::Of( *pSlider ) ) );
	REQUIRE( ui::CanAdjust( ui::Adjustable::Of( *pSwitch ) ) );
	REQUIRE( ui::CanAdjust( ui::Adjustable::Of( *pChoice ) ) );
	// ...and the other direction still moves them, which is what makes
	// "adjustable" the right word for all three.
	REQUIRE( ui::AdjustValue( ui::Adjustable::Of( *pSlider ), -1, false ) );
	REQUIRE( ui::AdjustValue( ui::Adjustable::Of( *pSwitch ), -1, false ) );
	REQUIRE( ui::AdjustValue( ui::Adjustable::Of( *pChoice ), -1, false ) );
}

TEST_CASE( "launcher: CanAdjust never contradicts AdjustValue", "[overlay_palette]" )
{
	// The one direction that MUST hold: a row CanAdjust() refuses can never
	// be moved by either arrow. (The converse is not a law -- see the end-stop
	// case above.) Asserted over every kind rather than the four in the
	// fixture, so a kind added later without a CanAdjust() case is caught
	// here instead of shipping a dead chevron.
	int   nPacked = 0x7BD3F0;
	int   nHue    = 200;
	int   nAnchor = 0;
	std::string sName = "profile";
	int   nBank   = 0b0101;
	bool  bRan    = false;

	static constexpr ui::Option kSources[] = { { 1, "wm" }, { 2, "drm" } };

	ui::Registry reg;
	ui::Area &a = reg.Add( "setup.appearance", "Appearance", ui::Section::Setup );
	a.Composite( "overlay.accent_color", "Accent colour", ui::CompositeKind::Color,
	             ui::Bind( &nPacked ) ).Help( "h" );
	a.Composite( "overlay.accent_hue", "Accent hue", ui::CompositeKind::Hue,
	             ui::Bind( &nHue ) ).Range( 0.0f, 360.0f ).Step( 1.0f ).Help( "h" );
	a.Composite( "overlay.hud_anchor", "HUD anchor", ui::CompositeKind::Anchor,
	             ui::Bind( &nAnchor ) ).Range( 0.0f, 8.0f ).Step( 1.0f ).Help( "h" );
	a.Text( "profile.name", "Profile name", ui::Bind( &sName ) ).Help( "h" );
	a.Bank( "log.sources", "Log sources", ui::Bind( &nBank ), kSources, 2 ).Help( "h" );
	a.Action( "config.reset", "Reset", "reset", [ & ] { bRan = true; } ).Help( "h" );
	a.Meter( "system.fps", "FPS", [] { return 42.0; }, 0.0, 240.0 ).Help( "h" );

	const char *kIds[] = {
		"overlay.accent_color", "overlay.accent_hue", "overlay.hud_anchor",
		"profile.name", "log.sources", "config.reset", "system.fps",
	};

	for ( const char *pszId : kIds )
	{
		const ui::Entry *pE = reg.FindEntry( pszId );
		INFO( "id: " << pszId );
		REQUIRE( pE != nullptr );
		if ( ui::CanAdjust( ui::Adjustable::Of( *pE ) ) )
			continue;
		REQUIRE_FALSE( ui::AdjustValue( ui::Adjustable::Of( *pE ), +1, false ) );
		REQUIRE_FALSE( ui::AdjustValue( ui::Adjustable::Of( *pE ), -1, false ) );
	}

	// The Action must not have fired as a side effect of being probed.
	REQUIRE_FALSE( bRan );

	// The two composites that ARE ordered stay so -- blanket-refusing every
	// composite to make the Color case work would break two controls to fix
	// one, which is the mistake the Color case's own test already guards.
	REQUIRE( ui::CanAdjust( ui::Adjustable::Of( *reg.FindEntry( "overlay.accent_hue" ) ) ) );
	REQUIRE( ui::CanAdjust( ui::Adjustable::Of( *reg.FindEntry( "overlay.hud_anchor" ) ) ) );
	REQUIRE_FALSE( ui::CanAdjust( ui::Adjustable::Of( *reg.FindEntry( "overlay.accent_color" ) ) ) );
}

TEST_CASE( "launcher: an unbound declaration is not adjustable", "[overlay_palette]" )
{
	// The launcher draws chevrons from this answer, and a chevron on a row
	// with nothing behind it is a control that renders and does nothing --
	// the defect class this redesign spent itself removing.
	ui::Registry reg;
	ui::Area &a = reg.Add( "display.upscaling", "Upscaling", ui::Section::Display );
	a.Slider( "display.orphan", "Orphan", ui::AnyBind{} ).Range( 0.0f, 1.0f ).Help( "h" );

	const ui::Entry *pE = reg.FindEntry( "display.orphan" );
	REQUIRE( pE != nullptr );
	REQUIRE_FALSE( pE->Binding().IsBound() );
	REQUIRE_FALSE( ui::CanAdjust( ui::Adjustable::Of( *pE ) ) );
	REQUIRE_FALSE( ui::AdjustValue( ui::Adjustable::Of( *pE ), +1, false ) );
}

// =========================================================================
//  Discoverability
// =========================================================================
TEST_CASE( "palette: every setting is reachable in <= 3 characters", "[overlay_palette]" )
{
	// Direction B enforced this as a BUILD gate. It ships here as a test
	// instead -- see AUTONOMOUS-DECISIONS.md D16 for the reasoning: the same
	// property, without a registration abort that would take the compositor
	// down over a search-ranking regression.
	Fixture f;
	std::string sWorst;
	const int nWorst = ui::WorstCharsToReach( f.reg, 8, &sWorst );
	INFO( "worst id: " << sWorst << " needed " << nWorst << " chars" );
	REQUIRE( nWorst <= 3 );
}
