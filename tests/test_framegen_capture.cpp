// Unit tests for FrameGen/FrameGenCapture.{h,cpp}: the parts of `ritz_fg_capture` that need no GPU
// (superdoc/features/frame-generation-capture.md): the decision ring, the arrival table, the argument
// parsing, the free-space arithmetic, the text formats and the tight-row writer.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "FrameGen/FrameGenCapture.h"
#include "../subprojects/FrameGen/gpu/framegen.h"

using namespace fghost::capture;

namespace
{
	std::vector<std::string_view> Args( std::initializer_list<const char *> l )
	{
		std::vector<std::string_view> v;
		for ( const char *p : l )
			v.emplace_back( p );
		return v;
	}

	// Splits a line on single spaces into "key=value" tokens (the grammar a replay tool relies on).
	std::vector<std::string> Tokens( const std::string &s )
	{
		std::vector<std::string> v;
		std::istringstream in( s );
		std::string t;
		while ( in >> t )
			v.push_back( t );
		return v;
	}

	DecisionRec Rec( uint64_t ulOut )
	{
		DecisionRec r;
		r.ns = ulOut * 1000;
		r.outId = ulOut;
		return r;
	}
}

TEST_CASE( "capture: the decision ring keeps the newest records, oldest first, across the wrap", "[framegen_capture]" )
{
	DecisionRing ring;
	REQUIRE( ring.Snapshot( 10 ).empty() );

	for ( uint64_t i = 1; i <= 5; i++ )
		ring.Push( Rec( i ) );
	auto v = ring.Snapshot( 200 );
	REQUIRE( v.size() == 5 );
	REQUIRE( v.front().outId == 1 );
	REQUIRE( v.back().outId == 5 );
	REQUIRE( v.front().counter == 1 );

	// Past the ring's size: only the newest kDecisionRing are left; a snapshot of 200 is the last 200.
	for ( uint64_t i = 6; i <= uint64_t( kDecisionRing ) * 2 + 7; i++ )
		ring.Push( Rec( i ) );
	const uint64_t ulLast = uint64_t( kDecisionRing ) * 2 + 7;
	REQUIRE( ring.Count() == ulLast );
	v = ring.Snapshot( kDecisionsDumped );
	REQUIRE( v.size() == size_t( kDecisionsDumped ) );
	REQUIRE( v.back().outId == ulLast );
	REQUIRE( v.back().counter == ulLast );
	REQUIRE( v.front().outId == ulLast - kDecisionsDumped + 1 );
	for ( size_t i = 1; i < v.size(); i++ )
		REQUIRE( v[ i ].outId == v[ i - 1 ].outId + 1 );   // contiguous, oldest first

	// Asking for more than the ring holds gives the whole ring.
	v = ring.Snapshot( 100000 );
	REQUIRE( v.size() == size_t( kDecisionRing ) );
	REQUIRE( v.front().outId == ulLast - kDecisionRing + 1 );
}

TEST_CASE( "capture: the arrival table finds the last ids and forgets older ones", "[framegen_capture]" )
{
	ArrivalTable t;
	REQUIRE( t.Find( 7 ) == -1 );
	REQUIRE( t.Find( 0 ) == -1 );
	for ( uint64_t i = 1; i <= uint64_t( kArrivalSlots ); i++ )
		t.Note( i, i * 100 );
	REQUIRE( t.Find( 1 ) == 100 );
	REQUIRE( t.Find( kArrivalSlots ) == int64_t( kArrivalSlots ) * 100 );
	t.Note( 99, 5 );                 // overwrites the oldest (id 1)
	REQUIRE( t.Find( 1 ) == -1 );
	REQUIRE( t.Find( 2 ) == 200 );
	REQUIRE( t.Find( 99 ) == 5 );
	t.Note( 99, 6 );                 // the same id again updates in place, evicts nothing
	REQUIRE( t.Find( 99 ) == 6 );
	REQUIRE( t.Find( 2 ) == 200 );
}

TEST_CASE( "capture: the argument parsing", "[framegen_capture]" )
{
	auto a = Args( { "ritz_fg_capture" } );
	ArgResult r = ParseArgs( a );
	REQUIRE( r.ok );
	REQUIRE( r.seconds == 0 );

	a = Args( { "ritz_fg_capture", "3" } );
	r = ParseArgs( a );
	REQUIRE( r.ok );
	REQUIRE( r.seconds == 3 );

	a = Args( { "ritz_fg_capture", "0" } );
	REQUIRE( ParseArgs( a ).ok );
	a = Args( { "ritz_fg_capture", "600" } );
	r = ParseArgs( a );
	REQUIRE( r.ok );
	REQUIRE( r.seconds == 600 );

	for ( const char *pszBad : { "601", "-1", "abc", "3s", "", "1.5", "+3", "9999999" } )
	{
		a = Args( { "ritz_fg_capture", pszBad } );
		r = ParseArgs( a );
		INFO( pszBad );
		REQUIRE_FALSE( r.ok );
		REQUIRE_FALSE( r.error.empty() );
	}
	a = Args( { "ritz_fg_capture", "3", "4" } );
	REQUIRE_FALSE( ParseArgs( a ).ok );
}

TEST_CASE( "capture: the free-space arithmetic", "[framegen_capture]" )
{
	// (N + N + 2) images of w x h x bpp, plus 64 MiB.
	const uint64_t ulImage = 1440ull * 1080ull * 4ull;
	REQUIRE( RequiredBytes( 1440, 1080, 4, 40 ) == 82ull * ulImage + 64ull * 1024 * 1024 );
	REQUIRE( RequiredBytes( 1440, 1080, 8, 40 ) == 82ull * ulImage * 2 + 64ull * 1024 * 1024 );
	REQUIRE( RequiredBytes( 0, 0, 4, 40 ) == kReserveBytes );
	// No overflow at an absurd size (64-bit math).
	REQUIRE( RequiredBytes( 16384, 16384, 8, 40 ) > RequiredBytes( 8192, 8192, 8, 40 ) );

	const uint64_t ulNeed = RequiredBytes( 1440, 1080, 4, 40 );
	REQUIRE( HasEnoughSpace( ulNeed, ulNeed ) );
	REQUIRE_FALSE( HasEnoughSpace( ulNeed - 1, ulNeed ) );
	REQUIRE( HasEnoughSpace( ulNeed + 1, ulNeed ) );
}

