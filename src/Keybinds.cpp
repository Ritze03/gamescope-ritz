// The editable-keybinds system. Design, rationale and the recovery path:
// superdoc/features/keybinds.md. The API contract and the threading rule are
// on Keybinds.h; this file is the grammar, the store and the gesture engine.

#include "Keybinds.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <span>
#include <utility>

#include "Config/ConfigManager.h"
#include "convar.h"
#include "log.hpp"

#include <xkbcommon/xkbcommon-keysyms.h>

namespace gamescope::keybinds
{
	static LogScope log_keybinds( "keybinds" );

	namespace
	{
		// ---------------------------------------------------------------------
		//  Keysym normalisation
		// ---------------------------------------------------------------------
		// A LOCAL COPY of WaylandServer/GamescopeActionBinding.h's
		// NormalizeKeysymForHotkey(), and deliberately so: that header drags in
		// the generated Wayland protocol headers and wlroots, which would put
		// this file (and therefore tests/test_keybinds.cpp) behind a compositor
		// build. The two must agree, and a divergence is SAFE-BY-SHAPE rather
		// than silent: wlserver hands ProcessKey() an ALREADY-normalised keysym,
		// so this copy is only ever used to parse a chord the user typed. If it
		// ever drifted, the symptom would be "the chord I typed never matches",
		// visible immediately, not a binding quietly firing on the wrong key.
		constexpr std::pair<xkb_keysym_t, xkb_keysym_t> kRemap[] {
			{ XKB_KEY_ISO_Left_Tab,      XKB_KEY_Tab },
			{ XKB_KEY_ISO_Enter,         XKB_KEY_Return },
			{ XKB_KEY_Meta_L,            XKB_KEY_Super_L },
			{ XKB_KEY_Meta_R,            XKB_KEY_Super_R },
			{ XKB_KEY_ISO_Level3_Shift,  XKB_KEY_Alt_R },
		};

		xkb_keysym_t NormalizeSym( xkb_keysym_t uSym )
		{
			uSym = xkb_keysym_to_upper( uSym );
			for ( auto [ uBad, uGood ] : kRemap )
			{
				if ( uSym == uBad )
					uSym = uGood;
			}
			return uSym;
		}

		// ---------------------------------------------------------------------
		//  The modifier vocabulary
		// ---------------------------------------------------------------------
		// Order matters twice: it is the order FormatChord() prints modifiers
		// in (so a chord has exactly one spelling), and the either-side entry
		// must be found before its two sided ones when formatting.
		struct ModToken
		{
			const char  *pszName;
			xkb_keysym_t uA;
			xkb_keysym_t uB;   // NoSymbol for the sided forms
			int          nSlot;  // 0 Ctrl, 1 Alt, 2 Shift, 3 Super -- the print order,
			                     // the conventional one a keyboard shortcut is
			                     // written in ("Ctrl+Alt+Shift+O"), so the string
			                     // the UI shows is the one people already read.
		};

		constexpr ModToken kMods[] {
			{ "Ctrl",   XKB_KEY_Control_L, XKB_KEY_Control_R, 0 },
			{ "LCtrl",  XKB_KEY_Control_L, XKB_KEY_NoSymbol,  0 },
			{ "RCtrl",  XKB_KEY_Control_R, XKB_KEY_NoSymbol,  0 },
			{ "Shift",  XKB_KEY_Shift_L,   XKB_KEY_Shift_R,   2 },
			{ "LShift", XKB_KEY_Shift_L,   XKB_KEY_NoSymbol,  2 },
			{ "RShift", XKB_KEY_Shift_R,   XKB_KEY_NoSymbol,  2 },
			{ "Alt",    XKB_KEY_Alt_L,     XKB_KEY_Alt_R,     1 },
			{ "LAlt",   XKB_KEY_Alt_L,     XKB_KEY_NoSymbol,  1 },
			{ "RAlt",   XKB_KEY_Alt_R,     XKB_KEY_NoSymbol,  1 },
			{ "Super",  XKB_KEY_Super_L,   XKB_KEY_Super_R,   3 },
			{ "LSuper", XKB_KEY_Super_L,   XKB_KEY_NoSymbol,  3 },
			{ "RSuper", XKB_KEY_Super_R,   XKB_KEY_NoSymbol,  3 },
		};

