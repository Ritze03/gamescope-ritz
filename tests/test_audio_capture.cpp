#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <limits>
#include <map>
#include <string>
#include <vector>

#include "Audio/Capture.h"

// The pure parts of game audio capture (src/Audio/Capture.h): how the session
// tag is merged into the game's environment. The libpipewire half has no unit
// test (Capture.cpp is not in this binary, which links no PipeWire): it needs a
// running PipeWire, and superdoc/features/audio-capture.md lists the live checks.

using namespace gamescope::Audio::Capture;

TEST_CASE( "PIPEWIRE_PROPS: unset or empty gets a fresh object", "[audio_capture]" )
{
	const std::string sWant = "{ gamescope.ritz.session = 4242 state.restore-target = false }";
	REQUIRE( Env::MergePipewireProps( "", "4242" ) == sWant );
	REQUIRE( Env::MergePipewireProps( "   ", "4242" ) == sWant );
	REQUIRE( Env::MergePipewireProps( "{}", "4242" ) == sWant );
	REQUIRE( Env::MergePipewireProps( "{ }", "4242" ) == sWant );
}

TEST_CASE( "PIPEWIRE_PROPS: an existing object is kept and ours is merged in", "[audio_capture]" )
{
	// Insert before the closing brace; the launcher's pairs stay verbatim.
	REQUIRE( Env::MergePipewireProps( "{ media.role = Game }", "7" ) ==
		"{ media.role = Game gamescope.ritz.session = 7 state.restore-target = false }" );
	REQUIRE( Env::MergePipewireProps( "{media.role=\"Game\" node.latency=\"256/48000\"}", "7" ) ==
		"{ media.role=\"Game\" node.latency=\"256/48000\" gamescope.ritz.session = 7 state.restore-target = false }" );
	// Surrounding whitespace.
	REQUIRE( Env::MergePipewireProps( "  { a = b }\n", "7" ) ==
		"{ a = b gamescope.ritz.session = 7 state.restore-target = false }" );
}

TEST_CASE( "PIPEWIRE_PROPS: a bare list (no braces) is wrapped", "[audio_capture]" )
{
	REQUIRE( Env::MergePipewireProps( "media.role=Game", "7" ) ==
		"{ media.role=Game gamescope.ritz.session = 7 state.restore-target = false }" );
}

TEST_CASE( "PIPEWIRE_PROPS: the launcher's own state.restore-target is honoured, not duplicated", "[audio_capture]" )
{
	REQUIRE( Env::MergePipewireProps( "{ state.restore-target = true }", "7" ) ==
		"{ state.restore-target = true gamescope.ritz.session = 7 }" );
}

TEST_CASE( "PIPEWIRE_PROPS: already carrying our tag is returned unchanged", "[audio_capture]" )
{
	const std::string sOuter = "{ gamescope.ritz.session = 99 state.restore-target = false }";
	REQUIRE( Env::MergePipewireProps( sOuter, "7" ) == sOuter );
	// And merging twice is a no-op the second time.
	const std::string sOnce = Env::MergePipewireProps( "{ a = b }", "7" );
	REQUIRE( Env::MergePipewireProps( sOnce, "7" ) == sOnce );
}

TEST_CASE( "PULSE_PROP: pairs are appended, the launcher's string kept", "[audio_capture]" )
{
	REQUIRE( Env::AppendPulseProp( "", "4242" ) == "gamescope.ritz.session=4242 state.restore-target=false" );
	REQUIRE( Env::AppendPulseProp( "media.role=game", "4242" ) ==
		"media.role=game gamescope.ritz.session=4242 state.restore-target=false" );
	REQUIRE( Env::AppendPulseProp( "  media.role=game  ", "4242" ) ==
		"media.role=game gamescope.ritz.session=4242 state.restore-target=false" );
	// A quoted value with a space survives untouched.
	REQUIRE( Env::AppendPulseProp( "application.name='My Game'", "1" ) ==
		"application.name='My Game' gamescope.ritz.session=1 state.restore-target=false" );
}

