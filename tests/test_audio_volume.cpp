#include <catch2/catch_test_macros.hpp>

#include "Audio/Volume.h"

using namespace gamescope::Audio;

// Fixtures below are real `wpctl status`/`inspect`/`get-volume` output
// captured against a live PipeWire/WirePlumber session while testing this
// feature (a `paplay`-fed silent stream, so no audible sound was involved),
// not hand-invented text.

TEST_CASE( "Volume curve conversion", "[audio_volume]" )
{
	SECTION( "display fraction to linear amplitude is cubic" )
	{
		REQUIRE( DisplayToLinearVolume( 0.0f ) == 0.0f );
		REQUIRE( DisplayToLinearVolume( 1.0f ) == 1.0f );

		float flLinear = DisplayToLinearVolume( 0.4f );
		REQUIRE( flLinear > 0.063f );
		REQUIRE( flLinear < 0.065f ); // 0.4^3 == 0.064, per WirePlumber's own documented default
	}

	SECTION( "linear to display is the inverse (cube root)" )
	{
		float flDisplay = LinearToDisplayVolume( 0.064f );
		REQUIRE( flDisplay > 0.399f );
		REQUIRE( flDisplay < 0.401f );
	}

	SECTION( "round-trips within floating point tolerance" )
	{
		for ( float flDisplay : { 0.0f, 0.1f, 0.5f, 0.75f, 1.0f, 1.5f } )
		{
			float flLinear = DisplayToLinearVolume( flDisplay );
			float flBack = LinearToDisplayVolume( flLinear );
			REQUIRE( std::abs( flBack - flDisplay ) < 0.001f );
		}
	}
}

TEST_CASE( "FormatLinearVolume avoids locale-sensitive formatting", "[audio_volume]" )
{
	REQUIRE( Parse::FormatLinearVolume( 0.3f ) == "0.3000" );
	REQUIRE( Parse::FormatLinearVolume( 0.064f ) == "0.0640" );
	REQUIRE( Parse::FormatLinearVolume( 1.0f ) == "1.0000" );
	REQUIRE( Parse::FormatLinearVolume( -1.0f ) == "0.0000" ); // clamped
}

static constexpr std::string_view k_svStatusStreams =
	" \xE2\x94\x94\xE2\x94\x80 Streams:\n"
	"       253. speech-dispatcher-dummy                                     \n"
	"            229. output_FL       > ALCS1200A Analog:playback_FL\t[init]\n"
	"            244. output_FR       > ALCS1200A Analog:playback_FR\t[init]\n"
	"       353. paplay                                                      \n"
	"            247. output_FR       > Easy Effects Sink:playback_FR\t[active]\n"
	"            270. output_FL       > Easy Effects Sink:playback_FL\t[active]\n"
	"\n"
	"Video\n"
	" \xE2\x94\x9C\xE2\x94\x80 Devices:\n";

TEST_CASE( "StatusStreamNodes finds node lines and skips port sub-lines", "[audio_volume]" )
{
	auto nodes = Parse::StatusStreamNodes( k_svStatusStreams );

	REQUIRE( nodes.size() == 2 );
	REQUIRE( nodes[ 0 ].first == 253 );
	REQUIRE( nodes[ 0 ].second == "speech-dispatcher-dummy" );
	REQUIRE( nodes[ 1 ].first == 353 );
	REQUIRE( nodes[ 1 ].second == "paplay" );
}

TEST_CASE( "StatusStreamNodes on empty Streams section finds nothing", "[audio_volume]" )
{
	auto nodes = Parse::StatusStreamNodes( " \xE2\x94\x94\xE2\x94\x80 Streams:\n\nVideo\n" );
	REQUIRE( nodes.empty() );
}

static constexpr std::string_view k_svInspectPaplayStream =
	"id 353, type PipeWire:Interface:Node\n"
	"    application.language = \"en_US.UTF-8\"\n"
	"  * application.name = \"paplay\"\n"
	"    application.process.binary = \"pacat\"\n"
	"    application.process.host = \"mo-cachyos\"\n"
	"    application.process.id = \"2926365\"\n"
	"    application.process.machine-id = \"b0e437b169fd4d43a4b9af4f5f8573ad\"\n"
	"    application.process.session-id = \"2\"\n"
	"    application.process.user = \"mo\"\n"
	"  * client.id = \"353\"\n"
	"  * media.class = \"Stream/Output/Audio\"\n"
	"  * node.name = \"paplay\"\n";

static constexpr std::string_view k_svInspectSink =
	"id 63, type PipeWire:Interface:Node\n"
	"    alsa.card = \"3\"\n"
	"  * node.name = \"alsa_output.usb-Kingston_HyperX_Quadcast_4110-00.analog-stereo\"\n";

TEST_CASE( "InspectField extracts a quoted value", "[audio_volume]" )
{
	auto oPid = Parse::InspectField( k_svInspectPaplayStream, "application.process.id" );
	REQUIRE( oPid.has_value() );
	REQUIRE( *oPid == "2926365" );

	auto oMissing = Parse::InspectField( k_svInspectPaplayStream, "no.such.key" );
	REQUIRE_FALSE( oMissing.has_value() );
}

TEST_CASE( "IsAudioOutputStream distinguishes streams from other nodes", "[audio_volume]" )
{
	REQUIRE( Parse::IsAudioOutputStream( k_svInspectPaplayStream ) );
	REQUIRE_FALSE( Parse::IsAudioOutputStream( k_svInspectSink ) );
}

TEST_CASE( "GetVolumeOutput parses unmuted and muted forms", "[audio_volume]" )
{
	auto oUnmuted = Parse::GetVolumeOutput( "Volume: 0.30\n" );
	REQUIRE( oUnmuted.has_value() );
	REQUIRE( oUnmuted->flLinear > 0.299f );
	REQUIRE( oUnmuted->flLinear < 0.301f );
	REQUIRE_FALSE( oUnmuted->bMuted );

	auto oMuted = Parse::GetVolumeOutput( "Volume: 0.30 [MUTED]\n" );
	REQUIRE( oMuted.has_value() );
	REQUIRE( oMuted->bMuted );

	auto oGarbage = Parse::GetVolumeOutput( "sh: wpctl: command not found\n" );
	REQUIRE_FALSE( oGarbage.has_value() );
}

TEST_CASE( "Fresh state before any target PID is set reports not detected", "[audio_volume]" )
{
	VolumeState state = GetState();
	REQUIRE_FALSE( state.bDetected );
	REQUIRE( state.nMatchedNodes == 0 );
}
