// The Friends area -- see PanelFriends.h for the two rules this file exists to
// keep, and superdoc/features/steam-friends.md for the feature.
//
// THE ONE DESIGN DECISION WORTH READING BEFORE THE CODE
// ----------------------------------------------------
// The list shows EVERY friend who is in a game, not only the ones that can be
// joined -- which is a deliberate departure from the plan
// (superdoc/planning/steam-friends-join.md §7b phase 3, which says "one line
// per joinable friend").
//
// `Why:` m_steamIDLobby's offset inside FriendGameInfo_t is still unproven
// (§6e). Nobody was in a joinable lobby at any moment the read path was
// measured against the live client, and a WRONG OFFSET READS AS ZERO EXACTLY
// LIKE "not in a lobby". A joinable-only list would therefore be
// indistinguishable from a broken one: the user would see an empty panel and
// have no way to tell whether their friends are all busy or the feature does
// not work. Listing everyone in a game degrades honestly -- the friends and
// their games are visible either way, and only the Join is missing -- and the
// day a real lobby id appears, the marks light up and nothing else changes.
//
// Privacy: persona names are ON SCREEN, because that is the feature. They stay
// OUT OF THE LOG, exactly as src/SteamFriends.cpp's own lines and
// `friends_dump`'s output do -- nothing in this file logs, toasts or writes a
// name, a SteamID or a lobby id.
#include "PanelFriends.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <vector>

#include "Config/ConfigManager.h"
#include "Keybinds.h"
#include "Notifications.h"
#include "SteamFriends.h"
#include "UI/Colors.h"
#include "UI/Controls.h"

namespace gamescope
{
	namespace
	{
		using steamfriends::Friend;
		using steamfriends::Joinability;

		// ---- reading the poller ----------------------------------------------
		// EVERY read goes through steamfriends::CurrentView(), which copies
		// the poller's published rows under its own lock and returns. This
		// file keeps NO cache of its own, deliberately: the getters here run
		// on the draw thread AND -- through `overlay_e2_get` / `overlay_e2_set`
		// -- on the console thread, and a std::vector<Friend> cached in a
		// static would be written by one while the other reads it. Copying a
		// few dozen small rows a handful of times a frame is not a cost worth
		// buying a data race with.
		steamfriends::View ViewNow() { return steamfriends::CurrentView(); }

		// ---- the one setting on this page ------------------------------------
		// global.json, not the session profile: see ConfigSchema.h's
		// friends_lookup_names. Cached against the config generation, exactly
		// as PanelSystem.cpp and PanelCursor.cpp do for their own global rows
		// -- re-reading the file on every draw would be a file read per frame.
		bool     s_bGlobalLoaded = false;
		uint64_t s_ulGlobalGeneration = 0;
		config::Settings s_Global;

		void EnsureGlobalLoaded()
		{
			const uint64_t ulGeneration = config::ConfigGeneration();
			if ( s_bGlobalLoaded && ulGeneration == s_ulGlobalGeneration )
				return;
			s_Global = config::LoadGlobal();
			s_ulGlobalGeneration = ulGeneration;
			s_bGlobalLoaded = true;
		}

		// ---- selection and the pending join ---------------------------------
		// THE SELECTION IS A PERSON, NOT A ROW NUMBER, and that is the whole
		// point of storing a SteamID here rather than an int.
		//
		// `Why:` the list is SORTED (SteamFriendsCmd.h's FriendOrderLess --
		// invites, then joinable, then everyone else) and the poller replaces
		// it every three seconds. The moment one friend joins a lobby, every
		// row below them moves. An index-based selection would then be
		// pointing at whoever slid into that slot -- the outline would jump to
		// a different person under a user who had not touched anything, and
		// the Join verb would be aimed at them. Keeping the SteamID and
		// looking the index up per frame makes the highlight FOLLOW the person
		// through every reorder, which is what a reader expects and is the
		// classic bug in a list that sorts itself.
		//
		// The pending join keeps the same id for the same reason one step
		// later: Tick() must join who was clicked, not who is at that index by
		// the time the frame runs.
		uint64_t s_ulSelectedSteamId = 0;   // 0 == nothing selected
		bool     s_bJoinPending = false;
		uint64_t s_ulPendingSteamId = 0;
		uint32_t s_uPendingAppId = 0;