TEST_CASE( "PULSE_PROP: a restore-target the launcher set is not repeated, our tag is idempotent", "[audio_capture]" )
{
	REQUIRE( Env::AppendPulseProp( "state.restore-target=true", "5" ) ==
		"state.restore-target=true gamescope.ritz.session=5" );
	const std::string sOnce = Env::AppendPulseProp( "a=b", "5" );
	REQUIRE( Env::AppendPulseProp( sOnce, "5" ) == sOnce );
}

TEST_CASE( "session id and sink name", "[audio_capture]" )
{
	REQUIRE( !SessionId().empty() );
	REQUIRE( SessionId().find_first_not_of( "0123456789" ) == std::string::npos );
	REQUIRE( SinkNameFor( "4242" ) == "gamescope-ritz.4242" );
}

// ---- the playback copy (CopyPlaneSafe): the output must be safe whatever the input ----

namespace
{
	// A destination pre-filled with a loud sentinel, so any byte the copy fails
	// to write shows up as a full-scale value.
	std::vector<float> Dst( size_t nFloats ) { return std::vector<float>( nFloats, 0.75f ); }

	PlaneSource Src( const std::vector<float> &v, uint32_t uSizeBytes, uint32_t uOffset = 0, bool bBad = false )
	{
		PlaneSource s;
		s.pData = reinterpret_cast<const uint8_t *>( v.data() );
		s.uMaxSize = (uint32_t)( v.size() * sizeof( float ) );
		s.uOffset = uOffset;
		s.uSize = uSizeBytes;
		s.bEmptyOrCorrupt = bBad;
		return s;
	}

	uint32_t Run( std::vector<float> &dst, uint32_t uWantBytes, const PlaneSource *pSrc )
	{
		return CopyPlaneSafe( reinterpret_cast<uint8_t *>( dst.data() ), (uint32_t)( dst.size() * sizeof( float ) ), uWantBytes, pSrc );
	}

	bool AllZero( const std::vector<float> &v, size_t nFrom, size_t nTo )
	{
		for ( size_t i = nFrom; i < nTo; i++ )
			if ( v[i] != 0.0f ) return false;
		return true;
	}
}

TEST_CASE( "copy: a full valid source is copied verbatim", "[audio_capture]" )
{
	std::vector<float> in = { 0.1f, -0.2f, 0.3f, -0.4f };
	std::vector<float> dst = Dst( 4 );
	PlaneSource s = Src( in, 16 );
	REQUIRE( Run( dst, 16, &s ) == 16 );
	REQUIRE( dst == in );
}

TEST_CASE( "copy: no source at all is silence, of the wanted size", "[audio_capture]" )
{
	std::vector<float> dst = Dst( 8 );
	REQUIRE( Run( dst, 16, nullptr ) == 16 );
	REQUIRE( AllZero( dst, 0, 4 ) );
	REQUIRE( dst[4] == 0.75f );   // nothing written past the wanted size
	PlaneSource none;              // a source with no data pointer
	REQUIRE( Run( dst, 32, &none ) == 32 );
	REQUIRE( AllZero( dst, 0, 8 ) );
}

TEST_CASE( "copy: a short source is zero-padded", "[audio_capture]" )
{
	std::vector<float> in = { 0.5f, 0.5f, 0.5f, 0.5f };
	std::vector<float> dst = Dst( 4 );
	PlaneSource s = Src( in, 8 );   // only 2 floats valid
	REQUIRE( Run( dst, 16, &s ) == 16 );
	REQUIRE( dst[0] == 0.5f );
	REQUIRE( dst[1] == 0.5f );
	REQUIRE( dst[2] == 0.0f );
	REQUIRE( dst[3] == 0.0f );
}

