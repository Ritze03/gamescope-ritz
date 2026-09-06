// pointer_grab_client_x11 -- the CS2-shaped "game" for scripts/pointer-regression.sh.
//
// CS2 is an SDL3 game running under gamescope's Xwayland. In a match it calls
// SDL_SetWindowRelativeMouseMode(window, true): SDL3's X11 driver hides the
// cursor (an empty-mask cursor, which the X server reports as no cursor),
// takes an XGrabPointer confined to the window, and from then on ignores core
// MotionNotify and reads XI2 raw motion selected on the ROOT window for
// XIAllMasterDevices, turning the raw valuators into deltas. Xwayland, seeing a
// hidden cursor plus a confining grab, asks gamescope for a
// zwp_locked_pointer_v1. This client does exactly that, natively in SDL3 --
// no sdl2-compat in between -- and additionally hooks SDL's X11 event loop
// (SDL_SetX11EventHook) so every raw XI2 event is printed with the device it
// came from and that device's axis mode, which is the one fact that
// discriminates the emitters:
//
//   Xwayland has TWO pointer slaves. wl_pointer.motion (absolute) goes through
//   "xwayland-pointer" (Abs X/Abs Y axes); zwp_relative_pointer_v1 motion goes
//   through "xwayland-relative-pointer" (Rel X/Rel Y). Both post through the
//   master "Virtual core pointer", whose axis classes are a copy of whichever
//   slave posted LAST. SDL3 caches the master's classes the first time it sees
//   a raw event from it and never refreshes them (XI_DeviceChanged is not
//   handled), so what the menu drove the master with decides how the match's
//   raw deltas are read: relative axes taken as absolute get differenced
//   (delta = value - previous value), which is the "springs back to centre"
//   joystick. See superdoc/features/cursor-pipeline.md, "The device SDL3
//   remembers".
//
// Options
//   --lock            enter relative mode immediately (the old client's --lock)
//   --lock-after S    stay a plain windowed client (a game's menu) for S
//                     seconds, THEN enter relative mode -- the CS2 order
//   --fullscreen      SDL_WINDOW_FULLSCREEN, as CS2 runs
//   --seconds N       exit after N seconds (default 60; 0 = run until closed)
//
// Output, one line each, to stdout:
//   READY lock=<0|1> driver=x11
//   DEVICE id=<n> name=<..> axis0=<abs|rel> axis1=<abs|rel>   (once per device
//                     id seen in a raw event, queried at that moment)
//   RAW dev=<id> src=<id> v0=<..> v1=<..>   every XI_RawMotion (raw_values)
//   CORE x=<..> y=<..>                      every core MotionNotify
//   MOTION x=<..> y=<..> xrel=<..> yrel=<..>  every SDL_EVENT_MOUSE_MOTION
//   LOCKED                                   relative mode was just enabled
//   STATS motions=<n> relative=<0|1>         every 500 ms
//   DONE motions=<n>
//
// Not a meson test: it needs a running gamescope. Built alongside the unit
// tests when SDL3 + XInput2 are available; run with SDL_VIDEODRIVER=x11 inside
// gamescope's own Xwayland.
#include <SDL3/SDL.h>
#include <SDL3/SDL_system.h>
#include <X11/Xlib.h>
#include <X11/extensions/XInput2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static Display *s_pDisplay = NULL;
static int s_nXiOpcode = -1;

// Devices we have already described, so DEVICE prints once per id.
static int s_nSeenDevices[64];
static int s_nSeenDeviceCount = 0;

