// The FrameGen library's single translation unit (subprojects/FrameGen/gpu/
// framegen.cpp, a pinned submodule we must not edit), compiled as part of
// gamescope. A relative #include rather than a file in src/meson.build because
// meson's sandbox will not hand a file under subprojects/ to the parent project
// (see the note above `framegen_shaders` in src/meson.build). The `<name>_spv.h`
// headers framegen.cpp includes are generated into this build directory.
//
// The per-format variants (`<name>_rgb10_spv.h`, `<name>_f16_spv.h`) are found by
// framegen.cpp with `__has_include`. A header that did not exist when an object was
// compiled is not in that object's dependency file, so an INCREMENTAL build dir from
// before the variants were added would keep a library that "does not have" them
// (init() then says "the shader variants of this format are not built into the
// library"). This file is the one place to touch when the variant set changes: the
// edit forces the one recompile. Variants built: rgb10, f16 (NV12 / P010 are not).
#include "../../subprojects/FrameGen/gpu/framegen.cpp"