		bool IsModifierSym( xkb_keysym_t u )
		{
			for ( const ModToken &m : kMods )
			{
				if ( m.uA == u || m.uB == u )
					return true;
			}
			return false;
		}

		// The token whose (uA, uB) pair is exactly this term, or nullptr.
		const ModToken *ModTokenFor( const ChordTerm &term )
		{
			for ( const ModToken &m : kMods )
			{
				if ( m.uA == term.uA && m.uB == term.uB )
					return &m;
			}
			return nullptr;
		}

		bool IEquals( std::string_view a, std::string_view b )
		{
			if ( a.size() != b.size() )
				return false;
			for ( size_t i = 0; i < a.size(); i++ )
			{
				if ( std::tolower( (unsigned char)a[ i ] ) != std::tolower( (unsigned char)b[ i ] ) )
					return false;
			}
			return true;
		}

		std::string_view Trim( std::string_view s )
		{
			while ( !s.empty() && std::isspace( (unsigned char)s.front() ) ) s.remove_prefix( 1 );
			while ( !s.empty() && std::isspace( (unsigned char)s.back() ) )  s.remove_suffix( 1 );
			return s;
		}

		// A chord of more than this many keys is not a chord anybody can press.
		constexpr size_t kMaxTerms = 5;

		// ---------------------------------------------------------------------
		//  The action table
		// ---------------------------------------------------------------------
		// The defaults reproduce what wlserver.cpp hard-coded before this file
		// existed, exactly -- see the inventory in
		// superdoc/features/keybinds.md. `Open settings` is a lone RIGHT Shift
		// TAP; `Open launcher` adds the LEFT Ctrl; the alternate chord accepts
		// either Ctrl and either Shift, as it always has.
		constexpr ActionInfo kActions[ (size_t)Action::Count ] {
			{ "shell", "Open settings", "RShift",
			  "The chord that opens and closes the settings shell. A chord made only of "
			  "modifiers is a TAP: it fires when you let go, and only if you pressed nothing "
			  "else while holding it, so the key keeps its day job as a modifier." },
			{ "shell_alt", "Open settings (alternate)", "Ctrl+Shift+O",
			  "A second chord for the same thing, for when the first one is a modifier tap "
			  "and you want something you can hit deliberately." },
			{ "launcher", "Open launcher", "LCtrl+RShift",
			  "The chord that opens the command palette alone over the game -- and closes it "
			  "again when it is already up." },
			// Ctrl+Shift+Tab is Steam's own overlay chord for the friends
			// list. That is deliberate, and it is not a conflict: gamescope
			// swallows the key here, so neither the game nor Steam's overlay
			// (which this fork's own users have switched off anyway -- see
			// superdoc/planning/steam-friends-window.md §2) ever sees it, and
			// the muscle memory is already the right one.
			{ "companion", "Open Steam chat", "Ctrl+Shift+Tab",
			  "The chord that opens Steam's web chat over the game, in a browser gamescope runs "
			  "on its own display -- and hides it again when it is already up. The first press "
			  "has to start the browser, so it takes a moment; after that it is instant. Set the "
			  "browser and the page in Settings > System > Steam chat." },
		};

		// ---------------------------------------------------------------------
		//  Live state
		// ---------------------------------------------------------------------
		std::mutex g_Mutex;

		// Resolved chords, one per action. Guarded by g_Mutex; read on the
		// wlserver thread once per key event, written from the panel/console.
		Chord g_Chords[ (size_t)Action::Count ];
		bool  g_bChordsInit = false;