TEST_CASE( "capture: the format names and the file names", "[framegen_capture]" )
{
	REQUIRE( std::string( VkFormatName( 44 ) ) == "B8G8R8A8_UNORM" );
	REQUIRE( VkFormatBytes( 44 ) == 4 );
	REQUIRE( VkFormatBytes( 97 ) == 8 );
	REQUIRE( VkFormatBytes( 123456 ) == 0 );
	REQUIRE( OutFileName( 0 ) == "000_out.bgra" );
	REQUIRE( OutFileName( 39 ) == "039_out.bgra" );
	REQUIRE( RealFileName( 18446744073709551615ull ) == "id18446744073709551615_real.bgra" );
}

TEST_CASE( "capture: the header line is key=value tokens in one line", "[framegen_capture]" )
{
	HeaderInfo h;
	h.sWallStart = "2026-11-21T12:34:56+0100";
	h.ulStartNs = 123456789;
	h.uWidth = 1440;
	h.uHeight = 1080;
	h.nVkFormat = 44;
	h.uBytesPerPixel = 4;
	h.sLibraryCommit = "275fef0";
	h.sRitzVersion = "2026-11-20";
	h.sRitzCommit = "abc1234";
	h.sMotionWanted = "V3";
	h.sMotionEffective = "V1";
	h.uFlowScale = 2;
	const std::string s = FormatHeaderLine( h );
	REQUIRE( s.find( '\n' ) == std::string::npos );
	const auto t = Tokens( s );
	REQUIRE( t[ 0 ] == "capture" );
	REQUIRE( t[ 1 ] == "v=1" );
	for ( size_t i = 1; i < t.size(); i++ )
		REQUIRE( t[ i ].find( '=' ) != std::string::npos );
	REQUIRE( s.find( " width=1440 height=1080 vk_format=44 format_name=B8G8R8A8_UNORM bytes_per_pixel=4 " ) != std::string::npos );
	REQUIRE( s.find( "library_commit=275fef0" ) != std::string::npos );
	REQUIRE( s.find( "motion_wanted=V3 motion_effective=V1 fallback_v1=1 flowScale=2 n=40" ) != std::string::npos );

	// An empty value stays a token; a space inside one cannot split it.
	h.sLibraryCommit = "";
	h.sRitzVersion = "a b";
	const std::string s2 = FormatHeaderLine( h );
	REQUIRE( s2.find( "library_commit=unknown" ) != std::string::npos );
	REQUIRE( s2.find( "ritz_version=a_b" ) != std::string::npos );
}

TEST_CASE( "capture: one setting line per field of framegen::Settings", "[framegen_capture]" )
{
	const framegen::Settings s;
	const auto v = FormatSettingLines( s );
	REQUIRE( v.size() == 45 );   // every field (the static_assert in FrameGenCapture.cpp guards the struct's size)
	std::set<std::string> names;
	for ( const std::string &l : v )
	{
		REQUIRE( l.rfind( "setting ", 0 ) == 0 );
		const auto t = Tokens( l );
		REQUIRE( t.size() >= 2 );
		const size_t nEq = t[ 1 ].find( '=' );
		REQUIRE( nEq != std::string::npos );
		names.insert( t[ 1 ].substr( 0, nEq ) );
	}
	REQUIRE( names.size() == 45 );   // all distinct
	for ( const char *pszName : { "trustLow", "searchPenalty", "uiProtection", "blurWeights", "motionVersion", "v2Mode", "v2StaticMask",
		"v3Smear", "v3Guard", "v3Blend", "learnedNet", "learnedStaticGuard", "flowScale", "pyramidMinSide", "uiBoxOffsetX" } )
		REQUIRE( names.count( pszName ) == 1 );

	// Values and enum texts.
	framegen::Settings m;
	m.motionVersion = framegen::MotionVersion::V3;
	m.v3Guard = framegen::V3Guard::V2Motion;
	m.v3Blend = 3;
	m.uiBoxOffsetX = -5;
	m.v3Smear = 0.4f;
	m.subpixel = false;
	const auto w = FormatSettingLines( m );
	auto Has = [ & ]( const char *pszLine ) { return std::find( w.begin(), w.end(), std::string( pszLine ) ) != w.end(); };
	REQUIRE( Has( "setting motionVersion=3 text=V3" ) );
	REQUIRE( Has( "setting v3Guard=2 text=V2Motion" ) );
	REQUIRE( Has( "setting v3Blend=3" ) );
	REQUIRE( Has( "setting uiBoxOffsetX=-5" ) );
	REQUIRE( Has( "setting subpixel=0" ) );
	REQUIRE( Has( "setting v3Smear=0.400000006" ) );   // %.9g of the float: exact round trip
}

TEST_CASE( "capture: the decision line carries every field the pacer gives", "[framegen_capture]" )
{
	DecisionRec r;
	r.counter = 17;
	r.ns = 5000000000ull;
	r.newestId = 10;
	r.prevId = 9;
	r.currId = 10;
	r.outId = 77;
	r.showId = 10;
	r.t = 0.5f;
	r.t0 = 0.5f;
	r.t1 = 0.5f;
	r.newOutput = true;
	r.reason = 4;
	REQUIRE( DecisionGenerates( r ) );
	const std::string s = FormatDecisionLine( r );
	REQUIRE( s.rfind( "decision n=17 ns=5000000000 newestId=10 prevId=9 currId=10 outId=77 showId=10 t=0.5 t0=0.5 t1=0.5 ", 0 ) == 0 );
	REQUIRE( s.find( "inert=0 skip=0 reset=0 repaintNext=0 newOutput=1 discontinuity=0 generate=1 reason=4 reason_text=GameTooFast" ) != std::string::npos );

	r.t = 1.0f;   // a real frame: pass
	REQUIRE_FALSE( DecisionGenerates( r ) );
	r.t = 0.5f;
	r.inert = true;
	REQUIRE_FALSE( DecisionGenerates( r ) );
	r.inert = false;
	r.prevId = 0;
	REQUIRE_FALSE( DecisionGenerates( r ) );
	REQUIRE( std::string( ReasonName( 200 ) ) == "Unknown" );
}