		bool ListIsEmpty() { return ViewNow().vecFriends.empty(); }

		// Where the selected person is RIGHT NOW, or -1 if they are gone (went
		// offline, quit the game) -- which is a selection that has genuinely
		// stopped existing, not one that moved.
		int SelectedIndexIn( const steamfriends::View &v )
		{
			if ( !s_ulSelectedSteamId )
				return -1;
			for ( size_t i = 0; i < v.vecFriends.size(); i++ )
				if ( v.vecFriends[ i ].ulSteamId == s_ulSelectedSteamId )
					return (int)i;
			return -1;
		}

		int SelectedIndex() { return SelectedIndexIn( ViewNow() ); }

		// By VALUE, not by pointer: the view it came out of is a temporary,
		// and a pointer into it would dangle the moment the caller used it.
		std::optional<Friend> SelectedFriend()
		{
			const steamfriends::View v = ViewNow();
			const int nIndex = SelectedIndexIn( v );
			if ( nIndex < 0 )
				return std::nullopt;
			return v.vecFriends[ (size_t)nIndex ];
		}

		// The app id this session is running under, as a number. nullopt when
		// gamescope was not launched by Steam -- in which case every join is a
		// "different game" and gets the confirmation.
		std::optional<uint32_t> SessionAppIdNumber()
		{
			// strtoul, not stoul: this build has exceptions off
			// (meson.build's -fno-exceptions), and an app id that is not a
			// number is a "no app id" answer rather than an error.
			const std::optional<std::string> &oId = config::SessionAppId();
			if ( !oId || oId->empty() )
				return std::nullopt;
			char *pszEnd = nullptr;
			const unsigned long ul = strtoul( oId->c_str(), &pszEnd, 10 );
			if ( !pszEnd || *pszEnd != '\0' || ul == 0 || ul > 0xFFFFFFFFul )
				return std::nullopt;
			return (uint32_t)ul;
		}

		bool SameGameAsThisSession( uint32_t uAppId )
		{
			const std::optional<uint32_t> oMine = SessionAppIdNumber();
			return oMine && *oMine == uAppId && uAppId != 0;
		}

		// ---- the rows --------------------------------------------------------
		// The empty states are TEXT IN THE LIST, not blank space: an empty box
		// with a status line somewhere else is the shape that makes a user
		// wonder whether the panel is broken. Each state says a different,
		// true thing, and they all come from steamfriends::StatusText() so the
		// panel and `friends_dump` can never disagree about why the list is
		// short.
		std::vector<ui::ListItem> Items()
		{
			const steamfriends::View v = ViewNow();

			std::vector<ui::ListItem> items;
			if ( v.vecFriends.empty() )
			{
				std::string sLine = v.sStatus;
				if ( !sLine.empty() )
					sLine[ 0 ] = (char)toupper( (unsigned char)sLine[ 0 ] );
				items.push_back( ui::ListItem{ sLine.empty() ? "Asking Steam..." : sLine, "", "" } );
				return items;
			}

			items.reserve( v.vecFriends.size() );
			for ( const Friend &f : v.vecFriends )
			{
				// The secondary line carries the game and, when there is one,
				// the quiet reason this row cannot be joined. The LABEL (the
				// person) is never sacrificed to it -- that priority is the
				// list atom's own (Controls.h's LayoutListBoxItem).
				std::string sSecondary = f.sGame;
				if ( !f.CanJoin() )
				{
					sSecondary += " \xc2\xb7 ";
					sSecondary += std::string( steamfriends::JoinabilityText( f.eJoinable ) );
				}
				items.push_back( ui::ListItem{
					f.sPersona.empty() ? std::string( "(no name)" ) : f.sPersona,
					f.CanJoin() ? std::string( "[Join]" ) : std::string(),
					std::move( sSecondary ) } );
			}
			return items;
		}

