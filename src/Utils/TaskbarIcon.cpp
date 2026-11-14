#include "TaskbarIcon.h"

#include "Utils/DirHelpers.h"
#include "log.hpp"

// Static so these do not clash with the implementations steamcompmgr.cpp
// compiles, and so the unit tests can link this file alone.
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image.h>
#include <stb_image_write.h>

#include <ProjectIcon.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <sstream>
#include <unistd.h>

static LogScope s_log( "taskbar_icon" );

namespace gamescope::taskbar_icon
{
	const std::string &EnvAppId()
	{
		static const std::string s_sId = []
		{
			const char *p = getenv( "GAMESCOPE_APP_ID" );
			return std::string( p ? p : "" );
		}();
		return s_sId;
	}

	bool LargestImage( const std::vector<uint32_t> &data, Image &out )
	{
		bool bFound = false;
		size_t i = 0;
		while ( i + 2 <= data.size() )
		{
			const uint64_t w = data[i], h = data[i + 1];
			const uint64_t n = w * h;
			if ( !w || !h || n > data.size() - i - 2 )
				break;
			if ( !bFound || n > uint64_t( out.uWidth ) * out.uHeight )
			{
				out = { uint32_t( w ), uint32_t( h ), &data[i + 2] };
				bFound = true;
			}
			i += 2 + n;
		}
		return bFound;
	}

	std::string KeyForImage( const Image &img )
	{
		uint32_t h = 2166136261u;
		auto mix = [&]( uint32_t v )
		{
			for ( int s = 0; s < 32; s += 8 )
			{
				h ^= ( v >> s ) & 0xff;
				h *= 16777619u;
			}
		};
		mix( img.uWidth );
		mix( img.uHeight );
		for ( size_t i = 0; i < size_t( img.uWidth ) * img.uHeight; i++ )
			mix( img.pPixels[i] );
		char sz[40];
		snprintf( sz, sizeof( sz ), "%s-%08x", k_szFallbackKey, h );
		return sz;
	}

	std::vector<uint8_t> EncodePng( const Image &img )
	{
		const size_t n = size_t( img.uWidth ) * img.uHeight;
		std::vector<uint8_t> rgba( n * 4 );
		for ( size_t i = 0; i < n; i++ )
		{
			const uint32_t p = img.pPixels[i];
			rgba[i * 4 + 0] = ( p >> 16 ) & 0xff;
			rgba[i * 4 + 1] = ( p >> 8 ) & 0xff;
			rgba[i * 4 + 2] = p & 0xff;
			rgba[i * 4 + 3] = ( p >> 24 ) & 0xff;
		}
		std::vector<uint8_t> png;
		auto write = []( void *ctx, void *d, int len )
		{
			auto *v = static_cast<std::vector<uint8_t> *>( ctx );
			v->insert( v->end(), static_cast<uint8_t *>( d ), static_cast<uint8_t *>( d ) + len );
		};
		if ( !stbi_write_png_to_func( write, &png, img.uWidth, img.uHeight, 4, rgba.data(), img.uWidth * 4 ) )
			png.clear();
		return png;
	}

	std::string DesktopEntryText( const std::string &szKey, const std::string &szName, const std::string &szIconPath )
	{
		std::string szSafe;
		for ( char c : szName )
			if ( c != '\n' && c != '\r' )
				szSafe += c;
		std::ostringstream o;
		o << "[Desktop Entry]\n"
		  << "Type=Application\n"
		  << "Name=" << szSafe << "\n"
		  << "Icon=" << szIconPath << "\n"
		  << "Exec=true\n"
		  << "NoDisplay=true\n"
		  << "StartupWMClass=" << szKey << "\n";
		return o.str();
	}

	static std::string DataHome()
	{
		const char *p = getenv( "XDG_DATA_HOME" );
		if ( p && *p )
			return p;
		return std::string{ GetHomeDir() } + "/.local/share";
	}