		// The gesture engine's memory (see ProcessKey).
		std::unordered_set<xkb_keysym_t> g_setPeak;        // biggest held-set of the gesture in flight
		std::unordered_set<xkb_keysym_t> g_setOwnedPress;  // presses we swallowed; their releases are ours too

		// Capture.
		bool          g_bCapturing = false;
		Action        g_eCaptureAction = Action::Shell;
		CaptureResult g_PendingResult;

		// The persistent copy EnqueueOverlayWrite() keys its per-field merge on
		// (its ADDRESS is the caller -- ConfigManager.h's contract). File-static
		// so every write from this file is the same caller.
		config::OverlaySettings g_OverlayForWrite;

		Chord ParseOrDefault( Action eAction, std::string_view svText )
		{
			Chord c;
			std::string sErr;
			if ( !svText.empty() && ParseChord( svText, &c, &sErr ) )
				return c;
			if ( !svText.empty() )
			{
				log_keybinds.warnf( "'%s' is not a chord I can read (%s) -- '%s' stays on its default %s.",
					std::string( svText ).c_str(), sErr.c_str(),
					kActions[ (size_t)eAction ].pszId, kActions[ (size_t)eAction ].pszDefault );
			}
			ParseChord( kActions[ (size_t)eAction ].pszDefault, &c, &sErr );
			return c;
		}

		void EnsureInit()
		{
			if ( g_bChordsInit )
				return;
			g_bChordsInit = true;
			for ( size_t i = 0; i < (size_t)Action::Count; i++ )
				g_Chords[ i ] = ParseOrDefault( (Action)i, {} );
		}

		// The conflict rule, checked with g_Mutex held: no two actions may hold
		// the same chord, and nothing may hold the reserved one. Returns the
		// refusal reason, or "".
		std::string ConflictReason( Action eAction, const Chord &chord )
		{
			if ( chord == ReservedChord() )
			{
				return std::string( "That chord is reserved as the way back into the settings "
					"if a binding goes wrong (" ) + ReservedChordText() + ").";
			}
			for ( size_t i = 0; i < (size_t)Action::Count; i++ )
			{
				if ( (Action)i == eAction )
					continue;
				if ( g_Chords[ i ] == chord )
					return std::string( "Already used by \"" ) + kActions[ i ].pszTitle + "\".";
			}
			return {};
		}

		// The whole store as the map global.json carries: default-valued
		// actions are omitted, so a fresh config has no keybind keys at all.
		std::map<std::string, std::string> OverridesLocked()
		{
			std::map<std::string, std::string> out;
			for ( size_t i = 0; i < (size_t)Action::Count; i++ )
			{
				Chord dflt;
				std::string sErr;
				ParseChord( kActions[ i ].pszDefault, &dflt, &sErr );
				if ( g_Chords[ i ] != dflt )
					out[ kActions[ i ].pszId ] = FormatChord( g_Chords[ i ] );
			}
			return out;
		}

		// Reloads the freshest `overlay` from the config layer, puts our map on
		// it and writes it back. Caller's thread, never the wlserver one.
		void PersistLocked()
		{
			g_OverlayForWrite = config::LoadGlobal().overlay;
			g_OverlayForWrite.keybinds = OverridesLocked();
			config::EnqueueOverlayWrite( g_OverlayForWrite );
		}
	}

	// =========================================================================
	//  Actions
	// =========================================================================
	const ActionInfo &Info( Action eAction )
	{
		return kActions[ (size_t)eAction < (size_t)Action::Count ? (size_t)eAction : 0 ];
	}

	std::optional<Action> ActionFromId( std::string_view svId )
	{
		for ( size_t i = 0; i < (size_t)Action::Count; i++ )
		{
			if ( svId == kActions[ i ].pszId )
				return (Action)i;
		}
		return std::nullopt;
	}

