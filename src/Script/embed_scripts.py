#!/usr/bin/env python3
# Turns this fork's bundled Lua tree (scripts/00-gamescope/**.lua) into a C
# header holding every chunk's bytes as a static array plus an ordered table
# of them -- the same "generated header with a byte array" shape
# Overlay/fonts/embed_font.py already uses for fonts and
# Overlay/embed_changelog.py uses for CHANGELOG.md. Invoked as a meson
# custom_target(); see src/meson.build's bundled_scripts_header.
#
# WHY EMBED RATHER THAN INSTALL TO share/gamescope-ritz/scripts.
# The bundled Lua is not optional decoration: scripts/00-gamescope/displays/
# IS the known-displays database (HDR metadata, colorimetry, mode
# generators) that DRMBackend looks a panel up in, and common/ defines the
# gamescope.modegen helpers those display files call. Until this change the
# tree was copied to $prefix/share/gamescope-ritz/scripts by a separate
# install step (default_extras_install.sh) that install.sh PROMPTED for,
# defaulting to "no" -- so the ordinary install silently produced a binary
# with no display database at all, and nothing said so beyond a warnf in a
# log nobody reads. A build-time copy cannot be declined, cannot be a
# different vintage than the binary, and needs no path resolution.
#
# The cost is ~31 kB of rodata and one build rule.
#
# ORDER IS PART OF THE CONTRACT.
# The scripts are not independent: common/util.lua defines the global
# debug()/info() helpers and common/modegen.lua defines gamescope.modegen,
# both of which the displays/*.lua files call at load time. On disk that
# ordering came out of CScriptManager::RunFolder's traversal -- every .lua
# in a directory, sorted, then every subdirectory, sorted, recursively
# ("common" sorts before "displays"). This walker reproduces that traversal
# exactly so the embedded order and the on-disk order are the same order,
# and a packager replacing the tree on disk gets identical behaviour.
import os
import sys


def walk_in_runfolder_order( root ):
    """Every .lua under root, in CScriptManager::RunFolder's exact order.

    RunFolder collects the regular .lua files of one directory and its
    subdirectories separately, sorts each list, runs the files, then
    recurses into the directories in turn. Reproduced literally rather
    than approximated with os.walk(), because the load order is a
    behavioural contract (see the header comment).
    """
    out = []
    try:
        entries = list( os.scandir( root ) )
    except OSError:
        return out

    files = sorted( e.path for e in entries
                    if e.is_file() and e.name.endswith( ".lua" ) )
    dirs = sorted( e.path for e in entries if e.is_dir() )

    out.extend( files )
    for d in dirs:
        out.extend( walk_in_runfolder_order( d ) )
    return out


def ident_for( rel_path ):
    """A valid C identifier derived from a script's path within the tree."""
    return "".join( c if ( c.isalnum() or c == "_" ) else "_" for c in rel_path )


def main():
    if len( sys.argv ) != 3:
        sys.stderr.write( "usage: embed_scripts.py <scripts-dir> <output.h>\n" )
        return 1

    in_dir, out_path = sys.argv[1], sys.argv[2]
    paths = walk_in_runfolder_order( in_dir )

    if not paths:
        # LOUD, not a fallback. A gamescope-ritz binary with no display
        # database is exactly the state this whole change exists to make
        # impossible, so an empty scripts/ tree must fail the build rather
        # than quietly ship the broken thing under a new mechanism.
        sys.stderr.write(
            "embed_scripts.py: {}: no .lua files found. The bundled script "
            "tree is the known-displays database and cannot be empty.\n"
            .format( in_dir ) )
        return 1

    with open( out_path, "w" ) as f:
        f.write( "// Auto-generated from {} by embed_scripts.py -- do not edit.\n"
                 .format( os.path.basename( in_dir.rstrip( "/" ) ) ) )
        f.write( "#pragma once\n\n" )

        idents = []
        for path in paths:
            rel = os.path.relpath( path, in_dir )
            ident = ident_for( rel )
            idents.append( ( ident, rel ) )
            with open( path, "rb" ) as src:
                data = src.read()
            f.write( "// {}\n".format( rel ) )
            f.write( "static const unsigned char g_BundledScript_{}_Data[] = {{\n"
                     .format( ident ) )
            for i in range( 0, len( data ), 20 ):
                f.write( ",".join( str( b ) for b in data[i:i + 20] ) )
                f.write( ",\n" )
            f.write( "};\n" )
            f.write( "static const unsigned int g_BundledScript_{}_Size = {}u;\n\n"
                     .format( ident, len( data ) ) )

        # The table Script.cpp walks. Order here IS the load order.
        f.write( "struct GamescopeBundledScript_t\n{\n" )
        f.write( "\tconst char          *pszName;\n" )
        f.write( "\tconst unsigned char *pData;\n" )
        f.write( "\tunsigned int         uSize;\n" )
        f.write( "};\n\n" )
        f.write( "static const GamescopeBundledScript_t g_BundledScripts[] = {\n" )
        for ident, rel in idents:
            f.write( "\t{{ \"{}\", g_BundledScript_{}_Data, g_BundledScript_{}_Size }},\n"
                     .format( rel.replace( "\\", "/" ), ident, ident ) )
        f.write( "};\n" )
        f.write( "static const unsigned int g_BundledScriptCount = {}u;\n"
                 .format( len( idents ) ) )

    return 0


if __name__ == "__main__":
    sys.exit( main() )
