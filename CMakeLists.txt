# ZComms
#
# Phase 0 plus the audio engine core. This is not a Phase 1 app tree: there is
# no shell, no UI, no packaging. What lives here is the engine (plan §6.1-6.3),
# the Zoom binding for it, and the Spike A harness that seeded both.
cmake_minimum_required(VERSION 3.20)
project(zcomms LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(ZOOM_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/zoom-sdk")
set(MINIAUDIO_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/miniaudio")

# ---------------------------------------------------------------------------
# zcomms_audio -- the engine. Knows nothing about Zoom.
#
# The Zoom binding is a separate target on purpose. Everything here is needed
# on every phase path, including Phase 3's native fabric where Zoom is only one
# leg; keeping the dependency one-directional means the engine never has to be
# untangled from a transport later.
# ---------------------------------------------------------------------------
add_library(zcomms_audio STATIC
  src/audio/frame_ring.cpp
  src/audio/tx_pacer.cpp
  src/audio/envelope.cpp
  src/audio/limiter.cpp
  src/audio/frame_accumulator.cpp
  src/audio/devices.cpp
  src/audio/wav_sink.cpp
  src/audio/engine.cpp
  src/audio/miniaudio_impl.cpp
)
target_include_directories(zcomms_audio PUBLIC src/audio "${MINIAUDIO_DIR}")

# PUBLIC, and set here rather than in a source file: these macros change the
# layout of ma_device and ma_device_config, so a translation unit that sees
# miniaudio.h without them disagrees with the implementation about struct
# sizes. That corrupts memory instead of failing to build.
target_compile_definitions(zcomms_audio PUBLIC
  MA_NO_DECODING MA_NO_ENCODING MA_NO_GENERATION MA_NO_ENGINE MA_NO_NODE_GRAPH)

if(MSVC)
  target_compile_definitions(zcomms_audio PUBLIC
    WIN32 _UNICODE UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
  # /W4 is PRIVATE: consumers include the Zoom SDK headers, which are not
  # warning-clean, and must not be forced to choose between silencing real
  # warnings and drowning in vendor ones.
  target_compile_options(zcomms_audio PRIVATE /W4 /permissive- /utf-8)
  set_source_files_properties(src/audio/miniaudio_impl.cpp PROPERTIES
    COMPILE_OPTIONS "/W0")
endif()

# ---------------------------------------------------------------------------
# zcomms_zoom -- the Meeting SDK binding. Optional: the SDK is not
# redistributable and is gitignored (plan §3.5), so a fresh clone has none.
# ---------------------------------------------------------------------------
if(EXISTS "${ZOOM_SDK_DIR}/lib/sdk.lib")
  add_library(zcomms_zoom STATIC src/audio/mic_source.cpp)
  target_link_libraries(zcomms_zoom PUBLIC zcomms_audio "${ZOOM_SDK_DIR}/lib/sdk.lib")
  target_include_directories(zcomms_zoom PUBLIC "${ZOOM_SDK_DIR}/h")
  if(MSVC)
    target_compile_options(zcomms_zoom PRIVATE /W3 /utf-8)
  endif()
  set(ZCOMMS_HAVE_SDK TRUE)
else()
  set(ZCOMMS_HAVE_SDK FALSE)
  message(STATUS
    "Zoom SDK not found at ${ZOOM_SDK_DIR} -- building the engine only.\n"
    "   The SDK is not redistributable and is fetched separately (plan §3.5).")
endif()

# ---------------------------------------------------------------------------
# Engine CLI. Drives the engine with no UI and no meeting.
# ---------------------------------------------------------------------------
add_executable(zcomms-engine tools/engine-cli/main.cpp)
target_link_libraries(zcomms-engine PRIVATE zcomms_audio)
if(MSVC)
  target_compile_options(zcomms-engine PRIVATE /W4 /permissive- /utf-8)
  target_link_libraries(zcomms-engine PRIVATE winmm)
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
enable_testing()
add_executable(zcomms_audio_tests
  tests/audio/test_main.cpp
  tests/audio/test_envelope.cpp
  tests/audio/test_limiter.cpp
  tests/audio/test_frame_accumulator.cpp
  tests/audio/test_sample_ring.cpp
  tests/audio/test_frame_ring.cpp
)
target_link_libraries(zcomms_audio_tests PRIVATE zcomms_audio)
target_include_directories(zcomms_audio_tests PRIVATE tests/audio)
if(MSVC)
  target_compile_options(zcomms_audio_tests PRIVATE /W4 /permissive- /utf-8)
endif()
add_test(NAME zcomms_audio_tests COMMAND zcomms_audio_tests)

add_subdirectory(spikes/a-tx-latency)