static void describe_device( int nDeviceId )
{
	for ( int i = 0; i < s_nSeenDeviceCount; i++ )
		if ( s_nSeenDevices[i] == nDeviceId )
			return;
	if ( s_nSeenDeviceCount < (int)( sizeof( s_nSeenDevices ) / sizeof( s_nSeenDevices[0] ) ) )
		s_nSeenDevices[s_nSeenDeviceCount++] = nDeviceId;

	int nCount = 0;
	XIDeviceInfo *pInfo = XIQueryDevice( s_pDisplay, nDeviceId, &nCount );
	if ( !pInfo || nCount < 1 )
	{
		printf( "DEVICE id=%d name=? (XIQueryDevice failed)\n", nDeviceId );
		return;
	}
	const char *pszAxis[2] = { "none", "none" };
	int nAxis = 0;
	for ( int i = 0; i < pInfo->num_classes && nAxis < 2; i++ )
	{
		if ( pInfo->classes[i]->type != XIValuatorClass )
			continue;
		const XIValuatorClassInfo *pV = (const XIValuatorClassInfo *)pInfo->classes[i];
		if ( pV->number == 0 )
			pszAxis[0] = pV->mode == XIModeRelative ? "rel" : "abs";
		else if ( pV->number == 1 )
			pszAxis[1] = pV->mode == XIModeRelative ? "rel" : "abs";
		else
			continue;
		nAxis++;
	}
	printf( "DEVICE id=%d name=%s axis0=%s axis1=%s\n", nDeviceId, pInfo->name, pszAxis[0], pszAxis[1] );
	XIFreeDeviceInfo( pInfo );
}

// Sees every XEvent before SDL does. Returning true lets SDL process it as
// usual; nothing here consumes anything.
static bool x11_event_hook( void *pUserdata, XEvent *pEvent )
{
	(void)pUserdata;
	if ( !s_pDisplay )
		return true;

	if ( pEvent->type == MotionNotify )
	{
		printf( "CORE x=%d y=%d\n", pEvent->xmotion.x, pEvent->xmotion.y );
		return true;
	}

	if ( pEvent->type != GenericEvent || pEvent->xcookie.extension != s_nXiOpcode )
		return true;

	// SDL has already fetched the cookie data (its XGetEventData/
	// XFreeEventData pair brackets this hook); a second XGetEventData on an
	// already-claimed cookie fails, so read what is there and only fetch
	// ourselves if nothing is.
	if ( !pEvent->xcookie.data && !XGetEventData( s_pDisplay, &pEvent->xcookie ) )
		return true;

	if ( pEvent->xcookie.evtype == XI_RawMotion )
	{
		const XIRawEvent *pRaw = (const XIRawEvent *)pEvent->xcookie.data;
		double flValues[2] = { 0.0, 0.0 };
		int bHave[2] = { 0, 0 };
		int nValueIndex = 0;
		for ( int i = 0; i < pRaw->valuators.mask_len * 8; i++ )
		{
			if ( !XIMaskIsSet( pRaw->valuators.mask, i ) )
				continue;
			if ( i < 2 )
			{
				flValues[i] = pRaw->raw_values[nValueIndex];
				bHave[i] = 1;
			}
			nValueIndex++;
		}
		describe_device( pRaw->deviceid );
		printf( "RAW dev=%d src=%d v0=%s%.1f v1=%s%.1f\n",
			pRaw->deviceid, pRaw->sourceid,
			bHave[0] ? "" : "(none)", flValues[0],
			bHave[1] ? "" : "(none)", flValues[1] );
	}
	return true;
}

