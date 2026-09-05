# Third-Party Notices

Airelinna V1 uses the pinned dependencies listed below. GLFW, Dear ImGui, and
nlohmann/json are statically linked into the Windows product. Catch2 is used
only to build the test executables and is not included in the release archive.

## GLFW 3.4 — altered source

Source: https://github.com/glfw/glfw/releases/tag/3.4

License: zlib/libpng

Airelinna builds a patched copy of GLFW 3.4. The checked-in patch removes
`CS_OWNDC` only from GLFW's main Win32 window class because Airelinna creates
Vulkan `GLFW_NO_API` windows. GLFW's helper window class is unchanged. This is
an altered source version, and the exact change is recorded in
`third_party/patches/glfw-3.4-win32-no-own-dc.patch`.

Copyright (c) 2002-2006 Marcus Geelnard

Copyright (c) 2006-2019 Camilla Löwy

This software is provided 'as-is', without any express or implied warranty. In
no event will the authors be held liable for any damages arising from the use
of this software.

Permission is granted to anyone to use this software for any purpose, including
commercial applications, and to alter it and redistribute it freely, subject
to the following restrictions:

1. The origin of this software must not be misrepresented; you must not claim
   that you wrote the original software. If you use this software in a product,
   an acknowledgment in the product documentation would be appreciated but is
   not required.
2. Altered source versions must be plainly marked as such, and must not be
   misrepresented as being the original software.
3. This notice may not be removed or altered from any source distribution.

## Dear ImGui 1.92.9 — altered source

Source: https://github.com/ocornut/imgui/releases/tag/v1.92.9

License: MIT

Airelinna patches the Vulkan backend so fallible object creation commits
handles to backend-owned state only after success. This lets Airelinna stop the
backend immediately on an error without later destroying an undefined Vulkan
output. The exact change is recorded in
`third_party/patches/imgui-1.92.9-vulkan-safe-failure.patch`.

Copyright (c) 2014-2026 Omar Cornut

Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the "Software"), to deal in
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
the Software, and to permit persons to whom the Software is furnished to do so,
subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

## JSON for Modern C++ 3.12.0

Source: https://github.com/nlohmann/json/releases/tag/v3.12.0

License: MIT

Copyright (c) 2013-2025 Niels Lohmann

Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the "Software"), to deal in
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
the Software, and to permit persons to whom the Software is furnished to do so,
subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

## Catch2 3.8.1 — build and test dependency only

Source: https://github.com/catchorg/Catch2/releases/tag/v3.8.1

License: Boost Software License 1.0

Permission is hereby granted, free of charge, to any person or organization
obtaining a copy of the software and accompanying documentation covered by this
license (the "Software") to use, reproduce, display, distribute, execute, and
transmit the Software, and to prepare derivative works of the Software, and to
permit third-parties to whom the Software is furnished to do so, all subject to
the following:

The copyright notices in the Software and this entire statement, including the
above license grant, this restriction and the following disclaimer, must be
included in all copies of the Software, in whole or in part, and all derivative
works of the Software, unless such copies or derivative works are solely in the
form of machine-executable object code generated by a source language
processor.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE, TITLE AND NON-INFRINGEMENT. IN NO EVENT SHALL
THE COPYRIGHT HOLDERS OR ANYONE DISTRIBUTING THE SOFTWARE BE LIABLE FOR ANY
DAMAGES OR OTHER LIABILITY, WHETHER IN CONTRACT, TORT OR OTHERWISE, ARISING
FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
