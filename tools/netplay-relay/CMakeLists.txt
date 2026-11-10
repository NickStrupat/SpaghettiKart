# Netplay relay server. A standalone project, so it builds without the game, its assets or a ROM:
#   cmake -S tools/netplay-relay -B build-relay && cmake --build build-relay
cmake_minimum_required(VERSION 3.16)
project(netplay-relay CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

include(FetchContent)
FetchContent_Declare(
  asio
  GIT_REPOSITORY https://github.com/chriskohlhoff/asio.git
  GIT_TAG asio-1-30-2
  GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(asio)

find_package(Threads REQUIRED)

add_executable(netplay-relay main.cpp RelayProtocol.h WebSocket.h)
target_include_directories(netplay-relay PRIVATE ${asio_SOURCE_DIR}/asio/include)
target_compile_definitions(netplay-relay PRIVATE ASIO_STANDALONE)
target_link_libraries(netplay-relay PRIVATE Threads::Threads)
if(WIN32)
  target_compile_definitions(netplay-relay PRIVATE _WIN32_WINNT=0x0A00)
  target_link_libraries(netplay-relay PRIVATE ws2_32 mswsock)
endif()

install(TARGETS netplay-relay RUNTIME DESTINATION bin)
