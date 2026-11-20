// The "Frame generation" settings area -- see PanelFrameGen.h.
//
// SHAPE. Eleven per-profile rows -- Frame generation (the on/off switch), Multiplier
// (2x..8x / Target fps), Target fps, Priority, Pause at refresh rate, Limit to GPU speed, Motion version, Quality,
// Artifact safety, Static HUD protection, UI protection, Crosshair box size -- plus
// one live Status line. Every row's id IS its config key (`framegen.mode`,
// `framegen.target_fps`, ...), which is how the Shell's per-profile
// inherited/overridden dot and "Reset to inherited" find it -- no `.Key()`
// override needed (config::IsSettingsKey() answers from the serializer).
// The Multiplier row sets TWO keys (mode, and the multiplier for a fixed choice)
// but is tied to `framegen.mode`: the Shell's marker follows one key per row.
// `Why two rows:` the user asked for "a single toggle to enable and disable
// framegen in general, and then below that should just be the multiplier. It
// shouldn't be combined into one."
// Every edit calls fghost::SetConfig() at once; the renderer applies the
// presets live (a V1 Quality change costs it one GPU wait, which is its business;
// on V2 it is free, see FrameGenHost.cpp's EnsureReady()).
//
// QUALITY AND MOTION VERSION also serve the Motion blur area (2026-10-22): Frame
// generation and Motion blur run on ONE interpolator. Motion blur has its own two rows
// (`motion_blur.motion_version` / `motion_blur.quality`) and a "Use Frame generation's
// settings" switch (default on) that makes it follow these; with the switch off and the blur
// on, the blur's own pair is what the interpolator runs (every shown frame is a blurred
// one, so it cannot be V2 for one and V1 for the other). The merge of the two is in
// FrameGenHost.cpp's RecordBaseLayer(); this file knows nothing of it.
// `Why one Quality for both versions:` the user, on seeing Quality / Performance twice
// (V1's Quality, V2's "V2 mode"): "Right now we have quality with the options quality and
// performance and v2 mode with quality and performance. This could realistically be a
// single selector for just quality and performance, and just pass it the same way for v1
// and v2 since both of them have the same options."
//
// `Why only these:` the library's expert knobs are deliberately not exposed -- only the
// defaults were visually reviewed, so a slider would hand the user settings nobody has
// looked at. The one exception (2026-11-19) is the "Motion search" group at the end
// of the area (search radius, coarsest-level radius, pyramid smallest level, search
// penalty): the user asked to watch what more reach does while a game runs.
// EXPERT SETTINGS (2026-11-20). The Learned, V3 and Motion search rows (15 in all) are hidden
// (ShownWhen) unless the "Expert settings" switch (`framegen.expert`) is on AND the row acts on the
// effective motion version; while it is off the renderer gets their DEFAULTS
// (config::FrameGenEffective(), applied in PushToRenderer()), the stored values stay.
// `Why:` the user: "When expert settings is turned off, it should always use the defaults, so
// there are no \"ghost settings\"" -- a value you cannot see must not act.
// See superdoc/features/frame-generation-settings.md.
// `Why no keybind:` frame generation is a standing mode of the picture, not a
// momentary action; the Frame generation switch is the on/off.
#include "PanelFrameGen.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <cstdio>
#include <string>

#include "Config/ConfigManager.h"
#include "FrameGen/FrameGenHost.h"
#include "main.hpp"   // g_nNestedWidth / g_nNestedHeight, the game-size fallback

namespace gamescope
{
	namespace
	{
		bool s_bConfigLoaded = false;
		uint64_t s_ulLoadedGeneration = 0;
		config::Settings s_Settings;

		// ---- string <-> enum <-> option index --------------------------------
		// The config stores readable strings (matching autoclicker.mode); the
		// Choice rows bind ints; fghost wants its own enums. These are the
		// only three places that translate, and an unknown string falls to the
		// default the schema documents.
		// The Multiplier row folds the kind of multiplier and its value into one
		// choice ("up to 8x", "a target FPS mode"): 2..8 = fixed multiplier,
		// 9 = Target fps (not a multiplier, just the next free id).
		constexpr int kTargetChoice = 9;
		constexpr ui::Option kModeOptions[] = {
			{ 2, "2×" }, { 3, "3×" }, { 4, "4×" }, { 5, "5×" },
			{ 6, "6×" }, { 7, "7×" }, { 8, "8×" }, { kTargetChoice, "Target fps" } };
		constexpr ui::Option kPriorityOptions[] = { { 0, "Low latency" }, { 1, "Smoothness" } };
		constexpr ui::Option kQualityOptions[] = { { 0, "Quality" }, { 1, "Performance" } };
		// Display order V1, V2, V3, Learned; the values stay the enum / kMotionKeys indices (a Choice maps a click to
		// Option::nValue, not to the array position).
		constexpr ui::Option kMotionOptions[]  = { { 0, "V1" }, { 1, "V2" }, { 3, "V3" }, { 2, "Learned" } };
		// The ids are the indices into kLearnedNetKeys (GUI order); kLearnedNets maps them to the host enum.
		constexpr ui::Option kLearnedNetOptions[] = {
			{ 0, "e2v5 (default)" }, { 1, "e1v5 (small, cheaper)" }, { 2, "e2v4 (round 2)" }, { 3, "e1v4 (round 2, small)" },
			{ 4, "e2v3 (round 1)" }, { 5, "e1v3 (round 1, small)" },
			{ 6, "g2v2 Colour (games)" }, { 7, "g2v3 Features (games)" },
			{ 8, "m2v2 Colour (allround)" }, { 9, "m2v3 Features (allround)" } };
		constexpr ui::Option kLearnedPrecOptions[] = { { 0, "Full" }, { 1, "Half" } };
		constexpr ui::Option kSafetyOptions[]  = { { 3, "Off" }, { 4, "Very low" }, { 0, "Low" }, { 1, "Default" }, { 2, "High" } };
		constexpr ui::Option kHudOptions[]     = { { 0, "Off" }, { 1, "Normal" }, { 2, "Strong" } };
		// Motion guard (Motion version 3; 2026-11-20): the ids are fghost::Config::v3Guard's values, the index into kGuardKeys.
		constexpr ui::Option kGuardOptions[]   = { { 0, "Off" }, { 1, "Guard" }, { 2, "V2 motion" } };
		// The ids are the fghost::UiProt values (= framegen::UiProtection's); the order
		// here is the GUI's: V2 sits next to V1 so the two can be compared at a glance.
		constexpr ui::Option kUiOptions[]      = { { 0, "Off" }, { 1, "Crosshair" }, { 3, "Crosshair V2" }, { 2, "Whole screen" } };

		// Motion search reach (2026-11-19). The Choice ids ARE the pyramid's smallest-level sides (flow pixels):
		// each change re-creates the library's buffers and waits for the GPU, so a short list, not a free slider.
		// A stored value between two entries shows the nearest one and is not rewritten until the user picks.
		constexpr int kPyramidSides[] = { 16, 24, 32, 48, 64, 96, 128, 256 };
		constexpr ui::Option kPyramidOptions[] = {
			{ 16, "16" }, { 24, "24" }, { 32, "32" }, { 48, "48 (default)" },
			{ 64, "64" }, { 96, "96" }, { 128, "128" }, { 256, "256" } };