TEST_CASE( "copy: a chunk flagged empty or corrupt reads as silence, whatever it holds", "[audio_capture]" )
{
	std::vector<float> in = { 1.0f, 1.0f, 1.0f, 1.0f };
	std::vector<float> dst = Dst( 4 );
	PlaneSource s = Src( in, 16, 0, /*bBad*/ true );
	REQUIRE( Run( dst, 16, &s ) == 16 );
	REQUIRE( AllZero( dst, 0, 4 ) );
}

TEST_CASE( "copy: the source offset is honoured", "[audio_capture]" )
{
	std::vector<float> in = { 9.0f, 9.0f, 0.1f, 0.2f };   // 9.0 is before the valid range
	std::vector<float> dst = Dst( 2 );
	PlaneSource s = Src( in, 8, 8 );
	REQUIRE( Run( dst, 8, &s ) == 8 );
	REQUIRE( dst[0] == 0.1f );
	REQUIRE( dst[1] == 0.2f );
}

TEST_CASE( "copy: a size or offset that overruns the source's maxsize never reads outside it", "[audio_capture]" )
{
	std::vector<float> in = { 0.1f, 0.2f };
	std::vector<float> dst = Dst( 8 );

	PlaneSource big = Src( in, 4000 );   // claims far more than maxsize (8 bytes)
	REQUIRE( Run( dst, 32, &big ) == 32 );
	REQUIRE( dst[0] == 0.1f );
	REQUIRE( dst[1] == 0.2f );
	REQUIRE( AllZero( dst, 2, 8 ) );

	dst = Dst( 8 );
	PlaneSource past = Src( in, 8, 8 );    // offset == maxsize: nothing valid
	REQUIRE( Run( dst, 32, &past ) == 32 );
	REQUIRE( AllZero( dst, 0, 8 ) );
	dst = Dst( 8 );
	PlaneSource far = Src( in, 8, 4000 );  // offset way past maxsize
	REQUIRE( Run( dst, 32, &far ) == 32 );
	REQUIRE( AllZero( dst, 0, 8 ) );
}

TEST_CASE( "copy: the wanted size is clamped to the destination and floored to whole floats", "[audio_capture]" )
{
	std::vector<float> in( 16, 0.25f );
	std::vector<float> dst = Dst( 4 );
	PlaneSource s = Src( in, 64 );
	REQUIRE( Run( dst, 64, &s ) == 16 );   // dst holds only 16 bytes
	REQUIRE( Run( dst, 6, &s ) == 4 );     // 6 bytes -> 1 whole float
	REQUIRE( Run( dst, 0, &s ) == 0 );
	REQUIRE( CopyPlaneSafe( nullptr, 16, 16, &s ) == 0 );
}

TEST_CASE( "copy: every output sample is clamped to [-1, 1], NaN and infinity become 0", "[audio_capture]" )
{
	const float flInf = std::numeric_limits<float>::infinity();
	const float flNaN = std::numeric_limits<float>::quiet_NaN();
	std::vector<float> in = { 5.0f, -5.0f, flNaN, flInf, -flInf, 1.0f, -1.0f, 0.5f, 3.4e38f };
	std::vector<float> dst = Dst( in.size() );
	PlaneSource s = Src( in, (uint32_t)( in.size() * 4 ) );
	REQUIRE( Run( dst, (uint32_t)( in.size() * 4 ), &s ) == in.size() * 4 );
	const float want[] = { 1.0f, -1.0f, 0.0f, 0.0f, 0.0f, 1.0f, -1.0f, 0.5f, 1.0f };
	for ( size_t i = 0; i < in.size(); i++ )
		REQUIRE( dst[i] == want[i] );
}