	// =========================================================================
	//  Chords -- grammar
	// =========================================================================
	bool Chord::operator==( const Chord &o ) const
	{
		if ( terms.size() != o.terms.size() )
			return false;
		// Sets, not sequences: ParseChord already canonicalises the order, but
		// comparing as sets means a hand-written Chord in a test compares the
		// way the model says it should.
		std::vector<bool> used( o.terms.size(), false );
		for ( const ChordTerm &t : terms )
		{
			bool bFound = false;
			for ( size_t i = 0; i < o.terms.size() && !bFound; i++ )
			{
				if ( !used[ i ] && o.terms[ i ] == t )
					used[ i ] = bFound = true;
			}
			if ( !bFound )
				return false;
		}
		return true;
	}

	bool ParseChord( std::string_view svText, Chord *pOut, std::string *psError )
	{
		auto Fail = [ & ]( std::string sWhy ) {
			if ( psError ) *psError = std::move( sWhy );
			return false;
		};

		Chord chord;
		svText = Trim( svText );
		if ( svText.empty() )
			return Fail( "A chord needs at least one key." );

		size_t nPos = 0;
		while ( nPos <= svText.size() )
		{
			const size_t nPlus = svText.find( '+', nPos );
			const std::string_view svTok = Trim( svText.substr( nPos,
				nPlus == std::string_view::npos ? std::string_view::npos : nPlus - nPos ) );

			if ( svTok.empty() )
				return Fail( "Two '+' in a row, or a '+' with nothing after it." );

			ChordTerm term;
			const ModToken *pMod = nullptr;
			for ( const ModToken &m : kMods )
			{
				if ( IEquals( svTok, m.pszName ) )
				{
					pMod = &m;
					break;
				}
			}

			if ( pMod )
			{
				term.uA = pMod->uA;
				term.uB = pMod->uB;
			}
			else
			{
				const std::string sTok( svTok );
				const xkb_keysym_t uSym = xkb_keysym_from_name( sTok.c_str(), XKB_KEYSYM_CASE_INSENSITIVE );
				if ( uSym == XKB_KEY_NoSymbol )
					return Fail( "\"" + sTok + "\" is not a key I know." );
				term.uA = NormalizeSym( uSym );
				term.uB = XKB_KEY_NoSymbol;
			}

			// Overlapping terms are refused rather than silently deduplicated:
			// `Ctrl+LCtrl` cannot be pressed (one physical key cannot satisfy
			// two terms), so accepting it would mint a chord that never fires.
			for ( const ChordTerm &t : chord.terms )
			{
				if ( t.Accepts( term.uA ) || ( term.uB != XKB_KEY_NoSymbol && t.Accepts( term.uB ) ) ||
				     term.Accepts( t.uA ) || ( t.uB != XKB_KEY_NoSymbol && term.Accepts( t.uB ) ) )
					return Fail( "\"" + std::string( svTok ) + "\" repeats a key already in the chord." );
			}

			chord.terms.push_back( term );
			if ( chord.terms.size() > kMaxTerms )
				return Fail( "That is more keys than a chord can have." );

			if ( nPlus == std::string_view::npos )
				break;
			nPos = nPlus + 1;
		}

		// Canonical order, so FormatChord has exactly one answer per chord.
		std::stable_sort( chord.terms.begin(), chord.terms.end(),
			[]( const ChordTerm &a, const ChordTerm &b )
			{
				const ModToken *pA = ModTokenFor( a );
				const ModToken *pB = ModTokenFor( b );
				if ( ( pA != nullptr ) != ( pB != nullptr ) )
					return pA != nullptr;                       // modifiers lead
				if ( pA && pB && pA->nSlot != pB->nSlot )
					return pA->nSlot < pB->nSlot;               // Ctrl, Alt, Shift, Super
				return a.uA < b.uA;
			} );

		if ( pOut )
			*pOut = std::move( chord );
		if ( psError )
			psError->clear();
		return true;
	}

