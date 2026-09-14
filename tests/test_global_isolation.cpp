// Process-wide safety net: makes the WHOLE gamescope_tests binary allergic to
// the real ~/.config/gamescope-ritz, not just the test cases that remembered
// to ask for their own isolation via a local TempConfigHome fixture (the
// pattern repeated in tests/test_config.cpp, test_resolution.cpp,
// test_overlay_profiles.cpp and test_effects_curve.cpp).
//
// Found 2026-09-15: tests/test_keybinds.cpp's held-action and mouse-chord
// test cases call SetChord()/ResetAll() -- which persist via
// Keybinds.cpp's PersistLocked() -- and that file has no config-home
// override anywhere in it (its own header comment used to claim, wrongly,
// that no successful SetChord() existed in the file). Every full run of the
// suite therefore rewrote the developer's REAL global.json twice --
// confirmed with `inotifywait -m ~/.config/gamescope-ritz` around a run of
// `./gamescope_tests`, and reproduced in isolation with
// `./gamescope_tests "[keybinds],[effects_curve]"` (keybinds alone finishes
// before ConfigManager's 50-500ms write-coalescing window elapses, so the
// background writer thread never gets scheduled before the process exits
// and the write is silently lost -- pairing it with another tag just keeps
// the process alive long enough for the write to land).
//
// A per-file fixture only protects the file that remembered to add one, and
// nothing stops the next new test file forgetting the same way. So this is
// the root fix: a Catch2 event listener that points XDG_CONFIG_HOME at a
// fresh run-wide temp directory before the FIRST test case runs, and tears
// it down after the LAST one -- so no test case, present or future, can
// reach the real config home no matter what it calls, even if it adds no
// fixture of its own.
//
// The existing per-file TempConfigHome fixtures still work unmodified on
// top of this, but they do NOT nest as cleanly as their own comments assume:
// each one's destructor unconditionally unsetenv()s XDG_CONFIG_HOME rather
// than restoring whatever value it overwrote, so the first such fixture to
// tear down would otherwise blow this listener's env back open for every
// following test in the same process. Rather than touch four existing
// fixtures (and hope every future one gets the restore-don't-clear rule
// right), this listener re-clamps XDG_CONFIG_HOME to the run-wide directory
// on testCaseStarting too -- so the very next test case is always isolated
// again regardless of what the previous one's teardown did to the
// environment.
#include <catch2/reporters/catch_reporter_event_listener.hpp>
#include <catch2/reporters/catch_reporter_registrars.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <filesystem>
#include <string>

#include "Config/ConfigManager.h"

namespace
{
	// Deliberately never destroyed at static-teardown time -- same
	// reasoning as ConfigManager.cpp's own ConfigWriter::Instance(): its
	// only destruction point is testRunEnded, which always runs before the
	// process's static destructors get a chance to race it.
	std::filesystem::path *g_pRunDir = nullptr;

	void ClampConfigHome()
	{
		if ( g_pRunDir )
			setenv( "XDG_CONFIG_HOME", g_pRunDir->c_str(), 1 );
	}

	class GlobalConfigHomeIsolation : public Catch::EventListenerBase
	{
	public:
		using Catch::EventListenerBase::EventListenerBase;

		void testRunStarting( Catch::TestRunInfo const & ) override
		{
			std::filesystem::path base = std::filesystem::temp_directory_path() /
				"gamescope-ritz-tests-run-XXXXXX";
			std::string sTemplate = base.string();
			char *pszResult = mkdtemp( sTemplate.data() );
			if ( !pszResult )
				return; // Nothing to isolate into; individual per-file fixtures still apply.

			g_pRunDir = new std::filesystem::path( pszResult );
			ClampConfigHome();
		}

		// Re-assert before every test case: a fixture's destructor may have
		// unsetenv()'d XDG_CONFIG_HOME on the way out of the previous test
		// case (see the file header above), so the guarantee this listener
		// gives is "every test case starts isolated", not "the env variable
		// is untouched between test cases".
		void testCaseStarting( Catch::TestCaseInfo const & ) override
		{
			ClampConfigHome();
		}

		void testRunEnded( Catch::TestRunStats const & ) override
		{
			gamescope::config::FlushPendingWrites();
			unsetenv( "XDG_CONFIG_HOME" );
			if ( g_pRunDir )
			{
				std::error_code ec;
				std::filesystem::remove_all( *g_pRunDir, ec );
				delete g_pRunDir;
				g_pRunDir = nullptr;
			}
		}
	};

	CATCH_REGISTER_LISTENER( GlobalConfigHomeIsolation )
}

// ---------------------------------------------------------------------------
//  Regression test: proves the isolation this file installs actually holds,
//  from a test case that adds no fixture of its own -- exactly the shape of
//  test that caused the bug in the first place.
// ---------------------------------------------------------------------------
TEST_CASE( "the test binary never points XDG_CONFIG_HOME at the real config home", "[config][isolation]" )
{
	const char *pszXdgConfigHome = getenv( "XDG_CONFIG_HOME" );
	REQUIRE( pszXdgConfigHome != nullptr );
	REQUIRE( *pszXdgConfigHome != '\0' );

	const std::filesystem::path home( pszXdgConfigHome );

	const char *pszHome = getenv( "HOME" );
	if ( pszHome && *pszHome )
	{
		const std::filesystem::path realConfigHome = std::filesystem::path( pszHome ) / ".config";
		CHECK( home != realConfigHome );

		// Also refuse a temp dir that happens to alias the real path via a
		// parent/child relationship (e.g. HOME itself, or somewhere under
		// the real config home).
		auto itHome = home.begin();
		auto itReal = realConfigHome.begin();
		bool bRealIsPrefixOfHome = true;
		for ( ; itReal != realConfigHome.end(); ++itReal, ++itHome )
		{
			if ( itHome == home.end() || *itHome != *itReal )
			{
				bRealIsPrefixOfHome = false;
				break;
			}
		}
		CHECK_FALSE( bRealIsPrefixOfHome );
	}

	// And ConfigManager itself must resolve every config path under the
	// isolated directory, not just agree that the env var looks right.
	const std::string sGlobalPath = gamescope::config::GlobalConfigPath();
	CHECK( sGlobalPath.rfind( home.string(), 0 ) == 0 );
}