		int NearestPyramidSide( int nSide )
		{
			int nBest = kPyramidSides[ 0 ];
			for ( const int n : kPyramidSides )
				if ( std::abs( n - nSide ) < std::abs( nBest - nSide ) )
					nBest = n;
			return nBest;
		}

		constexpr const char *kPriorityKeys[] = { "low_latency", "smoothness" };
		constexpr const char *kQualityKeys[] = { "quality", "performance" };
		constexpr const char *kMotionKeys[]  = { "v1", "v2", "learned", "v3" };
		constexpr const char *kLearnedNetKeys[] = { "e2v5", "e1v5", "e2v4", "e1v4", "e2v3", "e1v3", "g2v2", "g2v3", "m2v2", "m2v3" };
		constexpr fghost::LearnedNet kLearnedNets[] = {
			fghost::LearnedNet::E2v5, fghost::LearnedNet::E1v5, fghost::LearnedNet::E2v4,
			fghost::LearnedNet::E1v4, fghost::LearnedNet::E2v3, fghost::LearnedNet::E1v3,
			fghost::LearnedNet::G2v2, fghost::LearnedNet::G2v3,
			fghost::LearnedNet::M2v2, fghost::LearnedNet::M2v3 };
		constexpr const char *kSafetyKeys[]  = { "low", "default", "high", "off", "very_low" };
		constexpr const char *kHudKeys[]     = { "off", "normal", "strong" };
		constexpr const char *kGuardKeys[]   = { "off", "guard", "v2_motion" };
		// Indexed by fghost::UiProt, so the index IS the option id above.
		constexpr const char *kUiKeys[]      = { "off", "crosshair", "whole_screen", "crosshair_v2" };

		int IndexOf( const std::string &s, const char *const *ppszKeys, int n, int nFallback )
		{
			for ( int i = 0; i < n; ++i )
				if ( s == ppszKeys[ i ] )
					return i;
			return nFallback;
		}

		int ClampIdx( int n, int nMax ) { return n < 0 ? 0 : ( n > nMax ? nMax : n ); }

		// The row's value for the stored mode + multiplier, and back.
		int ModeChoice( const config::FrameGenSettings &f )
		{
			if ( f.mode == "target" )
				return kTargetChoice;
			if ( f.multiplier >= 2 )
				return f.multiplier > fghost::kMaxMultiplier ? fghost::kMaxMultiplier : f.multiplier;
			return 2;
		}

		void SetModeChoice( config::FrameGenSettings &f, int n )
		{
			if ( n == kTargetChoice )
			{
				f.mode = "target";
			}
			else
			{
				f.mode = "fixed";
				f.multiplier = n < 2 ? 2 : ( n > fghost::kMaxMultiplier ? fghost::kMaxMultiplier : n );
			}
		}

		// Target fps: 0 = the display's refresh, else 30..1000. A drag that lands
		// in the dead zone 1..29 snaps to whichever end is nearer, so the slider
		// can still be dragged back to "Display refresh".
		int NormalizeTarget( int n )
		{
			if ( n < 15 )
				return 0;
			return n < 30 ? 30 : ( n > 1000 ? 1000 : n );
		}

		// The Crosshair box size row is a float percent (0.5..10, step 0.1); the host
		// takes tenths of a percent.
		int BoxTenths( float flPercent )
		{
			return std::clamp( (int)std::lround( flPercent * 10.0f ), fghost::kUiBoxMinTenths, fghost::kUiBoxMaxTenths );
		}

		// The game's frame size for the "px" figure: the renderer's own once it has
		// seen a frame (the layer-0 size is what the library protects), else the
		// nested resolution the game was asked for.
		void GameSize( uint32_t *puW, uint32_t *puH )
		{
			if ( !fghost::GameFrameSize( puW, puH ) )
			{
				*puW = g_nNestedWidth > 0 ? (uint32_t)g_nNestedWidth : 1920u;
				*puH = g_nNestedHeight > 0 ? (uint32_t)g_nNestedHeight : 1080u;
			}
		}

		// "2.5% . 24 px": the setting and what it comes to for this game. Pure over
		// its inputs (the live caller below feeds the game size), so the wording is
		// one place.
		std::string BoxSizeText( float flPercent, uint32_t uGameW, uint32_t uGameH )
		{
			uint32_t uBoxW = 0, uBoxH = 0;
			fghost::UiBoxPixels( uGameW, uGameH, BoxTenths( flPercent ), &uBoxW, &uBoxH );
			char sz[ 64 ];
			std::snprintf( sz, sizeof( sz ), "%.1f%% · %u px", flPercent, uBoxW );
			return sz;
		}

		fghost::Config ToHostConfig( const config::FrameGenSettings &f )
		{
			fghost::Config c;
			c.enabled = f.enabled;
			c.mode = f.mode == "target" ? fghost::Mode::Target : fghost::Mode::Fixed;
			c.multiplier = f.multiplier;   // SetConfig() normalises (fixed below 2x -> disabled, above 8x -> 8x)
			c.targetFps = f.target_fps;
			c.priority = IndexOf( f.priority, kPriorityKeys, 2, 1 ) == 1
				? fghost::Priority::Smoothness : fghost::Priority::LowLatency;
			c.pauseAtRefresh = f.pause_at_refresh;
			c.pauseWhenCursorVisible = f.pause_when_cursor_visible;
			c.gpuLimit = f.gpu_limit;
			c.quality = IndexOf( f.quality, kQualityKeys, 2, 0 ) == 1
				? fghost::Quality::Performance : fghost::Quality::Quality;
			c.safety = (fghost::Safety)IndexOf( f.safety, kSafetyKeys, 5, 1 );
			c.hud = (fghost::HudProtect)IndexOf( f.hud_protection, kHudKeys, 3, 2 );
			c.ui = (fghost::UiProt)IndexOf( f.ui_protection, kUiKeys, 4, 1 );
			c.uiBoxTenths = BoxTenths( f.ui_box_height );
			c.motion = (fghost::Motion)IndexOf( f.motion_version, kMotionKeys, 4, 0 );   // the index IS the enum value
			c.learnedNet = kLearnedNets[ IndexOf( f.learned_net, kLearnedNetKeys, 10, 0 ) ];
			c.learnedHalf = f.learned_half;
			c.learnedZeroTest = f.learned_zero_test;
			c.learnedStaticGuard = f.learned_static_guard;
			c.searchRadius = f.search_radius;   // SetConfig() clamps all four to the library's ranges
			c.searchRadiusTop = f.search_radius_top;
			c.pyramidMinSide = f.pyramid_min_side;
			c.searchPenalty = f.search_penalty;
			c.v3Smear = f.v3_smear;   // ... and the four V3 values
			c.v3OcclSoft = f.v3_occl_soft;
			c.v3Unsure = f.v3_unsure;
			c.v3SceneCut = f.v3_scene_cut;
			c.v3ColourWindow = f.v3_colour_window;   // ... and its two switches
			c.v3DetailSplit = f.v3_detail_split;
			c.v3Guard = IndexOf( f.v3_guard, kGuardKeys, 3, 0 );   // ... and the motion guard
			return c;
		}

		// The one place config state becomes renderer state: after every edit,
		// after every (re)load, and at startup / profile switch via
		// PanelFrameGen_ApplyStartupConfig().
		void PushToRenderer( const config::FrameGenSettings &f )
		{
			// The effective values: with Expert settings off the 15 tuning values are the defaults (no "ghost settings").
			fghost::SetConfig( ToHostConfig( config::FrameGenEffective( f ) ) );
		}

