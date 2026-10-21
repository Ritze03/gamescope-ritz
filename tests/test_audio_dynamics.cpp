#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <random>
#include <string>
#include <vector>

#include "Audio/Dynamics.h"

// The compressor + look-ahead limiter (src/Audio/Dynamics.h), driven offline on
// synthetic signals. Pure DSP: no PipeWire, nothing is played.

using namespace gamescope::Audio::Dynamics;

namespace
{
	using Planes = std::vector<std::vector<float>>;

	constexpr double kPi = 3.14159265358979323846;

	float DbToLin( double flDb ) { return (float)std::pow( 10.0, flDb / 20.0 ); }
	double LinToDb( double fl ) { return 20.0 * std::log10( fl ); }

	// Process in place, in blocks of the given size (the last one may be short).
	void Run( Processor &proc, Planes &planes, size_t nBlock )
	{
		std::vector<float *> vPtrs( planes.size() );
		size_t nTotal = planes[0].size();
		for ( size_t nPos = 0; nPos < nTotal; nPos += nBlock )
		{
			size_t nNow = std::min( nBlock, nTotal - nPos );
			for ( size_t c = 0; c < planes.size(); c++ )
				vPtrs[c] = planes[c].data() + nPos;
			proc.Process( vPtrs.data(), (uint32_t)nNow );
		}
	}

	std::vector<float> Sine( size_t nFrames, double flRate, double flHz, double flAmp, double flPhase = 0.0 )
	{
		std::vector<float> v( nFrames );
		for ( size_t i = 0; i < nFrames; i++ )
			v[i] = (float)( flAmp * std::sin( 2.0 * kPi * flHz * (double)i / flRate + flPhase ) );
		return v;
	}

	float PeakOf( const std::vector<float> &v, size_t nFrom, size_t nTo )
	{
		float fl = 0.0f;
		for ( size_t i = nFrom; i < nTo; i++ )
			fl = std::max( fl, std::fabs( v[i] ) );
		return fl;
	}

	Params Custom( float flThreshold, float flRatio, float flMakeup, float flCeiling )
	{
		Params p;
		p.bEnabled = true;
		p.flThresholdDb = flThreshold;
		p.flRatio = flRatio;
		p.flAttackMs = 5.0f;
		p.flReleaseMs = 100.0f;
		p.flMakeupDb = flMakeup;
		p.flCeilingDb = flCeiling;
		return p;
	}

	// Checks a signal never goes over the ceiling AND that the limiter, not the
	// final clamp, kept it there.
	void RequireLimited( Processor &proc, Planes &planes, float flCeilingDb )
	{
		const float flCeil = DbToLin( flCeilingDb );
		Run( proc, planes, 480 );
		for ( auto &vPlane : planes )
			for ( float fl : vPlane )
			{
				REQUIRE( std::isfinite( fl ) );
				REQUIRE( std::fabs( fl ) <= flCeil * 1.00001f ); // the processor's own exp2f ceiling may differ by an ulp
			}
		REQUIRE( proc.BackstopHits() == 0 );
	}
}

TEST_CASE( "Dynamics: compressor static curve matches threshold and ratio", "[audio_dynamics]" )
{
	// Outside the 6 dB knee (|level - threshold| > 3 dB): out = T + (in - T) / R
	// above, out = in below, plus the make-up gain. Ceiling 0 dBFS and levels
	// chosen so the limiter stays out of it.
	const float flThreshold = -20.0f, flMakeup = 3.0f;
	for ( uint32_t uRate : { 44100u, 48000u } )
		for ( float flRatio : { 2.0f, 4.0f, 10.0f } )
		{
			for ( double flInDb : { -60.0, -40.0, -30.0, -24.0, -16.0, -10.0, -6.0, -3.0 } )
			{
				Processor proc;
				REQUIRE( proc.Prepare( uRate, 1 ) );
				proc.SetParams( Custom( flThreshold, flRatio, flMakeup, 0.0f ) );

				Planes planes{ Sine( uRate, uRate, 1000.0, DbToLin( flInDb ) ) }; // 1 s
				Run( proc, planes, 512 );

				double flOutDb = LinToDb( PeakOf( planes[0], uRate * 8 / 10, uRate ) );
				double flWant = flInDb < flThreshold ? flInDb : flThreshold + ( flInDb - flThreshold ) / flRatio;
				flWant += flMakeup;
				INFO( "rate " << uRate << " ratio " << flRatio << " in " << flInDb << " dB" );
				REQUIRE( std::fabs( flOutDb - flWant ) < 0.5 );
				REQUIRE( proc.BackstopHits() == 0 );
			}
		}
}

