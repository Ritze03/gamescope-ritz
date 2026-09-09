#pragma once

// =============================================================================
//  Turning an app id into a game's name -- the parts with no network in them
// =============================================================================
// The friends list gets an APP ID out of Steam and nothing else. Names came
// from Steam's own appmanifest_<id>.acf until 2026-09-09, which covers only the
// games THIS MACHINE HAS INSTALLED -- a friend playing something the user does
// not own read as "App 252490". This file closes that gap, and it is where the
// decision to make the compositor talk to the internet at all is written down.
//
// TWO SOURCES, IN THIS ORDER, AND THE SECOND IS ASKED ONLY WHAT THE FIRST
// COULD NOT ANSWER:
//
//   1. the local appmanifest (SteamFriendsCmd.h's AppNameFromManifest) -- free,
//      offline, already there;
//   2. Steam's own keyless public endpoint, for the ids left over.
//
// WHICH ENDPOINT, AND WHY THIS ONE. Measured 2026-09-09, from this machine:
//
//   ISteamApps/GetAppList/v2 and /v0002  -- HTTP 404, "Method 'GetAppList' not
//       found in interface 'ISteamApps'". The "download the whole app list
//       once" option is not a size trade-off any more; the endpoint is GONE.
//   store.../api/appdetails?appids=<id>  -- 36'501 bytes for ONE app, 15'231
//       with filters=basic (still the full store description). One request per
//       app, and the endpoint is the rate-limited one.
//       (filters=name is not a thing: it answers {"success":true,"data":[]}.)
//   api.../ICommunityService/GetApps/v1  -- keyless, BATCHED, and 711 bytes for
//       FOUR apps. An id Steam does not know comes back as {"appid":N} with no
//       name, which is a clean, authoritative "there is no name for this".
//
// So: ICommunityService/GetApps/v1. One request per poll rather than one per
// app, ~50x smaller than the alternative that still exists, and the rate limit
// that makes appdetails awkward never comes into play.
//
// WHAT LEAVES THE MACHINE, EXACTLY. The query string, and it is built by
// BuildAppNamesUrl() below out of std::to_string() over integers -- so it is
// APP IDS AND NOTHING ELSE. No SteamID, no persona name, no lobby id, no
// account name, no machine id, nothing about who is asking. Plus what any HTTP
// request unavoidably carries: this machine's IP address, and curl's own
// version string as the User-Agent. No cookie is sent (BuildFetchArgv() passes
// no jar and reads no curlrc), so the request cannot be tied to a Steam login.
//
// AND IT CAN BE TURNED OFF: OverlaySettings::friends_lookup_names. Off, this
// file's network half never runs and an unknown game stays "App <id>".
//
// WHAT LIVES HERE:
//   * AppName / AppNameMap  -- the cache's shape, including its "" negative.
//   * ParseAppNameCache()/SerialiseAppNameCache() -- the on-disk JSON, and its
//     deliberately total tolerance of a corrupt file.
//   * TrimAppNameCache()    -- the bound. A cache that grows forever is a bug.
//   * BuildAppNamesUrl()    -- the one URL this compositor ever fetches.
//   * ParseAppNamesResponse() -- Steam's answer, and its authoritative "no".
//   * BuildFetchArgv()      -- the curl command line, and every flag's reason.
//   * AppNameCachePath()    -- XDG_CACHE_HOME, never the config directory.
//
// The network half itself (spawning that argv, waiting for it without letting
// it delay gamescope's exit) is in src/SteamFriends.cpp, on the poller thread,
// because that is the only thread allowed to block on anything.

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace gamescope::steamfriends
{
	// =========================================================================
	//  The cache's shape
	// =========================================================================
	// sName EMPTY IS A REAL ANSWER, not a missing one: it means "Steam was
	// asked about this id and said it has no name for it". Storing that is the
	// whole reason a friend playing something unlisted costs one request ever
	// rather than one request every three seconds.
	//
	// A FAILED REQUEST IS NEVER STORED AS ONE. A timeout, no network, a 500 --
	// none of them are an answer about the app, and writing "" for them would
	// bake a temporary outage into a permanent wrong label. SteamFriends.cpp
	// keeps that case in memory for the session only.
	struct AppName
	{
		std::string sName;      // "" == Steam has no name for this app id
		int64_t     nSeen = 0;  // unix seconds, last time a friend was seen in it
	};

	// Compared in tests, so the eviction rule can be asserted to be a function
	// of the data rather than of the machine it ran on.
	inline bool operator==( const AppName &a, const AppName &b )
	{
		return a.sName == b.sName && a.nSeen == b.nSeen;
	}

	using AppNameMap = std::map<uint32_t, AppName>;

	// THE BOUND. 512 distinct games is more than a friends list produces in
	// years, and the file is then at most a few tens of kilobytes -- but "at
	// most" is the point: an unbounded cache in a long-lived compositor is a
	// leak with a filename.
	inline constexpr size_t kAppNameCacheMax = 512;

	// One request's worth of ids. The URL stays short, and a friends list
	// never has this many unknown games in it anyway.
	inline constexpr size_t kAppNamesPerRequest = 32;

	// Refuse to parse anything absurd rather than allocating it. A cache file
	// this size is corrupt or hostile, and either way the right answer is an
	// empty map.
	inline constexpr size_t kAppNameCacheMaxBytes = 1024 * 1024;

	// =========================================================================
	//  The file
	// =========================================================================
	// {"version":1,"apps":{"730":{"name":"Counter-Strike 2","seen":1757…}}}
	//
	// `Why a version field on a cache:` it is not for migration -- there is
	// nothing to migrate, the file can always be thrown away. It is so a
	// FUTURE format cannot be half-read by THIS build: an unknown version
	// parses as empty, which is the same as a fresh machine and is always safe.
	inline constexpr int kAppNameCacheVersion = 1;

	// Total: every malformed input answers with an empty map rather than
	// throwing, and the caller treats that exactly like "no cache file yet".
	// Truncation, a stray byte, a wrong type in one entry, a whole file of
	// zeroes -- all of them are the same, cheap, correct outcome.
	inline AppNameMap ParseAppNameCache( std::string_view svJson )
	{
		AppNameMap map;
		if ( svJson.empty() || svJson.size() > kAppNameCacheMaxBytes )
			return map;

		// allow_exceptions=false: this build compiles with -fno-exceptions,
		// so a throwing parse would be a std::abort() on a corrupt file --
		// i.e. a truncated cache would kill the compositor.
		const nlohmann::json j = nlohmann::json::parse( svJson, nullptr, false, true );
		if ( j.is_discarded() || !j.is_object() )
			return map;

		const auto itVersion = j.find( "version" );
		if ( itVersion == j.end() || !itVersion->is_number_integer() ||
		     itVersion->get<int>() != kAppNameCacheVersion )
			return map;

		const auto itApps = j.find( "apps" );
		if ( itApps == j.end() || !itApps->is_object() )
			return map;

		for ( auto it = itApps->begin(); it != itApps->end(); ++it )
		{
			if ( !it.value().is_object() )
				continue;

			// strtoul, not stoul: -fno-exceptions again. A key that is not a
			// number is skipped, not fatal.
			char *pszEnd = nullptr;
			const unsigned long ul = strtoul( it.key().c_str(), &pszEnd, 10 );
			if ( !pszEnd || *pszEnd != '\0' || ul == 0 || ul > 0xFFFFFFFFul )
				continue;

			AppName entry;
			const auto itName = it.value().find( "name" );
			if ( itName != it.value().end() && itName->is_string() )
				entry.sName = itName->get<std::string>();
			const auto itSeen = it.value().find( "seen" );
			if ( itSeen != it.value().end() && itSeen->is_number_integer() )
				entry.nSeen = itSeen->get<int64_t>();

			map[ (uint32_t)ul ] = std::move( entry );
			if ( map.size() >= kAppNameCacheMax )
				break;   // a file that grew past the bound is truncated on read too
		}
		return map;
	}

	inline std::string SerialiseAppNameCache( const AppNameMap &map )
	{
		nlohmann::json jApps = nlohmann::json::object();
		for ( const auto &[ uAppId, entry ] : map )
		{
			jApps[ std::to_string( (unsigned long long)uAppId ) ] = {
				{ "name", entry.sName },
				{ "seen", entry.nSeen },
			};
		}
		nlohmann::json j;
		j[ "version" ] = kAppNameCacheVersion;
		j[ "apps" ]    = std::move( jApps );
		return j.dump();
	}

	// THE BOUND, ENFORCED. Keeps the nMax most recently SEEN entries and drops
	// the rest -- an LRU over "when was a friend last playing this", which is
	// exactly the thing that makes an entry worth keeping.
	//
	// The app id is the tiebreak so the result depends only on the data: two
	// entries with the same timestamp must not be evicted in an order that
	// changes between runs, or two machines with the same history would
	// disagree about what is cached.
	inline void TrimAppNameCache( AppNameMap &map, size_t nMax = kAppNameCacheMax )
	{
		if ( map.size() <= nMax )
			return;

		std::vector<std::pair<int64_t, uint32_t>> vecByAge;
		vecByAge.reserve( map.size() );
		for ( const auto &[ uAppId, entry ] : map )
			vecByAge.push_back( { entry.nSeen, uAppId } );

		// Newest first; the tail past nMax goes.
		std::sort( vecByAge.begin(), vecByAge.end(),
			[]( const auto &a, const auto &b )
			{
				if ( a.first != b.first )
					return a.first > b.first;
				return a.second > b.second;
			} );

		for ( size_t i = nMax; i < vecByAge.size(); i++ )
			map.erase( vecByAge[ i ].second );
	}

	// =========================================================================
	//  The one URL this compositor ever fetches
	// =========================================================================
	// EVERY BYTE AFTER THE '?' IS std::to_string() OVER AN INTEGER, which is
	// the same property BuildJoinUrl() has and for the same reason: nothing a
	// remote person controls -- a persona name above all -- has a path into it.
	// A caller cannot smuggle a header, a second parameter or another host in
	// here, because there is no string input to smuggle it through.
	//
	// The brackets are percent-encoded rather than passed literally: curl
	// treats [] as a GLOB in a URL, and a bare `appids[0]` would either be
	// expanded or rejected depending on the flags in force. %5B/%5D depends on
	// no flag at all.
	inline constexpr std::string_view kAppNamesEndpoint =
		"https://api.steampowered.com/ICommunityService/GetApps/v1/";

	inline std::string BuildAppNamesUrl( const std::vector<uint32_t> &vecAppIds )
	{
		if ( vecAppIds.empty() )
			return {};

		std::string s( kAppNamesEndpoint );
		s += '?';
		size_t n = 0;
		for ( uint32_t uAppId : vecAppIds )
		{
			if ( uAppId == 0 )
				continue;
			if ( n >= kAppNamesPerRequest )
				break;
			if ( n )
				s += '&';
			s += "appids%5B";
			s += std::to_string( (unsigned long long)n );
			s += "%5D=";
			s += std::to_string( (unsigned long long)uAppId );
			n++;
		}
		return n ? s : std::string();
	}

	// Steam's answer. `{"response":{"apps":[{"appid":730,"name":"…"},…]}}`.
	//
	// An entry with an appid and NO name is Steam saying it has no name for
	// that id, and is returned as an empty name -- an authoritative negative
	// the caller caches so the id is never asked about again. An id that is
	// missing from the response entirely is NOT in the result, so the caller
	// leaves it unresolved rather than inventing a negative for it.
	inline AppNameMap ParseAppNamesResponse( std::string_view svJson, int64_t nNow )
	{
		AppNameMap map;
		if ( svJson.empty() || svJson.size() > kAppNameCacheMaxBytes )
			return map;

		const nlohmann::json j = nlohmann::json::parse( svJson, nullptr, false, true );
		if ( j.is_discarded() || !j.is_object() )
			return map;

		const auto itResponse = j.find( "response" );
		if ( itResponse == j.end() || !itResponse->is_object() )
			return map;
		const auto itApps = itResponse->find( "apps" );
		if ( itApps == itResponse->end() || !itApps->is_array() )
			return map;

		for ( const nlohmann::json &jApp : *itApps )
		{
			if ( !jApp.is_object() )
				continue;
			const auto itId = jApp.find( "appid" );
			if ( itId == jApp.end() || !itId->is_number_integer() )
				continue;
			const int64_t nId = itId->get<int64_t>();
			if ( nId <= 0 || nId > 0xFFFFFFFFll )
				continue;

			AppName entry;
			entry.nSeen = nNow;
			const auto itName = jApp.find( "name" );
			if ( itName != jApp.end() && itName->is_string() )
				entry.sName = itName->get<std::string>();

			// A name is drawn straight into the panel, so it is bounded here
			// rather than trusted: Steam does not ship 4 KB game names, and a
			// row is a row whatever a compromised or confused endpoint says.
			if ( entry.sName.size() > 128 )
				entry.sName.resize( 128 );
			// Control bytes would draw as glyph soup or worse; there are none
			// in a real game name.
			for ( char &c : entry.sName )
				if ( (unsigned char)c < 0x20 )
					c = ' ';

			map[ (uint32_t)nId ] = std::move( entry );
		}
		return map;
	}

	// =========================================================================
	//  The fetch
	// =========================================================================
	// `curl`, in a child process, rather than libcurl in ours. `Why:`
	//
	//   * the compositor gains NO new link-time dependency and no TLS stack in
	//     its own address space -- a network parser that goes wrong takes a
	//     child process with it, not the frame path;
	//   * --max-time is a hard bound somebody else enforces, so "the network
	//     hung" is a process that exits rather than a thread we have to learn
	//     to interrupt;
	//   * it is the same spawn discipline Join() already uses.
	//
	// EVERY FLAG IS A PRIVACY OR SAFETY DECISION, not a default:
	//   -q                 ignore ~/.curlrc, which could add headers, a proxy
	//                      or a cookie jar this code never intended to send.
	//   -s -S -f           quiet, but an HTTP error is a FAILURE rather than an
	//                      error page written into the cache file.
	//   --proto =https     https and nothing else, ever.
	//   --max-redirs 0     no redirect. If Valve moves this, we degrade to
	//                      "App <id>" rather than follow a stranger.
	//   --max-time /
	//   --connect-timeout  a slow or blackholed endpoint ends, on its own.
	//   -o <file>          the answer goes to a file, so no pipe plumbing and
	//                      no unbounded read into our own memory.
	//   --                 the URL can never be read as a flag.
	//
	// No cookie jar and no --netrc: nothing that could tie the request to the
	// user's Steam login is sent, and there is no flag here that could pick one
	// up by accident.
	inline constexpr std::string_view kFetchProgram = "curl";
	inline constexpr int kFetchTimeoutSec = 8;
	inline constexpr int kFetchConnectTimeoutSec = 4;

	inline std::vector<std::string> BuildFetchArgv( std::string_view svUrl, std::string_view svOutPath )
	{
		return {
			std::string( kFetchProgram ),
			"-q", "-s", "-S", "-f",
			"--proto", "=https",
			"--max-redirs", "0",
			"--connect-timeout", std::to_string( kFetchConnectTimeoutSec ),
			"--max-time", std::to_string( kFetchTimeoutSec ),
			"-o", std::string( svOutPath ),
			"--", std::string( svUrl ),
		};
	}

	// =========================================================================
	//  Where the cache lives
	// =========================================================================
	// A CACHE DIRECTORY, NOT THE CONFIG DIRECTORY. Game names are not a
	// setting: nothing here is the user's choice, none of it is worth backing
	// up, and deleting the whole file must cost nothing but a few hundred
	// bytes of traffic. ~/.config/gamescope-ritz is for things the user chose.
	//
	// GS_RITZ_APPNAME_CACHE overrides the whole path, which is how a test
	// points this at a fixture without going anywhere near a real home
	// directory -- the same lever GS_RITZ_STEAMCLIENT is for the library.
	inline std::string AppNameCacheDir()
	{
		if ( const char *psz = getenv( "XDG_CACHE_HOME" ); psz && *psz )
			return std::string( psz ) + "/gamescope-ritz";
		if ( const char *psz = getenv( "HOME" ); psz && *psz )
			return std::string( psz ) + "/.cache/gamescope-ritz";
		return {};
	}

	inline std::string AppNameCachePath()
	{
		if ( const char *psz = getenv( "GS_RITZ_APPNAME_CACHE" ); psz && *psz )
			return psz;
		const std::string sDir = AppNameCacheDir();
		return sDir.empty() ? std::string() : sDir + "/appnames.json";
	}
}
