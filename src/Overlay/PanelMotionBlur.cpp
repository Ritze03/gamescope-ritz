// The "Motion blur" settings area -- see PanelMotionBlur.h.
//
// SHAPE. Five per-profile rows -- Motion blur (the on/off switch), Samples (2..8),
// Blur amount (0..100 %), Measured against (Shown frame / Game frame), Weighting
// (Even / Gaussian) -- plus one live Status line. Every row's id IS its config key
// (`motion_blur.enabled`, `motion_blur.samples`, ...), which is how the Shell's
// per-profile inherited/overridden dot and "Reset to inherited" find it.
// Every edit calls fghost::SetBlurConfig() at once; the renderer and the pacer read
// it on the next frame (nothing waits).
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

		// ---- the Status line ----------------------------------------------------
		// One compact line, always. Pure over its inputs.
		//   Off                    -> "Off"
		//   applied                -> "4 samples · 1.8 ms window"
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

		a.Group( "Status" );

		a.Facts( "motion_blur.status", "Status", []
		{
			EnsureConfigLoaded();
			return StatusLine( s_Settings.motion_blur, fghost::GetPacingStatus(), fghost::GetRenderStatus() );
		} )
			.Help( "What the blur is doing right now: the number of samples and the length of "
			       "the shutter window in milliseconds, or why it is not applied (HDR or 10-bit "
			       "content, a GPU too slow to keep up, waiting for frames). The Quality preset "
			       "and UI protection in Frame generation apply to the blur too, even with "
			       "Frame generation itself off." )
			.Keywords( "motion blur status samples window reason" );
	}
}