	std::string FormatChord( const Chord &chord )
	{
		std::string s;
		for ( const ChordTerm &term : chord.terms )
		{
			if ( !s.empty() )
				s += "+";
			if ( const ModToken *pMod = ModTokenFor( term ) )
			{
				s += pMod->pszName;
				continue;
			}
			char szName[ 64 ] = "";
			if ( xkb_keysym_get_name( term.uA, szName, sizeof( szName ) ) <= 0 )
				snprintf( szName, sizeof( szName ), "0x%x", term.uA );
			s += szName;
		}
		return s;
	}

	bool IsModifierOnly( const Chord &chord )
	{
		if ( chord.Empty() )
			return false;
		for ( const ChordTerm &term : chord.terms )
		{
			if ( !IsModifierSym( term.uA ) )
				return false;
		}
		return true;
	}

	bool ChordMatches( const Chord &chord, const std::unordered_set<xkb_keysym_t> &setHeld )
	{
		if ( chord.terms.empty() || setHeld.size() != chord.terms.size() )
			return false;

		// A perfect matching between held keys and terms. SPECIFIC terms are
		// assigned first because an either-side term accepts everything a
		// sided one does and more -- taking them in that order means greedy
		// assignment cannot strand a key that only one term could have taken.
		std::vector<bool> used( chord.terms.size(), false );
		std::vector<xkb_keysym_t> unmatched;
		for ( xkb_keysym_t uSym : setHeld )
		{
			bool bTook = false;
			for ( size_t i = 0; i < chord.terms.size() && !bTook; i++ )
			{
				if ( !used[ i ] && !chord.terms[ i ].EitherSide() && chord.terms[ i ].uA == uSym )
					used[ i ] = bTook = true;
			}
			if ( !bTook )
				unmatched.push_back( uSym );
		}
		for ( xkb_keysym_t uSym : unmatched )
		{
			bool bTook = false;
			for ( size_t i = 0; i < chord.terms.size() && !bTook; i++ )
			{
				if ( !used[ i ] && chord.terms[ i ].EitherSide() && chord.terms[ i ].Accepts( uSym ) )
					used[ i ] = bTook = true;
			}
			if ( !bTook )
				return false;
		}
		return true;
	}

	// =========================================================================
	//  The reserved chord
	// =========================================================================
	const char *ReservedChordText() { return "Ctrl+Alt+Shift+O"; }

	const Chord &ReservedChord()
	{
		static const Chord s_Chord = []
		{
			Chord c;
			std::string sErr;
			ParseChord( ReservedChordText(), &c, &sErr );
			return c;
		}();
		return s_Chord;
	}

	// =========================================================================
	//  The store
	// =========================================================================
	Chord ChordFor( Action eAction )
	{
		std::scoped_lock lock( g_Mutex );
		EnsureInit();
		return g_Chords[ (size_t)eAction ];
	}

	std::string ChordTextFor( Action eAction )
	{
		return FormatChord( ChordFor( eAction ) );
	}

	bool IsDefault( Action eAction )
	{
		Chord dflt;
		std::string sErr;
		ParseChord( Info( eAction ).pszDefault, &dflt, &sErr );
		return ChordFor( eAction ) == dflt;
	}

	std::string SetChord( Action eAction, std::string_view svChord )
	{
		std::scoped_lock lock( g_Mutex );
		EnsureInit();

		const std::string_view svTrimmed = Trim( svChord );

		Chord chord;
		if ( svTrimmed.empty() )
		{
			// "Clear it to get the default back" -- Keybinds.h's contract.
			std::string sErr;
			ParseChord( Info( eAction ).pszDefault, &chord, &sErr );
		}
		else
		{
			std::string sErr;
			if ( !ParseChord( svTrimmed, &chord, &sErr ) )
				return sErr;
		}

		if ( std::string sWhy = ConflictReason( eAction, chord ); !sWhy.empty() )
			return sWhy;

		g_Chords[ (size_t)eAction ] = chord;
		PersistLocked();
		log_keybinds.infof( "%s = %s", Info( eAction ).pszId, FormatChord( chord ).c_str() );
		return {};
	}

	void ResetAll()
	{
		std::scoped_lock lock( g_Mutex );
		g_bChordsInit = false;
		EnsureInit();
		PersistLocked();
		log_keybinds.infof( "every keybind is back on its default." );
	}

