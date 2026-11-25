// Unit tests for FrameGen/FrameGenV3.h: Motion version 3's four runtime values packed in one 64-bit word
// (superdoc/features/frame-generation-motion-v3.md). No GPU, no compositor.
#include <catch2/catch_test_macros.hpp>
#include <limits>

#include "FrameGen/FrameGenV3.h"

using namespace fghost;

namespace
{
	Config RoundTrip( const Config &in )
	{
		Config out;
		out.v3Smear = out.v3OcclSoft = out.v3Unsure = out.v3SceneCut = -1.0f;   // prove every field is written
		UnpackV3( PackV3( in ), &out );
		return out;
	}

	bool Near( float a, float b ) { return a > b - 0.0001f && a < b + 0.0001f; }
}

TEST_CASE( "framegen v3: the defaults round-trip exactly", "[framegen_v3]" )
{
	const Config d;
	REQUIRE( d.v3Smear == 0.35f );
	REQUIRE( d.v3OcclSoft == 1.0f );
	REQUIRE( d.v3Unsure == 1.0f );
	REQUIRE( d.v3SceneCut == 0.0f );
	const Config r = RoundTrip( d );
	REQUIRE( r.v3Smear == 0.35f );      // exactly: 35 / 100.0
	REQUIRE( r.v3OcclSoft == 1.0f );
	REQUIRE( r.v3Unsure == 1.0f );
	REQUIRE( r.v3SceneCut == 0.0f );
	// The default word is not 0 (EnsureReady() uses 0 as "nothing applied") and uses no reserved bit.
	REQUIRE( PackV3( d ) != 0u );
	REQUIRE( ( PackV3( d ) >> 48 ) == 0u );
}

TEST_CASE( "framegen v3: the extremes round-trip", "[framegen_v3]" )
{
	Config lo; lo.v3Smear = 0.0f; lo.v3OcclSoft = 0.25f; lo.v3Unsure = 0.25f; lo.v3SceneCut = 0.0f;
	Config hi; hi.v3Smear = 1.0f; hi.v3OcclSoft = 4.0f; hi.v3Unsure = 4.0f; hi.v3SceneCut = 4.0f;
	Config r = RoundTrip( lo );
	REQUIRE( ( r.v3Smear == 0.0f && r.v3OcclSoft == 0.25f && r.v3Unsure == 0.25f && r.v3SceneCut == 0.0f ) );
	r = RoundTrip( hi );
	REQUIRE( ( r.v3Smear == 1.0f && r.v3OcclSoft == 4.0f && r.v3Unsure == 4.0f && r.v3SceneCut == 4.0f ) );
	REQUIRE( ( PackV3( hi ) >> 48 ) == 0u );
}

TEST_CASE( "framegen v3: every overlay step round-trips, each field independent of the others", "[framegen_v3]" )
{
	for ( int n = 0; n <= 20; n++ )
	{
		Config c; c.v3Smear = float( n * 5 ) / 100.0f;
		const Config r = RoundTrip( c );
		REQUIRE( Near( r.v3Smear, c.v3Smear ) );
		REQUIRE( ( r.v3OcclSoft == 1.0f && r.v3Unsure == 1.0f && r.v3SceneCut == 0.0f ) );
	}
	for ( int n = 5; n <= 80; n++ )
	{
		Config c; c.v3OcclSoft = float( n * 5 ) / 100.0f;
		REQUIRE( Near( RoundTrip( c ).v3OcclSoft, c.v3OcclSoft ) );
		Config d; d.v3Unsure = float( n * 5 ) / 100.0f;
		const Config r = RoundTrip( d );
		REQUIRE( Near( r.v3Unsure, d.v3Unsure ) );
		REQUIRE( ( r.v3Smear == 0.35f && r.v3OcclSoft == 1.0f && r.v3SceneCut == 0.0f ) );
	}
	for ( int n = 0; n <= 80; n++ )
	{
		Config c; c.v3SceneCut = float( n * 5 ) / 100.0f;
		const Config r = RoundTrip( c );
		REQUIRE( Near( r.v3SceneCut, c.v3SceneCut ) );
		REQUIRE( ( r.v3Smear == 0.35f && r.v3OcclSoft == 1.0f && r.v3Unsure == 1.0f ) );
	}
}

