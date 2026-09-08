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
//   texdark a textured stand-in for a real dark game frame (added for the
//           2026-09-07 "the image pulsates" investigation): a fixed grid of
//           16x16 cells, each a hashed grey 6..56 with a continuous, non-flat
//           distribution, and ~1.5 % of the cells a 160..255 light. Unlike
//           the flat scenes its histogram has no 800-tap bins, so the
//           measure pass's 64x64 tap grid actually samples it.
//   texmid  the same texture with the cells spread 40..220 -- a mid scene
//           with the same sampling properties.
//   texsplit --split PCT (default 50) of the cells bright (150..230), the
//           rest dark: "sky and ground", a bimodal histogram whose median
//           sits in the empty gap between the modes. Note this scatters the
//           two populations over the WHOLE frame -- it is a histogram test,
//           not a spatial one. halfsplit below is the spatial version.
//   halfsplit the left half of the frame is the dark scene's bands, the
//           right half the bright scene's: a genuine "dark interior, bright
//           sky" frame, and the case Local adaptation exists for.
//   halobox a flat 200 field with one flat 10 box (320x320) in the middle,
//           and haloinv the inverse (a 220 box on a 15 field). A local tone
//           operator's classic artefact is a rim around such a box; the
//           script samples a line out from the box's edge to measure it.
//   colors  ADDED 2026-09-08 for the Saturation/Vibrancy split (shader-
//           effects.md): five horizontal COLOUR bands (not the grey levels
//           above) at known saturations, so the two colour effects have
//           something to measure -- every other scene here is greyscale,
//           where both effects are an exact no-op. Band 0 is pure grey
//           (saturation 0, the invariant "left alone" case); bands 1-4 walk
//           a warm hue from near-neutral to fully saturated. See
//           PaintSpecial()'s nSpecial==4 case for the exact RGB values.
//   --lights PCT (default 1.5) sets texdark's share of light cells; at 2.0
//   the 98th percentile sits exactly in that scene's gap. --periodic makes
//   the textures repeat every 80 cells (one 1280-wide frame), so under
//   --motion the image's true statistics never change and every frame-to-
//   frame difference in the measurement is sampling noise alone.
//
// --motion PX scrolls the TEXTURED scenes horizontally by PX pixels every
// frame (the flat scenes never move, so their sampled regions stay put), so
// the tap grid lands on different pixels each frame the way it does under
// camera motion in a game. 0 (the default) is a perfectly still frame.
// SIGUSR2 pauses/resumes the motion, so one run can measure the same scene
// panning and then perfectly still.
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
static volatile sig_atomic_t s_nMotionToggle = 0;
static void OnUsr1( int sig ) { (void)sig; s_nAdvance++; }
static void OnUsr2( int sig ) { (void)sig; s_nMotionToggle++; }

// nSpecial: 0 the flat band scenes below, 1 halfsplit, 2 halobox,
// 3 haloinv -- the three scenes Local adaptation (2026-09-07) is measured
// on -- 4 colors, the Saturation/Vibrancy colour bands (2026-09-08). See
// PaintSpecial().
typedef struct
{
	const char *pszName;
	unsigned char bands[5];        // top to bottom
	unsigned char rectValue;       // the six middle-band rectangles
	int nRectW, nRectH;
	int bBlackCorner;              // dark only: a 0-valued rectangle top-left
	int nSpecial;
} Scene;

static const Scene kScenes[] = {
	{ "dark",    {   5,   8,  12,  16,  20 }, 240,  40,  40, 1, 0 },
	{ "bright",  { 200, 215, 230, 245, 255 },  30, 120,  50, 0, 0 },
	{ "mid",     {  26,  77, 128, 179, 230 },   0,   0,   0, 0, 0 },
	{ "texdark",  {   0,   0,   0,   0,   0 },   0,   0,   0, 0, 0 },
	{ "texmid",   {   0,   0,   0,   0,   0 },   0,   0,   0, 0, 0 },
	{ "texsplit", {   0,   0,   0,   0,   0 },   0,   0,   0, 0, 0 },
	{ "halfsplit",{   0,   0,   0,   0,   0 },   0,   0,   0, 0, 1 },
	{ "halobox",  {   0,   0,   0,   0,   0 },   0,   0,   0, 0, 2 },
	{ "haloinv",  {   0,   0,   0,   0,   0 },   0,   0,   0, 0, 3 },
	{ "colors",   {   0,   0,   0,   0,   0 },   0,   0,   0, 0, 4 },
};

// colors (2026-09-08): five horizontal bands, top to bottom, RGB. Band 0 is
// pure grey (saturation 0); 1-4 walk a warm hue from near-neutral to fully
// saturated (max(c)-min(c) = 0, 28, 86, 160, 255). Mirrored in
// scripts/effects_regression_sample.py's COLOR_BANDS -- change one,
// change both.
static const unsigned char kColorBands[5][3] = {
	{ 128, 128, 128 },
	{ 148, 134, 120 },
	{ 178, 140,  92 },
	{ 214, 118,  54 },
	{ 255,  60,   0 },
};