		// A click, an Enter, or `overlay_e2_set system.friends friends.list N`.
		// ALL IT DOES IS RECORD. See PanelFriends.h's second rule: this can
		// arrive on the console thread, where forking a process and opening a
		// modal are both illegal, so the acting is Tick()'s job.
		void ActivateRow( int nIndex )
		{
			const steamfriends::View v = ViewNow();
			if ( nIndex < 0 || (size_t)nIndex >= v.vecFriends.size() )
			{
				// The placeholder row (an empty list is one item of text).
				// Selecting it is not an error and is not a join.
				s_ulSelectedSteamId = 0;
				return;
			}

			const Friend &f = v.vecFriends[ (size_t)nIndex ];
			s_ulSelectedSteamId = f.ulSteamId;
			if ( !f.CanJoin() )
			{
				// The quiet reason, without saying WHO -- a toast is on screen
				// and in the notification history, and this file's privacy
				// rule is that a name never leaves the list.
				Notifications::Show( "Can't join: " +
					std::string( steamfriends::JoinabilityText( f.eJoinable ) ) + ".",
					Notifications::Kind::Info, 3.0f );
				return;
			}

			s_bJoinPending     = true;
			s_ulPendingSteamId = f.ulSteamId;
			s_uPendingAppId    = f.uAppId;
		}

		void FireJoin( const Friend &f )
		{
			std::string sWhy;
			if ( !steamfriends::Join( f, &sWhy ) )
			{
				Notifications::Show( sWhy, Notifications::Kind::Warning, 5.0f );
				return;
			}
			Notifications::Show( "Asked Steam to join " + f.sGame + ".",
				Notifications::Kind::Ok, 3.0f );
		}

		// Why the Join verb is dimmed, in one sentence -- the same contract
		// DisabledUnless() has for a row, and what the Inspector prints.
		std::string JoinBlocker()
		{
			if ( ListIsEmpty() )
				return steamfriends::CurrentView().sStatus;
			const std::optional<Friend> oSel = SelectedFriend();
			if ( !oSel )
				return "Pick somebody in the list first.";
			if ( !oSel->CanJoin() )
				return "They're " + std::string( steamfriends::JoinabilityText( oSel->eJoinable ) ) + ".";
			return "";
		}
	}

	// =========================================================================
	//  Tick -- the only place a join is actually fired
	// =========================================================================
	// Once per frame from Shell.cpp's Draw(), on the steamcompmgr thread. Every
	// caller of ActivateRow() -- a click, Enter, or a ConCommand on the console
	// thread -- lands here, so forking `steam` and opening a modal both happen
	// on the one thread that is allowed to do them.
	void PanelFriends_Tick()
	{
		if ( !s_bJoinPending )
			return;

		// Find the row the click was AIMED at, not the row at that index now:
		// the poller may have replaced the list in between, and joining
		// whoever happened to land on index 2 is the bug that would produce.
		const steamfriends::View v = ViewNow();
		const Friend *pTarget = nullptr;
		for ( const Friend &f : v.vecFriends )
		{
			// The app id is checked as well as the SteamID: a friend who
			// left one game for another between the click and this frame is
			// not the row that was clicked, and joining them anyway would
			// launch a game the user never picked.
			if ( f.ulSteamId == s_ulPendingSteamId && f.uAppId == s_uPendingAppId && f.CanJoin() )
			{
				pTarget = &f;
				break;
			}
		}

		s_bJoinPending = false;
		if ( !pTarget )
		{
			Notifications::Show( "That lobby is gone.", Notifications::Kind::Info, 3.0f );
			return;
		}

		// THE CONFIRMATION, AND EXACTLY WHEN IT APPEARS. Joining a friend in
		// the game you are already in is the case this feature is for: Steam
		// hands the running game a lobby join and nothing relaunches, so a
		// dialog there would be a speed bump on the common path. A DIFFERENT
		// app id means Steam starts another game over this one -- which is
		// worth stopping to ask about, and is the only thing the confirmation
		// asks about.
		if ( SameGameAsThisSession( pTarget->uAppId ) )
		{
			FireJoin( *pTarget );
			return;
		}

		if ( ui::IsModalOpen() )
			return;

		const Friend target = *pTarget;   // copied: the modal outlives this frame
		const std::optional<uint32_t> oMine = SessionAppIdNumber();
		const std::string sHere = oMine
			? ( config::SessionGameName().empty()
				? ( "App " + std::to_string( (unsigned long long)*oMine ) )
				: config::SessionGameName() )
			: std::string();

		ui::ModalSpec spec;
		spec.sTitle        = "Join " + target.sGame + "?";
		spec.sPrimaryLabel = "Join";
		spec.flMinBodyRows = 2.0f;
		const std::string sLine1 = sHere.empty()
			? std::string( "This starts a different game." )
			: ( "This leaves " + sHere + " and starts " + target.sGame + "." );
		spec.fnBody = [ sLine1 ]( ui::ModalBodyCtx &ctx )
		{
			ui::DrawText( ui::ModalNextBlock( ctx, ui::Px( ui::tok::kControlH ) ),
				ui::TypeRole::Body, ui::Col( ui::Role::TextBody ), sLine1.c_str() );
			ui::DrawText( ui::ModalNextBlock( ctx, ui::Px( ui::tok::kControlH ) ),
				ui::TypeRole::Body, ui::Col( ui::Role::TextMeta ),
				"Steam will launch it over the game running here." );
		};
		spec.fnPrimary = [ target ]{ FireJoin( target ); };
		ui::OpenModal( std::move( spec ) );
	}

