// The "Frame generation" settings area -- see PanelFrameGen.h.
//
// SHAPE. Four per-profile rows (multiplier, Quality, Artifact safety, Static
// HUD protection) plus one live Status line. Every row's id IS its config key
// (`framegen.multiplier`, ...), which is how the Shell's per-profile
// inherited/overridden dot and "Reset to inherited" find it -- no `.Key()`
// override needed (config::IsSettingsKey() answers from the serializer).
// Every edit calls fghost::SetConfig() at once; the renderer applies the
// presets live (a Quality change costs it one GPU wait, which is its business).
//
// `Why only these four:` the library's expert knobs (searchPenalty and the
// rest) are deliberately not exposed -- only the defaults were visually
// reviewed, so a slider would hand the user settings nobody has looked at.
// `Why no keybind:` frame generation is a standing mode of the picture, not a
// momentary action; the Frame generation row is the switch.
#include "PanelFrameGen.h"

#include <cstdint>
#include <cstdio>
#include <string>

#include "Config/ConfigManager.h"
#include "FrameGen/FrameGenHost.h"

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
		constexpr ui::Option kMultiplierOptions[] = {
			{ 0, "Off" }, { 2, "2×" }, { 3, "3×" }, { 4, "4×" } };
		constexpr ui::Option kQualityOptions[] = { { 0, "Quality" }, { 1, "Performance" } };
		constexpr ui::Option kSafetyOptions[]  = { { 0, "Low" }, { 1, "Default" }, { 2, "High" } };
		constexpr ui::Option kHudOptions[]     = { { 0, "Off" }, { 1, "Normal" }, { 2, "Strong" } };

		constexpr const char *kQualityKeys[] = { "quality", "performance" };
		constexpr const char *kSafetyKeys[]  = { "low", "default", "high" };
		constexpr const char *kHudKeys[]     = { "off", "normal", "strong" };

		int IndexOf( const std::string &s, const char *const *ppszKeys, int n, int nFallback )
		{
			for ( int i = 0; i < n; ++i )
				if ( s == ppszKeys[ i ] )
					return i;
			return nFallback;
		}

		int ClampIdx( int n, int nMax ) { return n < 0 ? 0 : ( n > nMax ? nMax : n ); }

		fghost::Config ToHostConfig( const config::FrameGenSettings &f )
		{
			fghost::Config c;
			c.multiplier = f.multiplier;   // SetConfig() normalises anything but 0/2/3/4
			c.quality = IndexOf( f.quality, kQualityKeys, 2, 0 ) == 1
				? fghost::Quality::Performance : fghost::Quality::Quality;
			c.safety = (fghost::Safety)IndexOf( f.safety, kSafetyKeys, 3, 1 );
			c.hud = (fghost::HudProtect)IndexOf( f.hud_protection, kHudKeys, 3, 1 );
			return c;
		}

		// The one place config state becomes renderer state: after every edit,
		// after every (re)load, and at startup / profile switch via
		// PanelFrameGen_ApplyStartupConfig().
		void PushToRenderer( const config::FrameGenSettings &f )
		{
			fghost::SetConfig( ToHostConfig( f ) );
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
			return s_Settings.framegen.multiplier >= 2;
		}

		// ---- the Status line (decision D18) -----------------------------------
		// One line, always. Pure over its inputs so the strings are one place;
		// the live caller below feeds it the two fghost snapshots.
		//   Off                       -> "Off"
		//   pacing live, N >= 2       -> "game 60 fps -> presented ~120 fps · 4× (2× active)
		//                                 · FG 0.42 ms · +8.3 ms delay"
		//   otherwise the reason      -> the renderer's (HDR, 10-bit, YCbCr, ...) when it
		//                                 refuses, else pacing's (game too fast, GPU too
		//                                 slow, warming up, reset, ...)
		//   nothing published yet     -> "Waiting for frames"
		// The font is Basic Latin + Latin-1 only, so the arrow is "->" (U+2192
		// is not in the atlas); "·" and "×" are Latin-1 and render.
		std::string StatusLine( int nChosen, const fghost::PacingStatus &ps, const fghost::RenderStatus &rs )
		{
			if ( nChosen < 2 )
				return "Off";

			if ( ps.valid && ps.activeN >= 2 )
			{
				char sz[ 192 ];
				int n = std::snprintf( sz, sizeof( sz ), "game %.0f fps -> presented ~%.0f fps · %d×",
					ps.gameFps, ps.presentedFps, ps.chosenN );
				std::string s( sz, n > 0 ? (size_t)n : 0 );
				if ( ps.activeN != ps.chosenN )
					s += " (" + std::to_string( ps.activeN ) + "× active)";
				if ( rs.lastPairGpuMs < 0.0f )
					s += " · FG n/a";
				else
				{
					std::snprintf( sz, sizeof( sz ), " · FG %.2f ms", rs.lastPairGpuMs );
					s += sz;
				}
				std::snprintf( sz, sizeof( sz ), " · +%.1f ms delay", ps.delayMs );
				s += sz;
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
		            "optical flow lsfg fsr3 fmf frames" );
		a.Summary( []
		{
			EnsureConfigLoaded();
			const int n = s_Settings.framegen.multiplier;
			return n >= 2 ? std::to_string( n ) + "×" : std::string( "off" );
		} );

		using S = config::FrameGenSettings;

		a.Group( "Frame generation" );

		a.Choice( "framegen.multiplier", "Frame generation",
			ui::AnyBind::Of<int>(
				[]{ EnsureConfigLoaded(); return s_Settings.framegen.multiplier; },
				[]( int n ) { EnsureConfigLoaded(); s_Settings.framegen.multiplier = n < 2 ? 0 : ( n > 4 ? 4 : n ); PersistAndPush(); } ),
			kMultiplierOptions, std::size( kMultiplierOptions ) )
			.Help( "Shows 2, 3 or 4 frames for every game frame by generating the ones in "
			       "between. It holds each real frame back, adding about 1/2 of a game frame of "
			       "delay at 2×, 2/3 at 3× and 3/4 at 4× -- not suited to twitch shooters. Works "
			       "best when the game runs at about 1/N of your refresh rate: the Frame limiter "
			       "caps the game and this multiplies the capped rate, so a limit of refresh/N is "
			       "the natural pairing." )
			.Default( S{}.multiplier )
			.Keywords( "frame generation multiplier 2x 3x 4x double triple quadruple interpolation off" );

		a.Choice( "framegen.quality", "Quality",
			ui::AnyBind::Of<int>(
				[]{ EnsureConfigLoaded(); return IndexOf( s_Settings.framegen.quality, kQualityKeys, 2, 0 ); },
				[]( int n ) { EnsureConfigLoaded(); s_Settings.framegen.quality = kQualityKeys[ ClampIdx( n, 1 ) ]; PersistAndPush(); } ),
			kQualityOptions, std::size( kQualityOptions ) )
			.Help( "Performance estimates motion at a lower resolution: about a third cheaper on "
			       "the GPU, but it can miss thin, fast detail. Switching pauses the picture for "
			       "an instant." )
			.Default( 0 )
			.Keywords( "frame generation quality performance cost gpu motion estimation" )
			.DisabledUnless( On, "frame generation is off" );

		a.Choice( "framegen.safety", "Artifact safety",
			ui::AnyBind::Of<int>(
				[]{ EnsureConfigLoaded(); return IndexOf( s_Settings.framegen.safety, kSafetyKeys, 3, 1 ); },
				[]( int n ) { EnsureConfigLoaded(); s_Settings.framegen.safety = kSafetyKeys[ ClampIdx( n, 2 ) ]; PersistAndPush(); } ),
			kSafetyOptions, std::size( kSafetyOptions ) )
			.Help( "How readily a doubtful area falls back to the real frame. High shows fewer "
			       "smeared or warped spots but a little less smoothing; Low is smoother with "
			       "more visible mistakes." )
			.Default( 1 )
			.Keywords( "frame generation artifact safety artefacts ghosting trust fallback" )
			.DisabledUnless( On, "frame generation is off" );

		a.Choice( "framegen.hud_protection", "Static HUD protection",
			ui::AnyBind::Of<int>(
				[]{ EnsureConfigLoaded(); return IndexOf( s_Settings.framegen.hud_protection, kHudKeys, 3, 1 ); },
				[]( int n ) { EnsureConfigLoaded(); s_Settings.framegen.hud_protection = kHudKeys[ ClampIdx( n, 2 ) ]; PersistAndPush(); } ),
			kHudOptions, std::size( kHudOptions ) )
			.Help( "Keeps a motionless on-screen HUD from wobbling, and a still crosshair in the "
			       "middle of the screen sharp. The game's own HUD can still artifact when "
			       "frames are generated; this fork's own Crosshair is drawn after generation "
			       "and stays sharp." )
			.Default( 1 )
			.Keywords( "frame generation static hud protection ui wobble crosshair" )
			.DisabledUnless( On, "frame generation is off" );

		a.Group( "Status" );

		a.Facts( "framegen.status", "Status", []
		{
			EnsureConfigLoaded();
			return StatusLine( s_Settings.framegen.multiplier, fghost::GetPacingStatus(), fghost::GetRenderStatus() );
		} )
			.Help( "What frame generation is doing right now, or why it is passing real frames "
			       "through: the game running too fast for the chosen multiplier, a GPU too slow "
			       "to keep up, or HDR / 10-bit content. It steps down to a lower multiplier "
			       "before giving up. MangoHud with output timing shows the presented rate; this "
			       "fork's HUD shows the game rate." )
			.Keywords( "frame generation status fps presented delay latency reason" );
	}
}
