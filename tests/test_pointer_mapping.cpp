// Requests 2026-09-05 item 10: the absolute pointer under a stretched (4:3
// in 16:9) resolution must land per axis where the composited image puts it.
//
// Pins the pure mapping in src/PointerMapping.h -- the scaler ratios
// (ComputeScalerRatios(), what calc_scale_factor_scaler() wraps), the base
// layer's transform built from them (MappingForBaseLayer(), what
// paint_window_commit() stores and update_touch_scaling() caches), and the
// output -> surface formula wlserver_touchmotion() applies with its inverse.
// The live re-sync on a mapping change (wlserver_resync_absolute_pointer())
// needs a compositor and is the laptop check's job.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "../src/PointerMapping.h"

using namespace gamescope;
using Catch::Matchers::WithinAbs;

namespace
{
	constexpr float kMaxScale = 3.4028235e38f; // g_flMaxWindowScale's FLT_MAX default

	// A 1280x960 game (nested = game, the launch-time -w/-h case) in a
	// 1920x1080 window.
	constexpr int kOutW = 1920, kOutH = 1080;
	constexpr int kGameW = 1280, kGameH = 960;
}

TEST_CASE( "stretch scales each axis on its own", "[pointer_mapping]" )
{
	const ScalerRatios r = ComputeScalerRatios( GamescopeUpscaleScaler::STRETCH,
		kOutW, kOutH, kGameW, kGameH, kGameW, kGameH, kMaxScale );

	CHECK_THAT( r.x, WithinAbs( 1920.0 / 1280.0, 1e-6 ) ); // 1.5
	CHECK_THAT( r.y, WithinAbs( 1080.0 / 960.0, 1e-6 ) );  // 1.125
	CHECK( r.x != r.y );
}

TEST_CASE( "stretch does not depend on the nested size", "[pointer_mapping]" )
{
	// The runtime case: the Display > Resolution area changed the nested
	// size, the game window followed. Whatever g_nNestedWidth/Height say,
	// stretch is output/source per axis -- the nested terms cancel.
	const ScalerRatios a = ComputeScalerRatios( GamescopeUpscaleScaler::STRETCH,
		kOutW, kOutH, kGameW, kGameH, kGameW, kGameH, kMaxScale );
	const ScalerRatios b = ComputeScalerRatios( GamescopeUpscaleScaler::STRETCH,
		kOutW, kOutH, 1920, 1080, kGameW, kGameH, kMaxScale );

	CHECK_THAT( a.x, WithinAbs( b.x, 1e-6 ) );
	CHECK_THAT( a.y, WithinAbs( b.y, 1e-6 ) );
}

TEST_CASE( "auto keeps the aspect and pillarboxes a 4:3 game", "[pointer_mapping]" )
{
	const ScalerRatios r = ComputeScalerRatios( GamescopeUpscaleScaler::AUTO,
		kOutW, kOutH, kGameW, kGameH, kGameW, kGameH, kMaxScale );

	// Height-limited: 1080/960.
	CHECK_THAT( r.x, WithinAbs( 1.125, 1e-6 ) );
	CHECK_THAT( r.y, WithinAbs( 1.125, 1e-6 ) );

	const AbsolutePointerMapping m = MappingForBaseLayer( r, kOutW, kOutH, kGameW, kGameH );
	// 1280 * 1.125 = 1440 wide, centred: 240 px bars each side, no vertical bar.
	CHECK_THAT( m.flOffsetX, WithinAbs( -240.0, 1e-6 ) );
	CHECK_THAT( m.flOffsetY, WithinAbs( 0.0, 1e-6 ) );
}

TEST_CASE( "auto does depend on the nested size, unlike stretch", "[pointer_mapping]" )
{
	// Why steamcompmgr_set_nested_mode() must write g_nNestedWidth/Height
	// (the Deck's atom path does not): the letterboxed scalers read it. It
	// only shows when the nested aspect differs from the output's -- a 4:3
	// nested mode in a 16:9 window, the case this item is about -- so take
	// a 1920x1080 window that has not (yet) followed a 1280x960 mode change.
	const ScalerRatios followed = ComputeScalerRatios( GamescopeUpscaleScaler::AUTO,
		kOutW, kOutH, kGameW, kGameH, 1920, 1080, kMaxScale );
	const ScalerRatios stale = ComputeScalerRatios( GamescopeUpscaleScaler::AUTO,
		kOutW, kOutH, 1920, 1080, 1920, 1080, kMaxScale );

	// min(1280/1920, 960/1080) * min(1920/1280, 1080/960) = 0.667 * 1.125.
	CHECK_THAT( followed.x, WithinAbs( 0.75, 1e-6 ) );
	CHECK_THAT( stale.x, WithinAbs( 1.0, 1e-6 ) );
	CHECK( followed.x != stale.x );
}

