# zlib for the browser build: Emscripten's port. The -sUSE_ZLIB=1 flag that every target is built with
# (WebFlags.cmake) provides the header and the library, so the target has nothing to add.
if(NOT TARGET ZLIB::ZLIB)
  add_library(ZLIB::ZLIB INTERFACE IMPORTED)
endif()
set(ZLIB_FOUND TRUE)
set(ZLIB_VERSION "1.3.1")
set(ZLIB_VERSION_STRING "${ZLIB_VERSION}")
set(ZLIB_INCLUDE_DIRS "")
set(ZLIB_LIBRARIES ZLIB::ZLIB)
