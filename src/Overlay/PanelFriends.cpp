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

#include "imgui.h"

namespace gamescope
{
	namespace
	{
		using steamfriends::Friend;
		using steamfriends::Joinability;

		// ---- the cached view -------------------------------------------------
		// steamfriends::CurrentView() never blocks (it copies what the poller
		// thread published), but it is asked once per frame and the answer is
		// wanted by Items(), the verbs, the summary and the facts -- so it is
		// read once into here and shared. `Why not just call it four times:`
		// four calls in one frame could see four different lists, and the
		// index a click produced would then be looked up in a list that had
		// already moved.
		steamfriends::View s_View;
		uint64_t           s_ulViewFrame = 0;

		const steamfriends::View &ViewNow()
		{
			// One read per frame, keyed on ImGui's own frame counter. Outside
			// a frame (the console thread's `overlay_e2_get`) the counter does
			// not move, which is exactly right: a script reads the same list
			// the last frame drew.
			const uint64_t ulFrame = (uint64_t)ImGui::GetFrameCount();
			if ( ulFrame != s_ulViewFrame || !s_View.bPolled )
			{
				s_ulViewFrame = ulFrame;
				s_View = steamfriends::CurrentView();
			}
			return s_View;
		}

		// ---- selection and the pending join ---------------------------------
		// -1 is "nothing selected". The index addresses ViewNow().vecFriends,
		// which the poller can replace between the click and the Tick() that
		// acts on it -- so the pending join stores the SteamID and app id it
		// was aimed at, not only the index, and Tick() refuses if the row it
		// finds is not the row that was clicked. Joining the wrong person
		// because a list refreshed underneath a click is exactly the bug an
		// index-only handoff produces.
		int      s_nSelected = -1;
		bool     s_bJoinPending = false;
		uint64_t s_ulPendingSteamId = 0;
		uint32_t s_uPendingAppId = 0;

		bool ListIsEmpty() { return ViewNow().vecFriends.empty(); }

		const Friend *SelectedFriend()
		{
			const steamfriends::View &v = ViewNow();
			if ( s_nSelected < 0 || (size_t)s_nSelected >= v.vecFriends.size() )
				return nullptr;
			return &v.vecFriends[ (size_t)s_nSelected ];
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
			const steamfriends::View &v = ViewNow();

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
			const steamfriends::View &v = ViewNow();
			if ( nIndex < 0 || (size_t)nIndex >= v.vecFriends.size() )
			{
				// The placeholder row (an empty list is one item of text).
				// Selecting it is not an error and is not a join.
				s_nSelected = -1;
				return;
			}

			s_nSelected = nIndex;
			const Friend &f = v.vecFriends[ (size_t)nIndex ];
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
			const Friend *pSel = SelectedFriend();
			if ( !pSel )
				return "Pick somebody in the list first.";
			if ( !pSel->CanJoin() )
				return "They're " + std::string( steamfriends::JoinabilityText( pSel->eJoinable ) ) + ".";
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
		const steamfriends::View &v = ViewNow();
		const Friend *pTarget = nullptr;
		for ( const Friend &f : v.vecFriends )
		{
			if ( f.ulSteamId == s_ulPendingSteamId && f.CanJoin() )
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
		// No badge: this area has no settings and writes no file, so the
		// session-profile badge every other area carries would be answering a
		// question ("where does what I change here get written") that nothing
		// on this page asks.
		a.Badge( []{ return std::string(); } );
		a.Summary( []{ return steamfriends::CurrentView().sStatus; } );

		a.Group( "Friends in a game" );

		a.Composite( "friends.list", "Friends", ui::CompositeKind::List,
			ui::AnyBind::Of<int>(
				[]{ return s_nSelected; },
				[]( int n ) { ActivateRow( n ); } ) )
			.Items( []{ return Items(); } )
			.ListAction( "Join", []
				{
					if ( JoinBlocker().empty() && s_nSelected >= 0 )
						ActivateRow( s_nSelected );
				}, /* bDanger */ false, JoinBlocker )
			.ListAction( "Refresh", []{ steamfriends::RequestRefresh(); } )
			.Help( "Everyone on your Steam friends list who is in a game right now, read straight "
			       "from the Steam client already running on this machine - no separate sign-in. "
			       "A friend marked [Join] is in a lobby you can join: click them, or press Enter, "
			       "and Steam moves you in. Friends without the mark are playing but not in a "
			       "joinable lobby; the line says which. Joining somebody in the game you are "
			       "already in happens straight away; joining a different game asks first, because "
			       "it starts that game over this one." )
			.Keywords( "friends list join lobby joinable playing game persona refresh" )
			.Live( "status", []
				{
					return ui::Fact{ "friends", steamfriends::CurrentView().sStatus };
				} )
			.Live( "selected", []
				{
					const Friend *p = SelectedFriend();
					if ( !p )
						return ui::Fact{ "selected", "nobody" };
					return ui::Fact{ "selected",
						p->sGame + ( p->CanJoin() ? " - joinable"
							: " - " + std::string( steamfriends::JoinabilityText( p->eJoinable ) ) ) };
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
				} );
	}
}