TEST_CASE( "Dynamics: limiter never exceeds its ceiling", "[audio_dynamics]" )
{
	const uint32_t uRate = 48000;
	std::mt19937 rng( 1234 );
	std::uniform_real_distribution<float> dist( -1.0f, 1.0f );

	SECTION( "sine bursts: sudden loud onsets after silence, mono and stereo" )
	{
		for ( double flPhase : { 0.0, kPi / 2.0 } ) // onset at a zero crossing and at a peak
			for ( double flHz : { 60.0, 440.0, 3000.0 } )
			{
				Processor proc;
				REQUIRE( proc.Prepare( uRate, 2 ) );
				proc.SetParams( PresetParams( Preset::LimiterOnly ) );

				Planes planes( 2, std::vector<float>( uRate, 0.0f ) );
				for ( size_t nStart : { 4800u, 21000u, 30001u } )
				{
					auto vBurst = Sine( 2400, uRate, flHz, 1.0, flPhase );
					for ( size_t i = 0; i < vBurst.size(); i++ )
					{
						planes[0][nStart + i] = vBurst[i];
						planes[1][nStart + i] = -0.7f * vBurst[i];
					}
				}
				INFO( "phase " << flPhase << " hz " << flHz );
				RequireLimited( proc, planes, -1.0f );
			}
	}

	SECTION( "a steady full-scale sine settles at the ceiling, not far below it" )
	{
		Processor proc;
		REQUIRE( proc.Prepare( uRate, 1 ) );
		proc.SetParams( PresetParams( Preset::LimiterOnly ) );
		Planes planes{ Sine( uRate, uRate, 1000.0, 1.0 ) };
		RequireLimited( proc, planes, -1.0f );
		double flDb = LinToDb( PeakOf( planes[0], uRate / 2, uRate ) );
		REQUIRE( flDb > -1.5 );
		REQUIRE( flDb <= -0.99 );
	}

	SECTION( "clipped full-scale noise, and input beyond full scale" )
	{
		for ( float flDrive : { 1.0f, 8.0f, 100.0f } )
		{
			Processor proc;
			REQUIRE( proc.Prepare( uRate, 2 ) );
			proc.SetParams( PresetParams( Preset::LimiterOnly ) );
			Planes planes( 2, std::vector<float>( uRate ) );
			for ( auto &vPlane : planes )
				for ( float &fl : vPlane )
					fl = std::min( std::max( dist( rng ) * flDrive, -1.0f ), 1.0f ) * ( flDrive > 50.0f ? 4.0f : 1.0f );
			INFO( "drive " << flDrive );
			RequireLimited( proc, planes, -1.0f );
		}
	}

	SECTION( "make-up gain driving it hard (every preset, several ceilings)" )
	{
		for ( Preset ePreset : { Preset::EvenOut, Preset::Night } )
			for ( float flCeilingDb : { -1.0f, -6.0f, 0.0f } )
			{
				Processor proc;
				REQUIRE( proc.Prepare( uRate, 2 ) );
				Params p = PresetParams( ePreset );
				p.flMakeupDb = 24.0f;
				p.flCeilingDb = flCeilingDb;
				proc.SetParams( p );

				Planes planes( 2, std::vector<float>( uRate ) );
				for ( size_t i = 0; i < planes[0].size(); i++ )
				{
					// Loud bursts over a noise floor, so the compressor is moving too.
					float flEnv = ( i / 6000 ) % 3 == 0 ? 1.0f : 0.05f;
					planes[0][i] = dist( rng ) * flEnv;
					planes[1][i] = dist( rng ) * flEnv;
				}
				INFO( "ceiling " << flCeilingDb );
				RequireLimited( proc, planes, flCeilingDb );
			}
	}
}