TEST_CASE( "framegen v3: out-of-range values clamp, non-finite falls back to the default", "[framegen_v3]" )
{
	Config c; c.v3Smear = -2.0f; c.v3OcclSoft = 0.0f; c.v3Unsure = 0.1f; c.v3SceneCut = -1.0f;
	Config r = RoundTrip( c );
	REQUIRE( ( r.v3Smear == 0.0f && r.v3OcclSoft == 0.25f && r.v3Unsure == 0.25f && r.v3SceneCut == 0.0f ) );
	c.v3Smear = 9.0f; c.v3OcclSoft = 99.0f; c.v3Unsure = 5.0f; c.v3SceneCut = 4.01f;
	r = RoundTrip( c );
	REQUIRE( ( r.v3Smear == 1.0f && r.v3OcclSoft == 4.0f && r.v3Unsure == 4.0f && r.v3SceneCut == 4.0f ) );

	const float flNaN = std::numeric_limits<float>::quiet_NaN();
	const float flInf = std::numeric_limits<float>::infinity();
	for ( const float f : { flNaN, flInf, -flInf } )
	{
		Config n; n.v3Smear = n.v3OcclSoft = n.v3Unsure = n.v3SceneCut = f;
		r = RoundTrip( n );
		REQUIRE( r.v3Smear == 0.35f );
		REQUIRE( r.v3OcclSoft == 1.0f );
		REQUIRE( r.v3Unsure == 1.0f );
		REQUIRE( r.v3SceneCut == 0.0f );
	}
}

TEST_CASE( "framegen v3: a word with a field out of range unpacks to the defaults", "[framegen_v3]" )
{
	Config r;
	UnpackV3( 0u, &r );   // smear 0 and scene cut 0 are valid; the occlusion softness and unsure sensitivity (min 25) are not
	REQUIRE( ( r.v3Smear == 0.0f && r.v3OcclSoft == 1.0f && r.v3Unsure == 1.0f && r.v3SceneCut == 0.0f ) );
	UnpackV3( 0xFFFFFFFFFFFFull, &r );   // every field 4095 (> every maximum)
	REQUIRE( ( r.v3Smear == 0.35f && r.v3OcclSoft == 1.0f && r.v3Unsure == 1.0f && r.v3SceneCut == 0.0f ) );
	// One field out of range does not take the others with it.
	UnpackV3( ( uint64_t( 4000 ) << 12 ) | ( uint64_t( 150 ) << 24 ) | ( uint64_t( 20 ) << 36 ) | 10u, &r );
	REQUIRE( ( r.v3Smear == 0.1f && r.v3OcclSoft == 1.0f && r.v3Unsure == 1.5f && r.v3SceneCut == 0.2f ) );
}

TEST_CASE( "framegen v3: bits 48-49 are the two switches, bits 50 and above are never written and are ignored", "[framegen_v3]" )
{
	Config c; c.v3Smear = 1.0f; c.v3OcclSoft = 4.0f; c.v3Unsure = 4.0f; c.v3SceneCut = 4.0f;
	REQUIRE( ( PackV3( c ) >> 48 ) == 0u );   // both switches off: nothing above bit 47
	const uint64_t u = PackV3( Config{} );
	Config a, b;
	UnpackV3( u, &a );
	UnpackV3( u | ( 0x3ull << 48 ) | ( 0x3FFFull << 50 ), &b );
	REQUIRE( ( a.v3Smear == b.v3Smear && a.v3OcclSoft == b.v3OcclSoft && a.v3Unsure == b.v3Unsure && a.v3SceneCut == b.v3SceneCut ) );
}

TEST_CASE( "framegen v3: a change of any one value changes the word", "[framegen_v3]" )
{
	const uint64_t u = PackV3( Config{} );
	Config c; c.v3Smear = 0.40f; REQUIRE( PackV3( c ) != u );
	c = Config{}; c.v3OcclSoft = 1.05f; REQUIRE( PackV3( c ) != u );
	c = Config{}; c.v3Unsure = 0.95f; REQUIRE( PackV3( c ) != u );
	c = Config{}; c.v3SceneCut = 0.50f; REQUIRE( PackV3( c ) != u );
}