	static bool ReadFile( const std::string &szPath, std::string &out )
	{
		std::ifstream f( szPath, std::ios::binary );
		if ( !f )
			return false;
		std::ostringstream o;
		o << f.rdbuf();
		out = o.str();
		return true;
	}

	// Write to a temp file in the same directory, then rename. Skips the write
	// when the file already holds exactly these bytes.
	static bool WriteAtomic( const std::string &szPath, const std::string &szBytes )
	{
		std::string szOld;
		if ( ReadFile( szPath, szOld ) && szOld == szBytes )
			return true;
		std::error_code ec;
		std::filesystem::create_directories( std::filesystem::path( szPath ).parent_path(), ec );
		const std::string szTmp = szPath + ".tmp" + std::to_string( getpid() );
		{
			std::ofstream f( szTmp, std::ios::binary | std::ios::trunc );
			f.write( szBytes.data(), std::streamsize( szBytes.size() ) );
			if ( !f )
				return false;
		}
		if ( rename( szTmp.c_str(), szPath.c_str() ) != 0 )
		{
			unlink( szTmp.c_str() );
			return false;
		}
		return true;
	}

	static bool WriteEntry( const std::string &szKey, const std::vector<uint8_t> &png, uint32_t uSize )
	{
		const std::string szData = DataHome();
		const std::string szIcon = szData + "/icons/hicolor/" + std::to_string( uSize ) + "x" + std::to_string( uSize ) + "/apps/" + szKey + ".png";
		const std::string szDesktop = szData + "/applications/" + szKey + ".desktop";
		// PNG first, then the .desktop that points at it.
		return WriteAtomic( szIcon, std::string( png.begin(), png.end() ) ) &&
			WriteAtomic( szDesktop, DesktopEntryText( szKey, "Gamescope", szIcon ) );
	}

	// One try per key per process: a failure is logged once and not retried.
	// ponytail: written synchronously on the caller's (steamcompmgr) thread;
	// two small files once per game, not worth an async path.
	static std::string EnsureOnce( const std::string &szKey, const std::vector<uint8_t> &png, uint32_t uSize )
	{
		static std::mutex s_mutex;
		static std::set<std::string> s_done;
		static std::set<std::string> s_failed;
		std::lock_guard lock( s_mutex );
		if ( s_done.count( szKey ) )
			return szKey;
		if ( s_failed.count( szKey ) )
			return {};
		if ( png.empty() || !WriteEntry( szKey, png, uSize ) )
		{
			s_log.errorf( "could not write taskbar icon files for %s", szKey.c_str() );
			s_failed.insert( szKey );
			return {};
		}
		s_done.insert( szKey );
		return szKey;
	}

	std::string EnsureForIcon( const std::vector<uint32_t> &data )
	{
		Image img;
		if ( !LargestImage( data, img ) )
			return {};
		return EnsureOnce( KeyForImage( img ), EncodePng( img ), img.uWidth );
	}

	std::string EnsureFallback()
	{
		const std::vector<uint32_t> &px = ProjectIconPixels();
		if ( px.size() < 3 )
			return {};
		return EnsureOnce( k_szFallbackKey,
			std::vector<uint8_t>( g_ProjectIcon_Data, g_ProjectIcon_Data + g_ProjectIcon_Size ), px[0] );
	}

	const std::vector<uint32_t> &ProjectIconPixels()
	{
		static const std::vector<uint32_t> s_px = []
		{
			std::vector<uint32_t> v;
			int w, h, c;
			unsigned char *d = stbi_load_from_memory( g_ProjectIcon_Data, int( g_ProjectIcon_Size ), &w, &h, &c, 4 );
			if ( !d )
				return v;
			v.reserve( 2 + size_t( w ) * h );
			v.push_back( w );
			v.push_back( h );
			for ( size_t i = 0; i < size_t( w ) * h; i++ )
				v.push_back( uint32_t( d[i * 4 + 3] ) << 24 | uint32_t( d[i * 4] ) << 16 | uint32_t( d[i * 4 + 1] ) << 8 | d[i * 4 + 2] );
			stbi_image_free( d );
			return v;
		}();
		return s_px;
	}
}
