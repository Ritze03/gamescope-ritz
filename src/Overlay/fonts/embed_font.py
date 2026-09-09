#!/usr/bin/env python3
# Turns one file into a C header holding its raw bytes as a static array,
# the same "generated header with a byte array" shape src/meson.build's own
# glsl_generator already uses for compiled shaders (see glsl_compiler's
# --vn flag there) -- reused here rather than adding a new build-time tool
# or dependency (e.g. `xxd -i`, ImGui's binary_to_compressed_c) just to get
# a font's bytes into the binary. Invoked as a meson generator(); see
# src/meson.build's font_embed_gen.
#
# IT EMBEDS ANY ASSET, NOT ONLY FONTS. The symbol prefix is an optional
# third argument (default "g_Font_"), which is the whole of what made this
# font-specific. src/meson.build declares a second generator over the same
# script, asset_embed_gen with prefix "g_Asset_", for the licence texts the
# About area renders -- one build tool for "bytes in, bytes out" rather than
# a near-copy of this file per asset kind.
#
# Output is consumed via #include "<BASENAME>.h" from Overlay/Fonts.cpp and
# Overlay/PanelChangelog.cpp, the same convention rendervulkan.cpp already
# uses for spirv_shaders' generated headers.
import sys

def main():
    if len( sys.argv ) not in ( 3, 4 ):
        sys.stderr.write( "usage: embed_font.py <input> <output.h> [symbol-prefix]\n" )
        return 1

    in_path, out_path = sys.argv[1], sys.argv[2]
    prefix = sys.argv[3] if len( sys.argv ) == 4 else "g_Font_"

    with open( in_path, "rb" ) as f:
        data = f.read()

    # Derive a valid C identifier from the input's basename (Geist
    # filenames use hyphens, e.g. "GeistMono-Regular.ttf").
    import os
    base = os.path.basename( in_path )
    stem = os.path.splitext( base )[0]
    ident = "".join( c if ( c.isalnum() or c == "_" ) else "_" for c in stem )

    with open( out_path, "w" ) as f:
        f.write( "// Auto-generated from {} by embed_font.py -- do not edit.\n".format( base ) )
        f.write( "#pragma once\n" )
        f.write( "static const unsigned char {}{}_Data[] = {{\n".format( prefix, ident ) )
        for i in range( 0, len( data ), 20 ):
            f.write( ",".join( str( b ) for b in data[i:i + 20] ) )
            f.write( ",\n" )
        f.write( "};\n" )
        f.write( "static const unsigned int {}{}_Size = {}u;\n".format( prefix, ident, len( data ) ) )

    return 0

if __name__ == "__main__":
    sys.exit( main() )