TEST_CASE( "Dynamics: look-ahead is at most 2 ms and the output is delayed by exactly that", "[audio_dynamics]" )
{
	for ( uint32_t uRate : { 44100u, 48000u, 96000u, 192000u } )
	{
		Processor proc;
		REQUIRE( proc.Prepare( uRate, 2 ) );
		proc.SetParams( PresetParams( Preset::LimiterOnly ) );

		uint32_t uLatency = proc.LatencyFrames();
		REQUIRE( uLatency == Processor::LookaheadFrames( uRate ) );
		REQUIRE( uLatency > 0 );
		REQUIRE( 1000.0 * uLatency / uRate <= 2.0 );

		// An impulse well under the ceiling (so no gain change), fed in odd blocks.
		const size_t nImpulse = 100, nTotal = 3000;
		Planes planes( 2, std::vector<float>( nTotal, 0.0f ) );
		planes[0][nImpulse] = 0.25f;
		planes[1][nImpulse + 5] = -0.5f;
		Run( proc, planes, 37 );

		for ( size_t i = 0; i < nTotal; i++ )
		{
			float flWant0 = i == nImpulse + uLatency ? 0.25f : 0.0f;
			float flWant1 = i == nImpulse + 5 + uLatency ? -0.5f : 0.0f;
			INFO( "rate " << uRate << " frame " << i );
			REQUIRE( std::fabs( planes[0][i] - flWant0 ) < 1e-6f );
			REQUIRE( std::fabs( planes[1][i] - flWant1 ) < 1e-6f );
		}
	}
}

TEST_CASE( "Dynamics: Off is a bit-exact, zero-delay pass-through", "[audio_dynamics]" )
{
	std::mt19937 rng( 99 );
	std::uniform_real_distribution<float> dist( -5.0f, 5.0f ); // well outside [-1, 1]

	Processor proc;
	REQUIRE( proc.Prepare( 48000, 3 ) );
	REQUIRE( !PresetParams( Preset::Off ).bEnabled );
	proc.SetParams( PresetParams( Preset::Off ) );
	REQUIRE( proc.LatencyFrames() == 0 );

	Planes planes( 3, std::vector<float>( 5000 ) );
	for ( auto &vPlane : planes )
		for ( float &fl : vPlane )
			fl = dist( rng );
	// NaN and inf are passed through too: Off touches nothing.
	planes[1][10] = std::numeric_limits<float>::quiet_NaN();
	planes[2][11] = std::numeric_limits<float>::infinity();
	Planes orig = planes;

	Run( proc, planes, 480 );
	for ( size_t c = 0; c < planes.size(); c++ )
		REQUIRE( std::memcmp( planes[c].data(), orig[c].data(), planes[c].size() * sizeof( float ) ) == 0 );

	SECTION( "also after being on and switched off again" )
	{
		proc.SetParams( PresetParams( Preset::Night ) );
		REQUIRE( proc.LatencyFrames() > 0 );
		Planes loud = orig;
		Run( proc, loud, 480 );
		proc.SetParams( PresetParams( Preset::Off ) );
		REQUIRE( proc.LatencyFrames() == 0 );
		// Switching off is an 8 ms fade, not a cut; once it has run out (here: a
		// quiet stretch longer than that) the pass-through is bit-exact again.
		Planes quiet( 3, std::vector<float>( 1000, 0.0f ) );
		Run( proc, quiet, 333 );
		Planes again = orig;
		Run( proc, again, 333 );
		for ( size_t c = 0; c < again.size(); c++ )
			REQUIRE( std::memcmp( again[c].data(), orig[c].data(), again[c].size() * sizeof( float ) ) == 0 );
	}
}