TEST_CASE( "capture: the frame line and the end line", "[framegen_capture]" )
{
	FrameInfo f;
	f.nSeq = 3;
	f.ulOutId = 100;
	f.ulPrevId = 8;
	f.ulCurrId = 9;
	f.ulNewestId = 9;
	f.flT0 = 0.25f;
	f.flT1 = 0.25f;
	f.nArrivalPrevNs = 1000;
	f.nArrivalCurrNs = -1;
	f.ulNowNs = 2000;
	f.uWidth = 1440;
	f.uHeight = 1080;
	f.bEstimateRecorded = true;
	f.nEstimateRet = 1;
	f.nSynthRet = 1;
	f.nOutSlot = 2;
	f.nPrevSlot = 0;
	f.nCurrSlot = 1;
	f.sOutFile = OutFileName( 3 );
	f.sPrevFile = RealFileName( 8 );
	f.sCurrFile = RealFileName( 9 );
	const std::string s = FormatFrameLine( f );
	REQUIRE( s == "frame seq=3 outId=100 prevId=8 currId=9 newestId=9 t0=0.25 t1=0.25 arrival_prev_ns=1000 arrival_curr_ns=-1 now_ns=2000 "
		"width=1440 height=1080 estimate=recorded estimate_ret=1 synth_ret=1 cached=0 near=0 out_slot=2 prev_slot=0 curr_slot=1 "
		"out_file=003_out.bgra prev_file=id8_real.bgra curr_file=id9_real.bgra" );

	f.bEstimateRecorded = false;
	f.bCached = true;
	const std::string s2 = FormatFrameLine( f );
	REQUIRE( s2.find( "estimate=reused estimate_ret=na " ) != std::string::npos );
	REQUIRE( s2.find( " cached=1 " ) != std::string::npos );

	REQUIRE( FormatEndLine( 40, 21, 0, "complete" ) == "end frames=40 reals=21 dropped=0 reason=complete" );
	REQUIRE( FormatEndLine( 3, 2, 1, "disk full" ) == "end frames=3 reals=2 dropped=1 reason=disk_full" );
}

TEST_CASE( "capture: rows are written tightly packed from a padded pitch", "[framegen_capture]" )
{
	// 3 pixels x 4 bytes = 12 bytes per row, rows 16 bytes apart (4 bytes of padding that must not appear).
	uint8_t src[ 16 * 3 ];
	for ( int y = 0; y < 3; y++ )
	{
		for ( int x = 0; x < 16; x++ )
			src[ y * 16 + x ] = x < 12 ? uint8_t( y * 10 + x ) : 0xEE;
	}
	char *pBuf = nullptr;
	size_t nSize = 0;
	FILE *f = open_memstream( &pBuf, &nSize );
	REQUIRE( f );
	REQUIRE( WriteTightRows( f, src, 16, 12, 3 ) );
	fclose( f );
	REQUIRE( nSize == 36 );
	for ( int y = 0; y < 3; y++ )
	{
		for ( int x = 0; x < 12; x++ )
			REQUIRE( uint8_t( pBuf[ y * 12 + x ] ) == uint8_t( y * 10 + x ) );
	}
	free( pBuf );

	// A pitch smaller than a row is refused, not read past.
	f = open_memstream( &pBuf, &nSize );
	REQUIRE_FALSE( WriteTightRows( f, src, 8, 12, 3 ) );
	fclose( f );
	free( pBuf );
}

TEST_CASE( "capture: the timing line and the largest decision gap", "[framegen_capture]" )
{
	std::vector<DecisionRec> v;
	uint64_t ulNs = 1000;
	for ( uint64_t i = 1; i <= 5; i++ )
	{
		DecisionRec r;
		r.counter = i;
		ulNs += ( i == 4 ? 150000000ull : 4000000ull );   // a 150 ms stall before decision 4
		r.ns = ulNs;
		v.push_back( r );
	}
	uint64_t ulAfter = 0;
	REQUIRE( MaxDecisionGapNs( v, &ulAfter ) == 150000000ull );
	REQUIRE( ulAfter == 4 );
	REQUIRE( MaxDecisionGapNs( {}, &ulAfter ) == 0 );
	REQUIRE( ulAfter == 0 );
	REQUIRE( FormatTimingLine( 100, 400, 7, 3, 9 ) ==
		"timing first_frame_ns=100 last_frame_ns=400 burst_ns=300 max_decision_gap_ns=7 gap_after_n=3 last_decision_to_start_ns=9" );
	REQUIRE( FormatTimingLine( 0, 0, 0, 0, 0 ).find( "burst_ns=0 " ) != std::string::npos );
}

TEST_CASE( "capture: the header carries the preparation time", "[framegen_capture]" )
{
	HeaderInfo h;
	h.flPrepareMs = 12.5f;
	h.bPreparedEarly = true;
	REQUIRE( FormatHeaderLine( h ).find( " n=40 prepare_ms=12.500 prepared_early=1" ) != std::string::npos );
	h.bPreparedEarly = false;
	REQUIRE( FormatHeaderLine( h ).find( "prepared_early=0" ) != std::string::npos );
}

// ---------------------------------------------------------------------------
//  Watch mode
// ---------------------------------------------------------------------------

