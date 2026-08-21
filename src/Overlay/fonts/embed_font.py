#!/usr/bin/env python3
# Turns one .ttf into a C header holding its raw bytes as a static array,
# the same "generated header with a byte array" shape src/meson.build's own
# glsl_generator already uses for compiled shaders (see glsl_compiler's
# --vn flag there) -- reused here rather than adding a new build-time tool
# or dependency (e.g. `xxd -i`, ImGui's binary_to_compressed_c) just to get
# a font's bytes into the binary. Invoked as a meson generator(); see
# src/meson.build's font_embed_gen.
#
# Output is consumed via #include "<BASENAME>.h" from Overlay/Fonts.cpp,
# same convention rendervulkan.cpp already uses for spirv_shaders' generated
# headers.
import sys

def main():
    if len( sys.argv ) != 3:
        sys.stderr.write( "usage: embed_font.py <input.ttf> <output.h>\n" )
        return 1

    in_path, out_path = sys.argv[1], sys.argv[2]

    with open( in_path, "rb" ) as f:
        data = f.read()

    # Derive a valid C identifier from the input's basename (IBM Plex
    # filenames use hyphens, e.g. "IBMPlexSans-Regular.ttf").
    import os
    stem = os.path.splitext( os.path.basename( in_path ) )[0]
    ident = "".join( c if ( c.isalnum() or c == "_" ) else "_" for c in stem )

    with open( out_path, "w" ) as f:
        f.write( "// Auto-generated from {}.ttf by embed_font.py -- do not edit.\n".format( stem ) )
        f.write( "#pragma once\n" )
        f.write( "static const unsigned char g_Font_{}_Data[] = {{\n".format( ident ) )
        for i in range( 0, len( data ), 20 ):
            f.write( ",".join( str( b ) for b in data[i:i + 20] ) )
            f.write( ",\n" )
        f.write( "};\n" )
        f.write( "static const unsigned int g_Font_{}_Size = {}u;\n".format( ident, len( data ) ) )

    return 0

if __name__ == "__main__":
    sys.exit( main() )
