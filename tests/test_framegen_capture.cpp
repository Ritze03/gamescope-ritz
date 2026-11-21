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
	REQUIRE( v.size() == 44 );   // every field (the static_assert in FrameGenCapture.cpp guards the struct's size)
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
	REQUIRE( names.size() == 44 );   // all distinct
	for ( const char *pszName : { "trustLow", "searchPenalty", "uiProtection", "blurWeights", "motionVersion", "v2Mode", "v2StaticMask",
		"v3Smear", "v3Guard", "learnedNet", "learnedStaticGuard", "flowScale", "pyramidMinSide", "uiBoxOffsetX" } )
		REQUIRE( names.count( pszName ) == 1 );

	// Values and enum texts.
	framegen::Settings m;
	m.motionVersion = framegen::MotionVersion::V3;
	m.v3Guard = framegen::V3Guard::V2Motion;
	m.uiBoxOffsetX = -5;
	m.v3Smear = 0.4f;
	m.subpixel = false;
	const auto w = FormatSettingLines( m );
	auto Has = [ & ]( const char *pszLine ) { return std::find( w.begin(), w.end(), std::string( pszLine ) ) != w.end(); };
	REQUIRE( Has( "setting motionVersion=3 text=V3" ) );
	REQUIRE( Has( "setting v3Guard=2 text=V2Motion" ) );
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
		"width=1440 height=1080 estimate=recorded estimate_ret=1 synth_ret=1 cached=0 out_slot=2 prev_slot=0 curr_slot=1 "
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