TEST_CASE( "capture watch: the argument parsing (and the old forms unchanged)", "[framegen_capture]" )
{
	auto a = Args( { "ritz_fg_capture", "watch" } );
	ArgResult r = ParseArgs( a );
	REQUIRE( r.ok );
	REQUIRE( r.watch );
	REQUIRE( r.seconds == 60 );
	REQUIRE( r.sensitivity == 1.0f );

	a = Args( { "ritz_fg_capture", "watch", "30" } );
	r = ParseArgs( a );
	REQUIRE( r.ok );
	REQUIRE( r.seconds == 30 );
	REQUIRE( r.sensitivity == 1.0f );

	a = Args( { "ritz_fg_capture", "watch", "30", "0" } );
	r = ParseArgs( a );
	REQUIRE( r.ok );
	REQUIRE( r.sensitivity == 0.0f );

	a = Args( { "ritz_fg_capture", "watch", "600", "2.5" } );
	r = ParseArgs( a );
	REQUIRE( r.ok );
	REQUIRE( r.seconds == 600 );
	REQUIRE( r.sensitivity == 2.5f );

	for ( auto l : { Args( { "ritz_fg_capture", "watch", "0" } ), Args( { "ritz_fg_capture", "watch", "601" } ),
		Args( { "ritz_fg_capture", "watch", "abc" } ), Args( { "ritz_fg_capture", "watch", "10", "-1" } ),
		Args( { "ritz_fg_capture", "watch", "10", "x" } ), Args( { "ritz_fg_capture", "watch", "10", "1.2.3" } ),
		Args( { "ritz_fg_capture", "watch", "10", "1", "2" } ), Args( { "ritz_fg_capture", "watch", "1.5" } ),
		Args( { "ritz_fg_capture", "Watch" } ) } )
	{
		REQUIRE_FALSE( ParseArgs( l ).ok );
		REQUIRE_FALSE( ParseArgs( l ).error.empty() );
	}

	// The old forms.
	a = Args( { "ritz_fg_capture" } );
	r = ParseArgs( a );
	REQUIRE( r.ok );
	REQUIRE_FALSE( r.watch );
	REQUIRE( r.seconds == 0 );
	a = Args( { "ritz_fg_capture", "5" } );
	r = ParseArgs( a );
	REQUIRE( r.ok );
	REQUIRE_FALSE( r.watch );
	REQUIRE( r.seconds == 5 );
}

namespace
{
	// A w x h BGRA image filled by f( x, y, c ).
	template <typename F>
	std::vector<uint8_t> Img( uint32_t w, uint32_t h, uint32_t uPitch, F f )
	{
		std::vector<uint8_t> v( size_t( uPitch ) * h, 0 );
		for ( uint32_t y = 0; y < h; y++ )
			for ( uint32_t x = 0; x < w; x++ )
				for ( uint32_t c = 0; c < 4; c++ )
					v[ size_t( y ) * uPitch + x * 4 + c ] = uint8_t( f( x, y, c ) );
		return v;
	}
	BlockMeans Means( const std::vector<uint8_t> &v, uint32_t w, uint32_t h, uint32_t uPitch, uint32_t uStep = kMetricStep )
	{
		BlockMeans m;
		REQUIRE( ComputeBlockMeans( v.data(), uPitch, w, h, 44, uStep, &m ) );
		return m;
	}
	uint32_t Hash( uint32_t x )
	{
		x ^= x >> 16; x *= 0x7feb352dU; x ^= x >> 15; x *= 0x846ca68bU; x ^= x >> 16;
		return x;
	}
}

