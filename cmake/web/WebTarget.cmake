# How the browser build (docs/WEB.md) is linked and what goes next to it. The result is build-web/web/: index.html,
# its .js and .wasm, and the game's two archives, ready for tools/web-serve.py.
set(WEB_DIR ${CMAKE_BINARY_DIR}/web)
set_target_properties(${PROJECT_NAME} PROPERTIES
  OUTPUT_NAME "index"
  SUFFIX ".html"
  RUNTIME_OUTPUT_DIRECTORY ${WEB_DIR})

target_link_options(${PROJECT_NAME} PRIVATE
  --shell-file ${CMAKE_SOURCE_DIR}/src/port/web/shell.html
  # WebGL 2 is OpenGL ES 3; FULL_ES3 adds the calls WebGL lacks, such as mapping a buffer into memory.
  -sMIN_WEBGL_VERSION=2
  -sMAX_WEBGL_VERSION=2
  -sFULL_ES3=1
  # libultraship takes a texture address below 0x10000000 for an N64 address that no segment resolved and skips it
  # (gfx_set_timg_handler_rdp). Other platforms' pointers are far above that; here all of the game's memory starts
  # there instead of at 0, leaving the first 256 MB unused. Untouched memory costs nothing.
  -sGLOBAL_BASE=256MB
  # The unused start, the game's 256 MB memory pool and room for the loaded archives; the rest grows on demand.
  -sINITIAL_MEMORY=1GB
  -sALLOW_MEMORY_GROWTH=1
  -sMAXIMUM_MEMORY=2GB
  # Emscripten's default stack is 64 KB; the game's frames are deeper than that.
  -sSTACK_SIZE=8MB
  -sDEFAULT_PTHREAD_STACK_SIZE=2MB
  # A thread only starts once the page's main thread goes back to the browser, which the game's start-up does not
  # do: the workers that threads run on are made before main(). libultraship's resource pool takes one per
  # processor; the sound thread and the logger take the rest.
  -sPTHREAD_POOL_SIZE=navigator.hardwareConcurrency+4
  # The page writes the archives into the file system and keeps settings and saves in the browser's storage.
  # Netplay: a WebSocket to the relay, WebRTC to the other players (src/port/netplay/RelayLinkWeb.cpp).
  -lwebsocket.js
  --js-library ${CMAKE_SOURCE_DIR}/src/port/web/WebRtcLink.js
  -sFORCE_FILESYSTEM=1
  -lidbfs.js
  -sEXPORTED_RUNTIME_METHODS=FS,IDBFS,ENV,addRunDependency,removeRunDependency
  -sEXIT_RUNTIME=0
  -sENVIRONMENT=web,worker)

# The page and the JavaScript library are inputs of the link: changing them relinks.
set_property(TARGET ${PROJECT_NAME} APPEND PROPERTY LINK_DEPENDS
  ${CMAKE_SOURCE_DIR}/src/port/web/shell.html ${CMAKE_SOURCE_DIR}/src/port/web/WebRtcLink.js)

target_link_options(${PROJECT_NAME} PRIVATE
  "$<$<CONFIG:Debug>:-sASSERTIONS=2;-gsource-map>"
  "$<$<CONFIG:RelWithDebInfo>:-sASSERTIONS=1>")

# Netplay rollback finds the simulation's globals by their section (src/port/netplay/SimSection.h), a name that
# clang's WebAssembly backend drops: the launcher puts it back into each object file.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(WEB_SIM_LAUNCHER ${Python3_EXECUTABLE} -I ${CMAKE_SOURCE_DIR}/tools/web/sim-segments.py --compile)
set_target_properties(${PROJECT_NAME} PROPERTIES
  C_COMPILER_LAUNCHER "${WEB_SIM_LAUNCHER}"
  CXX_COMPILER_LAUNCHER "${WEB_SIM_LAUNCHER}")

# The archives the page loads: made by a native build (see docs/BUILDING.md) and not part of the repository. Without
# mk64.o2r the page asks for the player's ROM and makes it.
foreach(ARCHIVE mk64.o2r spaghetti.o2r)
  if(EXISTS ${CMAKE_SOURCE_DIR}/${ARCHIVE})
    add_custom_command(
      TARGET ${PROJECT_NAME} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/${ARCHIVE} ${WEB_DIR}/${ARCHIVE}
      VERBATIM)
  elseif(ARCHIVE STREQUAL "spaghetti.o2r")
    message(WARNING "${ARCHIVE} is missing from ${CMAKE_SOURCE_DIR}: copy it to ${WEB_DIR} before serving the page")
  endif()
endforeach()

# The archive maker (tools/web/extractor): Torch on its own, for a page that has to make mk64.o2r from the player's
# ROM. Built as a separate project with this one's Emscripten, and run by ExtractWorker.js.
include(ExternalProject)
ExternalProject_Add(WebExtractor
  SOURCE_DIR ${CMAKE_SOURCE_DIR}/tools/web/extractor
  BINARY_DIR ${CMAKE_BINARY_DIR}/extractor
  CMAKE_ARGS -DCMAKE_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE} -DCMAKE_BUILD_TYPE=Release
             -DSK_SOURCE_DIR=${CMAKE_SOURCE_DIR}
  BUILD_ALWAYS ON
  # Its libraries' own install rules would put their headers here too: only the three files go next to the page.
  INSTALL_COMMAND ${CMAKE_COMMAND} -E copy_if_different <BINARY_DIR>/extractor.js <BINARY_DIR>/extractor.wasm
                  <BINARY_DIR>/extractor.data ${WEB_DIR})
add_dependencies(${PROJECT_NAME} WebExtractor)
add_custom_command(
  TARGET ${PROJECT_NAME} POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/src/port/web/ExtractWorker.js ${WEB_DIR}/
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/src/port/web/manifest.webmanifest ${WEB_DIR}/
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/icon.png ${WEB_DIR}/
  VERBATIM)
