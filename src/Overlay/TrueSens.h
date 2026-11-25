#pragma once

// TrueSens.h -- "True Sens" (2026-11-25): the same hand distance per 360 degree
// turn in every game. The user's own request (the feature request pasted
// 2026-11-25):
//   *"Store two global values, which are the "true sensitivity" of the player.
//   One for "Low" and one for "High". These should be stored globally."*
//   *"All of the values for this should be game specific and fully independent
//   of the selected profile."*
//   *"Also let the player set a DPI, so he can change that, if he changes his
//   DPI on the mouse (ignore that mice arent precise, when it comes to dpi's)"*
//   *"The sensitivity multiplier should only be applied, when the cursor is not
//   visible at the moment (to detect ingame from inventories)"*
// See superdoc/features/true-sens.md for the whole feature.
//
// THE MATHS. A game needs a fixed number of mouse counts for one 360 degree turn
// at its in-game setting: `counts_360`, measured with the multiplier OFF. The
// player's true sensitivity is a hand distance `true_inches` per turn (counts /
// DPI at recording time). Target counts = true_inches * dpi, and
//     multiplier = counts_360 / (true_inches * dpi)
// scales the game's mouse movement so its turn takes exactly the player's own
// distance. The calculator (the same numbers, for a game that should be set in
// its own options instead) is `typed * counts_360 / target_counts`, assuming the
// game's turn per count is proportional to its sensitivity value.
//
// THIS FILE has two halves. The first (namespace `gamescope::truesens`) is PURE:
// no Config / wlserver / ImGui includes, no threads, no clocks, so the unit test
// (tests/test_truesens.cpp) links only this header, the pattern of NullBinds.h.
// The second half, at the bottom, declares the glue TrueSens.cpp implements:
// the mouse hook, the per-frame tick and the interface the settings area, hint
// and picture call.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>

namespace gamescope::truesens
{
	// ---- constants --------------------------------------------------------------

	inline constexpr int kDefaultDpi = 800;
	inline constexpr int kMinDpi = 100;
	inline constexpr int kMaxDpi = 32000;

	// D18: a bad recording must not make the mouse unusable.
	inline constexpr double kMinMultiplier = 0.05;
	inline constexpr double kMaxMultiplier = 20.0;

	// A multiplier this close to 1 is 1: a game recorded at the player's own
	// sensitivity is passed through untouched instead of being quantised to the
	// 1/256 wire grid for no gain.
	inline constexpr double kUnityBand = 0.001;

	// The fewest net counts a recording may end on and still count. A real 360
	// degree turn is thousands of counts (a fast, low-DPI setup is around 3000;
	// 800 DPI at 30 cm per turn is about 9400); a recording below 200 means the
	// player barely moved or, far more likely, that no counts arrived at all
	// (the game hid the cursor without locking the mouse -- see true-sens.md).
	// 200 is high enough to refuse "I pressed Stop straight away" and a nudged
	// mouse, low enough that no real turn is refused.
	inline constexpr double kMinRecordingCounts = 200.0;

	inline constexpr double kCmPerInch = 2.54;

	// The resolution of a wl_fixed value on the wire: relative motion leaves
	// gamescope quantised to 1/256 (see the scout notes in true-sens.md).
	inline constexpr double kWireQuantum = 1.0 / 256.0;

	// ---- number plumbing ---------------------------------------------------------

	// The build uses -ffast-math (meson.build), which lets the compiler assume no
	// NaN or infinity exists and delete a std::isnan()/std::isfinite() branch. A
	// previous worker found exactly that in a unit test. So finiteness is read
	// off the bit pattern: an IEEE double is NaN or infinity exactly when its
	// 11 exponent bits are all ones. Plain integer code, immune to fast-math.
	inline bool IsFinite( double d )
	{
		uint64_t u;
		std::memcpy( &u, &d, sizeof( u ) );
		return ( ( u >> 52 ) & 0x7FFu ) != 0x7FFu;
	}

	// A usable positive quantity: finite and strictly greater than zero.
	inline bool IsPositive( double d )
	{
		return IsFinite( d ) && d > 0.0;
	}

	inline int ClampDpi( int dpi )
	{
		return dpi < kMinDpi ? kMinDpi : ( dpi > kMaxDpi ? kMaxDpi : dpi );
	}

	// ---- conversions -------------------------------------------------------------