TEST_CASE( "capture watch: the coarse-error metric on synthetic frames", "[framegen_capture]" )
{
	const uint32_t w = 160, h = 100, p = w * 4 + 32;   // a padded pitch; 10 x 6 whole blocks (the last 4 rows ignored)
	auto prev = Img( w, h, p, []( uint32_t x, uint32_t y, uint32_t c ) { return ( x + 2 * y + 40 * c ) & 0xff; } );
	auto curr = Img( w, h, p, []( uint32_t x, uint32_t y, uint32_t c ) { return ( x + 8 + 2 * y + 40 * c ) & 0xff; } );
	const BlockMeans mp = Means( prev, w, h, p ), mc = Means( curr, w, h, p );
	REQUIRE( mp.uBw == 10 );
	REQUIRE( mp.uBh == 6 );
	REQUIRE( mp.v.size() == 10 * 6 * 3 );

	SECTION( "identical frames give 0" )
	{
		REQUIRE( BlockDiff( mp, mp ) == 0.0f );
		REQUIRE( BlendDiff( mp, mp, mp, 0.3f ) == 0.0f );
		REQUIRE_FALSE( PixelTriggers( 0.0f, 0.0f, 1.0f ) );
	}
	SECTION( "an output that is the blend gives about 0" )
	{
		const float t = 0.5f;
		auto out = Img( w, h, p, [ & ]( uint32_t x, uint32_t y, uint32_t c ) {
			return uint32_t( ( 1 - t ) * prev[ size_t( y ) * p + x * 4 + c ] + t * curr[ size_t( y ) * p + x * 4 + c ] + 0.5f ); } );
		const float flCoarse = BlendDiff( Means( out, w, h, p ), mp, mc, t );
		REQUIRE( flCoarse < 0.6f );
		REQUIRE_FALSE( PixelTriggers( flCoarse, BlockDiff( mp, mc ), 1.0f ) );
	}
	SECTION( "unrelated noise blocks are flagged" )
	{
		auto out = Img( w, h, p, []( uint32_t x, uint32_t y, uint32_t c ) { return Hash( ( x / 16 ) * 131 + ( y / 16 ) * 7919 + c ) & 0xff; } );
		const float flPair = BlockDiff( mp, mc );
		const float flCoarse = BlendDiff( Means( out, w, h, p ), mp, mc, 0.5f );
		REQUIRE( flCoarse > Threshold( flPair, 1.0f ) );
		REQUIRE( PixelTriggers( flCoarse, flPair, 1.0f ) );
	}
	SECTION( "a bright / dark shift is flagged" )
	{
		auto out = Img( w, h, p, [ & ]( uint32_t x, uint32_t y, uint32_t c ) {
			const int v = int( prev[ size_t( y ) * p + x * 4 + c ] ) + ( c < 3 ? 40 : 0 );
			return uint32_t( std::min( v, 255 ) ); } );
		const float flCoarse = BlendDiff( Means( out, w, h, p ), mp, mp, 0.5f );
		REQUIRE( flCoarse > 20.0f );
		REQUIRE( PixelTriggers( flCoarse, 0.0f, 1.0f ) );
		auto dark = Img( w, h, p, [ & ]( uint32_t x, uint32_t y, uint32_t c ) { return c < 3 ? prev[ size_t( y ) * p + x * 4 + c ] / 2 : 255u; } );
		REQUIRE( PixelTriggers( BlendDiff( Means( dark, w, h, p ), mp, mp, 0.5f ), 0.0f, 1.0f ) );
	}
	SECTION( "the threshold formula and the sensitivity" )
	{
		REQUIRE( Threshold( 0.0f, 1.0f ) == 6.0f );
		REQUIRE( Threshold( 14.6f, 1.0f ) == ( 6.0f + 1.5f * 14.6f ) );
		REQUIRE( Threshold( 2.0f, 2.0f ) == 4.5f );
		REQUIRE( Threshold( 2.0f, 0.0f ) == 0.0f );
		REQUIRE_FALSE( PixelTriggers( 8.5f, 14.6f, 1.0f ) );   // the largest good frame of a real capture
		REQUIRE( PixelTriggers( 0.0f, 0.0f, 0.0f ) );          // sensitivity 0: every measured frame
	}
	SECTION( "the sparse estimate is close to the full mean on smooth content" )
	{
		const BlockMeans full = Means( prev, w, h, p, 1 );
		REQUIRE( BlockDiff( full, mp ) < 3.0f );
	}
	SECTION( "bad input is refused" )
	{
		BlockMeans m;
		REQUIRE_FALSE( ComputeBlockMeans( prev.data(), p, 8, 8, 44, 4, &m ) );     // smaller than a block
		REQUIRE_FALSE( ComputeBlockMeans( prev.data(), p, w, h, 12345, 4, &m ) );  // unknown format
		REQUIRE_FALSE( ComputeBlockMeans( prev.data(), p, w, h, 44, 3, &m ) );     // step must divide 16
		BlockMeans small;
		REQUIRE( ComputeBlockMeans( prev.data(), p, 32, 32, 44, 4, &small ) );
		REQUIRE( BlockDiff( small, mp ) < 0.0f );
	}
}

TEST_CASE( "capture watch: 10-bit and fp16 pixels are read on the 0..255 scale", "[framegen_capture]" )
{
	const uint32_t w = 16, h = 16;
	std::vector<uint32_t> px( w * h, ( 1020u ) | ( 400u << 10 ) | ( 0u << 20 ) | ( 3u << 30 ) );
	BlockMeans m;
	REQUIRE( ComputeBlockMeans( reinterpret_cast<const uint8_t *>( px.data() ), w * 4, w, h, 58, 4, &m ) );
	REQUIRE( m.v[ 0 ] == 255.0f );
	REQUIRE( m.v[ 1 ] == 100.0f );
	REQUIRE( m.v[ 2 ] == 0.0f );
	std::vector<uint16_t> hp( w * h * 4 );
	for ( size_t i = 0; i < w * h; i++ )
	{
		hp[ i * 4 + 0 ] = 0x3c00;   // 1.0
		hp[ i * 4 + 1 ] = 0x3800;   // 0.5
		hp[ i * 4 + 2 ] = 0x0000;
		hp[ i * 4 + 3 ] = 0x3c00;
	}
	REQUIRE( ComputeBlockMeans( reinterpret_cast<const uint8_t *>( hp.data() ), w * 8, w, h, 97, 4, &m ) );
	REQUIRE( m.v[ 0 ] == 255.0f );
	REQUIRE( m.v[ 1 ] == 127.5f );
	REQUIRE( m.v[ 2 ] == 0.0f );
}

TEST_CASE( "capture watch: the flags", "[framegen_capture]" )
{
	uint64_t ulCurr = 0, ulOut = 0;
	FlagInput in;
	in.ulOutId = 10; in.ulPrevId = 4; in.ulCurrId = 5; in.bEstimateRecorded = true; in.nEstimateRet = 1; in.nSynthRet = 1;
	REQUIRE( EvalFlags( in, &ulCurr, &ulOut ) == 0 );
	REQUIRE( ulCurr == 5 );
	REQUIRE( ulOut == 10 );
	REQUIRE( FormatFlags( 0 ) == "-" );

	in.ulOutId = 11; in.ulPrevId = 5; in.ulCurrId = 7;
	REQUIRE( EvalFlags( in, &ulCurr, &ulOut ) == kFlagGap );

	in.ulOutId = 9; in.ulPrevId = 5; in.ulCurrId = 6;
	REQUIRE( EvalFlags( in, &ulCurr, &ulOut ) == kFlagBack );   // outId 9 < 11, currId 6 < 7

	in.ulOutId = 12; in.ulPrevId = 6; in.ulCurrId = 7;
	in.bEstimateRecorded = false; in.nEstimateRet = -1; in.bStaleHost = true;
	REQUIRE( EvalFlags( in, &ulCurr, &ulOut ) == kFlagStale );
	in.bEstimateRecorded = true; in.nEstimateRet = 1;   // a fresh estimate is never stale
	in.ulOutId = 13;
	REQUIRE( EvalFlags( in, &ulCurr, &ulOut ) == 0 );
	in.bStaleHost = false;

	in.ulOutId = 14; in.nEstimateRet = 0;
	REQUIRE( EvalFlags( in, &ulCurr, &ulOut ) == kFlagRefused );
	in.ulOutId = 15; in.nEstimateRet = 1; in.nSynthRet = 0;
	REQUIRE( EvalFlags( in, &ulCurr, &ulOut ) == kFlagRefused );
	in.nSynthRet = 1;

	in.ulOutId = 15; in.bNear = true; in.bMismatchHost = true;
	const uint32_t u = EvalFlags( in, &ulCurr, &ulOut );
	REQUIRE( u == ( kFlagNear | kFlagPairMismatch ) );
	REQUIRE( FormatFlags( u ) == "near,pairmismatch" );
	REQUIRE( ( u & kDumpFlags ) == kFlagPairMismatch );
	REQUIRE( ( ( kFlagGap | kFlagBack | kFlagNear ) & kDumpFlags ) == 0 );
	REQUIRE( FormatFlags( kFlagGap | kFlagBack | kFlagStale | kFlagRefused ) == "gap,back,stale,refused" );
}

