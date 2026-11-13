# The browser build's archive maker (docs/WEB.md): Torch compiled to WebAssembly, which turns the player's Mario Kart
# 64 ROM into mk64.o2r in their browser, as the desktop game does on its first start (src/port/GameExtractor.cpp).
#
# A project of its own, built by cmake/web/WebTarget.cmake as an external project: Torch needs none of the game's
# threads or flags, and the page loads it only on the visit that has to make the archive. The result is
# extractor.js, extractor.wasm and extractor.data (Torch's asset descriptions: config.yml, yamls/ and what they name).
cmake_minimum_required(VERSION 3.20)
project(Mk64Extractor C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT SK_SOURCE_DIR)
  message(FATAL_ERROR "SK_SOURCE_DIR must name the SpaghettiKart repository")
endif()

# Torch's exceptions report a ROM it cannot read; Emscripten turns exceptions off otherwise. Torch starts no threads,
# but its spdlog will not configure without them; the page is cross-origin isolated anyway.
add_compile_options(-fwasm-exceptions -pthread)
add_link_options(-fwasm-exceptions -pthread)
# CMake's check for threads fails inside Torch's folder, whose link flags are for its JavaScript API (embind) and
# cannot link a test program in C. With -pthread the C library has them: the answer, given in advance.
set(CMAKE_HAVE_LIBC_PTHREAD 1 CACHE INTERNAL "")
# Torch is a submodule this fork does not own; some of its files use std::strtoull without including <cstdlib>, which
# other platforms' headers happen to include for them.
add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:SHELL:-include cstdlib>")

# Torch as a library, with only what Mario Kart 64 needs, as in cmake/dependencies/common.cmake.
set(USE_STANDALONE OFF CACHE BOOL "" FORCE)
set(BUILD_STORMLIB OFF CACHE BOOL "" FORCE)
set(BUILD_SM64 OFF CACHE BOOL "" FORCE)
set(BUILD_MK64 ON CACHE BOOL "" FORCE)
set(BUILD_SF64 OFF CACHE BOOL "" FORCE)
set(BUILD_FZERO OFF CACHE BOOL "" FORCE)
set(BUILD_MARIO_ARTIST OFF CACHE BOOL "" FORCE)
set(BUILD_PM64 OFF CACHE BOOL "" FORCE)
set(BUILD_BK64 OFF CACHE BOOL "" FORCE)
set(BUILD_OOT OFF CACHE BOOL "" FORCE)
set(BUILD_NAUDIO ON CACHE BOOL "" FORCE)
add_subdirectory(${SK_SOURCE_DIR}/torch torch)

add_executable(extractor Extractor.cpp)
target_link_libraries(extractor PRIVATE torch)
# The version it stamps into the archive (src/port/AssetsVersion.h), as the game's build computes it.
add_custom_target(AssetsVersion
  COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${SK_SOURCE_DIR} -DOUTPUT=${CMAKE_BINARY_DIR}/assets/AssetsVersionDefine.h
          -P ${SK_SOURCE_DIR}/cmake/AssetsVersion.cmake
  BYPRODUCTS ${CMAKE_BINARY_DIR}/assets/AssetsVersionDefine.h)
add_dependencies(extractor AssetsVersion)
target_include_directories(extractor PRIVATE ${CMAKE_BINARY_DIR}/assets)
target_link_options(extractor PRIVATE
  # Under Emscripten, Torch defines Companion::Instance in src/lib/web.cpp, next to its JavaScript API: using it links
  # that file, whose API needs embind's runtime.
  -lembind
  -sMODULARIZE=1
  -sEXPORT_ES6=1
  -sEXPORT_NAME=createExtractor
  -sENVIRONMENT=worker
  -sALLOW_MEMORY_GROWTH=1
  -sINVOKE_RUN=0
  -sEXIT_RUNTIME=0
  -sEXPORTED_RUNTIME_METHODS=FS,callMain
  # Torch reads its asset descriptions from a source folder: they are put under /torch. (SHELL: keeps CMake from
  # merging the repeated option.)
  "SHELL:--preload-file ${SK_SOURCE_DIR}/config.yml@/torch/config.yml"
  "SHELL:--preload-file ${SK_SOURCE_DIR}/yamls@/torch/yamls"
  "SHELL:--preload-file ${SK_SOURCE_DIR}/include/defines.h@/torch/include/defines.h"
  "SHELL:--preload-file ${SK_SOURCE_DIR}/meta/mods.toml@/torch/meta/mods.toml")
# The descriptions are inputs of the link: changing them relinks.
file(GLOB_RECURSE TORCH_DESCRIPTIONS CONFIGURE_DEPENDS ${SK_SOURCE_DIR}/yamls/*)
set_property(TARGET extractor APPEND PROPERTY LINK_DEPENDS ${TORCH_DESCRIPTIONS} ${SK_SOURCE_DIR}/config.yml
  ${SK_SOURCE_DIR}/include/defines.h ${SK_SOURCE_DIR}/meta/mods.toml)
