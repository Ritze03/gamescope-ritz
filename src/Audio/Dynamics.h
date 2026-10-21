// Audio dynamics for the game's captured audio: a stereo-linked compressor
// followed by a look-ahead peak limiter. Pure DSP (no PipeWire, no I/O), so the
// unit tests drive it offline; the live call site is the capture sink's
// playback process callback (src/Audio/Capture.cpp), on PipeWire's realtime
// thread, between the per-plane copy and the final [-1,1] clamp.
//
//   Why own code: no plugin dependency (plan D6).
//   Why ONE gain for all channels (the detector is the loudest channel of each
//   frame): per-channel gains would shift the stereo image whenever one side is
//   louder - in a shooter the direction of a footstep IS the information.
//   Why the limiter is the only delay: the compressor reacts to what it has
//   already heard, so it needs none; the limiter must know a peak is coming to
//   be at the right gain when it arrives (plan D14: <= 2 ms total).
//
// ---- Threading contract (for the caller that wires this in) --------------
//   Prepare()        non-RT, NOT concurrent with Process(). Allocates. Call it
//                    again whenever the rate or channel count changes (it
//                    resets all state).
//   SetParams()      any thread, any time, wait-free (relaxed atomics). Process()
//                    snapshots the parameters once per block, so a change takes
//                    effect at the next block. Calls must be serialised among
//                    themselves (one UI thread), as a block could otherwise see
//                    a mix of two calls' values - harmless but not intended.
//   Process()        RT thread only, one thread. No allocation, locks,
//                    syscalls or logging.
//   LatencyFrames()  any thread.
//
// ---- Switching on and off -----------------------------------------------
// Off (settled) is a bit-exact, zero-delay pass-through that never touches the
// delay line. A toggle is a crossfade, not a cut: turning on keeps the output dry
// for the 1.5 ms the delay line needs to fill and then fades to the processed
// signal over 8 ms; turning off fades back over 8 ms and then goes quiet. The
// make-up gain glides (10 ms) so a preset switch is not a step in level. During
// the fades the output is a blend with the unprocessed signal, so the ceiling
// is only guaranteed once fully on (and from the first frame for a processor that
// is already enabled when its first block arrives).
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace gamescope::Audio::Dynamics
{
	// What the user can set: the "Advanced" parameters, and what a preset is.
	// The knee width and the limiter's release are internal constants.
	struct Params
	{
		bool bEnabled = false;       // false: bit-exact pass-through, zero delay
		float flThresholdDb = -24.0f; // dBFS, where compression starts
		float flRatio = 1.0f;         // x:1; 1 = compressor bypassed
		float flAttackMs = 10.0f;
		float flReleaseMs = 150.0f;
		float flMakeupDb = 0.0f;
		float flCeilingDb = -1.0f;    // dBFS, the limiter's output ceiling
	};

	enum class Preset : uint8_t
	{
		Off,
		EvenOut,
		Night,
		LimiterOnly,
	};
	inline constexpr size_t kPresetCount = 4;

	// A preset is a complete Params set (the UI shows and edits it as such).
	// Values (all with the limiter at -1 dBFS, so a sum of sounds can't clip):
	//   Even out      -24 dBFS, 3:1, 10 ms, 150 ms, +6 dB: footsteps come up
	//                 relative to gunfire without pumping. 10/150 ms is slow
	//                 enough not to ride individual transients, and +6 dB means
	//                 0 dBFS lands at -10 and -48 dBFS at -42.
	//   Night         -30 dBFS, 6:1, 5 ms, 200 ms, +10 dB: a hard squeeze for
	//                 quiet hours (50 dB of input range becomes ~25 dB).
	//   Limiter only compressor bypassed (1:1, 0 dB make-up); only peaks change.
	Params PresetParams( Preset ePreset );
	const char *PresetName( Preset ePreset );

	inline constexpr uint32_t kMaxChannels = 8;

	class Processor
	{
	public:
		// Non-RT. false (and Process() stays a no-op pass-through) for 0 or more
		// than kMaxChannels channels or a rate outside 8-384 kHz.
		bool Prepare( uint32_t uSampleRate, uint32_t uChannels );

		void SetParams( const Params &params );

		// Look-ahead delay in frames the audio currently gets: 0 while disabled.
		uint32_t LatencyFrames() const;

		// The look-ahead for a rate, ~1.5 ms (never more than 2 ms at any rate).
		static uint32_t LookaheadFrames( uint32_t uSampleRate );

		// In place on planar float: ppChannels[c][0..uFrames). RT-safe.
		void Process( float *const *ppChannels, uint32_t uFrames );

		// Samples the final hard clamp had to catch beyond 0.001 dB over the
		// ceiling. The limiter is meant to do the work, so anything but 0 is a
		// bug; kept for the tests (and a debug line, if wanted).
		uint64_t BackstopHits() const { return m_uBackstopHits.load( std::memory_order_relaxed ); }

	private:
		void Reset();

		// ---- Set by Prepare() ----
		uint32_t m_uSampleRate = 0;
		uint32_t m_uChannels = 0;
		uint32_t m_uLookahead = 0; // L, frames
		float m_flLimiterReleaseCoef = 0.0f;
		std::vector<float> m_vDelay;       // channels x L, the limiter's look-ahead
		std::vector<float> m_vGainRing;    // L+1 smoothed gains, for the boxcar
		std::vector<uint64_t> m_vMinIndex; // sliding-minimum deque (L+1 slots)
		std::vector<float> m_vMinValue;

		// ---- Parameters (any thread writes, the RT thread reads) ----
		std::atomic<bool> m_bEnabled{ false };
		std::atomic<float> m_flThresholdDb{ -24.0f };
		std::atomic<float> m_flRatio{ 1.0f };
		std::atomic<float> m_flAttackMs{ 10.0f };
		std::atomic<float> m_flReleaseMs{ 150.0f };
		std::atomic<float> m_flMakeupDb{ 0.0f };
		std::atomic<float> m_flCeilingDb{ -1.0f };

		// ---- RT-thread state ----
		struct Snapshot // the last block's parameters while on (held through a fade-out)
		{
			float flThresholdDb = -24.0f, flRatio = 1.0f, flAttackMs = 10.0f, flReleaseMs = 150.0f;
			float flMakeupDb = 0.0f, flCeilingDb = -1.0f;
		} m_Snap;
		bool m_bRunning = false; // state is live: on, or fading out
		bool m_bFresh = true;    // no Process() call since Prepare()
		uint32_t m_uFill = 0;    // frames since coming on, capped at m_uLookahead
		float m_flMix = 0.0f;    // 0 dry .. 1 processed
		float m_flFadeStep = 0.0f;
		float m_flMakeupCoef = 0.0f;
		float m_flMakeupCur = 0.0f; // dB, glides to the target
		float m_flCompHeld = 0.0f;     // dB of reduction: release-held peak
		float m_flCompReduction = 0.0f; // dB of reduction: after the attack smoothing
		float m_flLimiterGain = 1.0f;  // release-smoothed target gain
		double m_dGainSum = 0.0;       // running sum of m_vGainRing
		size_t m_nDelayPos = 0;
		size_t m_nGainPos = 0;
		size_t m_nMinHead = 0; // deque: oldest entry
		size_t m_nMinCount = 0;
		uint64_t m_uFrame = 0;
		std::atomic<uint64_t> m_uBackstopHits{ 0 }; // read by the UI thread
	};
}
