# =================== SSE2NEON ===================
set(SSE2NEON_DIR ${CMAKE_BINARY_DIR}/_deps/sse2neon)
file(
  DOWNLOAD
  "https://raw.githubusercontent.com/DLTcollab/sse2neon/refs/heads/master/sse2neon.h"
  "${SSE2NEON_DIR}/sse2neon.h")

target_include_directories(${PROJECT_NAME} PRIVATE ${SSE2NEON_DIR})

# ================== SEMVER ===================
set(SEMVER_DIR ${CMAKE_BINARY_DIR}/_deps/semver)
file(
  DOWNLOAD
  "https://raw.githubusercontent.com/Neargye/semver/refs/tags/v1.0.0-rc/include/semver.hpp"
  "${SEMVER_DIR}/semver.hpp")

target_include_directories(${PROJECT_NAME} PRIVATE ${SEMVER_DIR})

# =================== DRLibs ===================
FetchContent_Declare(
  dr_libs
  GIT_REPOSITORY https://github.com/mackron/dr_libs.git
  GIT_TAG da35f9d6c7374a95353fd1df1d394d44ab66cf01)
FetchContent_MakeAvailable(dr_libs)

target_include_directories(${PROJECT_NAME} PRIVATE ${dr_libs_SOURCE_DIR})

# =================== tomlplusplus ===================
include(FetchContent)
FetchContent_Declare(
  tomlplusplus
  GIT_REPOSITORY https://github.com/marzer/tomlplusplus.git
  GIT_TAG v3.4.0)
FetchContent_MakeAvailable(tomlplusplus)
target_link_libraries(${PROJECT_NAME} PRIVATE tomlplusplus::tomlplusplus)

# =================== GekkoNet (netplay rollback) ===================
# Built from its sources here rather than through its own CMake project, which forces the static MSVC runtime.
FetchContent_Declare(
  gekkonet
  GIT_REPOSITORY https://github.com/HeatXD/GekkoNet.git
  GIT_TAG 9da2363d7c5a68cc77f8dfe2da7cf51d3afe3bee
  SOURCE_SUBDIR do-not-add-subdirectory)
FetchContent_MakeAvailable(gekkonet)
file(GLOB GEKKONET_SOURCES ${gekkonet_SOURCE_DIR}/GekkoLib/src/*.cpp)
add_library(GekkoNet STATIC ${GEKKONET_SOURCES})
target_include_directories(GekkoNet
  PUBLIC ${gekkonet_SOURCE_DIR}/GekkoLib/include
  PRIVATE ${gekkonet_SOURCE_DIR}/GekkoLib/include/private
          ${gekkonet_SOURCE_DIR}/GekkoLib/thirdparty
          ${gekkonet_SOURCE_DIR}/GekkoLib/thirdparty/asio)
target_compile_definitions(GekkoNet PUBLIC GEKKONET_STATIC)
target_compile_features(GekkoNet PRIVATE cxx_std_20)
if(WIN32)
  target_compile_definitions(GekkoNet PRIVATE _WIN32_WINNT=0x0A00)
  target_link_libraries(GekkoNet PRIVATE ws2_32 mswsock)
endif()
if(MSVC)
  # The same C runtime as the game (see CMakeLists.txt): mixing the static and DLL runtimes fails to link.
  set_property(TARGET GekkoNet PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
target_link_libraries(${PROJECT_NAME} PRIVATE GekkoNet)
# The relay client (src/port/netplay/RelayClient.cpp) uses GekkoNet's copy of standalone ASIO.
target_include_directories(${PROJECT_NAME} PRIVATE ${gekkonet_SOURCE_DIR}/GekkoLib/thirdparty/asio)
# Netplay desync recovery packs the host's state with zlib (src/port/netplay/Resync.cpp). The macOS SDK has it; the
# found package would add the include folder of whichever SDK it found, ahead of the C++ headers.
if(APPLE)
  target_link_libraries(${PROJECT_NAME} PRIVATE z)
else()
  find_package(ZLIB REQUIRED)
  target_link_libraries(${PROJECT_NAME} PRIVATE ZLIB::ZLIB)
endif()

# libultraship
# Removes MPQ/OTR support
set(EXCLUDE_MPQ_SUPPORT TRUE CACHE BOOL "")
set(ENABLE_EXP_AUTO_CONFIGURE_CONTROLLERS ON CACHE BOOL "")
target_compile_definitions(${PROJECT_NAME} PRIVATE EXCLUDE_MPQ_SUPPORT)

target_include_directories(
  ${PROJECT_NAME} PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/libultraship/include
  ${CMAKE_CURRENT_SOURCE_DIR}/libultraship/include/libultraship)

if(SPAGHETTIKART_WEB)
  include(cmake/dependencies/emscripten.cmake)
endif()
add_subdirectory(libultraship ${CMAKE_BINARY_DIR}/libultraship)
add_dependencies(${PROJECT_NAME} libultraship)
target_link_libraries(${PROJECT_NAME} PRIVATE libultraship)
if(SPAGHETTIKART_WEB)
  # libultraship's headers include libzip's, which other platforms find among the system's.
  target_link_libraries(${PROJECT_NAME} PRIVATE libzip::zip)
  target_compile_options(libultraship PRIVATE
                         "SHELL:-include ${CMAKE_SOURCE_DIR}/src/port/web/LibultrashipShim.h")
endif()

# Torch
option(USE_STANDALONE "Build as a standalone executable" OFF)
option(BUILD_STORMLIB "Build with StormLib support" OFF)

option(BUILD_SM64 "Build with Super Mario 64 support" OFF)
option(BUILD_MK64 "Build with Mario Kart 64 support" ON)
option(BUILD_SF64 "Build with Star Fox 64 support" OFF)
option(BUILD_FZERO "Build with F-Zero X support" OFF)
option(BUILD_MARIO_ARTIST "Build with Mario Artist support" OFF)
# Companion.cpp uses AudioManager unconditionally, so preserve Torch's ON
# default until its non-NAudio build is fixed upstream.
option(BUILD_NAUDIO "Build with NAudio support" ON)

# The browser build is given a finished mk64.o2r, so it has no use for Torch, which makes one from a ROM.
if(NOT SPAGHETTIKART_WEB)
  add_subdirectory(torch)
endif()
