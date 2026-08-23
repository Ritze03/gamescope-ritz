// Regression coverage for #65/#75/#76: DrainInputQueue() used to accept
// every raw motion coordinate/delta from wlserver with no sanity check, and
// a spurious out-of-range sample (normalized X == exactly -1.0, or a
// relative delta of -32768.0px) reliably hijacked ImGui's focus mid-drag.
// See SettingsOverlay.h's "Motion-event sanity checks" comment for why the
// predicates live there (header-only) rather than in SettingsOverlay.cpp.
#include <catch2/catch_test_macros.hpp>

#include <limits>

#include "../src/SettingsOverlay.h"

using namespace gamescope;

TEST_CASE( "SettingsOverlay_IsSaneNormalizedCoord accepts real on-surface positions", "[settings_overlay_input]" )
{
	CHECK( SettingsOverlay_IsSaneNormalizedCoord( 0.0 ) );
	CHECK( SettingsOverlay_IsSaneNormalizedCoord( 1.0 ) );
	CHECK( SettingsOverlay_IsSaneNormalizedCoord( 0.5 ) );
	CHECK( SettingsOverlay_IsSaneNormalizedCoord( 0.999999 ) );
	CHECK( SettingsOverlay_IsSaneNormalizedCoord( 0.000001 ) );
	// Small float-rounding slop right at an edge pixel is still real.
	CHECK( SettingsOverlay_IsSaneNormalizedCoord( -0.001 ) );
	CHECK( SettingsOverlay_IsSaneNormalizedCoord( 1.001 ) );
}

TEST_CASE( "SettingsOverlay_IsSaneNormalizedCoord rejects the live #65 garbage value", "[settings_overlay_input]" )
{
	// The concrete value confirmed live in issue #65: normalized X exactly
	// -1.0, one full output-width off.
	CHECK_FALSE( SettingsOverlay_IsSaneNormalizedCoord( -1.0 ) );
	CHECK_FALSE( SettingsOverlay_IsSaneNormalizedCoord( 2.0 ) );
	CHECK_FALSE( SettingsOverlay_IsSaneNormalizedCoord( -0.5 ) );
	CHECK_FALSE( SettingsOverlay_IsSaneNormalizedCoord( std::numeric_limits<double>::quiet_NaN() ) );
	CHECK_FALSE( SettingsOverlay_IsSaneNormalizedCoord( std::numeric_limits<double>::infinity() ) );
	CHECK_FALSE( SettingsOverlay_IsSaneNormalizedCoord( -std::numeric_limits<double>::infinity() ) );
}

TEST_CASE( "SettingsOverlay_IsSaneMotionDelta accepts real relative motion", "[settings_overlay_input]" )
{
	CHECK( SettingsOverlay_IsSaneMotionDelta( 0.0, 0.0 ) );
	CHECK( SettingsOverlay_IsSaneMotionDelta( 12.5, -8.0 ) );
	CHECK( SettingsOverlay_IsSaneMotionDelta( -500.0, 500.0 ) );
	// Comfortably below the sane bound, well above any real single-event
	// delta a physical mouse produces.
	CHECK( SettingsOverlay_IsSaneMotionDelta( 9999.0, -9999.0 ) );
}

TEST_CASE( "SettingsOverlay_IsSaneMotionDelta rejects the live #65 sentinel", "[settings_overlay_input]" )
{
	// dx=dy=-8388608 raw wl_fixed_t == -32768.0px after wl_fixed_to_double().
	CHECK_FALSE( SettingsOverlay_IsSaneMotionDelta( -32768.0, -32768.0 ) );
	CHECK_FALSE( SettingsOverlay_IsSaneMotionDelta( -32768.0, 0.0 ) );
	CHECK_FALSE( SettingsOverlay_IsSaneMotionDelta( 0.0, -32768.0 ) );
	CHECK_FALSE( SettingsOverlay_IsSaneMotionDelta( std::numeric_limits<double>::quiet_NaN(), 0.0 ) );
	CHECK_FALSE( SettingsOverlay_IsSaneMotionDelta( 0.0, std::numeric_limits<double>::infinity() ) );
}
