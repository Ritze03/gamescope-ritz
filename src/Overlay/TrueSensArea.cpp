// The True Sens settings area (id `system.true_sens`, INPUT group, after Null
// binds) -- see TrueSens.h for the core it drives and
// superdoc/features/true-sens.md ("The area") for the rows and the wording
// decisions. No pixel, colour or ImGui call lives here: every row is declared
// through ui::Area, the shell draws it.
//
// Everything here runs on the compositor thread (the Shell's draw thread), the
// thread TrueSens_*() expects; `overlay_e2_set/get` reach the getters and
// setters from the console thread like every other area's bindings do.
#include "TrueSens.h"

#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

#include "SettingsOverlay.h"
#include "UI/Registry.h"

namespace gamescope
{
	namespace
	{
		using truesens::Target;

		// What the calculator's text field last refused, shown beside the field
		// until a valid entry replaces it. A file-static string of this area: the
		// field is a Text row (D17), and Entry::Validate() is stored by the
		// registry but never consulted by the shell, so the setter has to do the
		// rejecting and this is where its message lives.
		std::string s_sIngameError;
		// TrueSens_IngameGeneration() when the error was raised: the stored value
		// changed since (a recording replaced it, a valid entry set it) means the
		// warning is stale and is not shown, so it never sits beside a valid field.
		uint64_t s_ulIngameErrorGen = 0;

		constexpr const char *kBadIngame = "Enter a number above 0";

		// ---- the calculator's text field (D17) --------------------------------

		// "0.35" and "0,35" are the same number (the player types in their own
		// locale). The whole string must be a positive, finite number; anything
		// else is nullopt. The upper bound only keeps a typo like 1e30 out of a
		// float.
		std::optional<double> ParseIngame( const std::string &sIn )
		{
			std::string s;
			for ( char c : sIn )
			{
				if ( c == ' ' || c == '\t' )
					continue;
				s += ( c == ',' ) ? '.' : c;
			}
			if ( s.empty() )
				return std::nullopt;
			char *pEnd = nullptr;
			const double d = std::strtod( s.c_str(), &pEnd );
			if ( pEnd == s.c_str() || *pEnd != '\0' )
				return std::nullopt;
			if ( !truesens::IsPositive( d ) || d > 1.0e6 )
				return std::nullopt;
			return d;
		}

		std::string ValidateIngame( const std::string &s )
		{
			// An empty field means "unset" and is allowed (it clears the value).
			if ( s.empty() )
				return {};
			return ParseIngame( s ) ? std::string() : std::string( kBadIngame );
		}

		std::string IngameText()
		{
			const float fl = TrueSens_GetIngameSens();
			if ( !( fl > 0.0f ) )
				return {};
			// Trim trailing zeros: 0.3500 -> 0.35, 1.0000 -> 1.
			std::string t = truesens::FormatFixed( fl, 4 );
			while ( t.back() == '0' )
				t.pop_back();
			if ( t.back() == '.' )
				t.pop_back();
			return t;
		}

		void SetIngameText( const std::string &s )
		{
			if ( s.empty() )
			{
				TrueSens_SetIngameSens( 0.0f );
				s_sIngameError.clear();
				return;
			}
			const std::optional<double> d = ParseIngame( s );
			if ( !d )
			{
				s_sIngameError = kBadIngame;
				s_ulIngameErrorGen = TrueSens_IngameGeneration();
				return; // the stored value is unchanged
			}
			s_sIngameError.clear();
			TrueSens_SetIngameSens( (float)*d );
		}

		std::string IngameStatus()
		{
			return s_ulIngameErrorGen == TrueSens_IngameGeneration() ? s_sIngameError : std::string();
		}

		// ---- read-only lines -----------------------------------------------------

		const char *ChosenName()
		{
			return TrueSens_GetUseHigh() ? "High" : "Low";
		}

		double ChosenInches()
		{
			return TrueSens_GetUseHigh() ? TrueSens_HighInches() : TrueSens_LowInches();
		}

		// What is missing for the chosen Low/High and this game, in the order the
		// player would fix it; "" when nothing is.
		std::string MissingText()
		{
			if ( !( TrueSens_GameCounts360() > 0.0 ) )
				return "Record the game first";
			if ( !( ChosenInches() > 0.0 ) )
				return std::string( "Record " ) + ChosenName() + " first";
			return {};
		}