		void EnsureConfigLoaded()
		{
			const uint64_t ulGeneration = config::ConfigGeneration();
			if ( s_bConfigLoaded && ulGeneration == s_ulLoadedGeneration )
				return;
			s_Settings = config::ResolvedSettings();
			s_ulLoadedGeneration = ulGeneration;
			s_bConfigLoaded = true;
			// A (re)load -- first draw, or a profile switch -- applies now,
			// not when the user next touches a row.
			PushToRenderer( s_Settings.framegen );
		}

		void PersistAndPush()
		{
			PushToRenderer( s_Settings.framegen );
			config::EnqueueRoutedWrite( s_Settings );
		}

		bool On()
		{
			EnsureConfigLoaded();
			return s_Settings.framegen.enabled;
		}

		// Learned net / precision / zero-motion test / static guard rows: they matter only while Learned motion is what the
		// interpolator actually runs. That is Frame generation's Motion version, or Motion blur's
		// own one while the blur is on and "Use Frame generation's settings" is off (the blur
		// then borrows these rows -- they are Frame generation's only), and
		// something has to be switched on (Frame generation or Motion blur).
		bool LearnedRowsUsable()
		{
			EnsureConfigLoaded();
			const config::Settings &st = s_Settings;
			const bool bBlurOwn = st.motion_blur.enabled && !st.motion_blur.same_as_framegen;
			const std::string &sEffective = bBlurOwn ? st.motion_blur.motion_version : st.framegen.motion_version;
			return ( st.framegen.enabled || st.motion_blur.enabled ) && sEffective == "learned";
		}

		// The motion version the interpolator actually runs (same rule as LearnedRowsUsable()).
		bool EffectiveMotionIsLearned()
		{
			const config::Settings &st = s_Settings;
			const bool bBlurOwn = st.motion_blur.enabled && !st.motion_blur.same_as_framegen;
			return ( bBlurOwn ? st.motion_blur.motion_version : st.framegen.motion_version ) == "learned";
		}

		// The same for Motion version 3 (2026-11-19): its four rows are usable only while V3 is what the interpolator
		// runs (and something is on). The rows that only drive settings V3 itself ignores (the search radii, the pyramid
		// side, the search penalty) are greyed while it runs with the Motion guard off; with the guard on they act on its V2 search.
		bool EffectiveMotionIsV3()
		{
			const config::Settings &st = s_Settings;
			const bool bBlurOwn = st.motion_blur.enabled && !st.motion_blur.same_as_framegen;
			return ( bBlurOwn ? st.motion_blur.motion_version : st.framegen.motion_version ) == "v3";
		}

		bool V3RowsUsable()
		{
			EnsureConfigLoaded();
			const config::Settings &st = s_Settings;
			return ( st.framegen.enabled || st.motion_blur.enabled ) && EffectiveMotionIsV3();
		}

		// Expert reach rows: they matter while the interpolator runs (Frame generation or Motion blur on); the search
		// radii not under Learned motion, which has no search.
		bool ReachRowsUsable()
		{
			EnsureConfigLoaded();
			return s_Settings.framegen.enabled || s_Settings.motion_blur.enabled;
		}

		// Under V3 the search settings act only on the Motion guard's V2 search (V3 itself ignores them), so
		// the rows are usable under V3 while the guard is not Off and greyed while it is Off.
		bool V3GuardOn()
		{
			// The EFFECTIVE guard: with Expert settings off the stored one is not in force.
			return EffectiveMotionIsV3() && config::FrameGenEffective( s_Settings.framegen ).v3_guard != "off";
		}

		// The version rule of the two search-row groups (which motion versions the rows act on), without "something is on".
		bool RadiusVersionApplies()
		{
			return !EffectiveMotionIsLearned() && ( !EffectiveMotionIsV3() || V3GuardOn() );
		}

		// Pyramid side and penalty: every version; under V3 only while the Motion guard is on (V3 builds its own
		// pyramid and ignores both; a pyramid change would rebuild the buffers without changing V3's picture).
		bool PenaltyVersionApplies()
		{
			return !EffectiveMotionIsV3() || V3GuardOn();
		}

		bool RadiusRowsUsable()
		{
			return ReachRowsUsable() && RadiusVersionApplies();
		}

		bool PenaltyRowsUsable()
		{
			return ReachRowsUsable() && PenaltyVersionApplies();
		}

		// Expert settings (2026-11-20): the 15 tuning rows are SHOWN (Entry::ShownWhen, so also absent from keyboard
		// navigation and the command palette) only while the switch is on and the row acts on the effective motion
		// version. Not whether frame generation is on: a shown row is greyed by its DisabledUnless() instead.
		bool ExpertOn()
		{
			EnsureConfigLoaded();
			return s_Settings.framegen.expert;
		}

		bool ShowLearnedRows() { return ExpertOn() && EffectiveMotionIsLearned(); }
		bool ShowV3Rows()      { return ExpertOn() && EffectiveMotionIsV3(); }
		bool ShowRadiusRows()  { return ExpertOn() && RadiusVersionApplies(); }
		bool ShowPenaltyRows() { return ExpertOn() && PenaltyVersionApplies(); }

		bool UiOn()
		{
			EnsureConfigLoaded();
			return s_Settings.framegen.enabled && s_Settings.framegen.ui_protection != "off";
		}

		// Pause at refresh rate: a Target already caps the output at the target
		// (0 = the refresh), so the switch only governs the fixed multipliers.
		bool PauseRowUsable()
		{
			EnsureConfigLoaded();
			return s_Settings.framegen.enabled && s_Settings.framegen.mode != "target";
		}

		bool TargetMode()
		{
			EnsureConfigLoaded();
			return s_Settings.framegen.mode == "target";
		}

