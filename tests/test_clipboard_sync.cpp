// Clipboard sync: the parts that can be tested without a compositor.
//
// The loop guard and the pipe reader/writer are the whole of the logic that
// can go wrong in a way a build cannot catch: everything else is protocol
// plumbing that only exists once there is a real host compositor on the other
// end. See superdoc/features/clipboard-sync.md.

#include <catch2/catch_test_macros.hpp>

#include "Clipboard/ClipboardSync.h"

#include <fcntl.h>
#include <thread>
#include <unistd.h>

using namespace gamescope;

TEST_CASE( "clipboard mime ranking prefers explicit UTF-8", "[clipboard_sync]" )
{
	REQUIRE( RankClipboardMime( "text/plain;charset=utf-8" ) > RankClipboardMime( "UTF8_STRING" ) );
	REQUIRE( RankClipboardMime( "UTF8_STRING" ) > RankClipboardMime( "text/plain" ) );
	REQUIRE( RankClipboardMime( "text/plain" ) > RankClipboardMime( "STRING" ) );
	REQUIRE( RankClipboardMime( "STRING" ) > 0 );
	REQUIRE( RankClipboardMime( "TEXT" ) > 0 );

	// Not text: must never be picked, however desperate we are.
	REQUIRE( RankClipboardMime( "image/png" ) == 0 );
	REQUIRE( RankClipboardMime( "text/html" ) == 0 );
	REQUIRE( RankClipboardMime( nullptr ) == 0 );
}

TEST_CASE( "clipboard text normalisation", "[clipboard_sync]" )
{
	SECTION( "trailing NULs are dropped" )
	{
		// X11 selection owners routinely NUL-terminate; Wayland ones never
		// do. If the NUL survived, a round trip would compare unequal and the
		// loop guard would stop working.
		REQUIRE( NormalizeClipboardText( std::string( "hello\0", 6 ) ) == "hello" );
		REQUIRE( NormalizeClipboardText( std::string( "hello\0\0\0", 8 ) ) == "hello" );
	}

	SECTION( "embedded NULs are preserved" )
	{
		std::string sIn( "a\0b", 3 );
		REQUIRE( NormalizeClipboardText( sIn ) == sIn );
	}

	SECTION( "empty and all-NUL input" )
	{
		REQUIRE( NormalizeClipboardText( "" ).empty() );
		REQUIRE( NormalizeClipboardText( std::string( "\0\0", 2 ) ).empty() );
	}

	SECTION( "oversized input is capped" )
	{
		std::string sHuge( k_nMaxClipboardBytes + 4096, 'x' );
		REQUIRE( NormalizeClipboardText( sHuge ).size() == k_nMaxClipboardBytes );
	}
}