		// D21: "the multiplier in use". Own wording because MultiplierText() says
		// "not recorded" for every case where no multiplier acts.
		std::string MultiplierLine()
		{
			if ( !TrueSens_GetEnabled() )
				return "Off";
			if ( const std::string s = MissingText(); !s.empty() )
				return s;
			return truesens::FormatMultiplier( TrueSens_Multiplier() );
		}

		std::string CalcLine()
		{
			if ( const std::string s = MissingText(); !s.empty() )
				return s;
			if ( !( TrueSens_GetIngameSens() > 0.0f ) )
				return "Enter the in-game value";
			const std::optional<double> d = TrueSens_CalcIngameSens();
			if ( !d )
				return "Enter the in-game value";
			char buf[32];
			std::snprintf( buf, sizeof( buf ), "%.4g", *d );
			return buf;
		}

		const char *TargetLabel( Target t )
		{
			switch ( t )
			{
				case Target::Low: return "Low";
				case Target::High: return "High";
				case Target::Game: return "Game";
				default: return "";
			}
		}

		std::string RecordingLine()
		{
			std::string s = std::string( TargetLabel( TrueSens_RecordingTarget() ) ) + ", " +
				truesens::FormatFixed( TrueSens_RecordingNet(), 0 ) + " counts so far";
			return s;
		}

		// A Facts value is one right-bound line, capped at 60% of the label zone:
		// about 24 characters of the value font in a half-width column (measured at
		// 1080p: the 26th character is already cut). The core's outcome is one to three
		// sentences, so it is shown as several Facts rows, one line each: sentence by
		// sentence (the first is what happened, the next what to do), a sentence
		// wrapped at word boundaries when it is longer than a line. The first row's
		// Details line in the Inspector still carries the whole text.
		constexpr size_t kOutcomeLineChars = 22;
		constexpr size_t kOutcomeRows = 8;

		size_t Utf8Len( const std::string &s )
		{
			size_t n = 0;
			for ( unsigned char c : s )
				if ( ( c & 0xC0 ) != 0x80 )
					n++;
			return n;
		}

		// Sentences end at ". " (the end of the text counts too); a parenthesis stays
		// in its sentence.
		std::vector<std::string> OutcomeLines( const std::string &s )
		{
			std::vector<std::string> out;
			size_t i = 0;
			while ( i < s.size() )
			{
				const size_t e = s.find( ". ", i );
				const std::string sent = e == std::string::npos ? s.substr( i ) : s.substr( i, e - i + 1 );
				i = e == std::string::npos ? s.size() : e + 2;
				std::string line;
				size_t p = 0;
				while ( p < sent.size() )
				{
					const size_t q = sent.find( ' ', p );
					const std::string word = sent.substr( p, q == std::string::npos ? std::string::npos : q - p );
					p = q == std::string::npos ? sent.size() : q + 1;
					if ( !line.empty() && Utf8Len( line ) + 1 + Utf8Len( word ) > kOutcomeLineChars )
					{
						out.push_back( line );
						line.clear();
					}
					line += ( line.empty() ? "" : " " ) + word;
				}
				if ( !line.empty() )
					out.push_back( line );
			}
			return out;
		}

		// Line `n` of the last outcome; "" when there is none (the row is then hidden).
		std::string OutcomeLine( size_t n )
		{
			if ( TrueSens_RecordingActive() )
				return {};
			const std::vector<std::string> v = OutcomeLines( TrueSens_LastOutcome() );
			return n < v.size() ? v[ n ] : std::string();
		}

		// ---- the actions ---------------------------------------------------------

		// D6: Record closes the settings window so the player can turn in the
		// game. A refusal (Zoom active, a recording already running) leaves the
		// window open; the core has already put its reason in LastOutcome(),
		// which the "Last recording" row below shows.
		void Record( Target t )
		{
			if ( TrueSens_StartRecording( t ).ok )
				SettingsOverlay_SetVisible( false );
		}

		const auto kNotRecording = []{ return !TrueSens_RecordingActive(); };
		const auto kRecording = []{ return TrueSens_RecordingActive(); };

