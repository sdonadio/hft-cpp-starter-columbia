# ─────────────────────────────────────────────────────────────────────────────
# AlgoArena — HFT C++ trading-bot client
#
# Builds a single executable, `hft_bot`, that connects to the EXISTING Python
# exchange over the same JSON-over-WebSocket protocol the Python bots use.
#
# Dependencies are pulled with FetchContent — no system packages required:
#   - IXWebSocket   (WebSocket client)          https://github.com/machinezone/IXWebSocket
#   - nlohmann/json (header-only JSON)          https://github.com/nlohmann/json
#
# Build:
#   cmake -B build
#   cmake --build build
#
# Run (see README.md for the full env-var list):
#   TEAM_ID=hft_alpha EXCHANGE_HOST=localhost EXCHANGE_PORT=8765 ./build/hft_bot
# ─────────────────────────────────────────────────────────────────────────────

cmake_minimum_required(VERSION 3.16)
project(algoarena_hft_client LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# TLS is only needed for hosted (wss://) play. Local play uses ws:// and needs
# nothing extra, so TLS is OFF by default to keep the build dependency-free.
# Turn it on with:  cmake -B build -DHFT_USE_TLS=ON
option(HFT_USE_TLS "Build IXWebSocket with TLS support (needed for wss://)" OFF)

include(FetchContent)

# ── nlohmann/json (header-only) ──────────────────────────────────────────────
FetchContent_Declare(
  nlohmann_json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG        v3.11.3
  GIT_SHALLOW    TRUE
)

# ── IXWebSocket ──────────────────────────────────────────────────────────────
if(HFT_USE_TLS)
  # Use the platform-native TLS backend so no extra system package is required:
  # Secure Transport on macOS, OpenSSL elsewhere.
  if(APPLE)
    set(USE_SECURE_TRANSPORT ON CACHE BOOL "" FORCE)
  else()
    set(USE_OPEN_SSL ON CACHE BOOL "" FORCE)
  endif()
  set(USE_TLS ON CACHE BOOL "" FORCE)
endif()

FetchContent_Declare(
  ixwebsocket
  GIT_REPOSITORY https://github.com/machinezone/IXWebSocket.git
  GIT_TAG        v11.4.5
  GIT_SHALLOW    TRUE
)

FetchContent_MakeAvailable(nlohmann_json ixwebsocket)

# ── Executable ───────────────────────────────────────────────────────────────
add_executable(hft_bot
  src/main.cpp
  src/arena_client.cpp
)

target_include_directories(hft_bot PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(hft_bot PRIVATE
  ixwebsocket
  nlohmann_json::nlohmann_json
)

# Threads (IXWebSocket runs its receive loop on its own thread).
find_package(Threads REQUIRED)
target_link_libraries(hft_bot PRIVATE Threads::Threads)

if(MSVC)
  target_compile_options(hft_bot PRIVATE /W4)
else()
  target_compile_options(hft_bot PRIVATE -Wall -Wextra)
endif()