	void ApplyFromConfig( const config::OverlaySettings &overlay )
	{
		std::scoped_lock lock( g_Mutex );
		g_bChordsInit = true;

		for ( size_t i = 0; i < (size_t)Action::Count; i++ )
		{
			const auto it = overlay.keybinds.find( kActions[ i ].pszId );
			Chord chord = ParseOrDefault( (Action)i,
				it == overlay.keybinds.end() ? std::string_view{} : std::string_view( it->second ) );

			// A conflicting file is repaired, never obeyed: only hand-editing
			// can produce one (SetChord refuses them), and the failure this
			// guards against -- two actions on one chord, one of them the way
			// into the settings -- is exactly the one the user cannot fix
			// from inside the UI.
			g_Chords[ i ] = chord;                    // provisional, so ConflictReason sees it
			if ( std::string sWhy = ConflictReason( (Action)i, chord ); !sWhy.empty() )
			{
				log_keybinds.warnf( "'%s' -> %s is refused (%s); falling back to %s.",
					kActions[ i ].pszId, FormatChord( chord ).c_str(), sWhy.c_str(),
					kActions[ i ].pszDefault );
				std::string sErr;
				ParseChord( kActions[ i ].pszDefault, &chord, &sErr );
			}
			g_Chords[ i ] = chord;
		}
	}

	std::vector<std::pair<std::string, std::string>> OverridesForConfig()
	{
		std::scoped_lock lock( g_Mutex );
		EnsureInit();
		std::vector<std::pair<std::string, std::string>> out;
		for ( const auto &[ sKey, sVal ] : OverridesLocked() )
			out.emplace_back( sKey, sVal );
		return out;
	}

	// =========================================================================
	//  Capture
	// =========================================================================
	void BeginCapture( Action eAction )
	{
		std::scoped_lock lock( g_Mutex );
		g_bCapturing = true;
		g_eCaptureAction = eAction;
		g_setPeak.clear();
		g_PendingResult = {};
	}

	void CancelCapture()
	{
		std::scoped_lock lock( g_Mutex );
		if ( !g_bCapturing )
			return;
		g_bCapturing = false;
		g_setPeak.clear();
		g_PendingResult = { CaptureStatus::Cancelled, g_eCaptureAction, {} };
	}

	bool CaptureActive( Action *peAction )
	{
		std::scoped_lock lock( g_Mutex );
		if ( g_bCapturing && peAction )
			*peAction = g_eCaptureAction;
		return g_bCapturing;
	}

	CaptureResult TakeCaptureResult()
	{
		std::scoped_lock lock( g_Mutex );
		CaptureResult r = g_PendingResult;
		g_PendingResult = {};
		return r;
	}