// The three Local-adaptation scenes, in the same 1280x720 reference frame
// the flat scenes use (scaled to the real window like everything else).
//
//   halfsplit  LEFT half the dark scene's five bands (5/8/12/16/20), RIGHT
//              half the bright scene's (200/215/230/245/255). The headline
//              case: no single global curve serves both halves, so this is
//              where local adaptation either earns itself or does not.
//   halobox    a flat 200 field with one flat 10 box, 320x320, centred.
//   haloinv    the inverse: a flat 15 field with one flat 220 box.
//              The two halo scenes exist to measure the artefact a local
//              tone operator is known for -- a bright rim around a dark
//              object, or a dark rim around a bright one -- by sampling a
//              line straight out from the box's right edge. A hard,
//              high-contrast, straight edge is the worst case there is.
#define HALO_BOX_W 320
#define HALO_BOX_H 320

static void FillRect( SDL_Surface *pSurface, int x, int y, int w, int h, unsigned char v )
{
	SDL_Rect r = { x, y, w, h };
	SDL_FillRect( pSurface, &r, SDL_MapRGB( pSurface->format, v, v, v ) );
}

static void FillRectRGB( SDL_Surface *pSurface, int x, int y, int w, int h,
                         unsigned char r8, unsigned char g8, unsigned char b8 )
{
	SDL_Rect r = { x, y, w, h };
	SDL_FillRect( pSurface, &r, SDL_MapRGB( pSurface->format, r8, g8, b8 ) );
}

static int s_nMotionPx = 0;      // --motion: horizontal scroll per frame (textured scenes)
static int s_bMotionPaused = 0;  // SIGUSR2 toggles
static float s_flLights = 1.5f;  // --lights: % of texdark cells that are a light
static float s_flSplit = 50.0f;  // --split: % of texsplit cells that are bright
static int s_bPeriodic = 0;      // --periodic: the texture repeats every 80 cells (one frame width), so a pan changes nothing but which pixels the taps land on
static long s_nFrame = 0;

// A cheap integer hash for the textured scenes: the same (cx, cy) always
// gives the same cell, so a scrolled frame is the SAME texture moved, not
// new noise -- exactly the "camera pan over static content" case.
static unsigned Hash( unsigned x, unsigned y )
{
	unsigned h = x * 0x9E3779B1u ^ ( y + 0x7F4A7C15u ) * 0x85EBCA77u;
	h ^= h >> 15; h *= 0x2C1B3C6Du; h ^= h >> 12; h *= 0x297A2D39u; h ^= h >> 15;
	return h;
}

// kind: 0 texdark (dark cells, `s_flLights` % lights 160..255), 1 texmid
// (cells 40..220), 2 texsplit (`s_flSplit` % bright cells 150..230, the
// rest dark -- a bimodal "sky and ground" histogram whose median sits in
// the gap between the two modes when the split is 50 %).
static void PaintTexture( SDL_Surface *pSurface, int nKind, int nScroll )
{
	const int W = pSurface->w, H = pSurface->h, CELL = 16;
	const int nFirst = nScroll / CELL;
	for ( int cy = 0; cy * CELL < H; cy++ )
	{
		for ( int cx = nFirst; ( cx - nFirst ) * CELL - ( nScroll % CELL ) < W; cx++ )
		{
			const unsigned h = Hash( (unsigned)( s_bPeriodic ? cx % 80 : cx ), (unsigned)cy );
			const float r = ( h & 0xFFFFu ) / 65535.0f;          // 0..1
			const float light = ( ( h >> 16 ) & 0xFFFu ) / 4095.0f;
			unsigned char v;
			if ( nKind == 1 )
				v = (unsigned char)( 40.0f + 180.0f * r );
			else if ( nKind == 2 && light < s_flSplit * 0.01f )
				v = (unsigned char)( 150.0f + 80.0f * r );
			else if ( nKind == 0 && light < s_flLights * 0.01f )
				v = (unsigned char)( 160.0f + 95.0f * r );
			else
				v = (unsigned char)( 6.0f + 50.0f * r * r );
			const int x = ( cx - nFirst ) * CELL - ( nScroll % CELL );
			FillRect( pSurface, x, cy * CELL, CELL, CELL, v );
		}
	}
}

static void PaintSpecial( SDL_Surface *pSurface, int nSpecial )
{
	const int W = pSurface->w, H = pSurface->h;
	const float sx = W / 1280.0f, sy = H / 720.0f;

	if ( nSpecial == 1 )
	{
		static const unsigned char kLeft[5]  = {   5,   8,  12,  16,  20 };
		static const unsigned char kRight[5] = { 200, 215, 230, 245, 255 };
		const int nMid = W / 2;
		for ( int i = 0; i < 5; i++ )
		{
			const int y0 = i * H / 5, y1 = ( i + 1 ) * H / 5;
			FillRect( pSurface, 0, y0, nMid, y1 - y0, kLeft[i] );
			FillRect( pSurface, nMid, y0, W - nMid, y1 - y0, kRight[i] );
		}
		return;
	}

	if ( nSpecial == 4 )
	{
		for ( int i = 0; i < 5; i++ )
		{
			const int y0 = i * H / 5, y1 = ( i + 1 ) * H / 5;
			FillRectRGB( pSurface, 0, y0, W, y1 - y0,
			            kColorBands[i][0], kColorBands[i][1], kColorBands[i][2] );
		}
		return;
	}

	{
		const unsigned char field = ( nSpecial == 2 ) ? 200 : 15;
		const unsigned char box   = ( nSpecial == 2 ) ?  10 : 220;
		const int bw = (int)( HALO_BOX_W * sx ), bh = (int)( HALO_BOX_H * sy );
		FillRect( pSurface, 0, 0, W, H, field );
		FillRect( pSurface, W / 2 - bw / 2, H / 2 - bh / 2, bw, bh, box );
	}
}

