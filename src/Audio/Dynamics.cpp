#include "Audio/Dynamics.h"

#include <algorithm>
#include <cmath>
#include <cstring>

// Built with -ffast-math (meson.build), which lets the compiler assume floats
// are never NaN or inf: std::isnan()/isfinite() may fold to "true". Everything
// that must survive bad input therefore looks at the bits instead.

namespace gamescope::Audio::Dynamics
{
	namespace
	{
		constexpr float kKneeDb = 6.0f;             // soft knee, internal
		constexpr float kLimiterReleaseMs = 50.0f;  // internal: not a user parameter
		constexpr float kLookaheadMs = 1.5f;
		constexpr float kMaxInput = 1.0e6f;         // keeps gain * input finite
		constexpr float kDbPerLog2 = 6.02059991f;   // 20 * log10( 2 )

		bool IsFinite( float fl )
		{
			uint32_t u;
			std::memcpy( &u, &fl, sizeof( u ) );
			return ( u & 0x7f800000u ) != 0x7f800000u;
		}

		// A parameter that is NaN/inf (or out of range) must not poison the state.
		float Sane( float fl, float flLo, float flHi, float flFallback )
		{
			if ( !IsFinite( fl ) )
				return flFallback;
			return std::min( std::max( fl, flLo ), flHi );
		}

		float DbToLinear( float flDb ) { return exp2f( flDb * ( 1.0f / kDbPerLog2 ) ); }

		// One-pole coefficient for a time constant in ms.
		float TimeCoef( float flMs, uint32_t uRate ) { return expf( -1.0f / ( flMs * 0.001f * (float)uRate ) ); }
	}

	Params PresetParams( Preset ePreset )
	{
		Params p;
		switch ( ePreset )
		{
		case Preset::EvenOut:
			p = { true, -24.0f, 3.0f, 10.0f, 150.0f, 6.0f, -1.0f };
			break;
		case Preset::Night:
			p = { true, -30.0f, 6.0f, 5.0f, 200.0f, 10.0f, -1.0f };
			break;
		case Preset::LimiterOnly:
			p = { true, 0.0f, 1.0f, 10.0f, 100.0f, 0.0f, -1.0f };
			break;
		case Preset::Off:
		default:
			break; // bEnabled = false
		}
		return p;
	}

	const char *PresetName( Preset ePreset )
	{
		switch ( ePreset )
		{
		case Preset::EvenOut: return "Even out";
		case Preset::Night: return "Night";
		case Preset::LimiterOnly: return "Limiter only";
		case Preset::Off:
		default: return "Off";
		}
	}

	uint32_t Processor::LookaheadFrames( uint32_t uSampleRate )
	{
		// ceil( rate * 1.5 ms ), in integers: 44.1 kHz -> 67 (1.52 ms), 48 -> 72.
		return ( uSampleRate * 3u + 1999u ) / 2000u;
	}

	bool Processor::Prepare( uint32_t uSampleRate, uint32_t uChannels )
	{
		m_uChannels = 0;
		if ( uChannels < 1 || uChannels > kMaxChannels || uSampleRate < 8000 || uSampleRate > 384000 )
			return false;

		m_uSampleRate = uSampleRate;
		m_uLookahead = LookaheadFrames( uSampleRate );
		m_flLimiterReleaseCoef = TimeCoef( kLimiterReleaseMs, uSampleRate );
		m_vDelay.assign( (size_t)uChannels * m_uLookahead, 0.0f );
		m_vGainRing.assign( m_uLookahead + 1, 1.0f );
		m_vMinIndex.assign( m_uLookahead + 1, 0 );
		m_vMinValue.assign( m_uLookahead + 1, 1.0f );
		m_uChannels = uChannels;
		Reset();
		return true;
	}