TEST_CASE( "capture watch: the rolling window never reuses a real under a kept output", "[framegen_capture]" )
{
	RollingWindow w( 4, 6 );
	auto All = []( uint64_t ) { return true; };
	// Invariant: every entry's reals hold its ids, refcounts equal the references, outputs are distinct.
	auto Check = [ & ]()
	{
		std::vector<int> ref( 6, 0 );
		std::set<int> outs;
		for ( const auto &e : w.Entries() )
		{
			REQUIRE( outs.insert( e.nOut ).second );
			for ( int i = 0; i < 2; i++ )
			{
				REQUIRE( w.RealId( e.nReal[ i ] ) == e.ulId[ i ] );
				ref[ size_t( e.nReal[ i ] ) ]++;
			}
		}
		for ( int k = 0; k < 6; k++ )
			REQUIRE( w.RefCount( k ) == ref[ size_t( k ) ] );
	};

	auto a = w.Add( 0, 1, 2, All );
	REQUIRE( a.ok );
	REQUIRE( a.bRealNew[ 0 ] );
	REQUIRE( a.bRealNew[ 1 ] );
	a = w.Add( 1, 1, 2, All );   // the same pair again (a second output of it): no new reals
	REQUIRE( a.ok );
	REQUIRE_FALSE( a.bRealNew[ 0 ] );
	REQUIRE_FALSE( a.bRealNew[ 1 ] );
	a = w.Add( 2, 2, 3, All );
	REQUIRE( a.ok );
	REQUIRE_FALSE( a.bRealNew[ 0 ] );
	REQUIRE( a.bRealNew[ 1 ] );
	Check();

	// Consecutive pairs and scattered ones: the window keeps the newest 4 records.
	uint64_t rec = 3;
	for ( uint64_t id = 3; id < 40; id++, rec++ )
	{
		const uint64_t ulPrev = ( id % 7 == 0 ) ? id + 100 : id;   // now and then a pair not sharing frames
		a = w.Add( rec, ulPrev, id + 1, All );
		REQUIRE( a.ok );
		REQUIRE( w.Entries().size() <= 4 );
		REQUIRE( w.Entries().back().ulRec == rec );
		Check();
	}
	REQUIRE( w.Entries().size() == 4 );
	REQUIRE( w.Entries().front().ulRec == rec - 4 );

	// Nothing may leave (not analysed yet): Add fails, blocked, and the window is unchanged.
	const auto before = w.Entries();
	a = w.Add( rec, 500, 501, []( uint64_t ) { return false; } );
	REQUIRE_FALSE( a.ok );
	REQUIRE( a.bBlocked );
	REQUIRE( w.Entries().size() == before.size() );
	Check();

	// Only the trigger record (the oldest) is protected: blocked as well.
	const uint64_t ulTrig = w.Entries().front().ulRec;
	a = w.Add( rec, 600, 601, [ & ]( uint64_t u ) { return u != ulTrig; } );
	REQUIRE_FALSE( a.ok );
	REQUIRE( a.bBlocked );
	Check();

	w.Clear();
	REQUIRE( w.Entries().empty() );
	for ( int k = 0; k < 6; k++ )
		REQUIRE( w.RefCount( k ) == 0 );
	// Too few real slots for a pair: refused, never a shared slot.
	RollingWindow tiny( 4, 1 );
	REQUIRE_FALSE( tiny.Add( 0, 1, 2, All ).ok );
}