		// ---- the Status line (decision D18) -----------------------------------
		// One compact line, always. Pure over its inputs so the strings are one
		// place; the live caller below feeds it the two fghost snapshots.
		//   Off                       -> "Off"
		//   generating (activeN >= 2) -> target: "60->280 · target 280 (×4.6) · +13 ms"
		//                                fixed:  "60->120 · 2× · +8 ms"
		//                                (a fixed multiplier the display saturates reads
		//                                 "60->280 · 8× (×4.6) · +13 ms"; a 10-bit / HDR
		//                                 game appends " · 10-bit" / " · HDR")
		//   otherwise the reason      -> the renderer's (HDR, 10-bit, YCbCr, ...) when it
		//                                 refuses, else pacing's (game already at the
		//                                 refresh or target, GPU too slow, VRR, warming
		//                                 up, ...)
		//   nothing published yet     -> "Waiting for frames"
		// The font is Basic Latin + Latin-1 only, so the arrow is "->" (U+2192
		// is not in the atlas); "·" and "×" are Latin-1 and render. The FG / UI GPU
		// times used to be on this line; the user found it overloaded and they now
		// live only in the debug log.
		std::string StatusLine( const config::FrameGenSettings &f, const fghost::PacingStatus &ps, const fghost::RenderStatus &rs )
		{
			if ( !f.enabled )
				return "Off";
			const int nChoice = ModeChoice( f );

			if ( ps.valid && ps.activeN >= 2 )
			{
				char sz[ 192 ];
				int n = std::snprintf( sz, sizeof( sz ), "%.0f->%.0f", ps.gameFps, ps.presentedFps );
				std::string s( sz, n > 0 ? (size_t)n : 0 );
				if ( nChoice == kTargetChoice )
				{
					std::snprintf( sz, sizeof( sz ), " · target %.0f (×%.1f)", ps.targetFps, ps.effectiveMultiplier );
					s += sz;
				}
				else
				{
					std::snprintf( sz, sizeof( sz ), " · %d×", nChoice );
					s += sz;
					// The display cannot show N x the game: say what it actually does.
					if ( ps.effectiveMultiplier > 0.0f && ps.effectiveMultiplier < float( nChoice ) - 0.15f )
					{
						std::snprintf( sz, sizeof( sz ), " (×%.1f)", ps.effectiveMultiplier );
						s += sz;
					}
				}
				std::snprintf( sz, sizeof( sz ), " · +%.0f ms", ps.delayMs );
				s += sz;
				// The game's format when it is not plain 8-bit: "HDR", "10-bit", "16-bit float".
				if ( rs.formatTag && rs.formatTag[ 0 ] )
				{
					s += " · ";
					s += rs.formatTag;
				}
				// V2 was asked for but the library refused it for this content or GPU: its reason.
				if ( rs.motionNote && rs.motionNote[ 0 ] )
				{
					s += " · V1: ";
					s += rs.motionNote;
				}
				return s;
			}

			if ( rs.reason != fghost::Unavailable::Ok )
				return fghost::UnavailableText( rs.reason );

			if ( ps.valid )
			{
				const std::string sReason = fghost::PassReasonText( ps.reason );
				if ( !sReason.empty() )
					return sReason;
			}
			return "Waiting for frames";
		}
	}

	void PanelFrameGen_ApplyStartupConfig( const config::Settings &config )
	{
		PushToRenderer( config.framegen );
	}

