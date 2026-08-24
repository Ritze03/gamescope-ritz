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
#
# IT ALSO EXTRACTS THE PROJECT'S VERSION.
# gamescope-ritz has no other version marker: it carries no tags and
# meson.build's project() declares none. Rather than ADD a second place to
# write the number -- which is a thing to keep in sync, and therefore a thing
# that eventually disagrees -- the number is DERIVED here from the top
# block's `## [x.y.z] - YYYY-MM-DD` heading, in the same pass that reads the
# file for embedding. A semver in the binary that disagrees with the one in
# the changelog is worse than neither, because both look authoritative; this
# makes the disagreement unrepresentable rather than merely discouraged.
import os
import re
import sys

# The top block's heading. Deliberately strict -- three all-numeric parts in
# brackets, an en dash or a hyphen, then the date -- because this is the one
# place the version is read from, and a loose pattern would silently accept a
# malformed heading and ship a wrong number.
VERSION_RE = re.compile(
    r"^##\s+\[(\d+\.\d+\.\d+)\]\s*[–-]\s*\d{4}-\d{2}-\d{2}\s*$" )


def extract_version( text ):
    """The first `## [x.y.z] - date` heading, or None if there is none."""
    for line in text.splitlines():
        if line.startswith( "## " ):
            m = VERSION_RE.match( line.strip() )
            # The FIRST `## ` heading decides: it is the newest block by the
            # newest-on-top rule, so a later well-formed heading must not be
            # able to stand in for a malformed newest one.
            return m.group( 1 ) if m else None
    return None

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
    "The base commit and build date above still come from git and are\n"
    "accurate; only the version number and these notes are missing.\n"
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

    if present:
        version = extract_version( data.decode( "utf-8", "replace" ) )
        if version is None:
            # LOUD, not a fallback. Everything else in this file degrades
            # gracefully because a missing changelog is a legitimate state
            # for a re-packaged tarball. A changelog that IS there but whose
            # newest heading cannot be read is different: it means the file
            # was edited into a shape the format does not allow, and the only
            # honest version to report would be a guess. Fail the build and
            # let whoever broke the heading fix it.
            sys.stderr.write(
                "embed_changelog.py: {}: could not read a version from the "
                "first '## ' heading.\n"
                "  Expected:  ## [x.y.z] - YYYY-MM-DD\n"
                "  The newest block's version is this project's version "
                "marker; see\n"
                "  superdoc/claude-instructions/changelog.md.\n"
                .format( in_path ) )
            return 1
    else:
        # No file, so no version to derive. The UI has g_Changelog_Present to
        # tell this apart from a real version and says so rather than
        # printing a number nothing backs.
        version = "unknown"

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
        # The project's version, derived from the top block above -- see the
        # header comment for why it is derived here rather than declared in
        # meson.build or a VERSION file.
        f.write( "static const char g_Changelog_Version[] = \"{}\";\n"
                 .format( version ) )

    return 0


if __name__ == "__main__":
    sys.exit( main() )
