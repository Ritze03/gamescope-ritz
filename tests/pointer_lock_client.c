// pointer_lock_client -- the "game" half of scripts/pointer-regression.sh.
//
// A minimal SDL2 window that, with --lock, does exactly what a first-person
// game does in play: SDL_SetRelativeMouseMode(SDL_TRUE), which under Xwayland
// hides the cursor, grabs and confines the pointer and makes Xwayland ask
// gamescope for a zwp_locked_pointer_v1 -- the LOCKED constraint the
// compositor-side check keys on. Without --lock it is a plain windowed
// client (a game's menu): an absolute pointer, no constraint.
//
// It prints one line per SDL_MOUSEMOTION it receives, so a script can count
// what the game actually saw. Why this detects absolute events too: Xwayland
// republishes every pointer event as an XI2 raw event, and SDL's relative
// mode reads raw valuators as deltas -- a wl_pointer.motion delivered to a
// locked client therefore shows up here as a motion line, which is precisely
// the event that must never arrive (superdoc/features/cursor-pipeline.md,
// "Locked pointer => never an absolute event").
//
// Not a meson test: it needs a running gamescope. Built alongside the unit
// tests when SDL2 is available; run by the script with SDL_VIDEODRIVER=x11
// inside gamescope's own Xwayland.
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main( int argc, char **argv )
{
	int bLock = 0;
	int nSeconds = 60;
	for ( int i = 1; i < argc; i++ )
	{
		if ( !strcmp( argv[i], "--lock" ) )
			bLock = 1;
		else if ( !strcmp( argv[i], "--seconds" ) && i + 1 < argc )
			nSeconds = atoi( argv[++i] );
		else
		{
			fprintf( stderr, "usage: pointer_lock_client [--lock] [--seconds N]\n" );
			return 2;
		}
	}

	setvbuf( stdout, NULL, _IOLBF, 0 );

	if ( SDL_Init( SDL_INIT_VIDEO ) != 0 )
	{
		fprintf( stderr, "SDL_Init: %s\n", SDL_GetError() );
		return 1;
	}

	SDL_Window *pWindow = SDL_CreateWindow( "pointer_lock_client",
		SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 640, 480, SDL_WINDOW_SHOWN );
	if ( !pWindow )
	{
		fprintf( stderr, "SDL_CreateWindow: %s\n", SDL_GetError() );
		return 1;
	}

	// Paint something so the compositor has a buffer to focus and present.
	SDL_Surface *pSurface = SDL_GetWindowSurface( pWindow );
	if ( pSurface )
	{
		SDL_FillRect( pSurface, NULL, SDL_MapRGB( pSurface->format, 0x20, 0x20, 0x20 ) );
		SDL_UpdateWindowSurface( pWindow );
	}

	if ( bLock )
	{
		if ( SDL_SetRelativeMouseMode( SDL_TRUE ) != 0 )
			fprintf( stderr, "SDL_SetRelativeMouseMode: %s\n", SDL_GetError() );
	}
	printf( "READY lock=%d driver=%s\n", bLock, SDL_GetCurrentVideoDriver() );

	const Uint32 uStart = SDL_GetTicks();
	Uint32 uLastStats = uStart;
	unsigned long ulMotions = 0;
	int bRunning = 1;
	while ( bRunning )
	{
		SDL_Event ev;
		while ( SDL_PollEvent( &ev ) )
		{
			switch ( ev.type )
			{
				case SDL_QUIT:
					bRunning = 0;
					break;
				case SDL_MOUSEMOTION:
					ulMotions++;
					printf( "MOTION t=%u x=%d y=%d xrel=%d yrel=%d\n",
						ev.motion.timestamp, ev.motion.x, ev.motion.y, ev.motion.xrel, ev.motion.yrel );
					break;
				case SDL_WINDOWEVENT:
					if ( ev.window.event == SDL_WINDOWEVENT_FOCUS_GAINED )
						printf( "FOCUS gained\n" );
					else if ( ev.window.event == SDL_WINDOWEVENT_FOCUS_LOST )
						printf( "FOCUS lost\n" );
					else if ( ev.window.event == SDL_WINDOWEVENT_ENTER )
						printf( "ENTER\n" );
					else if ( ev.window.event == SDL_WINDOWEVENT_SIZE_CHANGED )
					{
						// A resize (gamescope re-fitting the window after a
						// nested mode change) invalidates the window surface.
						pSurface = SDL_GetWindowSurface( pWindow );
						printf( "SIZE %dx%d\n", ev.window.data1, ev.window.data2 );
					}
					break;
				default:
					break;
			}
		}

		const Uint32 uNow = SDL_GetTicks();
		if ( uNow - uLastStats >= 500 )
		{
			printf( "STATS motions=%lu relative=%d\n", ulMotions, SDL_GetRelativeMouseMode() == SDL_TRUE );
			uLastStats = uNow;
		}
		if ( nSeconds > 0 && uNow - uStart >= (Uint32)nSeconds * 1000u )
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