	inline double InchesFromCounts( double counts, int dpi )
	{
		if ( !IsPositive( counts ) || dpi <= 0 )
			return 0.0;
		return counts / (double)dpi;
	}

	inline double TargetCounts( double inches, int dpi )
	{
		if ( !IsPositive( inches ) || dpi <= 0 )
			return 0.0;
		return inches * (double)dpi;
	}

	inline double CmFromInches( double inches )
	{
		return inches * kCmPerInch;
	}

	// 1.0 (leave the game alone) for any invalid input: no recorded game counts,
	// no recorded true distance, or anything non-finite. Otherwise clamped to
	// [kMinMultiplier, kMaxMultiplier]; within kUnityBand of 1 it is exactly 1.0
	// (no scaling).
	inline double Multiplier( double game_counts, double true_inches, int dpi )
	{
		if ( !IsPositive( game_counts ) || !IsPositive( true_inches ) || dpi <= 0 )
			return 1.0;
		const double target = true_inches * (double)dpi;
		if ( !IsPositive( target ) )
			return 1.0;
		const double m = game_counts / target;
		if ( !IsPositive( m ) )
			return 1.0;
		const double c = m < kMinMultiplier ? kMinMultiplier : ( m > kMaxMultiplier ? kMaxMultiplier : m );
		return std::fabs( c - 1.0 ) < kUnityBand ? 1.0 : c;
	}

	// The calculator: the in-game value to set so the game's turn matches the
	// true sensitivity, given the value `typed` the game was recorded at.
	// nullopt for invalid input. Not clamped -- it is a number to type into
	// another program, not something this program applies.
	inline std::optional<double> IngameSensForTarget( double typed, double game_counts, double true_inches, int dpi )
	{
		if ( !IsPositive( typed ) || !IsPositive( game_counts ) )
			return std::nullopt;
		const double target = TargetCounts( true_inches, dpi );
		if ( !IsPositive( target ) )
			return std::nullopt;
		const double v = typed * game_counts / target;
		if ( !IsPositive( v ) )
			return std::nullopt;
		return v;
	}

	// ---- the recorder ------------------------------------------------------------

	enum class Target : int
	{
		Idle = 0, // no recording (not "None": X11/X.h #defines None)
		Low,
		High,
		Game,
	};

	struct OpResult
	{
		bool ok = true;
		std::string reason; // plain language, set when !ok
	};

	struct RecordResult
	{
		bool ok = false;
		Target target = Target::Idle;
		double counts = 0.0;  // |net X sum|, the number a valid recording stores
		double sum_y = 0.0;   // diagnostics only (D3: Y never enters the result)
		uint64_t samples = 0; // how many motion events were summed
		std::string reason;   // plain language, set when !ok
	};

	inline constexpr const char *kReasonNoMovement =
		"No mouse movement was seen. The game has to hold the mouse (cursor hidden) while you turn.";
	inline constexpr const char *kReasonTooLittle =
		"Too little mouse movement was seen. Turn one full 360 degrees and end where you started.";
	inline constexpr const char *kReasonZoom = "Zoom was active while recording. Record again with Zoom off.";

	// The pure verdict on one recording, shared by Recorder::Stop() and by the
	// glue (whose hook accumulates into atomics instead). `invalid_reason` is
	// the reason a running recording was marked invalid, empty when it was not.
	inline RecordResult Evaluate( Target target, double sum_x, double sum_y, uint64_t samples, const std::string &invalid_reason )
	{
		RecordResult r;
		r.target = target;
		r.sum_y = IsFinite( sum_y ) ? sum_y : 0.0;
		r.samples = samples;
		const double ax = IsFinite( sum_x ) ? std::fabs( sum_x ) : 0.0;
		r.counts = ax;
		if ( !invalid_reason.empty() )
		{
			r.reason = invalid_reason;
			return r;
		}
		if ( samples == 0 || ax <= 0.0 )
		{
			r.reason = kReasonNoMovement;
			return r;
		}
		if ( ax < kMinRecordingCounts )
		{
			r.reason = kReasonTooLittle;
			return r;
		}
		r.ok = true;
		return r;
	}