TEST_CASE( "capture watch: the repeat test (a new id over an old image)", "[framegen_capture]" )
{
	const uint32_t w = 64, h = 48, p = w * 4;
	auto Frame = [ & ]( uint32_t seed )
	{
		auto v = Img( w, h, p, [ & ]( uint32_t x, uint32_t y, uint32_t c ) { return Hash( seed * 7777 + y * 911 + x * 4 + c ) & 0xff; } );
		return std::make_pair( Means( v, w, h, p ), RowHash( v.data(), p, w, h, 4 ) );
	};
	const auto A = Frame( 1 ), B = Frame( 2 ), C = Frame( 3 );

	SECTION( "A B C A' triggers, naming both ids" )
	{
		RepeatDetector d;
		REQUIRE( d.Add( 10, A.first, A.second ).eKind == RepeatDetector::kNone );
		REQUIRE( d.Add( 11, B.first, B.second ).eKind == RepeatDetector::kNone );
		REQUIRE( d.Add( 12, C.first, C.second ).eKind == RepeatDetector::kNone );
		const auto r = d.Add( 13, A.first, A.second );
		REQUIRE( r.eKind == RepeatDetector::kRepeat );
		REQUIRE( r.ulSameAs == 10 );
		REQUIRE( d.Add( 13, A.first, A.second ).eKind == RepeatDetector::kNone );   // the same id again: ignored
	}
	SECTION( "A A A is a still picture, not a repeat" )
	{
		RepeatDetector d;
		REQUIRE( d.Add( 1, A.first, A.second ).eKind == RepeatDetector::kNone );
		auto r = d.Add( 2, A.first, A.second );
		REQUIRE( r.eKind == RepeatDetector::kStatic );
		REQUIRE( r.ulSameAs == 1 );
		REQUIRE( d.Add( 3, A.first, A.second ).eKind == RepeatDetector::kStatic );
	}
	SECTION( "A B A triggers" )
	{
		RepeatDetector d;
		d.Add( 1, A.first, A.second );
		d.Add( 2, B.first, B.second );
		const auto r = d.Add( 3, A.first, A.second );
		REQUIRE( r.eKind == RepeatDetector::kRepeat );
		REQUIRE( r.ulSameAs == 1 );
	}
	SECTION( "same block means but different full rows: not the same frame" )
	{
		auto v = Img( w, h, p, [ & ]( uint32_t x, uint32_t y, uint32_t c ) { return Hash( 1 * 7777 + y * 911 + x * 4 + c ) & 0xff; } );
		v[ size_t( h / 2 ) * p + 1 ] ^= 1;   // a pixel the sparse sample does not read, on a hashed row
		RepeatDetector d;
		d.Add( 1, A.first, A.second );
		d.Add( 2, B.first, B.second );
		REQUIRE( d.Add( 3, Means( v, w, h, p ), RowHash( v.data(), p, w, h, 4 ) ).eKind == RepeatDetector::kNone );
	}
	SECTION( "older than the history is forgotten" )
	{
		RepeatDetector d;
		d.Add( 1, A.first, A.second );
		for ( uint64_t i = 0; i < RepeatDetector::kHistory; i++ )
		{
			const auto f = Frame( 100 + uint32_t( i ) );
			d.Add( 2 + i, f.first, f.second );
		}
		REQUIRE( d.Add( 999, A.first, A.second ).eKind == RepeatDetector::kNone );
	}
	auto t = Tokens( FormatTriggerLine( "repeat", 9, 2, -1.0f, -1.0f, -1.0f, 16146, 16143 ) );
	REQUIRE( t.size() == 9 );
	REQUIRE( t[ 7 ] == "id=16146" );
	REQUIRE( t[ 8 ] == "same_as=16143" );
	REQUIRE( FormatFlags( kFlagRepeat | kFlagStatic ) == "repeat,static" );
	REQUIRE( ( kFlagRepeat & kDumpFlags ) != 0 );
	REQUIRE( ( kFlagStatic & kDumpFlags ) == 0 );
}

TEST_CASE( "capture watch: the pair span, its median and the distinct pairs over it", "[framegen_capture]" )
{
	std::vector<WatchRec> v;
	auto Add = [ & ]( uint64_t ulPrev, uint64_t ulCurr, int64_t nPrevNs, int64_t nCurrNs )
	{
		WatchRec r;
		r.ulIndex = v.size();
		r.ulPrevId = ulPrev;
		r.ulCurrId = ulCurr;
		r.nArrivalPrevNs = nPrevNs;
		r.nArrivalCurrNs = nCurrNs;
		v.push_back( r );
	};
	// Ten pairs at 5 ms, each synthesised twice (counted once), one at 12 ms (x2.4), one at 45 ms (x9), one unknown.
	for ( uint64_t i = 1; i <= 10; i++ )
	{
		Add( i, i + 1, int64_t( i ) * 5000000, int64_t( i + 1 ) * 5000000 );
		Add( i, i + 1, int64_t( i ) * 5000000, int64_t( i + 1 ) * 5000000 );
	}
	Add( 20, 21, 100000000, 112000000 );
	Add( 20, 21, 100000000, 112000000 );
	Add( 21, 22, 112000000, 157000000 );
	Add( 22, 23, -1, 160000000 );
	REQUIRE( PairSpanNs( v.back() ) == -1 );
	REQUIRE( PairSpanNs( v[ 0 ] ) == 5000000 );
	const SpanStats st = ComputeSpanStats( v );
	REQUIRE( st.ulKnown == 23 );
	REQUIRE( st.flMedianMs == 5.0 );
	REQUIRE( st.flMaxMs == 45.0 );
	REQUIRE( st.ulMaxRec == 22 );
	REQUIRE( st.ulMaxPrevId == 21 );
	REQUIRE( st.ulPairsOver[ 0 ] == 2 );   // > 10 ms: the 12 ms pair (once) and the 45 ms pair
	REQUIRE( st.ulPairsOver[ 1 ] == 1 );   // > 15 ms
	REQUIRE( st.ulPairsOver[ 2 ] == 1 );   // > 25 ms
	REQUIRE( st.ulPairsOver[ 3 ] == 1 );   // > 40 ms
	auto t = Tokens( FormatSpanTokens( st ) );
	for ( const char *psz : { "span_median_ms=5.000", "span_max_ms=45.000", "span_max_record=22", "span_gt2=2", "span_gt3=1", "span_gt8=1" } )
	{
		INFO( psz );
		REQUIRE( std::find( t.begin(), t.end(), psz ) != t.end() );
	}
	t = Tokens( FormatWatchLine( v[ 0 ] ) );
	REQUIRE( std::find( t.begin(), t.end(), std::string( "span_ms=5.000" ) ) != t.end() );
	t = Tokens( FormatWatchLine( v.back() ) );
	REQUIRE( std::find( t.begin(), t.end(), std::string( "span_ms=na" ) ) != t.end() );
	t = Tokens( FormatSpanTokens( ComputeSpanStats( {} ) ) );
	REQUIRE( std::find( t.begin(), t.end(), std::string( "span_median_ms=na" ) ) != t.end() );
	t = Tokens( FormatWatchSummary( WatchSummary(), "watch_done", FormatSpanTokens( st ) ) );
	REQUIRE( t.back() == "reason=watch_done" );
	REQUIRE( std::find( t.begin(), t.end(), std::string( "span_gt5=1" ) ) != t.end() );
}