TEST_CASE( "framegen v3: the two switches take bits 48 and 49, in all four combinations, and leave the four values alone", "[framegen_v3]" )
{
	for ( int i = 0; i < 4; i++ )
	{
		Config c; c.v3Smear = 0.80f; c.v3OcclSoft = 2.55f; c.v3Unsure = 0.25f; c.v3SceneCut = 3.95f;
		c.v3ColourWindow = ( i & 1 ) != 0;
		c.v3DetailSplit = ( i & 2 ) != 0;
		const uint64_t u = PackV3( c );
		REQUIRE( ( u >> 48 ) == uint64_t( i ) );   // bit 48 = colour window, bit 49 = detail split
		Config r;
		UnpackV3( u, &r );
		REQUIRE( r.v3ColourWindow == c.v3ColourWindow );
		REQUIRE( r.v3DetailSplit == c.v3DetailSplit );
		REQUIRE( ( r.v3Smear == 0.80f && r.v3OcclSoft == 2.55f && r.v3Unsure == 0.25f && r.v3SceneCut == 3.95f ) );
		// The low 48 bits are exactly those of the same word with both switches off.
		Config d = c; d.v3ColourWindow = false; d.v3DetailSplit = false;
		REQUIRE( ( u & 0xFFFFFFFFFFFFull ) == PackV3( d ) );
	}
	// A word with bits 48 / 49 clear unpacks to false / false, even into a Config that had them set.
	Config r; r.v3ColourWindow = true; r.v3DetailSplit = true;
	UnpackV3( PackV3( Config{} ), &r );
	REQUIRE( !r.v3ColourWindow );
	REQUIRE( !r.v3DetailSplit );
}

TEST_CASE( "framegen v3: the guard takes bits 50 and 51 (0, 1, 2) and leaves bits 0-49 alone", "[framegen_v3]" )
{
	for ( int i = 0; i < 4; i++ )
	{
		Config c; c.v3Smear = 0.80f; c.v3OcclSoft = 2.55f; c.v3Unsure = 0.25f; c.v3SceneCut = 3.95f;
		c.v3ColourWindow = ( i & 1 ) != 0;
		c.v3DetailSplit = ( i & 2 ) != 0;
		c.v3Guard = 0;
		const uint64_t uOff = PackV3( c );
		REQUIRE( ( uOff >> 50 ) == 0 );
		for ( int g = 1; g <= 2; g++ )
		{
			c.v3Guard = g;
			const uint64_t u = PackV3( c );
			REQUIRE( ( u >> 50 ) == uint64_t( g ) );
			REQUIRE( ( u & ( ( 1ull << 50 ) - 1 ) ) == uOff );   // bits 0-49 bit for bit as at Off
			Config r;
			UnpackV3( u, &r );
			REQUIRE( r.v3Guard == g );
			REQUIRE( r.v3ColourWindow == c.v3ColourWindow );
			REQUIRE( r.v3DetailSplit == c.v3DetailSplit );
			REQUIRE( ( r.v3Smear == 0.80f && r.v3OcclSoft == 2.55f && r.v3Unsure == 0.25f && r.v3SceneCut == 3.95f ) );
		}
	}
	// Off is the raw host struct's default and changes nothing (the setting's default Off reaches the host through
	// PanelFrameGen's ToHostConfig(), its only producer); the guard changes the word (so it reaches setSettings()); a value outside 0..2
	// packs as 0, and the unused value 3 unpacks as 0.
	REQUIRE( Config{}.v3Guard == 0 );
	Config c; c.v3Guard = 1; REQUIRE( PackV3( c ) != PackV3( Config{} ) );
	c.v3Guard = 2; REQUIRE( PackV3( c ) != PackV3( Config{} ) );
	c.v3Guard = 7; REQUIRE( PackV3( c ) == PackV3( Config{} ) );
	c.v3Guard = -1; REQUIRE( PackV3( c ) == PackV3( Config{} ) );
	Config r; r.v3Guard = 2;
	UnpackV3( PackV3( Config{} ) | ( 3ull << 50 ), &r );
	REQUIRE( r.v3Guard == 0 );
	UnpackV3( PackV3( Config{} ), &r );
	REQUIRE( r.v3Guard == 0 );
}