	// One recording, no threads, no clocks. The NET signed X movement of exactly
	// one turn (D11): corrections back and forth cancel, and only the absolute
	// value is taken at the end.
	class Recorder
	{
	public:
		bool Active() const { return m_target != Target::Idle; }
		Target CurrentTarget() const { return m_target; }
		double SumX() const { return m_sumX; }
		double SumY() const { return m_sumY; }
		uint64_t Samples() const { return m_samples; }

		// Starts (or restarts) a recording; the sums begin at zero.
		void Start( Target target )
		{
			m_target = target;
			m_sumX = 0.0;
			m_sumY = 0.0;
			m_samples = 0;
			m_invalid.clear();
		}

		// A motion event while recording. Ignored when idle.
		void OnDelta( double dx, double dy )
		{
			if ( !Active() )
				return;
			if ( IsFinite( dx ) )
				m_sumX += dx;
			if ( IsFinite( dy ) )
				m_sumY += dy;
			m_samples++;
		}

		// A running recording can no longer be trusted (Zoom came on, D15).
		// The first reason wins; Stop() then reports it and stores nothing.
		void Invalidate( const std::string &reason )
		{
			if ( Active() && m_invalid.empty() )
				m_invalid = reason;
		}

		// Ends the recording and returns its verdict. Idle again afterwards;
		// stopping while idle returns a not-ok result with a reason.
		RecordResult Stop()
		{
			if ( !Active() )
			{
				RecordResult r;
				r.reason = "No recording is running.";
				return r;
			}
			RecordResult r = Evaluate( m_target, m_sumX, m_sumY, m_samples, m_invalid );
			Start( Target::Idle );
			return r;
		}

	private:
		Target m_target = Target::Idle;
		double m_sumX = 0.0;
		double m_sumY = 0.0;
		uint64_t m_samples = 0;
		std::string m_invalid;
	};

	// ---- the fraction carry (D19) ------------------------------------------------

	// Scaled movement keeps its fractions: the result is quantised to the 1/256
	// wire resolution and what the quantising cuts off is carried into the next
	// call, per axis. Over any run of calls the sum of outputs equals the sum of
	// inputs times the multiplier to within one quantum per axis. Reset when the
	// multiplier changes or the feature goes inactive, so a stale remainder never
	// leaks into a different setting.
	class FractionCarry
	{
	public:
		void Reset()
		{
			m_carryX = 0.0;
			m_carryY = 0.0;
		}

		void Apply( double &dx, double &dy, double mult )
		{
			if ( mult != m_mult )
			{
				Reset();
				m_mult = mult;
			}
			dx = Axis( dx, m_carryX, mult );
			dy = Axis( dy, m_carryY, mult );
		}

		double CarryX() const { return m_carryX; }
		double CarryY() const { return m_carryY; }

	private:
		static double Axis( double d, double &carry, double mult )
		{
			if ( !IsFinite( d ) )
				return 0.0;
			const double want = d * mult + carry;
			// Round to the nearest quantum: floor(x + 1/2) on the 1/256 grid.
			const double q = std::floor( want / kWireQuantum + 0.5 ) * kWireQuantum;
			carry = want - q;
			return q;
		}

		double m_carryX = 0.0;
		double m_carryY = 0.0;
		double m_mult = 1.0;
	};

	// ---- text helpers ------------------------------------------------------------

	inline std::string FormatFixed( double v, int decimals )
	{
		char buf[64];
		std::snprintf( buf, sizeof( buf ), "%.*f", decimals, v );
		return buf;
	}

	// "34.2 cm per 360°"; "not recorded" for 0 or anything invalid.
	inline std::string FormatCmPer360( double inches )
	{
		if ( !IsPositive( inches ) )
			return "not recorded";
		return FormatFixed( CmFromInches( inches ), 1 ) + " cm per 360\xC2\xB0";
	}

	// "0.82x"; "not recorded" for 0 (nothing to show a multiplier for).
	inline std::string FormatMultiplier( double mult )
	{
		if ( !IsPositive( mult ) )
			return "not recorded";
		return FormatFixed( mult, 2 ) + "x";
	}

	// "12345 counts per 360°"; "not recorded" for 0.
	inline std::string FormatCounts360( double counts )
	{
		if ( !IsPositive( counts ) )
			return "not recorded";
		return FormatFixed( counts, 0 ) + " counts per 360\xC2\xB0";
	}

