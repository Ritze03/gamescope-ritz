// pointer_probe_win32 -- the Wine-shaped "game" for scripts/pointer-regression.sh.
//
// Rust is a Unity game run through Proton, so what gamescope's Xwayland talks
// to is Wine's winex11.drv, not SDL. Wine is its own pointer pipeline:
//
//   - the cursor position a Win32 game reads (GetCursorPos, WM_MOUSEMOVE)
//     comes from core X MotionNotify root coordinates, translated into
//     Wine's "virtual screen" -- whose size Wine takes from XRandR, i.e.
//     from the Xwayland root, which a nested-mode change resizes;
//   - Unity's CursorLockMode.Locked is ShowCursor(FALSE) + ClipCursor(window)
//     + SetCursorPos(centre) every frame, reading either the offset from the
//     centre or WM_INPUT raw deltas. Under Wine that is a hidden cursor plus
//     a confining XGrabPointer, which Xwayland turns into a
//     zwp_locked_pointer_v1 and relative motion (XI2 raw), and Wine's
//     SetCursorPos into a pointer-lock cursor hint;
//   - CursorLockMode.Confined (a menu) is ClipCursor(window) with the cursor
//     visible: a confining grab, no lock.
//
// This program does those things natively, so the regression script can
// measure -- inside gamescope, headless, with no Proton and no GPU -- what a
// Unity/Wine game sees across a runtime resolution change. Built with the
// mingw cross compiler when meson finds one (tests/meson.build), run with the
// system wine inside gamescope's own Xwayland.
//
// Options
//   --size WxH          client area size (default 640x480)
//   --screen            size the window to the whole (Windows) screen
//   --mode WxH          ChangeDisplaySettingsEx(CDS_FULLSCREEN) to that mode
//                       first, then a window of that size -- "exclusive
//                       fullscreen"; on Xwayland Wine emulates the mode
//   --follow-display    on WM_DISPLAYCHANGE, resize the window to the new
//                       screen -- a borderless "fullscreen window" game
//   --confine           ClipCursor to the window from the start (a menu that
//                       confines)
//   --lock-after S      after S seconds: hide the cursor, clip it to the
//                       window, re-centre it every frame -- Unity's Locked
//   --unlock-after S    after S seconds: undo --lock-after (back to a menu)
//   --seconds N         exit after N seconds (default 60)
//
// Output, one line each, to stdout:
//   READY screen=WxH window=x,y,w,h
//   MOTION x=<..> y=<..> sx=<..> sy=<..>     every WM_MOUSEMOVE: client and
//                                            screen coordinates
//   RAW dx=<..> dy=<..> flags=<..>           every WM_INPUT mouse packet
//   DELTA dx=<..> dy=<..>                    while locked: the cursor's
//                                            offset from the centre this frame
//                                            (what a non-raw-input Unity reads)
//   DISPLAYCHANGE screen=WxH                 WM_DISPLAYCHANGE
//   STATE screen=WxH window=x,y,w,h cursor=<..>,<..> clip=<..> locked=<0|1>
//                                            every 500 ms
//   LOCKED / UNLOCKED
//   DONE
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int s_bLocked = 0;
static int s_bConfine = 0;
static int s_bFollowDisplay = 0;
static HWND s_hWnd = NULL;

static void print_state( void )
{
	RECT rc = { 0 }, clip = { 0 };
	POINT pt = { 0 };
	GetWindowRect( s_hWnd, &rc );
	GetCursorPos( &pt );
	GetClipCursor( &clip );
	printf( "STATE screen=%dx%d window=%ld,%ld,%ld,%ld cursor=%ld,%ld clip=%ld,%ld,%ld,%ld locked=%d\n",
		GetSystemMetrics( SM_CXSCREEN ), GetSystemMetrics( SM_CYSCREEN ),
		rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top,
		pt.x, pt.y,
		clip.left, clip.top, clip.right, clip.bottom,
		s_bLocked );
}

static void apply_clip( void )
{
	if ( s_bLocked || s_bConfine )
	{
		RECT rc;
		GetWindowRect( s_hWnd, &rc );
		ClipCursor( &rc );
	}
	else
	{
		ClipCursor( NULL );
	}
}

static void centre_cursor( void )
{
	RECT rc;
	GetWindowRect( s_hWnd, &rc );
	SetCursorPos( ( rc.left + rc.right ) / 2, ( rc.top + rc.bottom ) / 2 );
}

