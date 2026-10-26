// The "Motion blur" settings area -- see PanelMotionBlur.h.
//
// SHAPE. Five per-profile rows -- Motion blur (the on/off switch), Samples (2..8),
// Blur amount (0..100 %), Measured against (Shown frame / Game frame), Weighting
// (Even / Gaussian) -- plus the "Use Frame generation's settings" switch with the blur's
// Quality and Motion version rows (see below) and one live Status line. Every row's id IS its config key
// (`motion_blur.enabled`, `motion_blur.samples`, ...), which is how the Shell's
// per-profile inherited/overridden dot and "Reset to inherited" find it.
// Every edit calls fghost::SetBlurConfig() at once; the renderer and the pacer read
// it on the next frame (nothing waits).
//
// THE INTERPOLATOR'S VERSION AND QUALITY (2026-10-22). Blur and Frame generation run on
// ONE interpolator, whose Motion version (V1 / V2) and Quality are Frame generation's
// `framegen.motion_version` / `framegen.quality`. This area has the "Use Frame generation's
// settings" switch (`motion_blur.same_as_framegen`, default on) and its own pair of rows
// (`motion_blur.motion_version` / `motion_blur.quality`). Switch on: the pair is greyed out
// and DISPLAYS Frame generation's current values (read from the renderer's live config,
// fghost::GetConfig(), so this panel never holds a copy of the `framegen` section), and the
// blur follows Frame generation. Switch off: the blur's own pair is pushed with the blur
// config and REPLACES Frame generation's whenever the blur is on (FrameGenHost.cpp's
// RecordBaseLayer()). `Why not a second interpolator for the blur:` every shown frame IS a
// blurred synth (one recordSynthBlur() per output, its sub-frames are what the blur
// averages), so one output cannot be V2 for the generated frame and V1 for its blur; the
// blur's choice simply applies to the whole picture while the blur is on, at no extra GPU
// cost. The user, asking for the switch: "use same settings as frame gen ... and by default
// it should be on. It should just disable the other two, like gray them out", and what
// unticking does: "It should really just copy over the settings essentially. Like if
// basically like when I have frame gen on v2 quality, then it should just set v2 and
// quality for motion blur as well." Why the version is selectable here at all: "Make sure
// that I can also select the v2 for the motion blur".
// With Frame generation off the renderer still runs the configured pair for the blur, and
// the Status line below appends the library's reason when V2 was refused and V1 runs.
//
// `Why this panel is thin:` the user's architecture rule -- "you're basically only
// building the GUI in this chat and most of the stuff should go into the frame gen
// itself". Which part of the pair each output averages, the cost guard, the
// composition with frame generation: all the library's. This file maps five choices
// onto fghost::BlurConfig and prints the pacer's Report.
// `Why every whole number 2..8 for Samples:` the user asked for it explicitly, not
// just 2 / 4 / 8 (the library accepts any count; odd counts work).
// `Why no keybind:` like frame generation, a standing mode of the picture, not a
// momentary action; the switch is the on/off.
#include "PanelMotionBlur.h"

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

		// The config stores readable strings (matching framegen.priority); the
		// Choice rows bind ints; fghost wants its own enums. An unknown string
		// falls to the default the schema documents.
		constexpr ui::Option kRelativeOptions[] = { { 0, "Shown frame" }, { 1, "Game frame" } };
		constexpr ui::Option kWeightOptions[]   = { { 0, "Even" }, { 1, "Gaussian" } };
		constexpr ui::Option kQualityOptions[] = { { 0, "Quality" }, { 1, "Performance" } };
		constexpr ui::Option kMotionOptions[]  = { { 0, "V1" }, { 1, "V2" }, { 2, "Learned" } };
		constexpr const char *kQualityKeys[] = { "quality", "performance" };
		constexpr const char *kMotionKeys[]  = { "v1", "v2", "learned" };
		constexpr const char *kRelativeKeys[] = { "shown", "game" };
		constexpr const char *kWeightKeys[]   = { "even", "gaussian" };

		int IndexOf( const std::string &s, const char *const *ppszKeys, int n, int nFallback )
		{
			for ( int i = 0; i < n; ++i )
				if ( s == ppszKeys[ i ] )
					return i;
			return nFallback;
		}

		int ClampIdx( int n, int nMax ) { return n < 0 ? 0 : ( n > nMax ? nMax : n ); }

		int ClampSamples( int n )
		{
			return n < fghost::kMinBlurSamples ? fghost::kMinBlurSamples
			     : ( n > fghost::kMaxBlurSamples ? fghost::kMaxBlurSamples : n );
		}

		fghost::BlurConfig ToHostConfig( const config::MotionBlurSettings &m )
		{
			fghost::BlurConfig c;
			c.enabled = m.enabled;
			c.samples = ClampSamples( m.samples );
			c.amountPercent = m.amount < 0 ? 0 : ( m.amount > 100 ? 100 : m.amount );
			c.relative = IndexOf( m.relative, kRelativeKeys, 2, 0 ) == 1 ? fghost::BlurRel::Game : fghost::BlurRel::Shown;
			c.weights = IndexOf( m.weights, kWeightKeys, 2, 1 ) == 0 ? fghost::BlurWeighting::Even : fghost::BlurWeighting::Gaussian;
			c.useOwnInterp = !m.same_as_framegen;
			c.motion = (fghost::Motion)IndexOf( m.motion_version, kMotionKeys, 3, 0 );   // the index IS the enum value
			c.quality = IndexOf( m.quality, kQualityKeys, 2, 0 ) == 1 ? fghost::Quality::Performance : fghost::Quality::Quality;
			return c;
		}

		// The one place config state becomes renderer / pacer state: after every
		// edit, after every (re)load, and at startup / profile switch via
		// PanelMotionBlur_ApplyStartupConfig().
		void PushToRenderer( const config::MotionBlurSettings &m )
		{
			fghost::SetBlurConfig( ToHostConfig( m ) );
		}

		void EnsureConfigLoaded()
		{
			const uint64_t ulGeneration = config::ConfigGeneration();
			if ( s_bConfigLoaded && ulGeneration == s_ulLoadedGeneration )
				return;
			s_Settings = config::ResolvedSettings();
			s_ulLoadedGeneration = ulGeneration;
			s_bConfigLoaded = true;
			// A (re)load -- first draw, or a profile switch -- applies now, not
			// when the user next touches a row.
			PushToRenderer( s_Settings.motion_blur );
		}

		void PersistAndPush()
		{
			PushToRenderer( s_Settings.motion_blur );
			config::EnqueueRoutedWrite( s_Settings );
		}

		bool On()
		{
			EnsureConfigLoaded();
			return s_Settings.motion_blur.enabled;
		}

		bool OwnInterpUsable()
		{
			EnsureConfigLoaded();
			return s_Settings.motion_blur.enabled && !s_Settings.motion_blur.same_as_framegen;
		}

		// ---- the Status line ----------------------------------------------------
		// One compact line, always. Pure over its inputs.
		//   Off                    -> "Off"
		//   applied                -> "4 samples · 1.8 ms window" (+ " · V1: <reason>" when