	inline const char *TargetName( Target t )
	{
		switch ( t )
		{
			case Target::Low: return "low";
			case Target::High: return "high";
			case Target::Game: return "game";
			default: return "none";
		}
	}

	// The lasting hint drawn while a recording runs (TrueSensHud.cpp, D16): two plain
	// lines. `sChord` is the chord that opens the Shell; an empty one (the action is
	// unbound) is replaced by `sFallback` (the reserved chord, which always works).
	// Both empty for Idle: nothing to show.
	struct HintText
	{
		std::string title;  // what is being recorded
		std::string action; // what to do
	};

	inline HintText HintLines( Target t, const std::string &sChord, const std::string &sFallback )
	{
		HintText h;
		switch ( t )
		{
			case Target::Low: h.title = "Recording Low sensitivity"; break;
			case Target::High: h.title = "Recording High sensitivity"; break;
			case Target::Game: h.title = "Recording game sensitivity"; break;
			default: return h;
		}
		h.action = "Turn once around (360\xC2\xB0) to the same spot, then press " +
			( sChord.empty() ? sFallback : sChord ) + " and Stop recording";
		return h;
	}

	// ---- the reference picture (TrueSensHud.cpp) -------------------------------
	// A frozen crop of the middle of the game picture, shown under the hint plate.

	// A crop rectangle in picture pixels; !ok = nothing to crop (picture too small).
	struct CropRect
	{
		int x = 0, y = 0, w = 0, h = 0;
		bool ok = false;
	};

	// The centre 25 % x 25 % of a `w` x `h` picture (integer division, so an odd size
	// loses at most one pixel and stays centred to within one pixel). Pictures under
	// 4 px on a side have nothing sensible to show: !ok.
	inline CropRect ReferenceCrop( int w, int h )
	{
		CropRect r;
		if ( w < 4 || h < 4 )
			return r;
		r.w = w / 4;
		r.h = h / 4;
		r.x = ( w - r.w ) / 2;
		r.y = ( h - r.h ) / 2;
		r.ok = true;
		return r;
	}

	// Integer reduction that makes a `w` x `h` crop fit in a `maxSide` square texture (1 = none).
	inline int ReferenceDownscale( int w, int h, int maxSide )
	{
		const int big = w > h ? w : h;
		if ( maxSide < 1 || big <= maxSide )
			return 1;
		return ( big + maxSide - 1 ) / maxSide;
	}

	// Where the picture is drawn, in output px; !ok = no room, draw nothing.
	struct DisplayRect
	{
		float x = 0, y = 0, w = 0, h = 0;
		bool ok = false;
	};

	// The picture is shown at 25 % x 25 % of the output (the scale of the live middle of the
	// screen), horizontally centred, its top at `flPlateBottom + 8 * flScale`. If its bottom
	// would come closer than 8 % of the height to the screen centre (the spot the player aims
	// at) it shrinks, keeping the aspect `flAspect` (crop width / height); below 24 px high there
	// is no room and nothing is drawn.
	inline DisplayRect ReferenceDisplay( float flOutW, float flOutH, float flPlateBottom, float flScale, float flAspect )
	{
		DisplayRect d;
		if ( !( flOutW > 0.0f ) || !( flOutH > 0.0f ) || !( flAspect > 0.0f ) || !( flScale > 0.0f ) )
			return d;
		float w = flOutW * 0.25f;
		float h = w / flAspect;
		d.y = flPlateBottom + 8.0f * flScale;
		const float flMaxBottom = flOutH * 0.5f - flOutH * 0.08f;
		if ( d.y + h > flMaxBottom )
		{
			h = flMaxBottom - d.y;
			w = h * flAspect;
		}
		if ( h < 24.0f )
			return d;
		d.w = w;
		d.h = h;
		d.x = ( flOutW - w ) * 0.5f;
		d.ok = true;
		return d;
	}
}

// ---- the glue (TrueSens.cpp) ----------------------------------------------------

namespace gamescope
{
	// THE HOOK, called from wlserver_mousemotion() (src/wlserver.cpp) on the input
	// thread with wlserver_lock() held, after the settings overlay's capture gate
	// and BEFORE Zoom's own mouse scaling. `bCursorHidden` is the hidden-cursor
	// signal the Zoom already uses (no cursor image OR pointer locked), NOT
	// wlserver.bCursorHidden. May modify dx, dy. Relaxed-atomic reads only: no
	// config, no allocation, no locks.
	void TrueSens_OnMotion( double &dx, double &dy, bool bCursorHidden );