static LRESULT CALLBACK wnd_proc( HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam )
{
	switch ( uMsg )
	{
		case WM_MOUSEMOVE:
		{
			POINT pt = { (SHORT)LOWORD( lParam ), (SHORT)HIWORD( lParam ) };
			POINT spt = pt;
			ClientToScreen( hWnd, &spt );
			printf( "MOTION x=%ld y=%ld sx=%ld sy=%ld\n", pt.x, pt.y, spt.x, spt.y );
			return 0;
		}
		case WM_INPUT:
		{
			RAWINPUT raw;
			UINT uSize = sizeof( raw );
			if ( GetRawInputData( (HRAWINPUT)lParam, RID_INPUT, &raw, &uSize, sizeof( RAWINPUTHEADER ) ) != (UINT)-1 &&
				 raw.header.dwType == RIM_TYPEMOUSE )
			{
				printf( "RAW dx=%ld dy=%ld flags=%u\n", raw.data.mouse.lLastX, raw.data.mouse.lLastY, raw.data.mouse.usFlags );
			}
			return 0;
		}
		case WM_DISPLAYCHANGE:
			printf( "DISPLAYCHANGE screen=%dx%d\n", (int)LOWORD( lParam ), (int)HIWORD( lParam ) );
			if ( s_bFollowDisplay )
				SetWindowPos( hWnd, HWND_TOP, 0, 0, (int)LOWORD( lParam ), (int)HIWORD( lParam ), SWP_NOACTIVATE );
			return 0;
		case WM_SIZE:
		case WM_MOVE:
		{
			RECT rc;
			GetWindowRect( hWnd, &rc );
			printf( "WINDOW %ld,%ld,%ld,%ld\n", rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top );
			apply_clip();
			return 0;
		}
		case WM_SETCURSOR:
			if ( s_bLocked )
			{
				SetCursor( NULL );
				return TRUE;
			}
			break;
		case WM_PAINT:
		{
			PAINTSTRUCT ps;
			HDC hdc = BeginPaint( hWnd, &ps );
			FillRect( hdc, &ps.rcPaint, (HBRUSH)GetStockObject( DKGRAY_BRUSH ) );
			EndPaint( hWnd, &ps );
			return 0;
		}
		case WM_CLOSE:
		case WM_DESTROY:
			PostQuitMessage( 0 );
			return 0;
		default:
			break;
	}
	return DefWindowProc( hWnd, uMsg, wParam, lParam );
}