	// =========================================================================
	//  The gesture engine
	// =========================================================================
	// TWO FIRING RULES, DECIDED BY THE CHORD ITSELF, not by a flag anyone sets.
	// This is the generalisation of what wlserver.cpp hard-coded, and it
	// reproduces all three of its bindings exactly:
	//
	//   * A MODIFIER-ONLY chord (RShift; LCtrl+RShift) is a TAP. It fires on a
	//     RELEASE, and only if the biggest set held during the gesture was
	//     exactly the chord -- i.e. nothing else was pressed while it was
	//     down. It is never swallowed. Both halves are forced: a modifier that
	//     fired on its own press could not be used as a modifier any more, and
	//     swallowing a modifier's release after its press was delivered leaves
	//     the game holding a Shift that is physically up.
	//
	//   * ANY OTHER chord (Ctrl+Shift+O) fires on the PRESS that completes it
	//     and IS swallowed, so the letter never reaches the game. Its own
	//     release is swallowed too, tracked per key rather than by a boolean,
	//     so the modifiers around it are unaffected.
	//
	// WHY THE PEAK, AND WHAT IT REPLACES. The old code armed a gesture on Right
	// Shift's press, upgraded it when Left Ctrl arrived, and disarmed it on any
	// other key -- three rules, each of which had to be remembered at every new
	// exit, and the fourth break of that binding is what produced them. The
	// peak set says the same thing once and structurally: `Ctrl+Shift+O` peaks
	// at three keys, so no two-key tap can match it, so the tap cannot eat the
	// longer gesture; and a tap can fire at most once per gesture because
	// firing clears the peak. Nothing has to be disarmed by hand.
	KeyResult ProcessKey( xkb_keysym_t normalizedKeysym, bool bPress,
	                      const std::unordered_set<xkb_keysym_t> &setHeld )
	{
		std::scoped_lock lock( g_Mutex );
		EnsureInit();

		KeyResult res;

		// ---- capture: swallow everything, match nothing ---------------------
		if ( g_bCapturing )
		{
			if ( bPress )
			{
				if ( setHeld.size() > g_setPeak.size() )
					g_setPeak = setHeld;
				g_setOwnedPress.insert( normalizedKeysym );
				res.bConsume = true;
				return res;
			}

			// Only a release whose press we swallowed is ours to swallow; a
			// modifier already held when the field was clicked keeps its
			// release, so the game cannot be left holding it down.
			res.bConsume = g_setOwnedPress.erase( normalizedKeysym ) > 0;

			if ( !g_setPeak.empty() )
			{
				const bool bEscape = g_setPeak.size() == 1 && *g_setPeak.begin() == XKB_KEY_Escape;
				Chord captured;
				for ( xkb_keysym_t uSym : g_setPeak )
				{
					// A captured MODIFIER is generalised to either side --
					// except in a modifier-only chord, where the side is the
					// whole point (a lone `Shift` tap would fire every time
					// the player crouches). See superdoc/features/keybinds.md.
					captured.terms.push_back( ChordTerm{ uSym, XKB_KEY_NoSymbol } );
				}
				bool bAllMods = true;
				for ( const ChordTerm &t : captured.terms )
					bAllMods = bAllMods && IsModifierSym( t.uA );
				if ( !bAllMods )
				{
					for ( ChordTerm &t : captured.terms )
					{
						for ( const ModToken &m : kMods )
						{
							if ( m.uB != XKB_KEY_NoSymbol && ( m.uA == t.uA || m.uB == t.uA ) )
							{
								t.uA = m.uA;
								t.uB = m.uB;
								break;
							}
						}
					}
				}

				g_bCapturing = false;
				g_setPeak.clear();
				if ( bEscape )
					g_PendingResult = { CaptureStatus::Cancelled, g_eCaptureAction, {} };
				else
				{
					// Round-trip through the parser so the stored value is
					// canonical and is one the grammar accepts.
					Chord canonical;
					std::string sErr;
					const std::string sText = FormatChord( captured );
					if ( ParseChord( sText, &canonical, &sErr ) )
						g_PendingResult = { CaptureStatus::Captured, g_eCaptureAction, FormatChord( canonical ) };
					else
						g_PendingResult = { CaptureStatus::Cancelled, g_eCaptureAction, {} };
				}
			}
			return res;
		}

		// ---- the peak of the gesture in flight ------------------------------
		if ( bPress )
		{
			if ( setHeld.size() > g_setPeak.size() )
				g_setPeak = setHeld;
		}

		// ---- press-fired chords ---------------------------------------------
		if ( bPress )
		{
			for ( size_t i = 0; i < (size_t)Action::Count; i++ )
			{
				if ( IsModifierOnly( g_Chords[ i ] ) || !ChordMatches( g_Chords[ i ], setHeld ) )
					continue;
				g_setOwnedPress.insert( normalizedKeysym );
				return KeyResult{ true, (Action)i, true };
			}

			if ( ChordMatches( ReservedChord(), setHeld ) )
			{
				log_keybinds.infof( "reserved %s -- opening the settings.", ReservedChordText() );
				g_setOwnedPress.insert( normalizedKeysym );
				return KeyResult{ true, Action::Shell, true };
			}
			return res;
		}

		// ---- releases --------------------------------------------------------
		res.bConsume = g_setOwnedPress.erase( normalizedKeysym ) > 0;

		for ( size_t i = 0; i < (size_t)Action::Count; i++ )
		{
			if ( !IsModifierOnly( g_Chords[ i ] ) || !ChordMatches( g_Chords[ i ], g_setPeak ) )
				continue;
			g_setPeak.clear();   // one tap per gesture, by construction
			res.bFired  = true;
			res.eAction = (Action)i;
			return res;
		}

		if ( setHeld.empty() )
			g_setPeak.clear();

		return res;
	}