TEST_CASE( "copy: garbage bytes as floats can never come out above full scale", "[audio_capture]" )
{
	// Deterministic pseudo-random bit patterns reinterpreted as floats: includes
	// huge values, denormals, NaNs and infinities, like uninitialised memory.
	std::vector<uint32_t> bits( 4096 );
	uint32_t x = 0x12345678u;
	for ( auto &b : bits ) { x = x * 1664525u + 1013904223u; b = x; }
	std::vector<float> in( bits.size() );
	memcpy( in.data(), bits.data(), bits.size() * 4 );
	std::vector<float> dst = Dst( in.size() );
	PlaneSource s = Src( in, (uint32_t)( in.size() * 4 ) );
	REQUIRE( Run( dst, (uint32_t)( in.size() * 4 ), &s ) == in.size() * 4 );
	for ( float f : dst )
	{
		REQUIRE( f <= 1.0f );
		REQUIRE( f >= -1.0f );
	}
}

// ---- the original route: where the game's audio was headed before us --------

namespace
{
	const char *kOurSink = "gamescope-ritz.4242";
	const char *kDefault = "alsa_output.pci-0000_00_1f.3.analog-stereo";

	std::map<uint32_t, SinkInfo> Sinks()
	{
		return {
			{ 60, SinkInfo{ "alsa_output.pci-0000_00_1f.3.analog-stereo", "FL,FR", 51 } },
			{ 61, SinkInfo{ "easyeffects_sink", "FL,FR", 77 } },
			{ 62, SinkInfo{ "bluez_output.headset", "FL,FR", 90 } },
		};
	}
}

TEST_CASE( "route: a metadata value resolves to a known sink's node.name", "[audio_capture]" )
{
	const auto sinks = Sinks();
	// target.object: a serial, or a name.
	REQUIRE( Route::ResolveRouteValue( "77", false, sinks ) == "easyeffects_sink" );
	REQUIRE( Route::ResolveRouteValue( "easyeffects_sink", false, sinks ) == "easyeffects_sink" );
	// target.node: a node id.
	REQUIRE( Route::ResolveRouteValue( "62", true, sinks ) == "bluez_output.headset" );
	// A number that is no serial / id of that kind is tried as the other.
	REQUIRE( Route::ResolveRouteValue( "62", false, sinks ) == "bluez_output.headset" );
	REQUIRE( Route::ResolveRouteValue( "90", true, sinks ) == "bluez_output.headset" );
	// Nothing, "no target", a stale name, an unknown number: not a route.
	REQUIRE( Route::ResolveRouteValue( "", false, sinks ).empty() );
	REQUIRE( Route::ResolveRouteValue( "-1", true, sinks ).empty() );
	REQUIRE( Route::ResolveRouteValue( "gone_sink", false, sinks ).empty() );
	REQUIRE( Route::ResolveRouteValue( "12345", false, sinks ).empty() );
	REQUIRE( Route::ResolveRouteValue( "99999999999999", true, sinks ).empty() );
}

TEST_CASE( "route: the initial route is the metadata entry, else the node prop, else the link", "[audio_capture]" )
{
	Route::Observation o;
	o.sMetaTarget = "easyeffects_sink";
	o.sPropTarget = "bluez_output.headset";
	o.sLinkedSink = "other_sink";
	REQUIRE( Route::PickInitialRoute( o, kOurSink, kDefault ) == "easyeffects_sink" );

	o.sMetaTarget.clear();
	REQUIRE( Route::PickInitialRoute( o, kOurSink, kDefault ) == "bluez_output.headset" );

	o.sPropTarget.clear();
	REQUIRE( Route::PickInitialRoute( o, kOurSink, kDefault ) == "other_sink" );

	// Nothing known: the default output.
	o.sLinkedSink.clear();
	REQUIRE( Route::PickInitialRoute( o, kOurSink, kDefault ).empty() );
}

