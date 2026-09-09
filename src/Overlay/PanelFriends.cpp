// The Friends area -- see PanelFriends.h for the three rules this file exists
// to keep, and superdoc/features/steam-friends.md for the feature.
//
// THE ONE DESIGN DECISION WORTH READING BEFORE THE CODE
// ----------------------------------------------------
// The list shows ONLY the friends who are in the game this session is running
// AND in a lobby you can walk into. Everyone else -- a friend in a different
// game, a friend in this game who is not joinable -- is not a row.
//
// `Why that is safe even though m_steamIDLobby's offset is still unproven
// (superdoc/planning/steam-friends-join.md §6e):` a wrong offset reads as ZERO,
// which is byte-for-byte identical to "not in a lobby you can join", so a
// joinable-only LIST would on its own be indistinguishable from a broken one --
// an empty box, and no way to tell which. That is exactly why the STATUS ROW
// carries two numbers instead of one: "3 friends in this game, 0 you can join"
// says the read is working and nobody is joinable, where "0 friends in this
// game" says nobody is here at all. The diagnosability lives in one line of
// text rather than in a list full of rows the user cannot act on.
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

		// ---- reading the poller ----------------------------------------------
		// EVERY read goes through steamfriends::CurrentView(), which copies
		// the poller's published rows under its own lock and returns. This
		// file keeps NO cache of its own, deliberately: the getters here run
		// on the draw thread AND -- through `overlay_e2_get` / `overlay_e2_set`
		// -- on the console thread, and a std::vector<Friend> cached in a
		// static would be written by one while the other reads it. Copying a
		// few small rows a handful of times a frame is not a cost worth buying
		// a data race with.
		steamfriends::View ViewNow() { return steamfriends::CurrentView(); }

		// ---- selection and the pending join ---------------------------------
		// THE SELECTION IS A PERSON, NOT A ROW NUMBER, and that is the whole
		// point of storing a SteamID here rather than an int.
		//
		// `Why:` the list is SORTED (SteamFriendsCmd.h's FriendOrderLess) and
		// the poller replaces it every three seconds. The moment one friend
		// leaves their lobby, every row below them moves. An index-based
		// selection would then be pointing at whoever slid into that slot --
		// the outline would jump to a different person under a user who had
		// not touched anything, and the Join verb would be aimed at them.
		// Keeping the SteamID and looking the index up per frame makes the
		// highlight FOLLOW the person through every reorder, which is what a
		// reader expects and is the classic bug in a list that sorts itself.
		//
		// The pending join keeps the same id for the same reason one step
		// later: Tick() must join who was clicked, not who is at that index by
		// the time the frame runs.
		uint64_t s_ulSelectedSteamId = 0;   // 0 == nothing selected
		bool     s_bJoinPending = false;
		uint64_t s_ulPendingSteamId = 0;

		bool ListIsEmpty() { return ViewNow().vecFriends.empty(); }

		// Where the selected person is RIGHT NOW, or -1 if they are gone (left
		// the lobby, quit the game, went offline) -- which is a selection that
		// has genuinely stopped existing, not one that moved.
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

			// A row is a PERSON and a [Join], and nothing else. There is no
			// game column any more: every row is the game already running, so
			// a name there would repeat the same words down the whole list --
			// which is the clutter this narrowing was asked for to remove.
			items.reserve( v.vecFriends.size() );
			for ( const Friend &f : v.vecFriends )
			{
				items.push_back( ui::ListItem{
					f.sPersona.empty() ? std::string( "(no name)" ) : f.sPersona,
					std::string( "[Join]" ),
					std::string() } );
			}
			return items;
		}

		// A click, an Enter, or `overlay_e2_set system.friends friends.list N`.
		// ALL IT DOES IS RECORD. See PanelFriends.h's second rule: this can
		// arrive on the console thread, where forking a process is illegal, so
		// the acting is Tick()'s job.
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

			// Every row in this list is joinable by construction, so there is
			// no "you cannot join that" branch here any more -- the rows that
			// would have needed one are not drawn at all.
			const Friend &f = v.vecFriends[ (size_t)nIndex ];
			s_ulSelectedSteamId = f.ulSteamId;
			s_bJoinPending      = true;
			s_ulPendingSteamId  = f.ulSteamId;
		}

		void FireJoin( const Friend &f )
		{
			std::string sWhy;
			if ( !steamfriends::Join( f, &sWhy ) )
			{
				Notifications::Show( sWhy, Notifications::Kind::Warning, 5.0f );
				return;
			}
			// No name and no id: the toast is on screen and in the
			// notification history, and this file's privacy rule is that a
			// persona name never leaves the list.
			Notifications::Show( "Asked Steam to move you into their lobby.",
				Notifications::Kind::Ok, 3.0f );
		}

		// Why the Join verb is dimmed, in one sentence -- the same contract
		// DisabledUnless() has for a row, and what the Inspector prints.
		std::string JoinBlocker()
		{
			if ( ListIsEmpty() )
				return steamfriends::CurrentView().sStatus;
			if ( !SelectedFriend() )
				return "Pick somebody in the list first.";
			return "";
		}
	}

	// =========================================================================
	//  Tick -- the only place a join is actually fired
	// =========================================================================
	// Once per frame from Shell.cpp's Draw(), on the steamcompmgr thread. Every
	// caller of ActivateRow() -- a click, Enter, or a ConCommand on the console
	// thread -- lands here, so forking `steam` happens on the one thread that
	// is allowed to do it.
	//
	// THERE IS NO CONFIRMATION DIALOG ANY MORE, and its absence is the point:
	// it existed only for the "this friend is in a DIFFERENT game, so Steam
	// will close yours and launch theirs" case, and that row cannot exist now
	// that the list is scoped to this session's own app id. Joining somebody in
	// the game you are already in relaunches nothing -- the running game gets
	// the lobby through its own callback -- so a dialog would be a speed bump
	// on the only path there is.
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
			if ( f.ulSteamId == s_ulPendingSteamId )
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
		FireJoin( *pTarget );
	}

	void PanelFriends_RegisterArea( ui::Registry &reg )
	{
		ui::Area &a = reg.Add( "system.friends", "Friends", ui::Section::System );
		a.Keywords( "friends friend steam join joinable lobby party play playing game "
		            "list persona online who" );

		// HIDDEN ENTIRELY WHEN THERE IS NO STEAM APP ID -- the user's own
		// instruction ("dont show the 'Friends' menu at all, if it isnt a
		// steam game"), and the right shape for it regardless: with no app id
		// the list can only ever be empty, because every row has to match an
		// id that does not exist. Registry::RailAreas(), CommandPalette.cpp's
		// two walks and Shell.cpp's SelectedArea() all skip an area that is
		// not Available(), so this one predicate removes the rail entry, the
		// palette rows and the sheet together -- no disabled entry, nothing to
		// click on and be told no.
		//
		// It reads the SEEDED id rather than config::SessionAppId() so that
		// the panel and the poller can never disagree about which game this
		// is; PanelFriends_SeedFromConfig() below is what puts it there.
		a.AvailableWhen( []{ return steamfriends::SessionAppId() != 0; } );

		// NO BADGE, deliberately. The badge answers "where does what I change
		// here get written?", and this area has nothing to change: its one
		// setting -- the online name lookup -- went with the lookup on
		// 2026-09-09. An empty override suppresses the registry's default
		// (the session profile), which would otherwise promise a routing
		// decision that never happens.
		a.Badge( []{ return std::string(); } );
		a.Summary( []{ return steamfriends::CurrentView().sStatus; } );

		a.Group( "Friends you can join" );

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
			.Help( "The friends who are in this game right now and in a lobby you can walk "
			       "into, read straight from the Steam client already running on this machine "
			       "- no separate sign-in and nothing leaves this machine. Click somebody, or "
			       "press Enter, and Steam moves you into their lobby without relaunching "
			       "anything. Friends in other games are not listed, because joining one would "
			       "close this game and start theirs. If the list is empty, the Status line "
			       "below says whether nobody is here or nobody here is joinable." )
			.Keywords( "friends list join lobby joinable playing game persona refresh order sort" )
			.Live( "status", []
				{
					return ui::Fact{ "friends", steamfriends::CurrentView().sStatus };
				} )
			.Live( "selected", []
				{
					const std::optional<Friend> o = SelectedFriend();
					return ui::Fact{ "selected", o ? "joinable" : "nobody" };
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

		a.Group( "Status" );

		// THE ROW THAT KEEPS A JOINABLE-ONLY LIST DIAGNOSABLE. It carries two
		// counts, not one, and the file header explains why: with
		// m_steamIDLobby's offset unproven, "0 you can join" beside a non-zero
		// "friends in this game" is the difference between a working read with
		// nobody available and a lobby field being read from the wrong place.
		a.Facts( "friends.status", "Status", []{ return steamfriends::CurrentView().sStatus; } )
			.Help( "Why the list above is the length it is: Steam not running, signed out, "
			       "nobody else in this game, or how many of the people who ARE in this game "
			       "you can actually join. The two numbers are separate on purpose - friends "
			       "here but none joinable is a different thing from nobody here. Read-only." )
			.Keywords( "status steam running signed out joinable count why empty" )
			.Live( "session", []
				{
					const uint32_t uAppId = steamfriends::SessionAppId();
					return ui::Fact{ "app id",
						uAppId ? std::to_string( (unsigned long long)uAppId )
						       : std::string( "not a Steam game" ) };
				} )
			.Live( "here", []
				{
					const steamfriends::View v = steamfriends::CurrentView();
					char sz[ 64 ];
					snprintf( sz, sizeof( sz ), "%zu (%zu joinable)",
						v.nInThisGame, v.vecFriends.size() );
					return ui::Fact{ "friends here", sz };
				} )
			.Live( "lobby_offset", []
				{
					// Stated in the product, not only in the docs: the one
					// thing about this feature that is not yet proven is
					// whether the joinable mark can ever light up on this
					// machine, and a user seeing "0 you can join" forever
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
	// means the app id the whole feature is scoped by is in place before the
	// first Steam call of the process, not before the first draw.
	void PanelFriends_SeedFromConfig()
	{
		// strtoul, not stoul: this build has exceptions off (meson.build's
		// -fno-exceptions), and an app id that is not a number is a "no app
		// id" answer rather than an error.
		uint32_t uAppId = 0;
		const std::optional<std::string> &oId = config::SessionAppId();
		if ( oId && !oId->empty() )
		{
			char *pszEnd = nullptr;
			const unsigned long ul = strtoul( oId->c_str(), &pszEnd, 10 );
			if ( pszEnd && *pszEnd == '\0' && ul != 0 && ul <= 0xFFFFFFFFul )
				uAppId = (uint32_t)ul;
		}
		steamfriends::SetSessionAppId( uAppId );
	}
}
