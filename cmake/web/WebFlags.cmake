# Flags of the browser build (docs/WEB.md), for every target: the libraries fetched below must be built the same way
# as the game, or they do not link.
#
# - Threads: libultraship loads resources on a thread pool and the game mixes sound on its own thread. A page with
#   threads needs the "cross-origin isolation" headers (tools/web-serve.py sends them).
# - C++ exceptions as WebAssembly exceptions: Emscripten turns exceptions off otherwise.
# - SDL2 and zlib come from Emscripten's ports, built on first use.
set(WEB_COMMON_FLAGS -pthread -fwasm-exceptions -sUSE_SDL=2 -sUSE_ZLIB=1)
add_compile_options(${WEB_COMMON_FLAGS})
add_link_options(${WEB_COMMON_FLAGS})

# FindZLIB.cmake here stands in for CMake's own, which does not know about the port.
list(PREPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}")

# libultraship draws through its OpenGL ES 3 path, which is WebGL 2 in a browser.
set(USE_OPENGLES ON CACHE BOOL "Use OpenGLES instead of Desktop OpenGL" FORCE)