		constexpr const char *kRecordHelp =
			"Aim at a spot you can find again, press Record, turn exactly once around (360\xC2\xB0) "
			"until you aim at the same spot, open the settings again and press Stop recording.";
	}

	void TrueSens_RegisterArea( ui::Registry &reg )
	{
		ui::Area &a = reg.Add( "system.true_sens", "True Sens", ui::Section::System );

		a.Keywords( "true sens sensitivity mouse dpi 360 cm per 360 edpi calibrate calibration "
		            "same feel every game multiplier calculator record low high" );
		// Two columns (the owner: "Make the UI page use two collumns."): this game and its
		// calculator on the left, the global values and the recording on the right.
		a.Columns( 2 );
		a.Summary( []{ return TrueSens_GetEnabled() ? std::string( "on" ) : std::string( "off" ); } );
		// Where the values live: outside every profile, per game (or the shared
		// entry when no game is identified).
		a.Badge( []{ return TrueSens_SessionKeyShared() ? std::string( "shared" ) : std::string( "per game" ); } );

		// ---- the tip (D8), then this game --------------------------------------------
		// The owner's sentence is the captions of the two columns' first groups, left half
		// then right half, so it reads across the top of the sheet. A group band is one
		// line the width of its column (it ends in "..." when too long), and a Facts
		// value is capped at ~24 characters, so neither can carry the whole sentence in
		// one column. Fixed text, and the first thing in the area.
		a.Group( "Setting a low in-game sensitivity is usually the" );
		a.GroupColumn( 0 );

		a.Facts( "true_sens.shared", "Game", []{ return std::string( "not identified, shared" ); } )
			.Help( "No game was identified for this session, so these values are shared by all sessions "
			       "without an identified game." )
			.ShownWhen( []{ return TrueSens_SessionKeyShared(); } )
			.Keywords( "unknown game shared" );

		// ---- this game (the rows below share the tip's group: a group with only the
		// conditional "shared" row would show no caption) ------------------------------
		a.Switch( "true_sens.enabled", "True Sens",
		          ui::AnyBind::Of<bool>( []{ return TrueSens_GetEnabled(); },
		                                 []( bool b ) { TrueSens_SetEnabled( b ); } ) )
			.Help( "Gives this game your own mouse feel. While the cursor is hidden, mouse movement is "
			       "scaled so a full turn takes the same hand distance as your recorded sensitivity. "
			       "Needs a recorded game sensitivity and a recorded Low or High." )
			.Default( false )
			.Keywords( "true sens enable on off multiplier this game" );

		static const ui::Option kLowHigh[] = { { 0, "Low" }, { 1, "High" } };
		a.Choice( "true_sens.profile", "Sensitivity",
		          ui::AnyBind::Of<int>( []{ return TrueSens_GetUseHigh() ? 1 : 0; },
		                                []( int n ) { TrueSens_SetUseHigh( n == 1 ); } ),
		          kLowHigh, std::size( kLowHigh ) )
			.Help( "Which of your two recorded sensitivities this game uses." )
			.Default( 0 )
			.Keywords( "low high true sensitivity choose" );

		a.Action( "true_sens.record_game", "Record game sensitivity", "Record", []{ Record( Target::Game ); } )
			.Help( kRecordHelp )
			.ShownWhen( kNotRecording )
			.Keywords( "record game sensitivity calibrate turn 360 counts" );

		a.Facts( "true_sens.game_value", "Game sensitivity", []{ return TrueSens_GameText(); } )
			.Help( "The mouse movement one full turn takes in this game at its current in-game "
			       "sensitivity, as you recorded it. Read-only." );

		a.Facts( "true_sens.multiplier", "Multiplier", []{ return MultiplierLine(); } )
			.Help( "How much this game's mouse movement is scaled right now. Off while True Sens is off "
			       "for this game. Read-only." );

		// ---- true sensitivity (global): right ----------------------------------------
		a.Group( "most reliable way to use True Sens in a game." );
		a.GroupColumn( 1 );

		a.Stepper( "true_sens.dpi", "Mouse DPI",
		           ui::AnyBind::Of<int>( []{ return TrueSens_GetDpi(); },
		                                 []( int n ) { TrueSens_SetDpi( n ); } ) )
			.Range( (float)truesens::kMinDpi, (float)truesens::kMaxDpi ).Step( 50.0f )
			.Help( "The DPI set on your mouse. Your recorded Low and High are kept as a hand distance, so "
			       "changing this later needs no new recording." )
			.Default( truesens::kDefaultDpi )
			.Keywords( "dpi mouse counts per inch cpi" );

		a.Facts( "true_sens.low", "Low (true sensitivity)", []{ return TrueSens_LowText(); } )
			.Help( "Your recorded Low sensitivity: the hand distance of one full turn. Read-only." );

		a.Action( "true_sens.record_low", "Record Low", "Record", []{ Record( Target::Low ); } )
			.Help( ( std::string( "Records your Low sensitivity, and this game's value with it. " ) + kRecordHelp ).c_str() )
			.ShownWhen( kNotRecording )
			.Keywords( "record low true sensitivity calibrate turn 360" );

		a.Facts( "true_sens.high", "High (true sensitivity)", []{ return TrueSens_HighText(); } )
			.Help( "Your recorded High sensitivity: the hand distance of one full turn. Read-only." );

		a.Action( "true_sens.record_high", "Record High", "Record", []{ Record( Target::High ); } )
			.Help( ( std::string( "Records your High sensitivity, and this game's value with it. " ) + kRecordHelp ).c_str() )
			.ShownWhen( kNotRecording )
			.Keywords( "record high true sensitivity calibrate turn 360" );

		// ---- recording -------------------------------------------------------------
		a.Group( "Recording" );
		a.GroupColumn( 1 );

		a.Action( "true_sens.stop", "Stop recording", "Stop", []{ TrueSens_StopRecording(); } )
			.Help( "Ends the recording and stores it, if you turned once around and the game held the "
			       "mouse. Nothing stops by itself." )
			.ShownWhen( kRecording )
			.Keywords( "stop recording finish" );

		a.Facts( "true_sens.recording", "Recording", []{ return RecordingLine(); } )
			.Help( "Which recording is running and how far the mouse has moved sideways so far. Read-only." )
			.ShownWhen( kRecording );

		// The outcome, in full: line 1 is what happened, the lines after it what to do
		// (see OutcomeLines). Same help on every row; the first carries the Details.
		static constexpr const char *kOutcomeHelp =
			"What the last recording stored, or why nothing was stored, and what to do about it. Read-only.";
		a.Facts( "true_sens.outcome", "Last recording", []{ return OutcomeLine( 0 ); } )
			.Help( kOutcomeHelp )
			.ShownWhen( []{ return !OutcomeLine( 0 ).empty(); } )
			.Live( "detail", []{ return ui::Fact{ "result", TrueSens_LastOutcome() }; } );
		for ( size_t n = 1; n < kOutcomeRows; n++ )
		{
			static std::string sIds[ kOutcomeRows ];
			sIds[ n ] = "true_sens.outcome_" + std::to_string( n + 1 );
			a.Facts( sIds[ n ].c_str(), "", [n]{ return OutcomeLine( n ); } )
				.Help( kOutcomeHelp )
				.ShownWhen( [n]{ return !OutcomeLine( n ).empty(); } );
		}

		// ---- calculator (D9, D17): about this game, so left, under its rows -------------
		a.Group( "Calculator" );
		a.GroupColumn( 0 );

		a.Text( "true_sens.ingame", "In-game value",
		        ui::AnyBind::Of<std::string>( []{ return IngameText(); },
		                                      []( std::string s ) { SetIngameText( s ); } ) )
			.Help( "The in-game sensitivity value you had set when you recorded this game. Numbers with a "
			       "dot or a comma both work. Recording the game again clears it: enter the value again." )
			.Validate( ValidateIngame )
			.StatusWhen( []{ return IngameStatus(); } )
			.Keywords( "calculator in-game value sensitivity number" );

		a.Facts( "true_sens.calc", "Set in the game", []{ return CalcLine(); } )
			.Help( "The in-game value that gives you your own feel, for the sensitivity chosen above. "
			       "After setting it in the game, switch True Sens off for this game or record the game "
			       "again, or the correction is applied twice. Read-only." )
			.Keywords( "calculator set ingame value result" );
	}
}