TEST_CASE( "capture watch: the text formats", "[framegen_capture]" )
{
	WatchRec r;
	r.ulIndex = 7; r.ulNs = 123; r.ulOutId = 50; r.ulPrevId = 9; r.ulCurrId = 10; r.ulNewestId = 11;
	r.flT0 = r.flT1 = 0.5f; r.bEstimateRecorded = false; r.nSynthRet = 1; r.bNear = true; r.nOutSlot = 2;
	r.uFlags = kFlagNear;
	auto t = Tokens( FormatWatchLine( r ) );
	REQUIRE( t[ 0 ] == "rec" );
	for ( const char *psz : { "i=7", "outId=50", "prevId=9", "currId=10", "newestId=11", "estimate=reused", "estimate_ret=na", "synth_ret=1",
		"cached=0", "near=1", "out_slot=2", "coarse=na", "pair=na", "flags=near" } )
	{
		INFO( psz );
		REQUIRE( std::find( t.begin(), t.end(), psz ) != t.end() );
	}
	for ( size_t i = 1; i < t.size(); i++ )
		REQUIRE( t[ i ].find( '=' ) != std::string::npos );
	r.flCoarse = 3.25f; r.flPair = 1.5f; r.uFlags = 0;
	t = Tokens( FormatWatchLine( r ) );
	REQUIRE( std::find( t.begin(), t.end(), std::string( "coarse=3.250" ) ) != t.end() );
	REQUIRE( std::find( t.begin(), t.end(), std::string( "flags=-" ) ) != t.end() );

	WatchSummary sum;
	SummaryAdd( &sum, r );                 // ratio 3.25 / 8.25
	WatchRec r2 = r;
	r2.ulIndex = 8; r2.flCoarse = 9.0f; r2.flPair = 2.0f; r2.uFlags = kFlagGap | kFlagStale;   // ratio 1.0
	SummaryAdd( &sum, r2 );
	WatchRec r3;
	r3.ulIndex = 9; r3.uFlags = kFlagRefused;   // not measured
	SummaryAdd( &sum, r3 );
	REQUIRE( sum.ulRecords == 3 );
	REQUIRE( sum.ulMeasured == 2 );
	REQUIRE( sum.ulNotMeasured == 1 );
	REQUIRE( sum.maxRec.ulIndex == 8 );
	t = Tokens( FormatWatchSummary( sum, "watch_done" ) );
	REQUIRE( t[ 0 ] == "summary" );
	REQUIRE( t.back() == "reason=watch_done" );
	for ( const char *psz : { "records=3", "measured=2", "not_measured=1", "max_ratio=1.0000", "max_record=8", "max_coarse=9.000",
		"max_pair=2.000", "flag_gap=1", "flag_stale=1", "flag_refused=1", "flag_near=0", "flag_pairmismatch=0" } )
	{
		INFO( psz );
		REQUIRE( std::find( t.begin(), t.end(), psz ) != t.end() );
	}
	t = Tokens( FormatWatchSummary( WatchSummary(), "x" ) );
	REQUIRE( std::find( t.begin(), t.end(), std::string( "max_ratio=na" ) ) != t.end() );

	t = Tokens( FormatTriggerLine( "pixel", 42, 3, 20.0f, 4.0f, 12.0f ) );
	REQUIRE( t == std::vector<std::string>{ "trigger", "kind=pixel", "record=42", "seq=3", "coarse=20.000", "pair=4.000", "threshold=12.000" } );
	t = Tokens( FormatTriggerLine( "refused", 5, -1, -1.0f, -1.0f, -1.0f ) );
	REQUIRE( t[ 3 ] == "seq=-1" );
	REQUIRE( t[ 4 ] == "coarse=na" );

	HeaderInfo h;
	h.uWidth = 64; h.uHeight = 32; h.nVkFormat = 44; h.uBytesPerPixel = 4; h.sMotionWanted = "V3"; h.sMotionEffective = "V3";
	t = Tokens( FormatWatchHeader( h, 30, 0.0f ) );
	REQUIRE( t[ 0 ] == "watch" );
	REQUIRE( t[ 1 ] == "v=1" );
	for ( const char *psz : { "seconds=30", "sensitivity=0", "width=64", "motion_effective=V3", "threshold=(6+1.5*pair)/sensitivity" } )
	{
		INFO( psz );
		REQUIRE( std::find( t.begin(), t.end(), psz ) != t.end() );
	}

	// The burst's frame line carries near=.
	FrameInfo f;
	f.bNear = true;
	t = Tokens( FormatFrameLine( f ) );
	REQUIRE( std::find( t.begin(), t.end(), std::string( "near=1" ) ) != t.end() );
	REQUIRE( WatchRequiredBytes( 100, 100, 4, 60 ) >= 3 * RequiredBytes( 100, 100, 4, kFrames ) );
}

// V3 Hybrid (2026-11-23): the header (and the watch header) names the blend variant; plain V3 says nothing extra.
TEST_CASE( "capture: the header names the V3 Hybrid variant, plain V3 nothing", "[framegen_capture]" )
{
	HeaderInfo h;
	h.sMotionWanted = "V3";
	h.sMotionEffective = "V3";
	h.uFlowScale = 2;
	REQUIRE( FormatHeaderLine( h ).find( "blend=" ) == std::string::npos );
	REQUIRE( FormatWatchHeader( h, 10, 1.0f ).find( "blend=" ) == std::string::npos );
	const char *const kName[] = { "A", "B", "C" };
	for ( int i = 0; i < 3; i++ )
	{
		h.nBlend = i + 1;
		const std::string sKey = std::string( "fallback_v1=0 blend=" ) + kName[ i ] + " flowScale=2";
		REQUIRE( FormatHeaderLine( h ).find( sKey ) != std::string::npos );
		REQUIRE( FormatWatchHeader( h, 10, 1.0f ).find( sKey ) != std::string::npos );
		const auto t = Tokens( FormatHeaderLine( h ) );
		for ( size_t k = 1; k < t.size(); k++ )
			REQUIRE( t[ k ].find( '=' ) != std::string::npos );
	}
}
