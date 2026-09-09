## Licences and attribution

This section lists the third-party code and data that is compiled into the
gamescope-ritz binary, and reproduces the licences that require it. It is
shown in the overlay's About area as well as living here in the source tree,
because a binary is redistributed on its own and the notices have to travel
with it.

Only components whose code or data actually ends up inside the binary are
listed. System libraries loaded at runtime -- wlroots, libdrm, the Vulkan
loader, SDL2, libinput, LuaJIT, PipeWire, pixman, the X11 libraries,
libdisplay-info, libliftoff, libdecor, OpenVR -- are linked dynamically and
come from your distribution with their own licences; they are not copied in
here. If you build with meson's vendored fallbacks instead, those copies
carry their own LICENSE files under subprojects/.

### What is compiled in

- gamescope and gamescope-ritz -- BSD 2-Clause. Valve Corporation, NVIDIA
  Corporation. Text under "gamescope" below.
- Tetrahedral 3D LUT code from OpenColorIO -- BSD 3-Clause. Text under
  "gamescope" below.
- ReShade FX effect compiler (src/reshade/source) -- BSD 3-Clause. Copyright
  2014 Patrick Mours. Text under "gamescope" below.
- sol2 (thirdparty/sol) -- MIT. Copyright (c) 2013-2020 Rapptz, ThePhD and
  contributors.
- Dear ImGui -- MIT. Copyright (c) 2014-2026 Omar Cornut.
- nlohmann/json -- MIT. Copyright (c) 2013-2022 Niels Lohmann.
- stb (stb_image, stb_image_write, stb_truetype) -- dual-licensed MIT or
  public domain; used here under MIT. Copyright (c) 2017 Sean Barrett.
- OpenGL Mathematics (GLM) -- dual-licensed The Happy Bunny License or MIT;
  used here under MIT. Copyright (c) 2005 G-Truc Creation.
- SPIRV-Headers -- MIT-style Khronos licence. Copyright (c) 2015-2018 The
  Khronos Group Inc. Text below.
- Geist and Geist Mono -- SIL Open Font License 1.1. Copyright 2024 The Geist
  Project Authors. The glyph outlines are embedded in the binary; full text
  at the end of this section.

### MIT License

Applies to sol2, Dear ImGui, nlohmann/json, stb and GLM, each under its own
copyright notice listed above.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

### SPIRV-Headers

Copyright (c) 2015-2018 The Khronos Group Inc.

Permission is hereby granted, free of charge, to any person obtaining a
copy of this software and/or associated documentation files (the
"Materials"), to deal in the Materials without restriction, including
without limitation the rights to use, copy, modify, merge, publish,
distribute, sublicense, and/or sell copies of the Materials, and to
permit persons to whom the Materials are furnished to do so, subject to
the following conditions:

The above copyright notice and this permission notice shall be included
in all copies or substantial portions of the Materials.

MODIFICATIONS TO THIS FILE MAY MEAN IT NO LONGER ACCURATELY REFLECTS
KHRONOS STANDARDS. THE UNMODIFIED, NORMATIVE VERSIONS OF KHRONOS
SPECIFICATIONS AND HEADER INFORMATION ARE LOCATED AT
   https://www.khronos.org/registry/

THE MATERIALS ARE PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
MATERIALS OR THE USE OR OTHER DEALINGS IN THE MATERIALS.
