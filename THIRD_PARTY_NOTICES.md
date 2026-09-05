# Third-Party Notices

## GLFW 3.4 — altered source

Airelinna builds a patched copy of GLFW 3.4 from the official source release.
The patch removes `CS_OWNDC` from the main Win32 window class because Airelinna's
replica windows use Vulkan rather than an OpenGL device context. The GLFW helper
window class is not changed. The exact alteration is recorded in
`third_party/patches/glfw-3.4-win32-no-own-dc.patch`.

Project: GLFW

Source: https://github.com/glfw/glfw/releases/tag/3.4

License: zlib/libpng

Copyright (c) 2002-2006 Marcus Geelnard

Copyright (c) 2006-2019 Camilla Löwy

This software is provided 'as-is', without any express or implied
warranty. In no event will the authors be held liable for any damages
arising from the use of this software.

Permission is granted to anyone to use this software for any purpose,
including commercial applications, and to alter it and redistribute it
freely, subject to the following restrictions:

1. The origin of this software must not be misrepresented; you must not
   claim that you wrote the original software. If you use this software
   in a product, an acknowledgment in the product documentation would
   be appreciated but is not required.

2. Altered source versions must be plainly marked as such, and must not
   be misrepresented as being the original software.

3. This notice may not be removed or altered from any source
   distribution.
