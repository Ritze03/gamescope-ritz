// The FrameGen library's single translation unit (subprojects/FrameGen/gpu/
// framegen.cpp, a pinned submodule we must not edit), compiled as part of
// gamescope. A relative #include rather than a file in src/meson.build because
// meson's sandbox will not hand a file under subprojects/ to the parent project
// (see the note above `framegen_shaders` in src/meson.build). The `<name>_spv.h`
// headers framegen.cpp includes are generated into this build directory.
#include "../../subprojects/FrameGen/gpu/framegen.cpp"