// V3 Hybrid (2026-11-23): the blend takes bits 52-55 (0 = off, 1 / 2 = Fluidity / Consistency) and leaves bits 0-51 alone.
TEST_CASE( "framegen v3: the blend takes bits 52-55 and leaves bits 0-51 alone", "[framegen_v3]" )
{
	Config c; c.v3Smear = 0.80f; c.v3OcclSoft = 2.55f; c.v3Unsure = 0.25f; c.v3SceneCut = 3.95f;
	c.v3ColourWindow = true; c.v3Guard = 2;
	const uint64_t uOff = PackV3( c );
	REQUIRE( ( uOff >> 52 ) == 0 );
	for ( int b = 1; b <= kHybridBlendMax; b++ )
	{
		c.v3Blend = b;
		const uint64_t u = PackV3( c );
		REQUIRE( ( u >> 52 ) == uint64_t( b ) );
		REQUIRE( ( u & ( ( 1ull << 52 ) - 1 ) ) == uOff );
		Config r;
		UnpackV3( u, &r );
		REQUIRE( r.v3Blend == b );
		REQUIRE( r.v3Guard == 2 );
	}
	// The default is off; a value outside 1..kHybridBlendMax (2) packs as 0; a blend change changes the word (so it reaches setSettings()).
	REQUIRE( Config{}.v3Blend == 0 );
	Config d; d.v3Blend = 1; REQUIRE( PackV3( d ) != PackV3( Config{} ) );
	d.v3Blend = 2; REQUIRE( PackV3( d ) != PackV3( Config{} ) );
	d.v3Blend = 3; REQUIRE( PackV3( d ) == PackV3( Config{} ) );
	d.v3Blend = 15; REQUIRE( PackV3( d ) == PackV3( Config{} ) );
	d.v3Blend = 16; REQUIRE( PackV3( d ) == PackV3( Config{} ) );
	d.v3Blend = -1; REQUIRE( PackV3( d ) == PackV3( Config{} ) );
	Config r; r.v3Blend = 3;
	UnpackV3( PackV3( Config{} ), &r );
	REQUIRE( r.v3Blend == 0 );
}

TEST_CASE( "framegen v3: config strings map to Motion plus blend", "[framegen_v3]" )
{
	const auto Is = []( const char *pszMotion, const char *pszVariant, Motion m, int nBlend )
	{
		const MotionChoice c = MotionFromKeys( pszMotion, pszVariant );
		return c.motion == m && c.blend == nBlend;
	};
	// Plain V3 is blend 0 whatever the variant key says.
	REQUIRE( Is( "v3", "fluidity", Motion::V3, 0 ) );
	REQUIRE( Is( "v3", "consistency", Motion::V3, 0 ) );
	// V3 Hybrid is Motion::V3 with blend 1 / 2; an unknown or old variant is Fluidity (config maps the two kept old strings on load).
	REQUIRE( Is( "v3_hybrid", "fluidity", Motion::V3, 1 ) );
	REQUIRE( Is( "v3_hybrid", "consistency", Motion::V3, 2 ) );
	REQUIRE( Is( "v3_hybrid", "", Motion::V3, 1 ) );
	REQUIRE( Is( "v3_hybrid", "x", Motion::V3, 1 ) );
	REQUIRE( Is( "v3_hybrid", "a_c", Motion::V3, 1 ) );
	// Every other version has blend 0.
	REQUIRE( Is( "v1", "b", Motion::V1, 0 ) );
	REQUIRE( Is( "v2", "b", Motion::V2, 0 ) );
	REQUIRE( Is( "learned", "c", Motion::Learned, 0 ) );
	REQUIRE( Is( "nonsense", "c", Motion::V1, 0 ) );
	REQUIRE( std::string( BlendText( 0 ) ).empty() );
	REQUIRE( std::string( BlendText( 1 ) ) == "Fluidity" );
	REQUIRE( std::string( BlendText( 2 ) ) == "Consistency" );
	REQUIRE( std::string( BlendText( 3 ) ).empty() );
}

TEST_CASE( "framegen v3: the status label names the blend honestly", "[framegen_v3]" )
{
	REQUIRE( BlendLabel( 1, true ) == "Fluidity" );
	REQUIRE( BlendLabel( 2, true ) == "Consistency" );
	REQUIRE( BlendLabel( 1, false ) == "Fluidity(off)" );
	REQUIRE( BlendLabel( 2, false ) == "Consistency(off)" );
	// Plain V3 (and anything out of range) says nothing, active or not.
	REQUIRE( BlendLabel( 0, true ).empty() );
	REQUIRE( BlendLabel( 0, false ).empty() );
	REQUIRE( BlendLabel( 3, true ).empty() );
	REQUIRE( BlendLabel( 16, false ).empty() );
}

