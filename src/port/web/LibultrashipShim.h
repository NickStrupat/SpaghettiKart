#ifndef WEB_LIBULTRASHIP_SHIM_H
#define WEB_LIBULTRASHIP_SHIM_H

/**
 * Force-included (see cmake/dependencies/common.cmake) into every libultraship file of the browser build: what
 * libultraship needs to compile with Emscripten, kept here so that the submodule stays as upstream has it.
 */

// StringHelper.cpp and DisplayList.cpp use strtol() and free() without including this; other platforms' headers
// happen to pull it in.
#ifdef __cplusplus
#include <cstdlib>
#else
#include <stdlib.h>
#endif

// The OpenGL ES path includes no GLEW but still calls this outside Linux. WebGL has nothing to set up.
#define glewInit() ((void) 0)

#endif // WEB_LIBULTRASHIP_SHIM_H