TEST_CASE( "Dynamics: not prepared is a pass-through", "[audio_dynamics]" )
{
	Processor proc;
	proc.SetParams( PresetParams( Preset::Night ) );
	REQUIRE( proc.LatencyFrames() == 0 );
	Planes planes{ { 0.5f, -2.0f, 0.25f } };
	Planes orig = planes;
	Run( proc, planes, 3 );
	REQUIRE( planes == orig );
	REQUIRE( !proc.Prepare( 48000, 0 ) );
	REQUIRE( !proc.Prepare( 48000, kMaxChannels + 1 ) );
	REQUIRE( !proc.Prepare( 100, 2 ) );
	Run( proc, planes, 3 );
	REQUIRE( planes == orig );
}

TEST_CASE( "Dynamics: NaN and inf input never reach the output", "[audio_dynamics]" )
{
	std::mt19937 rng( 7 );
	std::uniform_real_distribution<float> dist( -1.0f, 1.0f );
	const float flNaN = std::numeric_limits<float>::quiet_NaN();
	const float flInf = std::numeric_limits<float>::infinity();

	for ( Preset ePreset : { Preset::EvenOut, Preset::Night, Preset::LimiterOnly } )
	{
		Processor proc;
		REQUIRE( proc.Prepare( 48000, 2 ) );
		proc.SetParams( PresetParams( ePreset ) );

		Planes planes( 2, std::vector<float>( 20000 ) );
		for ( auto &vPlane : planes )
			for ( float &fl : vPlane )
				fl = dist( rng );
		for ( size_t i = 100; i < 20000; i += 1777 )
		{
			planes[0][i] = flNaN;
			planes[1][i + 3] = flInf;
			planes[0][i + 5] = -flInf;
			planes[1][i + 9] = 3.0e38f; // finite, but gain * this would overflow
		}
		Run( proc, planes, 480 );

		const float flCeil = DbToLin( -1.0f );
		for ( auto &vPlane : planes )
			for ( float fl : vPlane )
			{
				REQUIRE( std::isfinite( fl ) );
				REQUIRE( std::fabs( fl ) <= flCeil * 1.00001f ); // the processor's own exp2f ceiling may differ by an ulp
			}
		// The processor must still be sane afterwards: a quiet sine passes.
		Planes after{ Sine( 24000, 48000, 1000.0, 0.1 ), Sine( 24000, 48000, 1000.0, 0.1 ) };
		Run( proc, after, 480 );
		REQUIRE( PeakOf( after[0], 20000, 24000 ) > 0.01f );
	}

	SECTION( "garbage parameters" )
	{
		Processor proc;
		REQUIRE( proc.Prepare( 48000, 1 ) );
		Params p = PresetParams( Preset::Night );
		p.flThresholdDb = flNaN;
		p.flRatio = flInf;
		p.flAttackMs = -5.0f;
		p.flReleaseMs = flNaN;
		p.flMakeupDb = 1.0e9f;
		p.flCeilingDb = 50.0f;
		proc.SetParams( p );
		Planes planes{ Sine( 9600, 48000, 300.0, 1.0 ) };
		Run( proc, planes, 480 );
		for ( float fl : planes[0] )
		{
			REQUIRE( std::isfinite( fl ) );
			REQUIRE( std::fabs( fl ) <= 1.0f );
		}
	}
}

