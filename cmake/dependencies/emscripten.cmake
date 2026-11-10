# Libraries of the browser build (docs/WEB.md) that libultraship finds installed on other platforms. Emscripten has
# none installed, so they are fetched and built with the game; OVERRIDE_FIND_PACKAGE makes libultraship's
# find_package() calls use them. Versions follow libultraship/cmake/dependencies/android.cmake.
include(FetchContent)

FetchContent_Declare(
  nlohmann_json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG v3.12.0
  OVERRIDE_FIND_PACKAGE)
FetchContent_MakeAvailable(nlohmann_json)

set(tinyxml2_BUILD_TESTING OFF)
FetchContent_Declare(
  tinyxml2
  GIT_REPOSITORY https://github.com/leethomason/tinyxml2.git
  GIT_TAG 11.0.0
  OVERRIDE_FIND_PACKAGE)
FetchContent_MakeAvailable(tinyxml2)

FetchContent_Declare(
  spdlog
  GIT_REPOSITORY https://github.com/gabime/spdlog.git
  GIT_TAG v1.16.0
  OVERRIDE_FIND_PACKAGE)
FetchContent_MakeAvailable(spdlog)

# libzip reads the game's .o2r archives, which only use deflate: no other compression or encryption library.
set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
set(BUILD_TOOLS OFF)
set(BUILD_REGRESS OFF)
set(BUILD_EXAMPLES OFF)
set(BUILD_DOC OFF)
set(BUILD_OSSFUZZ OFF)
set(BUILD_SHARED_LIBS OFF)
set(ENABLE_BZIP2 OFF)
set(ENABLE_LZMA OFF)
set(ENABLE_ZSTD OFF)
set(ENABLE_COMMONCRYPTO OFF)
set(ENABLE_GNUTLS OFF)
set(ENABLE_MBEDTLS OFF)
set(ENABLE_OPENSSL OFF)
set(ENABLE_WINDOWS_CRYPTO OFF)
set(LIBZIP_DO_INSTALL OFF)
FetchContent_Declare(
  libzip
  GIT_REPOSITORY https://github.com/nih-at/libzip.git
  GIT_TAG v1.11.4
  OVERRIDE_FIND_PACKAGE)
FetchContent_MakeAvailable(libzip)