TEST_CASE( "clipboard loop guard breaks the cycle", "[clipboard_sync]" )
{
	SECTION( "our own push does not come back in" )
	{
		CClipboardLoopGuard guard;

		// An X11 client copied "one"; we publish it to the host.
		REQUIRE( guard.ShouldPushToHost( "one" ) );
		// The host now announces its clipboard is "one" -- which is us. If
		// this were accepted we would set the X selection, be notified, and
		// push again, forever.
		REQUIRE_FALSE( guard.ShouldAcceptFromHost( "one" ) );
	}

	SECTION( "what we took from the host is not echoed back to it" )
	{
		CClipboardLoopGuard guard;

		REQUIRE( guard.ShouldAcceptFromHost( "two" ) );
		REQUIRE_FALSE( guard.ShouldPushToHost( "two" ) );
	}

	SECTION( "a genuinely new value still crosses, in both directions" )
	{
		CClipboardLoopGuard guard;

		REQUIRE( guard.ShouldAcceptFromHost( "a" ) );
		REQUIRE( guard.ShouldPushToHost( "b" ) );
		REQUIRE( guard.ShouldAcceptFromHost( "c" ) );
		REQUIRE( guard.ShouldPushToHost( "d" ) );
	}

	SECTION( "alternating the same two values is not mistaken for a loop" )
	{
		CClipboardLoopGuard guard;

		REQUIRE( guard.ShouldAcceptFromHost( "a" ) );
		REQUIRE( guard.ShouldPushToHost( "b" ) );
		// Back to "a" from the host: a real change, because the clipboard now
		// holds "b". It must not be suppressed just because "a" was seen once.
		REQUIRE( guard.ShouldAcceptFromHost( "a" ) );
	}

	SECTION( "repeating a value in the same direction is a no-op" )
	{
		CClipboardLoopGuard guard;

		REQUIRE( guard.ShouldPushToHost( "same" ) );
		REQUIRE_FALSE( guard.ShouldPushToHost( "same" ) );

		CClipboardLoopGuard other;
		REQUIRE( other.ShouldAcceptFromHost( "same" ) );
		REQUIRE_FALSE( other.ShouldAcceptFromHost( "same" ) );
	}

	SECTION( "Reset forgets both directions" )
	{
		CClipboardLoopGuard guard;

		REQUIRE( guard.ShouldPushToHost( "x" ) );
		guard.Reset();
		// After the host device is revoked and re-created, the same text is a
		// legitimate transfer again.
		REQUIRE( guard.ShouldAcceptFromHost( "x" ) );
	}

	SECTION( "an empty clipboard is a value like any other" )
	{
		CClipboardLoopGuard guard;

		REQUIRE( guard.ShouldAcceptFromHost( "" ) );
		REQUIRE_FALSE( guard.ShouldPushToHost( "" ) );
	}
}

TEST_CASE( "clipboard pipe transfer", "[clipboard_sync]" )
{
	SECTION( "round trip through a real pipe" )
	{
		int nPipe[ 2 ];
		REQUIRE( pipe2( nPipe, O_CLOEXEC ) == 0 );

		const std::string sText = "the quick brown fox\nsecond line\n";
		std::thread writer( [ & ]{ WriteClipboardPipe( nPipe[ 1 ], sText, 2000 ); } );

		std::optional<std::string> osGot = ReadClipboardPipe( nPipe[ 0 ], 2000 );
		writer.join();

		REQUIRE( osGot.has_value() );
		REQUIRE( *osGot == sText );
	}

	SECTION( "a payload larger than the pipe buffer still arrives whole" )
	{
		// A pipe holds 64 KiB by default; anything bigger only completes if
		// the reader and writer actually interleave. This is the case that
		// would deadlock if either side ran on the compositor thread.
		int nPipe[ 2 ];
		REQUIRE( pipe2( nPipe, O_CLOEXEC ) == 0 );

		const std::string sText( 512 * 1024, 'z' );
		std::thread writer( [ & ]{ WriteClipboardPipe( nPipe[ 1 ], sText, 2000 ); } );

		std::optional<std::string> osGot = ReadClipboardPipe( nPipe[ 0 ], 2000 );
		writer.join();

		REQUIRE( osGot.has_value() );
		REQUIRE( osGot->size() == sText.size() );
	}

	SECTION( "a writer that closes without writing yields empty text, not a failure" )
	{
		int nPipe[ 2 ];
		REQUIRE( pipe2( nPipe, O_CLOEXEC ) == 0 );
		close( nPipe[ 1 ] );

		std::optional<std::string> osGot = ReadClipboardPipe( nPipe[ 0 ], 2000 );
		REQUIRE( osGot.has_value() );
		REQUIRE( osGot->empty() );
	}

	SECTION( "a source that never writes and never closes times out" )
	{
		// The reason there is a timeout at all: without it this thread would
		// live for the rest of the process.
		int nPipe[ 2 ];
		REQUIRE( pipe2( nPipe, O_CLOEXEC ) == 0 );

		std::optional<std::string> osGot = ReadClipboardPipe( nPipe[ 0 ], 50 );
		REQUIRE_FALSE( osGot.has_value() );

		close( nPipe[ 1 ] );
	}

	SECTION( "a bad fd is reported, not crashed on" )
	{
		REQUIRE_FALSE( ReadClipboardPipe( -1, 50 ).has_value() );
		WriteClipboardPipe( -1, "x", 50 ); // must not crash
	}
}
