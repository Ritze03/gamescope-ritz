// virtual_pointer_tool -- a host mouse for scripts/pointer-regression.sh.
//
// The pointer regression gate runs a nested `gamescope --backend wayland`
// inside a private, invisible sway that has NO input devices at all
// (WLR_LIBINPUT_NO_DEVICES=1 -- the whole point is never touching the user's
// desktop). That means the seat has no pointer, gamescope never binds a
// wl_pointer, and the nested backend's own host-side pointer path -- the
// absolute samples force-grab-off delivers through wlserver_touchmotion(),
// the host lock it requests when a game grabs, the relative motion it forwards
// once the host confirms -- can not be exercised through gamescopectl alone.
//
// This tool plugs a mouse into that sway: it binds zwlr_virtual_pointer_v1
// (wlroots' virtual pointer protocol; sway implements it) and then executes
// commands from stdin, one per line, for as long as stdin stays open:
//
//   abs <x> <y> <w> <h>   absolute motion at (x, y) within a w x h extent
//   move <dx> <dy>        relative motion (what a locked host pointer sends)
//   button <code> <0|1>   e.g. 272 1 / 272 0 for a left click
//   sleep <ms>
//   quit
//
// Keeping one process alive for the whole session matters: a virtual pointer
// is a seat device, and sway drops the seat's pointer capability the moment
// the last one goes away, which would tear gamescope's wl_pointer (and any
// host lock on it) down between commands.
//
// Not a meson test. Built alongside the unit tests; run by the script with
// WAYLAND_DISPLAY pointed at the private sway.
#include <wayland-client.h>
#include "wlr-virtual-pointer-unstable-v1-client-protocol.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static struct wl_seat *s_pSeat = NULL;
static struct zwlr_virtual_pointer_manager_v1 *s_pManager = NULL;

static void registry_global( void *pData, struct wl_registry *pRegistry, uint32_t uName, const char *pszInterface, uint32_t uVersion )
{
	(void)pData;
	if ( !strcmp( pszInterface, wl_seat_interface.name ) && !s_pSeat )
		s_pSeat = wl_registry_bind( pRegistry, uName, &wl_seat_interface, uVersion < 5 ? uVersion : 5 );
	else if ( !strcmp( pszInterface, zwlr_virtual_pointer_manager_v1_interface.name ) )
		s_pManager = wl_registry_bind( pRegistry, uName, &zwlr_virtual_pointer_manager_v1_interface, 1 );
}

static void registry_global_remove( void *pData, struct wl_registry *pRegistry, uint32_t uName )
{
	(void)pData; (void)pRegistry; (void)uName;
}

static const struct wl_registry_listener s_RegistryListener = {
	.global = registry_global,
	.global_remove = registry_global_remove,
};

static uint32_t now_ms( void )
{
	struct timespec ts;
	clock_gettime( CLOCK_MONOTONIC, &ts );
	return (uint32_t)( ts.tv_sec * 1000 + ts.tv_nsec / 1000000 );
}

int main( void )
{
	setvbuf( stdout, NULL, _IOLBF, 0 );

	struct wl_display *pDisplay = wl_display_connect( NULL );
	if ( !pDisplay )
	{
		fprintf( stderr, "virtual_pointer_tool: wl_display_connect failed (WAYLAND_DISPLAY?)\n" );
		return 1;
	}
	struct wl_registry *pRegistry = wl_display_get_registry( pDisplay );
	wl_registry_add_listener( pRegistry, &s_RegistryListener, NULL );
	wl_display_roundtrip( pDisplay );
	if ( !s_pSeat || !s_pManager )
	{
		fprintf( stderr, "virtual_pointer_tool: need wl_seat and zwlr_virtual_pointer_manager_v1 (seat=%p manager=%p)\n",
			(void *)s_pSeat, (void *)s_pManager );
		return 1;
	}

	struct zwlr_virtual_pointer_v1 *pPointer = zwlr_virtual_pointer_manager_v1_create_virtual_pointer( s_pManager, s_pSeat );
	wl_display_roundtrip( pDisplay );
	printf( "READY\n" );

	char szLine[256];
	while ( fgets( szLine, sizeof( szLine ), stdin ) )
	{
		char szCmd[32] = { 0 };
		double a = 0, b = 0, c = 0, d = 0;
		const int nArgs = sscanf( szLine, "%31s %lf %lf %lf %lf", szCmd, &a, &b, &c, &d );
		if ( nArgs < 1 )
			continue;
		if ( !strcmp( szCmd, "quit" ) )
			break;
		if ( !strcmp( szCmd, "sleep" ) && nArgs >= 2 )
		{
			usleep( (useconds_t)( a * 1000.0 ) );
			continue;
		}
		if ( !strcmp( szCmd, "abs" ) && nArgs >= 5 )
		{
			zwlr_virtual_pointer_v1_motion_absolute( pPointer, now_ms(), (uint32_t)a, (uint32_t)b, (uint32_t)c, (uint32_t)d );
			zwlr_virtual_pointer_v1_frame( pPointer );
		}
		else if ( !strcmp( szCmd, "move" ) && nArgs >= 3 )
		{
			zwlr_virtual_pointer_v1_motion( pPointer, now_ms(), wl_fixed_from_double( a ), wl_fixed_from_double( b ) );
			zwlr_virtual_pointer_v1_frame( pPointer );
		}
		else if ( !strcmp( szCmd, "button" ) && nArgs >= 3 )
		{
			zwlr_virtual_pointer_v1_button( pPointer, now_ms(), (uint32_t)a, b != 0 ? WL_POINTER_BUTTON_STATE_PRESSED : WL_POINTER_BUTTON_STATE_RELEASED );
			zwlr_virtual_pointer_v1_frame( pPointer );
		}
		else
		{
			fprintf( stderr, "virtual_pointer_tool: unknown command: %s", szLine );
			continue;
		}
		wl_display_roundtrip( pDisplay );
		printf( "OK %s", szLine );
	}

	zwlr_virtual_pointer_v1_destroy( pPointer );
	wl_display_roundtrip( pDisplay );
	wl_display_disconnect( pDisplay );
	return 0;
}