TEST_CASE( "output to surface under stretch, per axis", "[pointer_mapping]" )
{
	const ScalerRatios r = ComputeScalerRatios( GamescopeUpscaleScaler::STRETCH,
		kOutW, kOutH, kGameW, kGameH, kGameW, kGameH, kMaxScale );
	const AbsolutePointerMapping m = MappingForBaseLayer( r, kOutW, kOutH, kGameW, kGameH );

	// Stretch fills the output: no centring offset.
	CHECK_THAT( m.flOffsetX, WithinAbs( 0.0, 1e-6 ) );
	CHECK_THAT( m.flOffsetY, WithinAbs( 0.0, 1e-6 ) );

	double sx, sy;

	// Corners map to corners.
	OutputToSurface( m, 0, 0, &sx, &sy );
	CHECK_THAT( sx, WithinAbs( 0.0, 1e-6 ) );
	CHECK_THAT( sy, WithinAbs( 0.0, 1e-6 ) );

	OutputToSurface( m, kOutW, kOutH, &sx, &sy );
	CHECK_THAT( sx, WithinAbs( kGameW, 1e-6 ) );
	CHECK_THAT( sy, WithinAbs( kGameH, 1e-6 ) );

	// The centre maps to the centre.
	OutputToSurface( m, 960, 540, &sx, &sy );
	CHECK_THAT( sx, WithinAbs( 640.0, 1e-6 ) );
	CHECK_THAT( sy, WithinAbs( 480.0, 1e-6 ) );

	// An off-centre point gets a different divisor per axis: this is the
	// per-axis property a single "stretched" scalar would get wrong.
	OutputToSurface( m, 1440, 270, &sx, &sy );
	CHECK_THAT( sx, WithinAbs( 1440.0 / 1.5, 1e-6 ) );   // 960
	CHECK_THAT( sy, WithinAbs( 270.0 / 1.125, 1e-6 ) );  // 240
}

TEST_CASE( "output to surface under auto skips the bars", "[pointer_mapping]" )
{
	const ScalerRatios r = ComputeScalerRatios( GamescopeUpscaleScaler::AUTO,
		kOutW, kOutH, kGameW, kGameH, kGameW, kGameH, kMaxScale );
	const AbsolutePointerMapping m = MappingForBaseLayer( r, kOutW, kOutH, kGameW, kGameH );

	double sx, sy;

	// The left edge of the game image is 240 px into the output.
	OutputToSurface( m, 240, 0, &sx, &sy );
	CHECK_THAT( sx, WithinAbs( 0.0, 1e-6 ) );
	CHECK_THAT( sy, WithinAbs( 0.0, 1e-6 ) );

	// A point in the left bar maps outside the game (negative); the caller
	// clamps to the surface bounds, the mapping itself does not.
	OutputToSurface( m, 0, 0, &sx, &sy );
	CHECK( sx < 0.0 );

	OutputToSurface( m, 960, 540, &sx, &sy );
	CHECK_THAT( sx, WithinAbs( 640.0, 1e-6 ) );
	CHECK_THAT( sy, WithinAbs( 480.0, 1e-6 ) );
}

TEST_CASE( "surface to output inverts output to surface", "[pointer_mapping]" )
{
	for ( GamescopeUpscaleScaler eScaler : { GamescopeUpscaleScaler::STRETCH, GamescopeUpscaleScaler::AUTO,
											 GamescopeUpscaleScaler::FIT, GamescopeUpscaleScaler::FILL } )
	{
		const ScalerRatios r = ComputeScalerRatios( eScaler, kOutW, kOutH, kGameW, kGameH, kGameW, kGameH, kMaxScale );
		const AbsolutePointerMapping m = MappingForBaseLayer( r, kOutW, kOutH, kGameW, kGameH );

		for ( double x : { 0.0, 17.0, 640.0, 1279.9 } )
		{
			for ( double y : { 0.0, 3.0, 480.0, 959.9 } )
			{
				double ox, oy, sx, sy;
				SurfaceToOutput( m, x, y, &ox, &oy );
				OutputToSurface( m, ox, oy, &sx, &sy );
				CHECK_THAT( sx, WithinAbs( x, 1e-6 ) );
				CHECK_THAT( sy, WithinAbs( y, 1e-6 ) );
			}
		}
	}
}

TEST_CASE( "a runtime mode change moves the mapping under a stationary pointer", "[pointer_mapping]" )
{
	// Launched at 1920x1080 in a 1920x1080 window (identity), then the
	// Display > Resolution area picks 1280x960 under stretch. The same host
	// pointer sample must now land somewhere else in game space -- that is
	// the delta wlserver_resync_absolute_pointer() replays, and what the
	// client would have missed until its next real motion event.
	const ScalerRatios before = ComputeScalerRatios( GamescopeUpscaleScaler::STRETCH,
		kOutW, kOutH, 1920, 1080, 1920, 1080, kMaxScale );
	const ScalerRatios after = ComputeScalerRatios( GamescopeUpscaleScaler::STRETCH,
		kOutW, kOutH, kGameW, kGameH, kGameW, kGameH, kMaxScale );

	const AbsolutePointerMapping mBefore = MappingForBaseLayer( before, kOutW, kOutH, 1920, 1080 );
	const AbsolutePointerMapping mAfter = MappingForBaseLayer( after, kOutW, kOutH, kGameW, kGameH );

	CHECK( mBefore != mAfter );

	double bx, by, ax, ay;
	OutputToSurface( mBefore, 1440, 810, &bx, &by );
	OutputToSurface( mAfter, 1440, 810, &ax, &ay );

	CHECK_THAT( bx, WithinAbs( 1440.0, 1e-6 ) );
	CHECK_THAT( by, WithinAbs( 810.0, 1e-6 ) );
	CHECK_THAT( ax, WithinAbs( 960.0, 1e-6 ) );
	CHECK_THAT( ay, WithinAbs( 720.0, 1e-6 ) );
}