TEST_CASE( "Dynamics: channels are linked (one gain for all of them)", "[audio_dynamics]" )
{
	// A loud left channel and a quiet, different right channel. The right one
	// must be turned down by exactly the factor the left one is, at every frame,
	// including while the gain moves (loud half, then a quiet half).
	const uint32_t uRate = 48000;
	Processor proc;
	REQUIRE( proc.Prepare( uRate, 2 ) );
	proc.SetParams( PresetParams( Preset::Night ) );

	const size_t nTotal = uRate;
	Planes planes{ Sine( nTotal, uRate, 700.0, 0.9 ), Sine( nTotal, uRate, 310.0, 0.05, 0.3 ) };
	for ( size_t i = nTotal / 2; i < nTotal; i++ )
		planes[0][i] *= 0.1f;
	Planes orig = planes;
	Run( proc, planes, 480 );

	const size_t nLatency = proc.LatencyFrames();
	size_t nChecked = 0;
	double flMinGain = 1e9, flMaxGain = 0.0;
	for ( size_t i = 0; i + nLatency < nTotal; i++ )
	{
		float flInL = orig[0][i], flInR = orig[1][i];
		if ( std::fabs( flInL ) < 0.05f || std::fabs( flInR ) < 0.01f )
			continue;
		double flGainL = (double)planes[0][i + nLatency] / flInL;
		double flGainR = (double)planes[1][i + nLatency] / flInR;
		REQUIRE( std::fabs( flGainL - flGainR ) < 1e-4 * std::fabs( flGainL ) );
		flMinGain = std::min( flMinGain, flGainL );
		flMaxGain = std::max( flMaxGain, flGainL );
		nChecked++;
	}
	REQUIRE( nChecked > 1000 );
	REQUIRE( flMaxGain > flMinGain * 2.0 ); // the gain really did move
}

TEST_CASE( "Dynamics: the result does not depend on the block size", "[audio_dynamics]" )
{
	std::mt19937 rng( 5 );
	std::uniform_real_distribution<float> dist( -1.0f, 1.0f );
	Planes orig( 2, std::vector<float>( 30000 ) );
	for ( size_t i = 0; i < orig[0].size(); i++ )
	{
		float flEnv = ( i / 3000 ) % 2 ? 1.0f : 0.03f;
		orig[0][i] = dist( rng ) * flEnv;
		orig[1][i] = dist( rng ) * flEnv * 0.5f;
	}

	for ( Preset ePreset : { Preset::EvenOut, Preset::Night, Preset::LimiterOnly } )
	{
		Processor procBig;
		REQUIRE( procBig.Prepare( 44100, 2 ) );
		procBig.SetParams( PresetParams( ePreset ) );
		Planes big = orig;
		Run( procBig, big, orig[0].size() );

		Processor procSmall;
		REQUIRE( procSmall.Prepare( 44100, 2 ) );
		procSmall.SetParams( PresetParams( ePreset ) );
		Planes small = orig;
		{
			// Irregular blocks, including 1-frame ones.
			const size_t aBlocks[] = { 1, 7, 64, 333, 1, 1, 1024, 5, 2000, 17 };
			std::vector<float *> vPtrs( 2 );
			size_t nPos = 0, nBlock = 0;
			while ( nPos < small[0].size() )
			{
				size_t nNow = std::min( aBlocks[nBlock++ % 10], small[0].size() - nPos );
				vPtrs[0] = small[0].data() + nPos;
				vPtrs[1] = small[1].data() + nPos;
				procSmall.Process( vPtrs.data(), (uint32_t)nNow );
				nPos += nNow;
			}
		}
		for ( size_t c = 0; c < 2; c++ )
			for ( size_t i = 0; i < big[c].size(); i++ )
				REQUIRE( std::fabs( big[c][i] - small[c][i] ) < 1e-6f );
	}
}