	void PanelFriends_RegisterArea( ui::Registry &reg )
	{
		ui::Area &a = reg.Add( "system.friends", "Friends", ui::Section::System );
		a.Keywords( "friends friend steam join joinable lobby party invite play playing game "
		            "list persona online who" );
		// "global only", the badge Appearance, Cursor and Keybinds carry: the
		// one setting on this page (the name lookup below) writes global.json
		// whatever profile the session is editing, because "may this machine
		// reach the network" is a fact about the machine and not about the
		// game. Until 2026-09-09 this area had no settings at all and
		// deliberately carried no badge.
		a.Badge( []{ return std::string( "global only" ); } );
		a.Summary( []{ return steamfriends::CurrentView().sStatus; } );

		a.Group( "Friends in a game" );

		a.Composite( "friends.list", "Friends", ui::CompositeKind::List,
			ui::AnyBind::Of<int>(
				[]{ return SelectedIndex(); },
				[]( int n ) { ActivateRow( n ); } ) )
			.Items( []{ return Items(); } )
			.ListAction( "Join", []
				{
					const int nIndex = SelectedIndex();
					if ( JoinBlocker().empty() && nIndex >= 0 )
						ActivateRow( nIndex );
				}, /* bDanger */ false, JoinBlocker )
			.ListAction( "Refresh", []{ steamfriends::RequestRefresh(); } )
			.Help( "Everyone on your Steam friends list who is in a game right now, read straight "
			       "from the Steam client already running on this machine - no separate sign-in. "
			       "The ones you can join are listed first, then everyone else, alphabetically "
			       "within each group. A friend marked [Join] is in a lobby you can join: click "
			       "them, or press Enter, and Steam moves you in. Friends without the mark are "
			       "playing but not in a joinable lobby; the line says which. Joining somebody in "
			       "the game you are already in happens straight away; joining a different game "
			       "asks first, because it starts that game over this one." )
			.Keywords( "friends list join lobby joinable playing game persona refresh order sort" )
			.Live( "status", []
				{
					return ui::Fact{ "friends", steamfriends::CurrentView().sStatus };
				} )
			.Live( "selected", []
				{
					const std::optional<Friend> o = SelectedFriend();
					if ( !o )
						return ui::Fact{ "selected", "nobody" };
					return ui::Fact{ "selected",
						o->sGame + ( o->CanJoin() ? " - joinable"
							: " - " + std::string( steamfriends::JoinabilityText( o->eJoinable ) ) ) };
				} )
			.Live( "updated", []
				{
					const steamfriends::View v = steamfriends::CurrentView();
					if ( !v.bPolled )
						return ui::Fact{ "updated", "not yet - asking Steam" };
					char sz[ 64 ];
					snprintf( sz, sizeof( sz ), "%.0f seconds ago", v.flAgeSec );
					return ui::Fact{ "updated", sz };
				} )
			.Live( "hotkey", []
				{
					return ui::Fact{ "hotkey",
						keybinds::ChordTextFor( keybinds::Action::Friends ) + " opens this list" };
				} );

		a.Group( "Game names" );

		// THE ONE ROW IN THIS FORK THAT DECIDES WHETHER THE COMPOSITOR OPENS A
		// SOCKET. ConfigSchema.h's friends_lookup_names carries the full
		// argument for the default; the Help below is the user-facing half of
		// it, and it names exactly what leaves the machine rather than saying
		// "looks names up online" and leaving them to wonder.
		a.Switch( "overlay.friends_lookup_names", "Look up game names online",
			ui::AnyBind::Of<bool>(
				[]
				{
					EnsureGlobalLoaded();
					return s_Global.overlay.friends_lookup_names;
				},
				[]( bool b )
				{
					EnsureGlobalLoaded();
					s_Global.overlay.friends_lookup_names = b;
					// The runtime flag and the file move together: the poller
					// reads the flag, and a switch that only wrote the file
					// would not take effect until the next launch.
					steamfriends::SetLookupNames( b );
					config::EnqueueGlobalWrite( s_Global );
				} ) )
			.Help( "A friend playing something you have not installed shows as \"App 252490\", "
			       "because the name comes from the game's own files on this machine. With this "
			       "on, those ids are looked up once against Steam's public list and remembered "
			       "on disk, so it happens once per game ever. What is sent is a list of app ids "
			       "and nothing else - no Steam ID, no name, nothing about you - and only while "
			       "this list is open. Off, an unknown game stays \"App 252490\" and nothing "
			       "leaves this machine." )
			.Key( "overlay.friends_lookup_names" )
			.Default( config::OverlaySettings{}.friends_lookup_names )
			.Keywords( "game name lookup online network internet steam api cache offline privacy" )
			.Live( "cache", []
				{
					const steamfriends::NameCacheInfo info = steamfriends::NameCache();
					char sz[ 96 ];
					snprintf( sz, sizeof( sz ), "%zu of %zu names remembered",
						info.nEntries, info.nMax );
					return ui::Fact{ "cache", sz };
				} )
			.Live( "cache_file", []
				{
					const steamfriends::NameCacheInfo info = steamfriends::NameCache();
					return ui::Fact{ "stored in",
						info.sPath.empty() ? std::string( "nowhere - no cache directory" ) : info.sPath };
				} );

		a.Group( "Status" );

		a.Facts( "friends.status", "Status", []{ return steamfriends::CurrentView().sStatus; } )
			.Help( "Why the list above is the length it is: Steam not running, signed out, nobody "
			       "playing, or how many of the friends who are playing you can actually join. "
			       "Read-only." )
			.Keywords( "status steam running signed out joinable count why empty" )
			.Live( "session", []
				{
					const std::optional<uint32_t> o = SessionAppIdNumber();
					return ui::Fact{ "this game",
						o ? std::to_string( (unsigned long long)*o ) : std::string( "not a Steam game" ) };
				} )
			.Live( "lobby_offset", []
				{
					// Stated in the product, not only in the docs: the one
					// thing about this feature that is not yet proven is
					// whether the joinable mark can ever light up on this
					// machine, and a user seeing "none you can join" forever
					// deserves to know that is a candidate explanation.
					return ui::Fact{ "note",
						"the joinable mark has not yet been seen light up on a real lobby - "
						"see the Steam friends page in the docs" };
				} )
			.Live( "invites", []
				{
					// SAID IN THE PRODUCT, NOT ONLY IN THE DOCS, because
					// "where are my invites" is the obvious next question of
					// anybody looking at a friends list -- and the honest
					// answer is that Steam does not offer them to us. See
					// superdoc/features/steam-friends.md, "Received invites",
					// for the measurement behind this sentence.
					return ui::Fact{ "invites",
						"not shown - Steam has no way to tell this list about an invite you "
						"have been sent; accept those in Steam itself" };
				} );
	}

	// Called at startup, from main.cpp, beside PanelSystem_SeedFromConfig().
	// `Why it is not enough to seed at registration:` the registry is built
	// lazily, the first time the shell is drawn -- but `friends_dump` on the
	// console reaches the poller without the shell ever existing. Seeding here
	// means the network switch is honoured from the first Steam call of the
	// process, not from the first time somebody opens the overlay.
	void PanelFriends_SeedFromConfig()
	{
		EnsureGlobalLoaded();
		steamfriends::SetLookupNames( s_Global.overlay.friends_lookup_names );
	}
}