// The variant table (HybridVariants.h): two values, named, round-tripping.
TEST_CASE( "framegen v3: the variant table names Fluidity = 1 and Consistency = 2", "[framegen_v3]" )
{
	static_assert( kHybridBlendMax == 2 );
	REQUIRE( std::string( HybridLabel( 1 ) ) == "Fluidity" );
	REQUIRE( std::string( HybridLabel( 2 ) ) == "Consistency" );
	REQUIRE( std::string( HybridKey( 1 ) ) == "fluidity" );
	REQUIRE( std::string( HybridKey( 2 ) ) == "consistency" );
	for ( int b : { 0, 3, 15, 16, -1 } )
	{
		REQUIRE( std::string( HybridLabel( b ) ).empty() );
		REQUIRE( std::string( HybridKey( b ) ).empty() );
	}
	REQUIRE( HybridBlendFromKey( "fluidity" ) == 1 );
	REQUIRE( HybridBlendFromKey( "consistency" ) == 2 );
	// the old strings are no longer table keys, nor are unknown, empty or differently cased ones
	for ( const char *psz : { "a_c", "ar4_c", "a", "x", "", "Fluidity" } )
		REQUIRE( HybridBlendFromKey( psz ) == 0 );
}

// The saved profiles of the fifteen-variant period: only a_c and ar4_c carry over, everything else is the default.
TEST_CASE( "framegen v3: old stored variant strings map to Fluidity / Consistency or the default", "[framegen_v3]" )
{
	REQUIRE( HybridKeyFromStored( "a_c" ) == "fluidity" );
	REQUIRE( HybridKeyFromStored( "ar4_c" ) == "consistency" );
	REQUIRE( HybridKeyFromStored( "fluidity" ) == "fluidity" );
	REQUIRE( HybridKeyFromStored( "consistency" ) == "consistency" );
	for ( const char *psz : { "a", "b", "c", "a_a", "a_b", "ar_a", "ar3_b", "ar2_b", "ar4_a", "ar4_b", "", "garbage", "Consistency" } )
		REQUIRE( HybridKeyFromStored( psz ) == "fluidity" );
}

TEST_CASE( "framegen v3: the blend field packs 0..15 with the bound as a parameter", "[framegen_v3]" )
{
	Config c; c.v3Smear = 0.80f; c.v3Guard = 2; c.v3DetailSplit = true;
	const uint64_t uOff = PackV3( c );
	for ( int b = 0; b <= 15; b++ )
	{
		c.v3Blend = b;
		const uint64_t u = PackV3( c, 15 );
		REQUIRE( ( ( u >> 52 ) & 15u ) == uint64_t( b ) );
		REQUIRE( ( u >> 56 ) == 0 );
		REQUIRE( ( u & ( ( 1ull << 52 ) - 1 ) ) == ( uOff & ( ( 1ull << 52 ) - 1 ) ) );
		Config r;
		UnpackV3( u, &r, 15 );
		REQUIRE( r.v3Blend == b );
		REQUIRE( r.v3Guard == 2 );
		// at a lower bound the value beyond it packs as 0, and unpacks as 0
		for ( int nMax : { 2, 6, 12 } )
		{
			Config d; d.v3Blend = b;
			REQUIRE( ( ( PackV3( d, nMax ) >> 52 ) & 15u ) == uint64_t( b <= nMax ? b : 0 ) );
			Config e; e.v3Blend = 3;
			UnpackV3( u, &e, nMax );
			REQUIRE( e.v3Blend == ( b <= nMax ? b : 0 ) );
		}
	}
	c.v3Blend = 16; REQUIRE( PackV3( c, 15 ) == uOff );
	c.v3Blend = -1; REQUIRE( PackV3( c, 15 ) == uOff );
	// the default bound is the table: 1 and 2 are written, everything else is 0
	c.v3Blend = 0; REQUIRE( PackV3( c ) == uOff );
	c.v3Blend = kHybridBlendMax + 1; REQUIRE( PackV3( c ) == uOff );
	for ( int b : { 1, 2 } )
	{
		c.v3Blend = b;
		REQUIRE( ( ( PackV3( c ) >> 52 ) & 15u ) == uint64_t( b ) );
	}
}