TEST_CASE( "Dynamics: Prepare again at a new rate or channel count", "[audio_dynamics]" )
{
	Processor proc;
	proc.SetParams( PresetParams( Preset::LimiterOnly ) );

	for ( auto [uRate, uChannels] : { std::pair{ 48000u, 2u }, std::pair{ 44100u, 6u }, std::pair{ 96000u, 1u }, std::pair{ 48000u, 8u } } )
	{
		REQUIRE( proc.Prepare( uRate, uChannels ) );
		REQUIRE( proc.LatencyFrames() == Processor::LookaheadFrames( uRate ) );

		Planes planes( uChannels, std::vector<float>( 2000, 0.0f ) );
		planes[uChannels - 1][50] = 0.5f;
		Run( proc, planes, 480 );
		for ( size_t i = 0; i < 2000; i++ )
			REQUIRE( std::fabs( planes[uChannels - 1][i] - ( i == 50 + proc.LatencyFrames() ? 0.5f : 0.0f ) ) < 1e-6f );
	}
}

TEST_CASE( "Dynamics: presets", "[audio_dynamics]" )
{
	REQUIRE( !PresetParams( Preset::Off ).bEnabled );
	for ( Preset ePreset : { Preset::EvenOut, Preset::Night, Preset::LimiterOnly } )
	{
		Params p = PresetParams( ePreset );
		REQUIRE( p.bEnabled );
		REQUIRE( p.flCeilingDb == -1.0f );
	}
	REQUIRE( PresetParams( Preset::LimiterOnly ).flRatio == 1.0f );
	REQUIRE( PresetParams( Preset::LimiterOnly ).flMakeupDb == 0.0f );
	REQUIRE( std::string( PresetName( Preset::EvenOut ) ) == "Even out" );
	REQUIRE( std::string( PresetName( Preset::LimiterOnly ) ) == "Limiter only" );

	SECTION( "Even out lifts footsteps relative to loud sounds" )
	{
		// A quiet 1 kHz tone (-40 dBFS) and a loud one (-6 dBFS), run separately.
		auto fnLevel = [&]( double flInDb ) {
			Processor proc;
			REQUIRE( proc.Prepare( 48000, 1 ) );
			proc.SetParams( PresetParams( Preset::EvenOut ) );
			Planes planes{ Sine( 48000, 48000, 1000.0, DbToLin( flInDb ) ) };
			Run( proc, planes, 480 );
			return LinToDb( PeakOf( planes[0], 36000, 48000 ) );
		};
		double flQuiet = fnLevel( -40.0 ), flLoud = fnLevel( -6.0 );
		REQUIRE( flLoud - flQuiet < 34.0 - 10.0 ); // 34 dB apart in, well under 24 dB out
		REQUIRE( flQuiet > -40.0 + 5.0 );          // and the quiet one is actually louder
	}
}

TEST_CASE( "Dynamics: toggling is click-free", "[audio_dynamics]" )
{
	// A steady loud sine (so a cut between delayed/live or silent/live would be a
	// big step), toggled on, off, and through preset switches. The largest
	// sample-to-sample jump of the output may not exceed the sine's own largest
	// step by more than a small margin: no click.
	const uint32_t uRate = 48000;
	const double flHz = 400.0, flAmp = 0.3;
	const double flOwnStep = flAmp * 2.0 * kPi * flHz / uRate; // the sine's largest per-sample step
	const size_t nTotal = uRate;
	Planes planes{ Sine( nTotal, uRate, flHz, flAmp ), Sine( nTotal, uRate, flHz, flAmp, 0.5 ) };

	Processor proc;
	REQUIRE( proc.Prepare( uRate, 2 ) );
	proc.SetParams( PresetParams( Preset::Off ) );

	// Block by block with a parameter change at fixed frames.
	struct Change { size_t nAt; Preset ePreset; };
	const Change aChanges[] = {
		{ 6000, Preset::LimiterOnly }, { 15000, Preset::Off }, { 24001, Preset::EvenOut },
		{ 30000, Preset::Night }, { 36000, Preset::EvenOut }, { 42000, Preset::Off } };
	size_t nNext = 0;
	std::vector<float *> vPtrs( 2 );
	const size_t nBlock = 480;
	for ( size_t nPos = 0; nPos < nTotal; nPos += nBlock )
	{
		if ( nNext < std::size( aChanges ) && aChanges[nNext].nAt <= nPos )
			proc.SetParams( PresetParams( aChanges[nNext++].ePreset ) );
		vPtrs[0] = planes[0].data() + nPos;
		vPtrs[1] = planes[1].data() + nPos;
		proc.Process( vPtrs.data(), (uint32_t)std::min( nBlock, nTotal - nPos ) );
	}
	REQUIRE( nNext == std::size( aChanges ) );

	for ( auto &vPlane : planes )
	{
		double flMaxJump = 0.0;
		size_t nAt = 0;
		for ( size_t i = 1; i < vPlane.size(); i++ )
			if ( std::fabs( vPlane[i] - vPlane[i - 1] ) > flMaxJump )
				flMaxJump = std::fabs( vPlane[i] - vPlane[i - 1] ), nAt = i;
		INFO( "max jump " << flMaxJump << " at " << nAt << " vs the sine's own " << flOwnStep );
		// Even out / Night change the level (a +6/+10 dB make-up on a -10 dBFS
		// sine), so the steady output may legitimately step a few times larger;
		// a click is an order of magnitude beyond that (a cut is up to 2*amp).
		REQUIRE( flMaxJump < 3.0 * flOwnStep );
	}
}