int main( int argc, char **argv )
{
	int bLock = 0;
	int bFullscreen = 0;
	int nLockAfterSeconds = -1;
	int nSeconds = 60;
	for ( int i = 1; i < argc; i++ )
	{
		if ( !strcmp( argv[i], "--lock" ) )
			bLock = 1;
		else if ( !strcmp( argv[i], "--lock-after" ) && i + 1 < argc )
			nLockAfterSeconds = atoi( argv[++i] );
		else if ( !strcmp( argv[i], "--fullscreen" ) )
			bFullscreen = 1;
		else if ( !strcmp( argv[i], "--seconds" ) && i + 1 < argc )
			nSeconds = atoi( argv[++i] );
		else
		{
			fprintf( stderr, "usage: pointer_grab_client_x11 [--lock | --lock-after S] [--fullscreen] [--seconds N]\n" );
			return 2;
		}
	}

	setvbuf( stdout, NULL, _IOLBF, 0 );

	SDL_SetHint( SDL_HINT_VIDEO_DRIVER, "x11" );
	if ( !SDL_Init( SDL_INIT_VIDEO ) )
	{
		fprintf( stderr, "SDL_Init: %s\n", SDL_GetError() );
		return 1;
	}

	SDL_SetX11EventHook( x11_event_hook, NULL );

	SDL_WindowFlags uFlags = bFullscreen ? SDL_WINDOW_FULLSCREEN : 0;
	SDL_Window *pWindow = SDL_CreateWindow( "pointer_grab_client_x11", 640, 480, uFlags );
	if ( !pWindow )
	{
		fprintf( stderr, "SDL_CreateWindow: %s\n", SDL_GetError() );
		return 1;
	}

	s_pDisplay = (Display *)SDL_GetPointerProperty( SDL_GetWindowProperties( pWindow ), SDL_PROP_WINDOW_X11_DISPLAY_POINTER, NULL );
	if ( s_pDisplay )
	{
		int nEvent = 0, nError = 0;
		if ( !XQueryExtension( s_pDisplay, "XInputExtension", &s_nXiOpcode, &nEvent, &nError ) )
			s_nXiOpcode = -1;
	}

	// Paint something so the compositor has a buffer to focus and present.
	SDL_Surface *pSurface = SDL_GetWindowSurface( pWindow );
	if ( pSurface )
	{
		SDL_FillSurfaceRect( pSurface, NULL, SDL_MapSurfaceRGB( pSurface, 0x20, 0x20, 0x20 ) );
		SDL_UpdateWindowSurface( pWindow );
	}

	if ( bLock )
	{
		if ( !SDL_SetWindowRelativeMouseMode( pWindow, true ) )
			fprintf( stderr, "SDL_SetWindowRelativeMouseMode: %s\n", SDL_GetError() );
		else
			printf( "LOCKED\n" );
	}
	printf( "READY lock=%d driver=%s\n", bLock, SDL_GetCurrentVideoDriver() );

	const Uint64 uStart = SDL_GetTicks();
	Uint64 uLastStats = uStart;
	unsigned long ulMotions = 0;
	int bRunning = 1;
	while ( bRunning )
	{
		SDL_Event ev;
		while ( SDL_PollEvent( &ev ) )
		{
			switch ( ev.type )
			{
				case SDL_EVENT_QUIT:
					bRunning = 0;
					break;
				case SDL_EVENT_MOUSE_MOTION:
					ulMotions++;
					printf( "MOTION x=%.1f y=%.1f xrel=%.1f yrel=%.1f\n",
						ev.motion.x, ev.motion.y, ev.motion.xrel, ev.motion.yrel );
					break;
				case SDL_EVENT_WINDOW_FOCUS_GAINED:
					printf( "FOCUS gained\n" );
					break;
				case SDL_EVENT_WINDOW_FOCUS_LOST:
					printf( "FOCUS lost\n" );
					break;
				case SDL_EVENT_WINDOW_MOUSE_ENTER:
					printf( "ENTER\n" );
					break;
				case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
					pSurface = SDL_GetWindowSurface( pWindow );
					printf( "SIZE %dx%d\n", ev.window.data1, ev.window.data2 );
					break;
				default:
					break;
			}
		}

		const Uint64 uNow = SDL_GetTicks();
		if ( !bLock && nLockAfterSeconds >= 0 && uNow - uStart >= (Uint64)nLockAfterSeconds * 1000u )
		{
			bLock = 1;
			if ( !SDL_SetWindowRelativeMouseMode( pWindow, true ) )
				fprintf( stderr, "SDL_SetWindowRelativeMouseMode: %s\n", SDL_GetError() );
			else
				printf( "LOCKED\n" );
		}
		if ( uNow - uLastStats >= 500 )
		{
			printf( "STATS motions=%lu relative=%d\n", ulMotions, SDL_GetWindowRelativeMouseMode( pWindow ) ? 1 : 0 );
			uLastStats = uNow;
		}
		if ( nSeconds > 0 && uNow - uStart >= (Uint64)nSeconds * 1000u )
			bRunning = 0;

		// Keep presenting: gamescope only paints (and only then refreshes
		// the pointer mapping) for a client that keeps committing.
		if ( pSurface )
			SDL_UpdateWindowSurface( pWindow );
		SDL_Delay( 16 );
	}

	printf( "DONE motions=%lu\n", ulMotions );
	SDL_DestroyWindow( pWindow );
	SDL_Quit();
	return 0;
}
