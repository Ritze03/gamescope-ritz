// effects_scene_client -- the "game" half of scripts/effects-regression.sh.
//
// A minimal SDL2 window that paints one of three synthetic reference scenes
// for Adaptive Brightness (superdoc/features/shader-effects.md, "Dynamic"),
// each built from flat regions at KNOWN pixel positions so a screenshot can
// be sampled by rectangle rather than by guessing:
//
//   dark    five horizontal bands at 5 / 8 / 12 / 16 / 20 (encoded), a pure
//           black rectangle top-left, and six 240 highlight squares (~1 % of
//           the image, so they sit above the 98th percentile) across the
//           middle band -- "a super dark map with a few lights".
//   bright  bands at 200 / 215 / 230 / 245 / 255 and six 30-valued shadow
//           rectangles (~4 %, so the 2nd percentile IS the shadows) across
//           the middle band -- "a super bright map with some shade".
//   mid     bands at 26 / 77 / 128 / 179 / 230 (0.1 .. 0.9): the scene the
//           curve must leave alone.
//
// The layout is mirrored in scripts/effects_regression_sample.py; change one,
// change both. SIGUSR1 advances to the next scene in --scenes, which is how
// the script drives its temporal check (switch, then capture at 0.2 / 1 / 3
// s) without any input injection. Prints "scene <name>" on every change.
//
// Not a meson test: it needs a running gamescope. Built alongside the unit
// tests when SDL2 is available; run by the script with SDL_VIDEODRIVER=x11
// inside gamescope's own Xwayland, exactly like pointer_lock_client.
#include <SDL.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static volatile sig_atomic_t s_nAdvance = 0;
static void OnUsr1( int sig ) { (void)sig; s_nAdvance++; }

typedef struct
{
	const char *pszName;
	unsigned char bands[5];        // top to bottom
	unsigned char rectValue;       // the six middle-band rectangles
	int nRectW, nRectH;
	int bBlackCorner;              // dark only: a 0-valued rectangle top-left
} Scene;

static const Scene kScenes[] = {
	{ "dark",   {   5,   8,  12,  16,  20 }, 240,  40,  40, 1 },
	{ "bright", { 200, 215, 230, 245, 255 },  30, 120,  50, 0 },
	{ "mid",    {  26,  77, 128, 179, 230 },   0,   0,   0, 0 },
};

static const Scene *FindScene( const char *pszName )
{
	for ( size_t i = 0; i < sizeof( kScenes ) / sizeof( kScenes[0] ); i++ )
		if ( !strcmp( kScenes[i].pszName, pszName ) )
			return &kScenes[i];
	return NULL;
}

static void FillRect( SDL_Surface *pSurface, int x, int y, int w, int h, unsigned char v )
{
	SDL_Rect r = { x, y, w, h };
	SDL_FillRect( pSurface, &r, SDL_MapRGB( pSurface->format, v, v, v ) );
}

// Geometry, in a 1280x720 frame, scaled to the actual window: bands are
// fifths of the height; the six rectangles are centred in the middle band
// at x = 100 + k * 190 (k = 0..5); the black corner is (40,40)-(200,120).
static void Paint( SDL_Surface *pSurface, const Scene *pScene )
{
	const int W = pSurface->w, H = pSurface->h;
	const float sx = W / 1280.0f, sy = H / 720.0f;

	for ( int i = 0; i < 5; i++ )
	{
		const int y0 = i * H / 5, y1 = ( i + 1 ) * H / 5;
		FillRect( pSurface, 0, y0, W, y1 - y0, pScene->bands[i] );
	}

	if ( pScene->nRectW > 0 )
	{
		const int rw = (int)( pScene->nRectW * sx ), rh = (int)( pScene->nRectH * sy );
		const int cy = H / 2;
		for ( int k = 0; k < 6; k++ )
		{
			const int cx = (int)( ( 100 + k * 190 ) * sx );
			FillRect( pSurface, cx - rw / 2, cy - rh / 2, rw, rh, pScene->rectValue );
		}
	}

	if ( pScene->bBlackCorner )
		FillRect( pSurface, (int)( 40 * sx ), (int)( 40 * sy ), (int)( 160 * sx ), (int)( 80 * sy ), 0 );
}