TEST_CASE( "Dynamics: turning on stays dry for the look-ahead, then ends up fully processed", "[audio_dynamics]" )
{
	const uint32_t uRate = 48000;
	Processor proc;
	REQUIRE( proc.Prepare( uRate, 1 ) );
	proc.SetParams( PresetParams( Preset::Off ) );

	// Run Off for a while (so the processor is no longer "fresh"), then on.
	Planes warm{ Sine( 2000, uRate, 300.0, 0.1 ) };
	Run( proc, warm, 480 );
	proc.SetParams( PresetParams( Preset::LimiterOnly ) );

	// 0.1-amplitude sine: the limiter does nothing, so the fully-on output is the
	// input delayed by the look-ahead. Right after the switch it is the (undelayed)
	// input, and in the end the delayed one.
	Planes planes{ Sine( 6000, uRate, 300.0, 0.1 ) };
	Planes orig = planes;
	Run( proc, planes, 480 );
	const size_t nLook = proc.LatencyFrames();
	REQUIRE( nLook > 0 );
	for ( size_t i = 0; i < 40; i++ )
		REQUIRE( planes[0][i] == orig[0][i] ); // dry, not a silent gap
	for ( size_t i = 3000; i < 6000; i++ )
		REQUIRE( std::fabs( planes[0][i] - orig[0][i - nLook] ) < 1e-5f );
}

TEST_CASE( "Dynamics: make-up gain glides instead of stepping", "[audio_dynamics]" )
{
	const uint32_t uRate = 48000;
	Processor proc;
	REQUIRE( proc.Prepare( uRate, 1 ) );
	Params p = PresetParams( Preset::LimiterOnly );
	p.flMakeupDb = 0.0f;
	proc.SetParams( p );

	Planes planes{ std::vector<float>( 24000, 0.05f ) }; // DC, so the level IS the sample
	std::vector<float *> vPtrs( 1 );
	for ( size_t nPos = 0; nPos < planes[0].size(); nPos += 480 )
	{
		if ( nPos == 9600 )
		{
			p.flMakeupDb = 12.0f;
			proc.SetParams( p );
		}
		vPtrs[0] = planes[0].data() + nPos;
		proc.Process( vPtrs.data(), 480 );
	}
	double flMaxStep = 0.0;
	for ( size_t i = 9000; i < 12000; i++ )
		flMaxStep = std::max( flMaxStep, (double)std::fabs( planes[0][i] - planes[0][i - 1] ) );
	// 0.05 -> 0.2 over ~10 ms (480 frames); a step would be 0.15.
	REQUIRE( flMaxStep < 0.005 );
	REQUIRE( std::fabs( planes[0][20000] - 0.05f * 3.98107f ) < 0.002f );
}