	void ClearGestureState()
	{
		std::scoped_lock lock( g_Mutex );
		g_setPeak.clear();
		g_setOwnedPress.clear();
		if ( g_bCapturing )
		{
			g_bCapturing = false;
			g_PendingResult = { CaptureStatus::Cancelled, g_eCaptureAction, {} };
		}
	}

	// =========================================================================
	//  The console surface -- and the second half of the recovery path
	// =========================================================================
	// `ritz_keybinds_reset` is the answer to "I bound the shell to something I
	// cannot type any more and the settings UI is now unreachable" for anyone
	// who would rather not remember the reserved chord. It reaches a running
	// compositor through gamescopectl, from outside the session.
	static ConCommand cc_ritz_keybinds(
		"ritz_keybinds",
		"List this fork's own hotkeys and the chord each one is bound to.",
		[]( std::span<std::string_view> )
		{
			for ( size_t i = 0; i < (size_t)Action::Count; i++ )
			{
				const Action e = (Action)i;
				console_log.infof( "  %-10s  %-18s  %s%s", Info( e ).pszId,
					ChordTextFor( e ).c_str(), Info( e ).pszTitle,
					IsDefault( e ) ? "" : "  (changed)" );
			}
			console_log.infof( "  %-10s  %-18s  %s", "(reserved)", ReservedChordText(),
				"always opens the settings; cannot be rebound" );
		} );

	static ConCommand cc_ritz_keybind(
		"ritz_keybind",
		"Rebind one hotkey: ritz_keybind <action> <chord>, e.g. ritz_keybind shell_alt "
		"\"Ctrl+Shift+P\". Chords are '+'-separated: Ctrl/Shift/Alt/Super take either side, "
		"LCtrl/RShift/... name one. An empty chord restores that action's default. Through "
		"gamescopectl the arguments must be ONE quoted argument.",
		[]( std::span<std::string_view> args )
		{
			if ( args.size() < 2 )
			{
				console_log.errorf( "usage: ritz_keybind <action> <chord>" );
				return;
			}
			const std::optional<Action> oAction = ActionFromId( args[ 1 ] );
			if ( !oAction )
			{
				console_log.errorf( "no such keybind action: %s", std::string( args[ 1 ] ).c_str() );
				return;
			}
			const std::string sChord = args.size() >= 3 ? std::string( args[ 2 ] ) : std::string();
			if ( const std::string sWhy = SetChord( *oAction, sChord ); !sWhy.empty() )
			{
				console_log.errorf( "%s", sWhy.c_str() );
				return;
			}
			console_log.infof( "%s = %s", Info( *oAction ).pszId, ChordTextFor( *oAction ).c_str() );
		} );

	static ConCommand cc_ritz_keybinds_reset(
		"ritz_keybinds_reset",
		"Put every hotkey back on its compiled-in default. The way out of a binding that "
		"left the settings unreachable; " /* see this file's own comment */
		"Ctrl+Alt+Shift+O is the other one, and always works.",
		[]( std::span<std::string_view> )
		{
			ResetAll();
			for ( size_t i = 0; i < (size_t)Action::Count; i++ )
				console_log.infof( "%s = %s", kActions[ i ].pszId, ChordTextFor( (Action)i ).c_str() );
		} );
}