	void PanelFrameGen_RegisterArea( ui::Registry &reg )
	{
		ui::Area &a = reg.Add( "image.framegen", "Frame generation", ui::Section::Display );

		a.Keywords( "frame generation framegen interpolation fluid motion smooth fps multiplier "
		            "target priority latency smoothness optical flow fsr3 fmf frames" );
		a.Summary( []
		{
			EnsureConfigLoaded();
			if ( !s_Settings.framegen.enabled )
				return std::string( "off" );
			const int n = ModeChoice( s_Settings.framegen );
			if ( n == kTargetChoice )
				return s_Settings.framegen.target_fps > 0
					? "target " + std::to_string( s_Settings.framegen.target_fps ) : std::string( "target" );
			return std::to_string( n ) + "×";
		} );

		using S = config::FrameGenSettings;

		a.Columns( 2 );

		a.Group( "Frame generation" );
		a.GroupColumn( 0 );

		a.Switch( "framegen.enabled", "Frame generation",
			ui::AnyBind::Of<bool>(
				[]{ EnsureConfigLoaded(); return s_Settings.framegen.enabled; },
				[]( bool b ) { EnsureConfigLoaded(); s_Settings.framegen.enabled = b; PersistAndPush(); } ) )
			.Help( "Generates extra frames between the game's own frames. Adds delay, so it is "
			       "not suited to twitch shooters." )
			.Default( false )
			.Keywords( "frame generation framegen fg interpolation fluid motion smooth fps enable on off switch toggle" );

		a.Choice( "framegen.mode", "Multiplier",
			ui::AnyBind::Of<int>(
				[]{ EnsureConfigLoaded(); return ModeChoice( s_Settings.framegen ); },
				[]( int n ) { EnsureConfigLoaded(); SetModeChoice( s_Settings.framegen, n ); PersistAndPush(); } ),
			kModeOptions, std::size( kModeOptions ) )
			.Help( "How many frames are shown per game frame, 2× to 8× the game's rate, or aim at "
			       "a Target fps. It keeps generating until the game reaches your refresh rate (or "
			       "the target); a multiplier your display cannot show just fills every refresh. "
			       "The Frame limiter caps the game and this multiplies the capped rate." )
			.Default( kTargetChoice )
			.Keywords( "frame generation framegen fg multiplier 2x 3x 4x 5x 6x 7x 8x double triple quadruple target fps dynamic interpolation" )
			.DisabledUnless( On, "frame generation is off" );

		a.Slider( "framegen.target_fps", "Target fps",
			ui::AnyBind::Of<int>(
				[]{ EnsureConfigLoaded(); return s_Settings.framegen.target_fps; },
				[]( int n ) { EnsureConfigLoaded(); s_Settings.framegen.target_fps = NormalizeTarget( n ); PersistAndPush(); } ) )
			.Help( "The frame rate to aim for in Target fps mode. The number of generated frames "
			       "follows the game's frame times on every frame: if the game drops from 120 to "
			       "100 fps with a target of 240, it generates more per game frame at once. 0 "
			       "aims at your display's refresh rate. A target above the refresh rate only takes "
			       "effect where frames faster than the refresh can be shown (a desktop window, or "
			       "tearing); on a normal display the refresh rate is the limit." )
			.Range( 0.0f, 1000.0f ).Step( 5.0f ).Unit( "fps" )
			.ZeroMeans( "Display refresh" )
			.Default( 0 )
			.Keywords( "frame generation target fps frame rate dynamic refresh" )
			.DisabledUnless( TargetMode, "Multiplier is not set to Target fps" );

		a.Choice( "framegen.priority", "Priority",
			ui::AnyBind::Of<int>(
				[]{ EnsureConfigLoaded(); return IndexOf( s_Settings.framegen.priority, kPriorityKeys, 2, 1 ); },
				[]( int n ) { EnsureConfigLoaded(); s_Settings.framegen.priority = kPriorityKeys[ ClampIdx( n, 1 ) ]; PersistAndPush(); } ),
			kPriorityOptions, std::size( kPriorityOptions ) )
			.Help( "Low latency adds the least delay, but motion can stutter when the game's "
			       "frame times jitter. Smoothness spaces the frames perfectly evenly and adds "
			       "about one game frame of delay." )
			.Segmented()
			.Default( 1 )
			.Keywords( "frame generation priority low latency smoothness input lag jitter pacing" )
			.DisabledUnless( On, "frame generation is off" );

		a.Choice( "framegen.motion_version", "Motion version",
			ui::AnyBind::Of<int>(
				[]{ EnsureConfigLoaded(); return IndexOf( s_Settings.framegen.motion_version, kMotionKeys, 4, 0 ); },
				[]( int n ) { EnsureConfigLoaded(); s_Settings.framegen.motion_version = kMotionKeys[ ClampIdx( n, 3 ) ]; PersistAndPush(); } ),
			kMotionOptions, std::size( kMotionOptions ) )
			.Help( "V2 is a newer, experimental way of making the in-between frames: it bends "
			       "the picture along the motion and stretches it over gaps instead of falling "
			       "back to the real frame spot by spot, and it is cheaper on the GPU. The delay "
			       "is the same as V1. It works in every game format, HDR and 10-bit included; "
			       "a GPU without the shader features it needs stays on V1 (the Status line "
			       "says why). Switching shows real frames for an instant. Motion blur follows "
			       "this and Quality unless you turn off its \"Use Frame generation's settings\" "
			       "switch; then, while Motion blur is on, its own two settings are used instead. "
			       "V3: most fluid; smears where it is unsure; costs about three times V1. "
			       "Learned is an experimental, opt-in estimator: a small neural network finds the "
			       "motion instead of V1's search, then V1 makes the picture. It costs several "
			       "times V1's GPU time (1080p 2x on an RX 7900 XTX: about 1.4 ms with e2v5, 0.6 ms "
			       "with the small e1v5, V1 0.3 ms) and always works at Quality's resolution, so the "
			       "Quality row's Performance is ignored. Static HUD text over moving scenery can "
			       "still get a little torn; the Zero-motion test (shown with Expert settings on) fixes most of it. Two "
			       "identical frames are left alone. If it cannot run the Status line says why and "
			       "V1 runs." )
			.Default( 0 )
			.Keywords( "frame generation motion version v1 v2 v3 learned neural network ml net morph interpolation algorithm experimental fluid smear" )
			.DisabledUnless( On, "frame generation is off" );

		a.Choice( "framegen.quality", "Quality",
			ui::AnyBind::Of<int>(
				[]{ EnsureConfigLoaded(); return IndexOf( s_Settings.framegen.quality, kQualityKeys, 2, 0 ); },
				[]( int n ) { EnsureConfigLoaded(); s_Settings.framegen.quality = kQualityKeys[ ClampIdx( n, 1 ) ]; PersistAndPush(); } ),
			kQualityOptions, std::size( kQualityOptions ) )
			.Help( "Quality is the default look; Performance is cheaper on the GPU and a little "
			       "less clean. Motion version V1: Performance estimates motion at a quarter "
			       "resolution and can miss thin, fast detail (roughly 35-50% cheaper; 1440p 2x: "
			       "0.50 -> 0.25 ms per game frame, RX 7900 XTX); switching pauses the picture for "
			       "an instant. Motion version V2: Quality works out a motion value for every pixel "
			       "and keeps the side of the picture that is still visible at edges, Performance "
			       "is a simpler warp, a little less clean around moving edges (1080p 2x, RX 7900 "
			       "XTX: V1 0.30, V2 Quality 0.24, V2 Performance 0.14 ms per game frame); "
			       "switching does not interrupt the picture." )
			.Segmented()
			.Default( 0 )
			.Keywords( "frame generation quality performance cost gpu motion estimation v2 mode" )
			.DisabledUnless( On, "frame generation is off" );

		a.Switch( "framegen.expert", "Expert settings",
			ui::AnyBind::Of<bool>(
				[]{ EnsureConfigLoaded(); return s_Settings.framegen.expert; },
				[]( bool b ) { EnsureConfigLoaded(); s_Settings.framegen.expert = b; PersistAndPush(); } ) )
			.Help( "Shows the tuning settings of the selected Motion version. While it is off they are "
			       "hidden and their defaults are used; your own values are kept and apply again when "
			       "you turn it back on." )
			.Default( false )
			.Keywords( "frame generation expert advanced tuning settings options experimental hidden show" );

		a.Group( "Artifact protection" );
		a.GroupColumn( 0 );

		a.Choice( "framegen.safety", "Artifact safety",
			ui::AnyBind::Of<int>(
				[]{ EnsureConfigLoaded(); return IndexOf( s_Settings.framegen.safety, kSafetyKeys, 5, 1 ); },
				[]( int n ) { EnsureConfigLoaded(); s_Settings.framegen.safety = kSafetyKeys[ ClampIdx( n, 4 ) ]; PersistAndPush(); } ),
			kSafetyOptions, std::size( kSafetyOptions ) )
			.Help( "Decides when a pixel the motion estimate got wrong falls back to the real frame. "
			       "Each level sets the mismatch range (0-255 levels) between fully interpolated and "
			       "fully real: High 8-28, Default 12-40, Low 16-56, Very low 40-68 (smoothest ramp, ghosts "
			       "more where the motion is wrong). All four also show the real "
			       "frame when more than 15% of the picture is untrusted or on a scene cut. Off "
			       "never falls back: smoothest, but expect smearing on fast flicks and blended "
			       "scene cuts. Motion version 3 has no mismatch ranges (it has its own Smear and "
			       "Unsure settings): for it only Off matters, which stops its failed-estimate "
			       "fallback." )
			.Default( 1 )
			.Keywords( "frame generation artifact safety artefacts ghosting trust fallback" )
			.DisabledUnless( On, "frame generation is off" );

		a.Choice( "framegen.hud_protection", "Static HUD protection",
			ui::AnyBind::Of<int>(
				[]{ EnsureConfigLoaded(); return IndexOf( s_Settings.framegen.hud_protection, kHudKeys, 3, 2 ); },
				[]( int n ) { EnsureConfigLoaded(); s_Settings.framegen.hud_protection = kHudKeys[ ClampIdx( n, 2 ) ]; PersistAndPush(); } ),
			kHudOptions, std::size( kHudOptions ) )
			.Help( "A soft bias towards \"stay put\" when frames are generated, so a motionless "
			       "or see-through HUD wobbles less. It only nudges the motion estimate: it "
			       "cannot make a HUD pixel-exact (that is UI protection below). Strong "
			       "can hold back genuinely slow motion near the HUD. Under Motion version 3 it "
			       "steadies fixed overlays (text, lists, panels) against fast scene motion and "
			       "does not affect slow movement. Strong holds overlays harder but can cost a "
			       "little picture quality in busy scenes; it does not fix a radar or minimap "
			       "whose map itself moves." )
			.Default( 2 )
			.Keywords( "frame generation static hud protection ui wobble see-through" )
			.DisabledUnless( On, "frame generation is off" );

		a.Choice( "framegen.ui_protection", "UI protection",
			ui::AnyBind::Of<int>(
				[]{ EnsureConfigLoaded(); return IndexOf( s_Settings.framegen.ui_protection, kUiKeys, 4, 1 ); },
				[]( int n ) { EnsureConfigLoaded(); s_Settings.framegen.ui_protection = kUiKeys[ ClampIdx( n, 3 ) ]; PersistAndPush(); } ),
			kUiOptions, std::size( kUiOptions ) )
			.Help( "Keeps a still crosshair in the middle of the screen pixel-exact. Crosshair V2 "
			       "only protects pixels that stay still and stand out from what is behind them, "
			       "never pastes over moving content and has no fill-in step, so the specks and "
			       "moving patterns around the crosshair are gone, and a moving background no "
			       "longer drags a copy of it along, so no second crosshair while panning (an "
			       "opaque, unchanging crosshair only). Crosshair is the older method, kept to compare. Whole screen does the "
			       "older method for every solid still HUD element too (minimap, ammo, text), at "
			       "a small extra cost. None can protect see-through or changing UI. Off for "
			       "racing games or anything without a fixed HUD. Select this row or the box size "
			       "to see what lies under the box." )
			.Default( 1 )
			.Preview( ui::Entry::PreviewKind::UiBox )
			.Keywords( "frame generation ui protection crosshair v2 hud whole screen minimap static pixel exact specks artifacts" )
			.DisabledUnless( On, "frame generation is off" );

		a.Slider( "framegen.ui_box_height", "Crosshair box size",
			ui::AnyBind::Of<float>(
				[]{ EnsureConfigLoaded(); return s_Settings.framegen.ui_box_height; },
				[]( float fl )
				{
					EnsureConfigLoaded();
					s_Settings.framegen.ui_box_height = std::clamp( std::round( fl * 10.0f ) / 10.0f, 0.5f, fghost::kUiBoxMaxTenths / 10.0f );
					PersistAndPush();
				} ) )
			.Help( "The side of the square in the middle of the screen where frame generation "
			       "looks for the crosshair, as a share of the game's height. It should cover the "
			       "crosshair's solid pixels; protection reaches a pixel or two beyond its edge, "
			       "so the soft rim may stick out, and a bit of extra room is harmless. A box far "
			       "bigger than the crosshair may protect some still background too. The picture "
			       "in the Inspector shows the box and the protected area." )
			.Range( 0.5f, fghost::kUiBoxMaxTenths / 10.0f ).Step( 0.1f )
			.Default( 2.5f )
			.ValueText( []( const ui::Value &v )
			{
				uint32_t uW = 0, uH = 0;
				GameSize( &uW, &uH );
				return BoxSizeText( std::holds_alternative<float>( v ) ? std::get<float>( v ) : 2.5f, uW, uH );
			} )
			.Preview( ui::Entry::PreviewKind::UiBox )
			.Keywords( "frame generation ui protection crosshair box size area percent height pixels specks" )
			.DisabledUnless( UiOn, "UI protection or frame generation is off" );

		a.Group( "Pausing and limits" );
		a.GroupColumn( 1 );

		a.Switch( "framegen.pause_at_refresh", "Pause at refresh rate",
			ui::AnyBind::Of<bool>(
				[]{ EnsureConfigLoaded(); return s_Settings.framegen.pause_at_refresh; },
				[]( bool b ) { EnsureConfigLoaded(); s_Settings.framegen.pause_at_refresh = b; PersistAndPush(); } ) )
			.Help( "Only for the fixed multipliers (2× to 8×); with Target fps this switch has no "
			       "effect and the target is used as set. On: frame generation stops once "
			       "the game alone reaches your refresh rate, so the GPU does no pointless work. "
			       "Off: it keeps generating above your refresh rate (e.g. 800 fps): useful with "
			       "tearing or in a desktop window, where only the newest frame is shown; it "
			       "costs GPU time, and on a real display without tearing it cannot go past "
			       "the refresh rate." )
			.Default( true )
			.Keywords( "frame generation pause refresh rate stop cap limit above uncapped tearing multiplier" )
			.DisabledUnless( PauseRowUsable, "frame generation is off, or Multiplier is Target fps (the switch has no effect there)" );

		a.Switch( "framegen.pause_when_cursor_visible", "Pause while cursor visible",
			ui::AnyBind::Of<bool>(
				[]{ EnsureConfigLoaded(); return s_Settings.framegen.pause_when_cursor_visible; },
				[]( bool b ) { EnsureConfigLoaded(); s_Settings.framegen.pause_when_cursor_visible = b; PersistAndPush(); } ) )
			.Help( "Shows the game's real frames while its mouse cursor is visible, so menus and "
			       "settings screens do not get interpolation artifacts. Generation resumes by "
			       "itself when the cursor hides again." )
			.Default( false )
			.Keywords( "frame generation pause cursor mouse visible menu settings artifacts" )
			.DisabledUnless( On, "frame generation is off" );

		a.Switch( "framegen.gpu_limit", "Limit to GPU speed",
			ui::AnyBind::Of<bool>(
				[]{ EnsureConfigLoaded(); return s_Settings.framegen.gpu_limit; },
				[]( bool b ) { EnsureConfigLoaded(); s_Settings.framegen.gpu_limit = b; PersistAndPush(); } ) )
			.Help( "On: when frame generation takes too much of the GPU it lowers the output rate, "
			       "and as a last resort shows only real frames, so the game itself keeps its frame "
			       "rate. Off: always generate at the chosen rate, even if that slows the game down." )
			.Default( false )
			.Keywords( "frame generation limit gpu speed cost guard keep up slow lower output rate pass through" )
			.DisabledUnless( On, "frame generation is off" );

		a.Group( "Status" );
		a.GroupColumn( 1 );

		a.Facts( "framegen.status", "Status", []
		{
			EnsureConfigLoaded();
			return StatusLine( s_Settings.framegen, fghost::GetPacingStatus(), fghost::GetRenderStatus() );
		} )
			.Help( "What frame generation is doing right now, or why it is passing real frames "
			       "through: the game already reaching the refresh rate or target, a GPU too slow "
			       "to keep up, variable refresh, or HDR / 10-bit content. When the GPU is the "
			       "limit it generates fewer frames before giving up. MangoHud with output timing "
			       "shows the presented rate; this fork's HUD shows the game rate, the "
			       "presented rate, or both, per its FPS shown choice (Game / Output / Both)." )
			.Keywords( "frame generation status fps presented delay latency reason" );

		a.Group( "Learned motion" );
		a.GroupColumn( 1 );

		a.Choice( "framegen.learned_net", "Learned net",
			ui::AnyBind::Of<int>(
				[]{ EnsureConfigLoaded(); return IndexOf( s_Settings.framegen.learned_net, kLearnedNetKeys, 10, 0 ); },
				[]( int n ) { EnsureConfigLoaded(); s_Settings.framegen.learned_net = kLearnedNetKeys[ ClampIdx( n, 9 ) ]; PersistAndPush(); } ),
			kLearnedNetOptions, std::size( kLearnedNetOptions ) )
			.Help( "Which trained network Learned motion uses (experimental). e2v5 is the default "
			       "and the best one: about 1.4 ms of GPU time at 1080p 2x. e1v5 is the small one, "
			       "about 2.4 times cheaper (about 0.6 ms) and the one to pick when e2v5 costs too "
			       "much. e2v4 / e1v4 and e2v3 / e1v3 are the earlier versions of the same two nets, kept for "
			       "comparing. g2v2 and g2v3 are the colour and learned-features nets "
			       "trained on games, m2v2 and m2v3 the allrounders (about 1.8 / 1.9 ms at 1080p 2x, "
			       "colour / features); the allround features net m2v3 held up best even on game "
			       "clips in the library's spot check. Motion blur's Learned borrows this and the rows below: it has no "
			       "copy of its own. Only used while Motion version is Learned." )
			.Default( 0 )
			.Keywords( "frame generation learned net network e2v5 e1v5 e2v4 e1v4 e2v3 e1v3 colour features g2v2 g2v3 m2v2 m2v3 games allround small neural ml weights experimental" )
			.ShownWhen( ShowLearnedRows )
			.DisabledUnless( LearnedRowsUsable, "Motion version is not Learned (or nothing that uses it is on)" );

		a.Choice( "framegen.learned_half", "Learned precision",
			ui::AnyBind::Of<int>(
				[]{ EnsureConfigLoaded(); return s_Settings.framegen.learned_half ? 1 : 0; },
				[]( int n ) { EnsureConfigLoaded(); s_Settings.framegen.learned_half = n == 1; PersistAndPush(); } ),
			kLearnedPrecOptions, std::size( kLearnedPrecOptions ) )
			.Help( "How Learned motion stores its working data. Full is the exact version. Half "
			       "uses half the memory and about a quarter less GPU time (e2v5, 1080p: about "
			       "1.1 ms instead of 1.4) for a tiny, hardly visible difference. Only used while "
			       "Motion version is Learned." )
			.Default( 0 )
			.Keywords( "frame generation learned precision half full fp16 fp32 float memory experimental" )
			.ShownWhen( ShowLearnedRows )
			.DisabledUnless( LearnedRowsUsable, "Motion version is not Learned (or nothing that uses it is on)" );

		a.Switch( "framegen.learned_zero_test", "Zero-motion test",
			ui::AnyBind::Of<bool>(
				[]{ EnsureConfigLoaded(); return s_Settings.framegen.learned_zero_test; },
				[]( bool b ) { EnsureConfigLoaded(); s_Settings.framegen.learned_zero_test = b; PersistAndPush(); } ) )
			.Help( "On: wherever keeping a spot still fits the two real frames as well as the "
			       "network's motion, that spot is treated as not moving. This fixes most of the "
			       "warped, dragged static text and HUD (money counters, menu titles). It costs "
			       "almost nothing (about 0.006 ms at 1080p). Off: the network's raw motion, for "
			       "comparing. Only used while Motion version is Learned." )
			.Default( true )
			.Keywords( "frame generation learned zero motion test still static hud text warp experimental" )
			.ShownWhen( ShowLearnedRows )
			.DisabledUnless( LearnedRowsUsable, "Motion version is not Learned (or nothing that uses it is on)" );

		a.Switch( "framegen.learned_static_guard", "Static guard",
			ui::AnyBind::Of<bool>(
				[]{ EnsureConfigLoaded(); return s_Settings.framegen.learned_static_guard; },
				[]( bool b ) { EnsureConfigLoaded(); s_Settings.framegen.learned_static_guard = b; PersistAndPush(); } ) )
			.Help( "On: where both real frames are the same and the motion there is small, the "
			       "in-between frame shows a plain blend of the two real frames instead of the "
			       "network's picture, so still areas stay clean. It costs one extra full-screen "
			       "pass per generated frame (about 0.05 ms at 1080p). It does not repair a HUD "
			       "that sits on fast-moving scenery. Off by default. Only used while Motion "
			       "version is Learned." )
			.Default( false )
			.Keywords( "frame generation learned static guard still blend time mix clean experimental" )
			.ShownWhen( ShowLearnedRows )
			.DisabledUnless( LearnedRowsUsable, "Motion version is not Learned (or nothing that uses it is on)" );

		a.Group( "Motion version 3" );
		a.GroupColumn( 1 );

		a.Slider( "framegen.v3_smear", "Smear",
			ui::AnyBind::Of<float>(
				[]{ EnsureConfigLoaded(); return s_Settings.framegen.v3_smear; },
				[]( float fl )
				{
					EnsureConfigLoaded();
					s_Settings.framegen.v3_smear = std::clamp( std::round( fl * 20.0f ) / 20.0f, 0.0f, 1.0f );
					PersistAndPush();
				} ) )
			.Help( "How strongly Motion version 3 smears the picture along the motion where it is "
			       "unsure. 0 does not smear at all: sharper, but a wrong spot then shows as a sharp ghost. "
			       "Only used while Motion version is V3." )
			.Range( 0.0f, 1.0f ).Step( 0.05f )
			.Default( 0.35f )
			.Keywords( "frame generation v3 smear blur unsure motion ghost sharp" )
			.ShownWhen( ShowV3Rows )
			.DisabledUnless( V3RowsUsable, "Motion version is not V3 (or nothing that uses it is on)" );

		a.Slider( "framegen.v3_occl_soft", "Occlusion softness",
			ui::AnyBind::Of<float>(
				[]{ EnsureConfigLoaded(); return s_Settings.framegen.v3_occl_soft; },
				[]( float fl )
				{
					EnsureConfigLoaded();
					s_Settings.framegen.v3_occl_soft = std::clamp( std::round( fl * 20.0f ) / 20.0f, 0.25f, 4.0f );
					PersistAndPush();
				} ) )
			.Help( "How soft Motion version 3's decision is about areas that one of the two real "
			       "frames hides (behind a moving object). Higher is a softer, more one-sided "
			       "transition at the edge of such an area. Only used while Motion version is V3." )
			.Range( 0.25f, 4.0f ).Step( 0.05f )
			.Default( 1.0f )
			.Keywords( "frame generation v3 occlusion softness hidden edge disocclusion" )
			.ShownWhen( ShowV3Rows )
			.DisabledUnless( V3RowsUsable, "Motion version is not V3 (or nothing that uses it is on)" );

		a.Slider( "framegen.v3_unsure", "Unsure sensitivity",
			ui::AnyBind::Of<float>(
				[]{ EnsureConfigLoaded(); return s_Settings.framegen.v3_unsure; },
				[]( float fl )
				{
					EnsureConfigLoaded();
					s_Settings.framegen.v3_unsure = std::clamp( std::round( fl * 20.0f ) / 20.0f, 0.25f, 4.0f );
					PersistAndPush();
				} ) )
			.Help( "How readily Motion version 3 counts a spot as unsure. Higher trusts worse "
			       "matches: less smearing, more risk of wrong spots. Only used while Motion "
			       "version is V3." )
			.Range( 0.25f, 4.0f ).Step( 0.05f )
			.Default( 1.0f )
			.Keywords( "frame generation v3 unsure sensitivity trust match smear" )
			.ShownWhen( ShowV3Rows )
			.DisabledUnless( V3RowsUsable, "Motion version is not V3 (or nothing that uses it is on)" );

		a.Slider( "framegen.v3_scene_cut", "Scene cut threshold",
			ui::AnyBind::Of<float>(
				[]{ EnsureConfigLoaded(); return s_Settings.framegen.v3_scene_cut; },
				[]( float fl )
				{
					EnsureConfigLoaded();
					s_Settings.framegen.v3_scene_cut = std::clamp( std::round( fl * 20.0f ) / 20.0f, 0.0f, 4.0f );
					PersistAndPush();
				} ) )
			.Help( "0 = Never (default). Above 0, V3 shows the previous real frame when two frames "
			       "differ this much; on fast motion that looks like lag. Only used while Motion "
			       "version is V3." )
			.Range( 0.0f, 4.0f ).Step( 0.05f )
			.Default( 0.0f )
			.ZeroMeans( "Never" )
			.Keywords( "frame generation v3 scene cut threshold histogram cut transition" )
			.ShownWhen( ShowV3Rows )
			.DisabledUnless( V3RowsUsable, "Motion version is not V3 (or nothing that uses it is on)" );

		a.Switch( "framegen.v3_colour_window", "Colour-weighted matching",
			ui::AnyBind::Of<bool>(
				[]{ EnsureConfigLoaded(); return s_Settings.framegen.v3_colour_window; },
				[]( bool b ) { EnsureConfigLoaded(); s_Settings.framegen.v3_colour_window = b; PersistAndPush(); } ) )
			.Help( "Experiment: weights the motion search by colour similarity. Can sharpen clearly "
			       "coloured moving objects; can break areas next to flat dark regions. +0.05 ms." )
			.Default( false )
			.Keywords( "frame generation v3 colour color weighted matching window motion search experiment" )
			.ShownWhen( ShowV3Rows )
			.DisabledUnless( V3RowsUsable, "Motion version is not V3 (or nothing that uses it is on)" );

		a.Switch( "framegen.v3_detail_split", "Detail from one frame",
			ui::AnyBind::Of<bool>(
				[]{ EnsureConfigLoaded(); return s_Settings.framegen.v3_detail_split; },
				[]( bool b ) { EnsureConfigLoaded(); s_Settings.framegen.v3_detail_split = b; PersistAndPush(); } ) )
			.Help( "Experiment: blends only the coarse picture and takes fine detail from one frame. "
			       "Slightly crisper texture and text. +0.06 ms per generated frame." )
			.Default( false )
			.Keywords( "frame generation v3 detail split one frame texture text sharp crisp experiment" )
			.ShownWhen( ShowV3Rows )
			.DisabledUnless( V3RowsUsable, "Motion version is not V3 (or nothing that uses it is on)" );

		a.Choice( "framegen.v3_guard", "Motion guard",
			ui::AnyBind::Of<int>(
				[]{ EnsureConfigLoaded(); return IndexOf( s_Settings.framegen.v3_guard, kGuardKeys, 3, 0 ); },
				[]( int n ) { EnsureConfigLoaded(); s_Settings.framegen.v3_guard = kGuardKeys[ ClampIdx( n, 2 ) ]; PersistAndPush(); } ),
			kGuardOptions, std::size( kGuardOptions ) )
			.Help( "Experiment. Off: Motion version 3 alone (default). Guard: V2's motion search also runs "
			       "and corrects V3 where V2 matches the two frames clearly better. Reduces some large "
			       "artifacts (objects held in front of a moving background, frame edges); does not fix a "
			       "radar or minimap. About +0.34 ms per real frame and 14 MiB. V2 motion: V2's motion "
			       "everywhere, V3's picture generation on top. Removes more of V3's large artifacts but "
			       "loses V3's advantage on fast motion (looks like V2 there). About +0.31 ms per real "
			       "frame. While it is not Off, the Motion search rows and Search penalty act on it. "
			       "Only used while Motion version is V3." )
			.Segmented()
			.Default( 0 )
			.Keywords( "frame generation v3 motion guard v2 motion artifacts experiment large artifacts" )
			.ShownWhen( ShowV3Rows )
			.DisabledUnless( V3RowsUsable, "Motion version is not V3 (or nothing that uses it is on)" );

		a.Group( "Motion search" );
		a.GroupColumn( 1 );

		a.Slider( "framegen.search_radius", "Search radius",
			ui::AnyBind::Of<int>(
				[]{ EnsureConfigLoaded(); return s_Settings.framegen.search_radius; },
				[]( int n ) { EnsureConfigLoaded(); s_Settings.framegen.search_radius = std::clamp( n, 1, 12 ); PersistAndPush(); } ) )
			.Help( "How far the motion search looks around its guess at each step, in pixels of "
			       "the motion map, 1 to 12. A larger radius follows faster motion between two real "
			       "frames. Above 6 a wider, slower search runs. Large values cost GPU time and can "
			       "make frame generation lower its output rate: 12 / 12 / 16 at 1080p measured 1.29 ms "
			       "per game frame in V1 against 0.29 ms at the defaults (this fork's V1 budget at 1080p "
			       "is 0.33 ms). Motion versions V1 and V2, and V3 while Motion guard is on; Learned motion and V3 with the guard off ignore it." )
			.Range( 1.0f, 12.0f ).Step( 1.0f )
			.Default( 2 )
			.Keywords( "frame generation expert motion search radius reach fast motion vectors v1 v2" )
			.ShownWhen( ShowRadiusRows )
			.DisabledUnless( RadiusRowsUsable, "nothing that uses the motion search is on (or Motion version is Learned, or V3 with Motion guard off)" );

		a.Slider( "framegen.search_radius_top", "Search radius (coarsest level)",
			ui::AnyBind::Of<int>(
				[]{ EnsureConfigLoaded(); return s_Settings.framegen.search_radius_top; },
				[]( int n ) { EnsureConfigLoaded(); s_Settings.framegen.search_radius_top = std::clamp( n, 1, 12 ); PersistAndPush(); } ) )
			.Help( "The search radius on the smallest, coarsest picture of the motion search, 1 to "
			       "12. It sets the longest motion that can be found: each finer level doubles it. "
			       "Defaults 2 and 6 reach about 250 pixels between two real frames at 1080p. Above 6 "
			       "the wider search runs; large values cost GPU time and can make frame generation "
			       "lower its output rate (see Search radius). Motion versions V1 and V2, and V3 "
			       "while Motion guard is on; Learned motion and V3 with the guard off ignore it." )
			.Range( 1.0f, 12.0f ).Step( 1.0f )
			.Default( 6 )
			.Keywords( "frame generation expert motion search radius coarsest top level reach long vectors v1 v2" )
			.ShownWhen( ShowRadiusRows )
			.DisabledUnless( RadiusRowsUsable, "nothing that uses the motion search is on (or Motion version is Learned, or V3 with Motion guard off)" );

		a.Choice( "framegen.pyramid_min_side", "Pyramid smallest level",
			ui::AnyBind::Of<int>(
				[]{ EnsureConfigLoaded(); return NearestPyramidSide( s_Settings.framegen.pyramid_min_side ); },
				[]( int n ) { EnsureConfigLoaded(); s_Settings.framegen.pyramid_min_side = NearestPyramidSide( n ); PersistAndPush(); } ),
			kPyramidOptions, std::size( kPyramidOptions ) )
			.Help( "How small the coarsest picture of the motion search may get, as the shorter "
			       "side in pixels of the motion map. A smaller value adds levels: motion can be longer "
			       "(16 gives 6 levels instead of 4 at 1080p), costs a little more and risks a wrong "
			       "match on the coarsest level. Changing it re-creates the frame generation buffers and "
			       "pauses the picture for an instant. Large reach costs GPU time and can make frame "
			       "generation lower its output rate (see Search radius). Applies to every motion "
			       "version; V3 builds its own and uses it only for the Motion guard's V2 search." )
			.Default( 48 )
			.Keywords( "frame generation expert motion search pyramid levels smallest minimum side reach long vectors" )
			.ShownWhen( ShowPenaltyRows )
			.DisabledUnless( PenaltyRowsUsable, "frame generation and motion blur are off (or Motion version is V3 with Motion guard off)" );

		a.Slider( "framegen.search_penalty", "Search penalty",
			ui::AnyBind::Of<float>(
				[]{ EnsureConfigLoaded(); return s_Settings.framegen.search_penalty; },
				[]( float fl )
				{
					EnsureConfigLoaded();
					s_Settings.framegen.search_penalty = std::clamp( std::round( fl * 20.0f ) / 20.0f, 0.0f, 4.0f );
					PersistAndPush();
				} ) )
			.Help( "What the motion search pays for each step away from its guess. Higher gives "
			       "steadier motion on flat or repeating surfaces, lower follows small fast detail "
			       "more freely. 0 to 4 here; a profile file may hold up to 255. Motion versions V1, V2 and Learned, and V3 while Motion guard is on (otherwise V3 ignores it)." )
			.Range( 0.0f, 4.0f ).Step( 0.05f )
			.Default( 0.5f )
			.Keywords( "frame generation expert motion search penalty lambda steady flat repetitive detail v1 v2" )
			.ShownWhen( ShowPenaltyRows )
			.DisabledUnless( PenaltyRowsUsable, "frame generation and motion blur are off (or Motion version is V3 with Motion guard off)" );
	}
}