TEST_CASE( "framegen v3: MotionFromKeys yields only 1 or 2 for V3 Hybrid", "[framegen_v3]" )
{
	REQUIRE( MotionFromKeys( "v3_hybrid", "fluidity" ).blend == 1 );
	REQUIRE( MotionFromKeys( "v3_hybrid", "consistency" ).blend == 2 );
	REQUIRE( MotionFromKeys( "v3_hybrid", "ar4_c" ).blend == 1 );
	REQUIRE( MotionFromKeys( "v3_hybrid", "d" ).blend == 1 );
}

// V3.5 (2026-11-25): "v3_5" / "v3_5_hybrid" are Motion::V3 plus v3Motion = 1 (and the blend for the Hybrid); every older string has v3Motion 0.
TEST_CASE( "framegen v3: V3.5 config strings map to Motion V3 plus v3Motion 1", "[framegen_v3]" )
{
	const auto Is = []( const char *pszMotion, const char *pszVariant, Motion m, int nBlend, int nV35 )
	{
		const MotionChoice c = MotionFromKeys( pszMotion, pszVariant );
		return c.motion == m && c.blend == nBlend && c.v3Motion == nV35;
	};
	REQUIRE( Is( "v3_5", "fluidity", Motion::V3, 0, 1 ) );
	REQUIRE( Is( "v3_5", "consistency", Motion::V3, 0, 1 ) );
	REQUIRE( Is( "v3_5_hybrid", "fluidity", Motion::V3, 1, 1 ) );
	REQUIRE( Is( "v3_5_hybrid", "consistency", Motion::V3, 2, 1 ) );
	REQUIRE( Is( "v3_5_hybrid", "x", Motion::V3, 1, 1 ) );
	REQUIRE( Is( "v3", "fluidity", Motion::V3, 0, 0 ) );
	REQUIRE( Is( "v3_hybrid", "consistency", Motion::V3, 2, 0 ) );
	REQUIRE( Is( "v1", "", Motion::V1, 0, 0 ) );
	REQUIRE( Is( "v2", "", Motion::V2, 0, 0 ) );
	REQUIRE( Is( "learned", "", Motion::Learned, 0, 0 ) );
	REQUIRE( Is( "nonsense", "", Motion::V1, 0, 0 ) );
}

TEST_CASE( "framegen v3: v3Motion takes bit 56 and leaves bits 0-55 alone", "[framegen_v3]" )
{
	Config c; c.v3Smear = 0.80f; c.v3OcclSoft = 2.55f; c.v3Guard = 2; c.v3Blend = 2; c.v3ColourWindow = true;
	const uint64_t uOff = PackV3( c );
	REQUIRE( ( uOff >> 56 ) == 0 );
	c.v3Motion = 1;
	const uint64_t u = PackV3( c );
	REQUIRE( ( u >> 56 ) == 1 );
	REQUIRE( ( u & ( ( 1ull << 56 ) - 1 ) ) == uOff );
	Config r;
	UnpackV3( u, &r );
	REQUIRE( r.v3Motion == 1 );
	REQUIRE( r.v3Blend == 2 );
	REQUIRE( r.v3Guard == 2 );
	REQUIRE( Config{}.v3Motion == 0 );
	// anything but 1 packs as 0 (the library's kV3MotionMax is 1), so a change reaches setSettings() and nothing else does
	for ( int n : { 2, 3, -1, 255 } )
	{
		c.v3Motion = n;
		REQUIRE( PackV3( c ) == uOff );
	}
	Config e; e.v3Motion = 1;
	UnpackV3( uOff, &e );
	REQUIRE( e.v3Motion == 0 );
}

TEST_CASE( "framegen v3: the V3.5 label is honest", "[framegen_v3]" )
{
	REQUIRE( V35Label( 1, true ) == "V3.5" );
	REQUIRE( V35Label( 1, false ) == "V3.5(off)" );
	REQUIRE( V35Label( 0, true ).empty() );
	REQUIRE( V35Label( 0, false ).empty() );
	REQUIRE( V35Label( 2, true ).empty() );
}