	// Once a frame from paint_all(), beside NullBinds_Tick(), on the compositor
	// thread. Loads the values on the first call and keeps the hook's atomics
	// current; also runs the debug commands and watches Zoom during a recording.
	// Everything works without the settings window ever being opened.
	void TrueSens_Tick();

	// ---- the interface the settings area, the hint and the picture call ---------
	// All on the compositor thread (the setters write ConfigManager and refresh
	// the hook's atomics at once).

	// Recording state.
	bool TrueSens_RecordingActive();
	truesens::Target TrueSens_RecordingTarget();                  // Idle when no recording runs
	double TrueSens_RecordingNet();                               // |net X| so far
	std::string TrueSens_LastOutcome();                           // stored value or the reason nothing was stored; "" before any recording
	// Start/Stop take wlserver_lock() themselves, so they must be called on the compositor thread
	// WITHOUT the lock held (the Shell's Action callbacks and the debug commands run there; never
	// call them from a wlserver-thread ConCommand, it would deadlock).
	truesens::OpResult TrueSens_StartRecording( truesens::Target t ); // !ok: Zoom active / already recording / bad target
	truesens::RecordResult TrueSens_StopRecording();              // applies a valid result (see true-sens.md); a failed one changes nothing

	// This game's values.
	bool TrueSens_GetEnabled();
	void TrueSens_SetEnabled( bool bEnabled );
	bool TrueSens_GetUseHigh();                                   // false = Low, true = High
	void TrueSens_SetUseHigh( bool bHigh );
	float TrueSens_GetIngameSens();                               // 0 = unset
	void TrueSens_SetIngameSens( float flValue );
	uint64_t TrueSens_IngameGeneration();                         // changes when the stored in-game value changes or a recording replaces it
	double TrueSens_GameCounts360();                              // 0 = not recorded

	// Global values.
	int TrueSens_GetDpi();
	void TrueSens_SetDpi( int dpi );
	double TrueSens_LowInches();                                  // 0 = not recorded
	double TrueSens_HighInches();

	// Derived, read-only.
	double TrueSens_Multiplier();                                 // in use; 1.0 = none (off or incomplete)
	std::optional<double> TrueSens_CalcIngameSens();              // calculator result for the chosen Low/High
	std::string TrueSens_LowText();                               // "34.2 cm per 360°" / "not recorded"
	std::string TrueSens_HighText();
	std::string TrueSens_GameText();                              // "12345 counts per 360°" / "not recorded"
	std::string TrueSens_MultiplierText();                        // "0.82x" / "not recorded" when no multiplier acts
	const std::string &TrueSens_SessionKey();                     // the config key of this session's game entry
	bool TrueSens_SessionKeyShared();                             // true = the shared unknown-game entry
}

// The recording hint (TrueSensHud.cpp): draws it into the HUD layer's draw list
// (FpsDisplay.cpp calls it, only while TrueSens_RecordingActive()). Returns the
// plate's bottom edge in px (0 = nothing drawn); the reference picture goes below it.
struct ImDrawList;
namespace gamescope { float TrueSens_DrawHud( ImDrawList *pDrawList, float flWidth, float flHeight, float flYOffset ); }

// The reference picture (TrueSensHud.cpp). All on the compositor thread.
class LogScope;
namespace gamescope
{
	LogScope &TrueSens_LogScope();   // the "truesens" log scope (TrueSens.cpp)
	// Ask paint_all for one capture of the game picture (called when a recording starts);
	// drops any older picture first. The result arrives in a later paint, or never (no
	// picture, one log line says why).
	void TrueSens_ReferenceRequest();
	// Drop the picture and forget any capture still on its way (a recording ended).
	void TrueSens_ReferenceClear();
	// Changes whenever the picture appears, changes or goes: the HUD layer folds it into its
	// redraw hash (a new picture under the same texture id adds no draw command).
	uint64_t TrueSens_ReferenceGeneration();
}

// The settings area (TrueSensArea.cpp), declared outside the namespace like
// NullBinds_RegisterArea so Shell.cpp's RegisterAll() names it plainly.
namespace gamescope::ui { class Registry; }
namespace gamescope { void TrueSens_RegisterArea( ui::Registry &reg ); }