	void Processor::Reset()
	{
		std::fill( m_vDelay.begin(), m_vDelay.end(), 0.0f );
		std::fill( m_vGainRing.begin(), m_vGainRing.end(), 1.0f );
		m_dGainSum = (double)m_vGainRing.size();
		m_flCompHeld = 0.0f;
		m_flCompReduction = 0.0f;
		m_flLimiterGain = 1.0f;
		m_nDelayPos = 0;
		m_nGainPos = 0;
		m_nMinHead = 0;
		m_nMinCount = 0;
		m_uFrame = 0;
	}

	void Processor::SetParams( const Params &p )
	{
		m_flThresholdDb.store( p.flThresholdDb, std::memory_order_relaxed );
		m_flRatio.store( p.flRatio, std::memory_order_relaxed );
		m_flAttackMs.store( p.flAttackMs, std::memory_order_relaxed );
		m_flReleaseMs.store( p.flReleaseMs, std::memory_order_relaxed );
		m_flMakeupDb.store( p.flMakeupDb, std::memory_order_relaxed );
		m_flCeilingDb.store( p.flCeilingDb, std::memory_order_relaxed );
		m_bEnabled.store( p.bEnabled, std::memory_order_relaxed );
	}

	uint32_t Processor::LatencyFrames() const
	{
		return ( m_uChannels && m_bEnabled.load( std::memory_order_relaxed ) ) ? m_uLookahead : 0;
	}