int main( int argc, char **argv )
{
	int nWidth = 640, nHeight = 480;
	int bScreen = 0;
	int nModeW = 0, nModeH = 0;
	int nLockAfterS = -1, nUnlockAfterS = -1;
	int nSeconds = 60;
	for ( int i = 1; i < argc; i++ )
	{
		if ( !strcmp( argv[i], "--size" ) && i + 1 < argc )
			sscanf( argv[++i], "%dx%d", &nWidth, &nHeight );
		else if ( !strcmp( argv[i], "--screen" ) )
			bScreen = 1;
		else if ( !strcmp( argv[i], "--mode" ) && i + 1 < argc )
			sscanf( argv[++i], "%dx%d", &nModeW, &nModeH );
		else if ( !strcmp( argv[i], "--follow-display" ) )
			s_bFollowDisplay = 1;
		else if ( !strcmp( argv[i], "--confine" ) )
			s_bConfine = 1;
		else if ( !strcmp( argv[i], "--lock-after" ) && i + 1 < argc )
			nLockAfterS = atoi( argv[++i] );
		else if ( !strcmp( argv[i], "--unlock-after" ) && i + 1 < argc )
			nUnlockAfterS = atoi( argv[++i] );
		else if ( !strcmp( argv[i], "--seconds" ) && i + 1 < argc )
			nSeconds = atoi( argv[++i] );
		else
		{
			fprintf( stderr, "usage: pointer_probe_win32 [--size WxH | --screen | --mode WxH] [--follow-display] [--confine] [--lock-after S] [--unlock-after S] [--seconds N]\n" );
			return 2;
		}
	}
	// _IONBF, not _IOLBF: the Windows CRT treats line buffering as full
	// buffering, and Wine hands us a pipe, so lines would arrive in 4 KB lumps.
	setvbuf( stdout, NULL, _IONBF, 0 );

	WNDCLASS wc = { 0 };
	wc.lpfnWndProc = wnd_proc;
	wc.hInstance = GetModuleHandle( NULL );
	wc.hCursor = LoadCursor( NULL, IDC_ARROW );
	wc.lpszClassName = "pointer_probe_win32";
	RegisterClass( &wc );

	if ( nModeW && nModeH )
	{
		DEVMODE dm = { 0 };
		dm.dmSize = sizeof( dm );
		dm.dmPelsWidth = nModeW;
		dm.dmPelsHeight = nModeH;
		dm.dmBitsPerPel = 32;
		dm.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL;
		const LONG lResult = ChangeDisplaySettingsEx( NULL, &dm, NULL, CDS_FULLSCREEN, NULL );
		printf( "MODESET %dx%d result=%ld screen=%dx%d\n", nModeW, nModeH, lResult,
			GetSystemMetrics( SM_CXSCREEN ), GetSystemMetrics( SM_CYSCREEN ) );
		nWidth = nModeW;
		nHeight = nModeH;
	}
	if ( bScreen )
	{
		nWidth = GetSystemMetrics( SM_CXSCREEN );
		nHeight = GetSystemMetrics( SM_CYSCREEN );
	}
	// WS_POPUP: no frame, so the client area is the window and the X window
	// is exactly WxH -- what a fullscreen-windowed game looks like to X.
	s_hWnd = CreateWindowEx( 0, wc.lpszClassName, "pointer_probe_win32", WS_POPUP | WS_VISIBLE,
		0, 0, nWidth, nHeight, NULL, NULL, wc.hInstance, NULL );
	if ( !s_hWnd )
	{
		fprintf( stderr, "CreateWindowEx failed: %lu\n", GetLastError() );
		return 1;
	}
	SetForegroundWindow( s_hWnd );

	RAWINPUTDEVICE rid = { 0x01, 0x02, 0, s_hWnd };
	if ( !RegisterRawInputDevices( &rid, 1, sizeof( rid ) ) )
		fprintf( stderr, "RegisterRawInputDevices failed: %lu\n", GetLastError() );

	apply_clip();
	{
		RECT rc;
		GetWindowRect( s_hWnd, &rc );
		printf( "READY screen=%dx%d window=%ld,%ld,%ld,%ld\n",
			GetSystemMetrics( SM_CXSCREEN ), GetSystemMetrics( SM_CYSCREEN ),
			rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top );
	}

	const DWORD dwStart = GetTickCount();
	DWORD dwLastState = dwStart;
	int bRunning = 1;
	while ( bRunning )
	{
		MSG msg;
		while ( PeekMessage( &msg, NULL, 0, 0, PM_REMOVE ) )
		{
			if ( msg.message == WM_QUIT )
				bRunning = 0;
			TranslateMessage( &msg );
			DispatchMessage( &msg );
		}

		const DWORD dwNow = GetTickCount();
		if ( !s_bLocked && nLockAfterS >= 0 && dwNow - dwStart >= (DWORD)nLockAfterS * 1000u )
		{
			s_bLocked = 1;
			nLockAfterS = -1;
			ShowCursor( FALSE );
			apply_clip();
			centre_cursor();
			printf( "LOCKED\n" );
		}
		if ( s_bLocked && nUnlockAfterS >= 0 && dwNow - dwStart >= (DWORD)nUnlockAfterS * 1000u )
		{
			s_bLocked = 0;
			nUnlockAfterS = -1;
			ShowCursor( TRUE );
			apply_clip();
			printf( "UNLOCKED\n" );
		}
		if ( s_bLocked )
		{
			// Unity's Locked mode, per frame: read how far the cursor got
			// from the centre, then put it back.
			RECT rc;
			POINT pt;
			GetWindowRect( s_hWnd, &rc );
			GetCursorPos( &pt );
			const long cx = ( rc.left + rc.right ) / 2, cy = ( rc.top + rc.bottom ) / 2;
			if ( pt.x != cx || pt.y != cy )
			{
				printf( "DELTA dx=%ld dy=%ld\n", pt.x - cx, pt.y - cy );
				SetCursorPos( cx, cy );
			}
		}
		if ( dwNow - dwLastState >= 500 )
		{
			print_state();
			dwLastState = dwNow;
		}
		if ( nSeconds > 0 && dwNow - dwStart >= (DWORD)nSeconds * 1000u )
			bRunning = 0;
		Sleep( 16 );
	}
	printf( "DONE\n" );
	return 0;
}