static const Scene *FindScene( const char *pszName )
{
	for ( size_t i = 0; i < sizeof( kScenes ) / sizeof( kScenes[0] ); i++ )
		if ( !strcmp( kScenes[i].pszName, pszName ) )
			return &kScenes[i];
	return NULL;
}

// Geometry, in a 1280x720 frame, scaled to the actual window: bands are
// fifths of the height; the six rectangles are centred in the middle band
// at x = 100 + k * 190 (k = 0..5); the black corner is (40,40)-(200,120).
static void Paint( SDL_Surface *pSurface, const Scene *pScene )
{
	const int W = pSurface->w, H = pSurface->h;
	const float sx = W / 1280.0f, sy = H / 720.0f;
	if ( pScene->nSpecial != 0 )
	{
		PaintSpecial( pSurface, pScene->nSpecial );
		return;
	}
	if ( !strncmp( pScene->pszName, "tex", 3 ) )
	{
		const int nScroll = (int)( ( s_nFrame * (long)s_nMotionPx ) % 100000L );
		PaintTexture( pSurface, !strcmp( pScene->pszName, "texmid" ) ? 1 : !strcmp( pScene->pszName, "texsplit" ) ? 2 : 0, nScroll );
		return;
	}

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
	const Scene *pList[12];
	int nList = 0;

	for ( int i = 1; i < argc; i++ )
	{
		if ( !strcmp( argv[i], "--scenes" ) && i + 1 < argc )
		{
			char *psz = strdup( argv[++i] );
			for ( char *tok = strtok( psz, "," ); tok && nList < 12; tok = strtok( NULL, "," ) )
			{
				const Scene *p = FindScene( tok );
				if ( !p ) { fprintf( stderr, "unknown scene '%s' (dark|bright|mid|texdark|texmid|texsplit|halfsplit|halobox|haloinv|colors)\n", tok ); return 2; }
				pList[nList++] = p;
			}
			free( psz );
		}
		else if ( !strcmp( argv[i], "--width" ) && i + 1 < argc )   nW = atoi( argv[++i] );
		else if ( !strcmp( argv[i], "--height" ) && i + 1 < argc )  nH = atoi( argv[++i] );
		else if ( !strcmp( argv[i], "--seconds" ) && i + 1 < argc ) nSeconds = atoi( argv[++i] );
		else if ( !strcmp( argv[i], "--pidfile" ) && i + 1 < argc ) pszPidFile = argv[++i];
		else if ( !strcmp( argv[i], "--motion" ) && i + 1 < argc )  s_nMotionPx = atoi( argv[++i] );
		else if ( !strcmp( argv[i], "--lights" ) && i + 1 < argc )  s_flLights = (float)atof( argv[++i] );
		else if ( !strcmp( argv[i], "--split" ) && i + 1 < argc )   s_flSplit = (float)atof( argv[++i] );
		else if ( !strcmp( argv[i], "--periodic" ) )                 s_bPeriodic = 1;
		else
		{
			fprintf( stderr, "usage: effects_scene_client --scenes dark[,bright,mid,texdark,texmid,texsplit,halfsplit,halobox,haloinv,colors] [--width W] [--height H] [--seconds N] [--pidfile PATH] [--motion PX] [--lights PCT] [--split PCT] [--periodic]\n" );
			return 2;
		}
	}
	if ( nList == 0 )
		pList[nList++] = &kScenes[0];

	setvbuf( stdout, NULL, _IOLBF, 0 );
	signal( SIGUSR1, OnUsr1 );
	signal( SIGUSR2, OnUsr2 );
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

	int nCur = 0, nSeen = 0, nMotionSeen = 0;
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
		if ( s_nMotionToggle != nMotionSeen )
		{
			nMotionSeen = s_nMotionToggle;
			s_bMotionPaused = !s_bMotionPaused;
			printf( "motion %s\n", s_bMotionPaused ? "paused" : "running" );
		}

		// Repaint every frame, as a game would, so the compositor keeps
		// receiving fresh buffers and the effect pass keeps measuring.
		SDL_Surface *pSurface = SDL_GetWindowSurface( pWindow );
		if ( pSurface )
		{
			Paint( pSurface, pList[nCur] );
			SDL_UpdateWindowSurface( pWindow );
		}
		if ( !s_bMotionPaused )
			s_nFrame++;
		SDL_Delay( 16 );
	}
done:
	SDL_DestroyWindow( pWindow );
	SDL_Quit();
	return 0;
}