//                             Motion version V2 was refused and V1 runs)
		//   otherwise the reason   -> the renderer's (HDR, 10-bit, YCbCr, ...) when it
		//                             refuses, else pacing's (GPU too slow, warming up,
		//                             a frame gap, ...), else a note that amount 0 is
		//                             no blur
		//   nothing published yet  -> "Waiting for frames"
		// The font is Basic Latin + Latin-1, so "·" renders.
		std::string StatusLine( const config::MotionBlurSettings &m, const fghost::PacingStatus &ps, const fghost::RenderStatus &rs )
		{
			if ( !m.enabled )
				return "Off";

			if ( ps.valid && ps.blurActive )
			{
				char sz[ 96 ];
				std::snprintf( sz, sizeof( sz ), "%d samples · %.1f ms window", ps.blurSamples, ps.blurWindowMs );
				// V2 was asked for but the library refused it for this content or GPU.
				if ( rs.motionNote && rs.motionNote[ 0 ] )
				{
					std::string s = sz;
					s += " · V1: ";
					s += rs.motionNote;
					return s;
				}
				return sz;
			}

			if ( m.amount <= 0 )
				return "Blur amount is 0: no blur";

			if ( rs.reason != fghost::Unavailable::Ok )
				return fghost::UnavailableText( rs.reason );

			if ( ps.valid )
			{
				const std::string sReason = fghost::PassReasonText( ps.reason );
				// "Off" is pacing's reason for a blur-only plan with frame generation
				// off: not a reason blur is not applied, so it is not shown here.
				if ( !sReason.empty() && ps.reason != fghost::PassReason::Off )
					return sReason;
			}
			return "Waiting for frames";
		}
	}

	void PanelMotionBlur_ApplyStartupConfig( const config::Settings &config )
	{
		PushToRenderer( config.motion_blur );
	}

	void PanelMotionBlur_RegisterArea( ui::Registry &reg )
	{
		ui::Area &a = reg.Add( "image.motionblur", "Motion blur", ui::Section::Display );

		a.Keywords( "motion blur shutter smear camera film blend interpolation frames samples "
		            "frame generation motion trail" );
		a.Summary( []
		{
			EnsureConfigLoaded();
			const config::MotionBlurSettings &m = s_Settings.motion_blur;
			if ( !m.enabled )
				return std::string( "off" );
			return std::to_string( m.samples ) + " samples";
		} );

		a.Group( "Motion blur" );

		a.Switch( "motion_blur.enabled", "Motion blur",
			ui::AnyBind::Of<bool>(
				[]{ EnsureConfigLoaded(); return s_Settings.motion_blur.enabled; },
				[]( bool b ) { EnsureConfigLoaded(); s_Settings.motion_blur.enabled = b; PersistAndPush(); } ) )
			.Help( "Blends several in-between frames into each shown frame, like a camera "
			       "shutter. Works with or without Frame generation. It never waits for a "
			       "future frame, so it adds no delay. Costs about 0.03 ms per sample at "
			       "1280x960 and 0.1 ms at 1440p, per shown frame. UI protection (in Frame "
			       "generation) keeps the crosshair sharp inside the blur. It reduces clarity "
			       "in motion, so it is not for competitive play." )
			.Default( false )
			.Keywords( "motion blur shutter smear camera film enable on off switch toggle" );

		a.Slider( "motion_blur.samples", "Samples",
			ui::AnyBind::Of<int>(
				[]{ EnsureConfigLoaded(); return ClampSamples( s_Settings.motion_blur.samples ); },
				[]( int n ) { EnsureConfigLoaded(); s_Settings.motion_blur.samples = ClampSamples( n ); PersistAndPush(); } ) )
			.Help( "How many in-between frames are blended into each shown frame, 2 to 8. More "
			       "samples blur more smoothly but cost more GPU time (about 0.03 ms each at "
			       "1280x960, 0.1 ms at 1440p, per shown frame); too few show separate ghost "
			       "copies on fast motion." )
			.Range( float( fghost::kMinBlurSamples ), float( fghost::kMaxBlurSamples ) ).Step( 1.0f )
			.Default( 4 )
			.Keywords( "motion blur samples quality smooth ghost copies cost gpu" )
			.DisabledUnless( On, "motion blur is off" );

		a.Slider( "motion_blur.amount", "Blur amount",
			ui::AnyBind::Of<int>(
				[]{ EnsureConfigLoaded(); return s_Settings.motion_blur.amount; },
				[]( int n ) { EnsureConfigLoaded(); s_Settings.motion_blur.amount = n < 0 ? 0 : ( n > 100 ? 100 : n ); PersistAndPush(); } ) )
			.Help( "How long the shutter stays open, as a percentage of one frame interval "
			       "(which frame is set by Measured against). 50% is a half-open, film-style "
			       "shutter; 100% blends across the whole interval. 0% shows no blur." )
			.Range( 0.0f, 100.0f ).Step( 5.0f ).Unit( "%" )
			.Default( 50 )
			.Keywords( "motion blur amount strength shutter angle length intensity" )
			.DisabledUnless( On, "motion blur is off" );

		a.Choice( "motion_blur.relative", "Measured against",
			ui::AnyBind::Of<int>(
				[]{ EnsureConfigLoaded(); return IndexOf( s_Settings.motion_blur.relative, kRelativeKeys, 2, 0 ); },
				[]( int n ) { EnsureConfigLoaded(); s_Settings.motion_blur.relative = kRelativeKeys[ ClampIdx( n, 1 ) ]; PersistAndPush(); } ),
			kRelativeOptions, std::size( kRelativeOptions ) )
			.Help( "Shown frame measures the shutter against the time one displayed frame is on "
			       "screen: subtle, and it stays short as Frame generation shows more frames. "
			       "Game frame measures it against one of the game's own frames: the film look, "
			       "longer and heavier, and it needs more samples to stay smooth." )
			.Default( 0 )
			.Keywords( "motion blur measured against relative shown frame game frame film look subtle" )
			.DisabledUnless( On, "motion blur is off" );

		a.Choice( "motion_blur.weights", "Weighting",
			ui::AnyBind::Of<int>(
				[]{ EnsureConfigLoaded(); return IndexOf( s_Settings.motion_blur.weights, kWeightKeys, 2, 1 ); },
				[]( int n ) { EnsureConfigLoaded(); s_Settings.motion_blur.weights = kWeightKeys[ ClampIdx( n, 1 ) ]; PersistAndPush(); } ),
			kWeightOptions, std::size( kWeightOptions ) )
			.Help( "Even counts every sample the same: a hard-edged smear, like a real camera "
			       "shutter. Gaussian counts the middle samples most: softer, and effectively "
			       "a little shorter." )
			.Default( 1 )
			.Keywords( "motion blur weighting weights even gaussian soft box shutter" )
			.DisabledUnless( On, "motion blur is off" );

		a.Group( "Interpolation" );

		a.Switch( "motion_blur.same_as_framegen", "Use Frame generation's settings",
			ui::AnyBind::Of<bool>(
				[]{ EnsureConfigLoaded(); return s_Settings.motion_blur.same_as_framegen; },
				[]( bool b ) { EnsureConfigLoaded(); s_Settings.motion_blur.same_as_framegen = b; PersistAndPush(); } ) )
			.Help( "On: the blur uses Frame generation's Motion version and Quality (shown greyed "
			       "out below). Off: it uses the two below instead. Frame generation and Motion "
			       "blur make the picture together with one motion estimate, so while Motion blur "
			       "is on, its own two settings are used for every shown frame, the frames Frame "
			       "generation makes included; that costs nothing extra." )
			.Default( true )
			.Keywords( "motion blur use same settings frame generation follow motion version quality v1 v2" )
			.DisabledUnless( On, "motion blur is off" );

		a.Choice( "motion_blur.motion_version", "Motion version",
			ui::AnyBind::Of<int>(
				[]
				{
					EnsureConfigLoaded();
					return s_Settings.motion_blur.same_as_framegen
						? int( fghost::GetConfig().motion )
						: IndexOf( s_Settings.motion_blur.motion_version, kMotionKeys, 3, 0 );
				},
				[]( int n ) { EnsureConfigLoaded(); s_Settings.motion_blur.motion_version = kMotionKeys[ ClampIdx( n, 2 ) ]; PersistAndPush(); } ),
			kMotionOptions, std::size( kMotionOptions ) )
			.Help( "V2 is a newer, experimental way of making the in-between frames the blur "
			       "averages: it bends the picture along the motion and stretches it over gaps "
			       "instead of falling back to the real frame spot by spot, and it is cheaper on "
			       "the GPU. It works in every game format, HDR and 10-bit included; a GPU "
			       "without the shader features it needs stays on V1 (the Status line says why). "
			       "Learned is an experimental neural-network estimator (several times V1's GPU "
			       "time, always at Quality's resolution; see Frame generation's Motion version "
			       "help); its net and precision are Frame generation's Learned net and Learned "
			       "precision rows, this area has none of its own. "
			       "Switching shows real frames for an instant. Shown greyed out with Frame "
			       "generation's value while \"Use Frame generation's settings\" is on." )
			.Default( 0 )
			.Keywords( "motion blur motion version v1 v2 learned neural network ml morph interpolation algorithm experimental" )
			.DisabledUnless( OwnInterpUsable, "motion blur is off, or it uses Frame generation's settings" );

		a.Choice( "motion_blur.quality", "Quality",
			ui::AnyBind::Of<int>(
				[]
				{
					EnsureConfigLoaded();
					return s_Settings.motion_blur.same_as_framegen
						? int( fghost::GetConfig().quality )
						: IndexOf( s_Settings.motion_blur.quality, kQualityKeys, 2, 0 );
				},
				[]( int n ) { EnsureConfigLoaded(); s_Settings.motion_blur.quality = kQualityKeys[ ClampIdx( n, 1 ) ]; PersistAndPush(); } ),
			kQualityOptions, std::size( kQualityOptions ) )
			.Help( "Quality is the default look; Performance is cheaper on the GPU and a little "
			       "less clean. Motion version V1: Performance estimates motion at a quarter "
			       "resolution and can miss thin, fast detail (roughly 35-50% cheaper; 1440p 2x: "
			       "0.50 -> 0.25 ms per game frame, RX 7900 XTX); switching pauses the picture for "
			       "an instant. Motion version V2: Quality works out a motion value for every pixel "
			       "and keeps the side of the picture that is still visible at edges, Performance "
			       "is a simpler warp, a little less clean around moving edges (1080p 2x, RX 7900 "
			       "XTX: V1 0.30, V2 Quality 0.24, V2 Performance 0.14 ms per game frame); "
			       "switching does not interrupt the picture. Shown greyed out with Frame "
			       "generation's value while \"Use Frame generation's settings\" is on." )
			.Default( 0 )
			.Keywords( "motion blur quality performance cost gpu motion estimation v2 mode" )
			.DisabledUnless( OwnInterpUsable, "motion blur is off, or it uses Frame generation's settings" );

		a.Group( "Status" );

		a.Facts( "motion_blur.status", "Status", []
		{
			EnsureConfigLoaded();
			return StatusLine( s_Settings.motion_blur, fghost::GetPacingStatus(), fghost::GetRenderStatus() );
		} )
			.Help( "What the blur is doing right now: the number of samples and the length of "
			       "the shutter window in milliseconds, or why it is not applied (HDR or 10-bit "
			       "content, a GPU too slow to keep up, waiting for frames). The Quality preset, "
			       "Motion version and UI protection apply to the blur too, even with Frame "
			       "generation itself off; when V2 is not possible for this game or GPU, the "
			       "line ends with \" · V1:\" and the reason." )
			.Keywords( "motion blur status samples window reason" );
	}
}
