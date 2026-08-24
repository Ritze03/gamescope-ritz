#!/usr/bin/env python3
# Turns CHANGELOG.md into a C header holding its bytes as a static array --
# the same "generated header with a byte array" shape Overlay/fonts/
# embed_font.py already uses for fonts and glsl_generator uses for shaders.
# Reused rather than adding a new build-time tool or an install-path lookup.
# Invoked as a meson custom_target(); see src/meson.build's changelog_header.
#
# WHY EMBED RATHER THAN READ FROM DISK AT RUNTIME.
# The alternative is installing CHANGELOG.md to a data dir and open()ing it
# when the area is first drawn. Embedding wins on three counts that matter
# here:
#
#   1. IT CANNOT DISAGREE WITH THE BINARY. The changelog's whole job on this
#      screen is to say what THIS build contains, next to that build's
#      version strings. A file read at runtime can be a different vintage
#      than the binary reading it -- upgrade the package, keep an old
#      running process, or run ./build/src/gamescope against an installed
#      /usr/share copy, and the screen quietly lies. A build-time copy is
#      pinned to the build by construction.
#   2. NO PATH RESOLUTION, NO MISSING-FILE STATE AT RUNTIME. A dev build run
#      from ./build/src/ and an installed build have different data paths;
#      resolving that means a search list, and a search list means a "not
#      found" state on a screen whose entire purpose is to answer a
#      question.
#   3. NO I/O ON THE RENDER THREAD. The overlay draws inside the compositor's
#      frame loop. This is the same reason LogCapture hands the UI a cheap
#      snapshot instead of letting it touch a file.
#
# The cost is ~20 kB of rodata and one build rule. That is the right trade
# for a small, immutable, per-build document.
import os
import sys

# What gets embedded when CHANGELOG.md is absent. A source tarball exported
# without it, or a downstream re-packaging, should still BUILD -- and should
# still say plainly why the screen is empty rather than showing nothing and
# leaving the reader to guess whether the build has no changes or the file
# just went missing.
MISSING_TEXT = (
    "# Changelog\n"
    "\n"
    "CHANGELOG.md was not present in the source tree this binary was built\n"
    "from, so no changelog could be embedded.\n"
    "\n"
    "The version information above is still accurate -- it comes from the\n"
    "build, not from this file.\n"
)


def main():
    if len( sys.argv ) != 3:
        sys.stderr.write( "usage: embed_changelog.py <CHANGELOG.md> <output.h>\n" )
        return 1

    in_path, out_path = sys.argv[1], sys.argv[2]

    try:
        with open( in_path, "rb" ) as f:
            data = f.read()
        present = True
    except OSError:
        # Deliberately NOT an error: see MISSING_TEXT above.
        data = MISSING_TEXT.encode( "utf-8" )
        present = False

    with open( out_path, "w" ) as f:
        f.write( "// Auto-generated from {} by embed_changelog.py -- do not edit.\n"
                 .format( os.path.basename( in_path ) ) )
        f.write( "#pragma once\n" )
        f.write( "static const unsigned char g_Changelog_Data[] = {\n" )
        for i in range( 0, len( data ), 20 ):
            f.write( ",".join( str( b ) for b in data[i:i + 20] ) )
            f.write( ",\n" )
        f.write( "};\n" )
        f.write( "static const unsigned int g_Changelog_Size = {}u;\n".format( len( data ) ) )
        # Lets the UI distinguish "this build has no changelog" from "the
        # changelog says nothing", without string-matching the placeholder.
        f.write( "static const bool g_Changelog_Present = {};\n"
                 .format( "true" if present else "false" ) )

    return 0


if __name__ == "__main__":
    sys.exit( main() )