int main( int argc, char **argv )
{
	int nW = 1280, nH = 720, nSeconds = 600;
	const char *pszPidFile = NULL;   // so the script can SIGUSR1 exactly this process
	const Scene *pList[8];
	int nList = 0;

	for ( int i = 1; i < argc; i++ )
	{
		if ( !strcmp( argv[i], "--scenes" ) && i + 1 < argc )
		{
			char *psz = strdup( argv[++i] );
			for ( char *tok = strtok( psz, "," ); tok && nList < 8; tok = strtok( NULL, "," ) )
			{
				const Scene *p = FindScene( tok );
				if ( !p ) { fprintf( stderr, "unknown scene '%s' (dark|bright|mid)\n", tok ); return 2; }
				pList[nList++] = p;
			}
			free( psz );
		}
		else if ( !strcmp( argv[i], "--width" ) && i + 1 < argc )   nW = atoi( argv[++i] );
		else if ( !strcmp( argv[i], "--height" ) && i + 1 < argc )  nH = atoi( argv[++i] );
		else if ( !strcmp( argv[i], "--seconds" ) && i + 1 < argc ) nSeconds = atoi( argv[++i] );
		else if ( !strcmp( argv[i], "--pidfile" ) && i + 1 < argc ) pszPidFile = argv[++i];
		else
		{
			fprintf( stderr, "usage: effects_scene_client --scenes dark[,bright,mid] [--width W] [--height H] [--seconds N] [--pidfile PATH]\n" );
			return 2;
		}
	}
	if ( nList == 0 )
		pList[nList++] = &kScenes[0];

	setvbuf( stdout, NULL, _IOLBF, 0 );
	signal( SIGUSR1, OnUsr1 );
	if ( pszPidFile )
	{
		FILE *f = fopen( pszPidFile, "w" );
		if ( f ) { fprintf( f, "%ld\n", (long)getpid() ); fclose( f ); }
	}

	if ( SDL_Init( SDL_INIT_VIDEO ) != 0 )
	{
		fprintf( stderr, "SDL_Init: %s\n", SDL_GetError() );
		return 1;
	}

	SDL_Window *pWindow = SDL_CreateWindow( "effects_scene_client",
		SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, nW, nH, SDL_WINDOW_SHOWN );
	if ( !pWindow )
	{
		fprintf( stderr, "SDL_CreateWindow: %s\n", SDL_GetError() );
		return 1;
	}

	int nCur = 0, nSeen = 0;
	printf( "scene %s\n", pList[nCur]->pszName );

	const Uint32 uEnd = SDL_GetTicks() + (Uint32)nSeconds * 1000u;
	while ( SDL_GetTicks() < uEnd )
	{
		SDL_Event ev;
		while ( SDL_PollEvent( &ev ) )
			if ( ev.type == SDL_QUIT )
				goto done;

		if ( s_nAdvance != nSeen )
		{
			nSeen = s_nAdvance;
			nCur = ( nCur + 1 ) % nList;
			printf( "scene %s\n", pList[nCur]->pszName );
		}

		// Repaint every frame, as a game would, so the compositor keeps
		// receiving fresh buffers and the effect pass keeps measuring.
		SDL_Surface *pSurface = SDL_GetWindowSurface( pWindow );
		if ( pSurface )
		{
			Paint( pSurface, pList[nCur] );
			SDL_UpdateWindowSurface( pWindow );
		}
		SDL_Delay( 16 );
	}
done:
	SDL_DestroyWindow( pWindow );
	SDL_Quit();
	return 0;
}