TEST_CASE( "route: ours is never recorded, and does not end the search", "[audio_capture]" )
{
	Route::Observation o;
	// An entry left from an earlier move of ours: the next source still speaks.
	o.sMetaTarget = kOurSink;
	o.sPropTarget = "easyeffects_sink";
	REQUIRE( Route::PickInitialRoute( o, kOurSink, kDefault ) == "easyeffects_sink" );

	// Linked into our own sink (already moved) and nothing else: no route.
	o.sMetaTarget.clear();
	o.sPropTarget.clear();
	o.sLinkedSink = kOurSink;
	REQUIRE( Route::PickInitialRoute( o, kOurSink, kDefault ).empty() );

	// Our playback node's name is ours too.
	o.sLinkedSink = std::string( kOurSink ) + ".out";
	REQUIRE( Route::PickInitialRoute( o, kOurSink, kDefault ).empty() );

	// A foreign write that is ours leaves the recorded route alone.
	REQUIRE( Route::RecordForeignWrite( "easyeffects_sink", kOurSink, kOurSink ) == "easyeffects_sink" );
	REQUIRE( Route::RecordForeignWrite( "", kOurSink, kOurSink ).empty() );
	// ...and so does one that resolved to nothing.
	REQUIRE( Route::RecordForeignWrite( "easyeffects_sink", "", kOurSink ) == "easyeffects_sink" );

	// Playback is never pointed at our own sink, whatever got recorded.
	REQUIRE( Route::PlaybackTargetFor( kOurSink, kOurSink ).empty() );
	REQUIRE( Route::PlaybackTargetFor( "", kOurSink ).empty() );
	REQUIRE( Route::PlaybackTargetFor( "easyeffects_sink", kOurSink ) == "easyeffects_sink" );
}

TEST_CASE( "route: a link to the current default sink is the default, not a route", "[audio_capture]" )
{
	// WirePlumber's own choice: recording it would pin us to that device by
	// name and we would stop following a later change of the default.
	Route::Observation o;
	o.sLinkedSink = kDefault;
	REQUIRE( Route::PickInitialRoute( o, kOurSink, kDefault ).empty() );

	// A link to anything else is a route.
	o.sLinkedSink = "easyeffects_sink";
	REQUIRE( Route::PickInitialRoute( o, kOurSink, kDefault ) == "easyeffects_sink" );

	// An explicit target is one even when it names the default.
	o.sLinkedSink.clear();
	o.sMetaTarget = kDefault;
	REQUIRE( Route::PickInitialRoute( o, kOurSink, kDefault ) == kDefault );
}

TEST_CASE( "route: the latest foreign write wins", "[audio_capture]" )
{
	std::string sRoute;
	sRoute = Route::RecordForeignWrite( sRoute, "easyeffects_sink", kOurSink );
	REQUIRE( sRoute == "easyeffects_sink" );
	sRoute = Route::RecordForeignWrite( sRoute, "bluez_output.headset", kOurSink );
	REQUIRE( sRoute == "bluez_output.headset" );
	// Ours in between changes nothing.
	sRoute = Route::RecordForeignWrite( sRoute, kOurSink, kOurSink );
	REQUIRE( sRoute == "bluez_output.headset" );
	sRoute = Route::RecordForeignWrite( sRoute, "easyeffects_sink", kOurSink );
	REQUIRE( sRoute == "easyeffects_sink" );
}

TEST_CASE( "route: our playback is re-asserted only against a different known sink", "[audio_capture]" )
{
	// A route is recorded and a writer sends us elsewhere: fight.
	REQUIRE( Route::ShouldReassertPlayback( "bluez_output.headset", "easyeffects_sink" ) );
	// The same sink (EasyEffects' write agrees with the game's route): nothing to do.
	REQUIRE_FALSE( Route::ShouldReassertPlayback( "easyeffects_sink", "easyeffects_sink" ) );
	// No route recorded: nothing says where we should go, so no fight.
	REQUIRE_FALSE( Route::ShouldReassertPlayback( "", "easyeffects_sink" ) );
	// A foreign value that is no known sink is not fought.
	REQUIRE_FALSE( Route::ShouldReassertPlayback( "easyeffects_sink", "" ) );
}

TEST_CASE( "route: the Diagnostics wording", "[audio_capture]" )
{
	REQUIRE( Route::Describe( "easyeffects_sink" ) == "easyeffects_sink (game's original route)" );
	REQUIRE( Route::Describe( "" ) == "default output" );
}