TEST_CASE( "integer scaler floors a scale above one", "[pointer_mapping]" )
{
	// 640x480 in 1920x1080, nested = game: min(3, 2.25) = 2.25 -> 2.
	const ScalerRatios r = ComputeScalerRatios( GamescopeUpscaleScaler::INTEGER,
		kOutW, kOutH, 640, 480, 640, 480, kMaxScale );
	CHECK_THAT( r.x, WithinAbs( 2.0, 1e-6 ) );
	CHECK_THAT( r.y, WithinAbs( 2.0, 1e-6 ) );
}

// 2026-09-06, the CS2 "mouse look drifts back to centre" fix: the re-sync
// must fire at most once per REAL mapping change, never on a no-op. The
// live gate is update_touch_scaling()'s compare of the cached four floats
// against the freshly painted layer; this pins the comparison itself, and
// the two degenerate cases scripts/pointer-regression.sh had to steer
// around so that its "exactly one" assertion means something.
TEST_CASE( "the mapping compares equal to itself and different after a real change", "[pointer_mapping]" )
{
	// A windowed 640x480 client in a 1280x720 output, as the script runs.
	constexpr int kWinW = 640, kWinH = 480;

	const AbsolutePointerMapping autoNow = MappingForBaseLayer(
		ComputeScalerRatios( GamescopeUpscaleScaler::AUTO, kOutW, kOutH, kOutW, kOutH, kWinW, kWinH, kMaxScale ),
		kOutW, kOutH, kWinW, kWinH );
	const AbsolutePointerMapping autoAgain = MappingForBaseLayer(
		ComputeScalerRatios( GamescopeUpscaleScaler::AUTO, kOutW, kOutH, kOutW, kOutH, kWinW, kWinH, kMaxScale ),
		kOutW, kOutH, kWinW, kWinH );
	const AbsolutePointerMapping stretch = MappingForBaseLayer(
		ComputeScalerRatios( GamescopeUpscaleScaler::STRETCH, kOutW, kOutH, kOutW, kOutH, kWinW, kWinH, kMaxScale ),
		kOutW, kOutH, kWinW, kWinH );

	// Same inputs, same frame after frame: no change, no re-sync.
	CHECK( autoNow == autoAgain );
	// Auto -> Stretch on a 4:3 window in a 16:9 output: a real change.
	CHECK( autoNow != stretch );

	// Degenerate case 1: under Auto, a nested mode that shares the window's
	// aspect gives the same min(out/src) ratio -- the mapping does NOT move,
	// so a re-sync count of 0 there is correct, not a regression.
	const AbsolutePointerMapping sameAspectMode = MappingForBaseLayer(
		ComputeScalerRatios( GamescopeUpscaleScaler::AUTO, kOutW, kOutH, 960, 720, kWinW, kWinH, kMaxScale ),
		kOutW, kOutH, kWinW, kWinH );
	CHECK( autoNow == sameAspectMode );
	// ...whereas a 5:4 nested mode does move it (1280x1024 -> ratio 1.406).
	const AbsolutePointerMapping otherAspectMode = MappingForBaseLayer(
		ComputeScalerRatios( GamescopeUpscaleScaler::AUTO, kOutW, kOutH, 1280, 1024, kWinW, kWinH, kMaxScale ),
		kOutW, kOutH, kWinW, kWinH );
	CHECK( autoNow != otherAspectMode );

	// Degenerate case 2: the output centre lands on the window centre under
	// every scaler, so a re-sync from a centred sample moves nothing and is
	// (correctly) skipped -- the script samples off-centre for that reason.
	double ax, ay, sx, sy;
	OutputToSurface( autoNow, kOutW / 2.0, kOutH / 2.0, &ax, &ay );
	OutputToSurface( stretch, kOutW / 2.0, kOutH / 2.0, &sx, &sy );
	CHECK_THAT( ax, WithinAbs( sx, 1e-6 ) );
	CHECK_THAT( ay, WithinAbs( sy, 1e-6 ) );
	OutputToSurface( autoNow, kOutW / 4.0, kOutH / 4.0, &ax, &ay );
	OutputToSurface( stretch, kOutW / 4.0, kOutH / 4.0, &sx, &sy );
	CHECK( ax != sx );
}