	void Processor::Process( float *const *ppChannels, uint32_t uFrames )
	{
		if ( !m_uChannels || !m_bEnabled.load( std::memory_order_relaxed ) )
		{
			m_bWasOn = false; // Off: leave the audio (and the delay line) alone
			return;
		}
		if ( !m_bWasOn )
		{
			Reset(); // coming on: start from silence, not from stale audio
			m_bWasOn = true;
		}

		// Snapshot the parameters once per block.
		const float flThresholdDb = Sane( m_flThresholdDb.load( std::memory_order_relaxed ), -80.0f, 0.0f, 0.0f );
		const float flRatio = Sane( m_flRatio.load( std::memory_order_relaxed ), 1.0f, 20.0f, 1.0f );
		const float flAttack = TimeCoef( Sane( m_flAttackMs.load( std::memory_order_relaxed ), 0.1f, 500.0f, 10.0f ), m_uSampleRate );
		const float flRelease = TimeCoef( Sane( m_flReleaseMs.load( std::memory_order_relaxed ), 1.0f, 5000.0f, 150.0f ), m_uSampleRate );
		const float flMakeupDb = Sane( m_flMakeupDb.load( std::memory_order_relaxed ), -24.0f, 24.0f, 0.0f );
		const float flCeiling = DbToLinear( Sane( m_flCeilingDb.load( std::memory_order_relaxed ), -24.0f, 0.0f, -1.0f ) );
		const float flMakeup = DbToLinear( flMakeupDb );
		const bool bCompress = flRatio > 1.0001f;
		const float flSlope = 1.0f - 1.0f / flRatio; // dB removed per dB over the threshold

		const size_t nLook = m_uLookahead;
		const size_t nWindow = nLook + 1; // the limiter's window: now + L frames back
		const double dInvWindow = 1.0 / (double)nWindow;
		float *pDelay = m_vDelay.data();

		for ( uint32_t n = 0; n < uFrames; n++ )
		{
			// Detector: the loudest channel of this frame. Bad input becomes 0 here
			// and is written back, so nothing downstream ever sees a NaN.
			float flPeak = 0.0f;
			for ( uint32_t c = 0; c < m_uChannels; c++ )
			{
				float x = ppChannels[c][n];
				if ( !IsFinite( x ) )
					x = 0.0f;
				x = std::min( std::max( x, -kMaxInput ), kMaxInput );
				ppChannels[c][n] = x;
				flPeak = std::max( flPeak, std::fabs( x ) );
			}

			// ---- Compressor: soft-knee static curve, then attack/release on the
			// reduction in dB ("smooth decoupled peak detector": the release-held
			// maximum is what the attack smooths, so a sine's reduction does not
			// ripple with its waveform and the steady-state curve stays exact).
			float flGain = flMakeup;
			if ( bCompress )
			{
				float flOver = kDbPerLog2 * log2f( std::max( flPeak, 1.0e-9f ) ) - flThresholdDb;
				float flReduction = 0.0f;
				if ( 2.0f * flOver >= kKneeDb )
					flReduction = flSlope * flOver;
				else if ( 2.0f * flOver > -kKneeDb )
				{
					float flK = flOver + 0.5f * kKneeDb;
					flReduction = flSlope * flK * flK / ( 2.0f * kKneeDb );
				}
				m_flCompHeld = std::max( flReduction, flRelease * m_flCompHeld + ( 1.0f - flRelease ) * flReduction );
				if ( m_flCompHeld < 1.0e-6f )
					m_flCompHeld = 0.0f; // no denormals while idle
				m_flCompReduction = flAttack * m_flCompReduction + ( 1.0f - flAttack ) * m_flCompHeld;
				flGain = DbToLinear( flMakeupDb - m_flCompReduction );
			}

			// ---- Limiter. The gain needed to bring this frame's (post-compressor)
			// peak to the ceiling...
			float flSPeak = flPeak * flGain;
			float flReq = flSPeak > flCeiling ? flCeiling / flSPeak : 1.0f;

			// ...minimum over the last L+1 frames (monotonic deque: O(1) a frame)...
			const size_t nCap = nWindow;
			while ( m_nMinCount && m_vMinIndex[m_nMinHead] + nLook < m_uFrame )
			{
				m_nMinHead = ( m_nMinHead + 1 ) % nCap;
				m_nMinCount--;
			}
			while ( m_nMinCount && m_vMinValue[( m_nMinHead + m_nMinCount - 1 ) % nCap] >= flReq )
				m_nMinCount--;
			size_t nBack = ( m_nMinHead + m_nMinCount ) % nCap;
			m_vMinIndex[nBack] = m_uFrame;
			m_vMinValue[nBack] = flReq;
			m_nMinCount++;
			float flTarget = m_vMinValue[m_nMinHead];

			// ...with a smooth release. min() keeps r <= target, i.e. instant attack.
			m_flLimiterGain = std::min( flTarget, m_flLimiterReleaseCoef * m_flLimiterGain + ( 1.0f - m_flLimiterReleaseCoef ) * flTarget );

			// ...averaged over the same L+1 frames (a boxcar, so the gain ramps down
			// linearly instead of stepping). Why this holds the ceiling: the sample
			// leaving the delay line now entered L frames ago, and every one of the
			// L+1 terms averaged is a limiter gain computed at or after that
			// moment, whose minimum window contained that sample - so each term, and
			// therefore their mean, is <= the gain that sample needed.
			float &rOld = m_vGainRing[m_nGainPos];
			m_dGainSum += (double)m_flLimiterGain - (double)rOld;
			rOld = m_flLimiterGain;
			m_nGainPos = ( m_nGainPos + 1 ) % nWindow;
			float flLimit = (float)( m_dGainSum * dInvWindow );

			// ---- Out: the delayed compressed sample times the limiter gain.
			for ( uint32_t c = 0; c < m_uChannels; c++ )
			{
				float *pSlot = pDelay + (size_t)c * nLook + m_nDelayPos;
				float y = *pSlot * flLimit;
				*pSlot = ppChannels[c][n] * flGain;

				// The backstop: float rounding in the sum, or a ceiling that moved
				// mid-window, can leave a hair over; clamp it, and count real misses.
				if ( std::fabs( y ) > flCeiling )
				{
					if ( std::fabs( y ) > flCeiling * 1.0001f )
						m_uBackstopHits++;
					y = std::min( std::max( y, -flCeiling ), flCeiling );
				}
				ppChannels[c][n] = y;
			}
			m_nDelayPos = ( m_nDelayPos + 1 ) % nLook;
			m_uFrame++;
		}
	}
}
