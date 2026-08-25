# Spike A -- TX latency harness (plan §9).
#
# Self-contained on purpose. This is Phase 0 work and must not imply a project
# layout for Phase 1, so it configures standalone from this directory rather
# than hanging off a root CMakeLists that does not exist yet.
cmake_minimum_required(VERSION 3.20)
project(zcomms_spike_a LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(ZCOMMS_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")
set(ZOOM_SDK_DIR "${ZCOMMS_REPO_ROOT}/third_party/zoom-sdk")
set(MINIAUDIO_DIR "${ZCOMMS_REPO_ROOT}/third_party/miniaudio")

# ---------------------------------------------------------------------------
# Core: everything that is pure computation.
#
# Deliberately depends on neither the Zoom SDK nor an audio device, which is
# what lets the test binary exercise the measurement chain -- signal,
# correlation, timebase, pacing, statistics -- on a machine with no SDK, no
# meeting and no sound card. The confidence in the final number rests on these
# being testable in isolation.
# ---------------------------------------------------------------------------
add_library(spike_a_core STATIC
  src/signal.cpp
  src/correlator.cpp
  src/stats.cpp
  src/timebase.cpp
  src/frame_ring.cpp
  src/tx_pacer.cpp
  src/generator.cpp
  src/probe.cpp
)
target_include_directories(spike_a_core PUBLIC src)

if(MSVC)
  # /W4 is PRIVATE: the Zoom SDK headers are not warning-clean at /W4 and the
  # harness must not be forced to either silence real warnings or drown in
  # vendor ones.
  target_compile_options(spike_a_core PRIVATE /W4 /permissive- /utf-8)
  target_compile_definitions(spike_a_core PUBLIC
    WIN32 _UNICODE UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
add_executable(spike_a_tests
  tests/test_main.cpp
  tests/test_correlator.cpp
  tests/test_stats.cpp
  tests/test_frame_ring.cpp
  tests/test_timebase.cpp
  tests/test_pipeline.cpp
)
target_link_libraries(spike_a_tests PRIVATE spike_a_core)
target_include_directories(spike_a_tests PRIVATE tests)
if(MSVC)
  target_compile_options(spike_a_tests PRIVATE /W4 /permissive- /utf-8)
endif()

enable_testing()
add_test(NAME spike_a_tests COMMAND spike_a_tests)

# ---------------------------------------------------------------------------
# The harness itself. Needs the SDK, which is gitignored and may be absent.
# Absent is a normal state for a fresh clone (plan §3.5), so it degrades to
# "core and tests only" with a clear message rather than failing to configure.
# ---------------------------------------------------------------------------
if(EXISTS "${ZOOM_SDK_DIR}/lib/sdk.lib" AND EXISTS "${MINIAUDIO_DIR}/miniaudio.h")
  add_executable(zcomms_spike_a
    src/main.cpp
    src/loopback.cpp
    src/zoom_client.cpp
    src/mic_source.cpp
    src/config.cpp
    src/jwt.cpp
    src/miniaudio_impl.cpp
  )
  target_link_libraries(zcomms_spike_a PRIVATE
    spike_a_core "${ZOOM_SDK_DIR}/lib/sdk.lib" winmm bcrypt)
  target_include_directories(zcomms_spike_a PRIVATE "${ZOOM_SDK_DIR}/h" "${MINIAUDIO_DIR}")

  # Set on the target, never in one source file. These macros change the layout
  # of ma_device/ma_device_config, so a translation unit that sees the header
  # without them disagrees with the implementation about struct sizes, which
  # corrupts memory instead of failing to build.
  #
  # Loopback capture is WASAPI-only. That is also why plan §6.3's preference
  # for miniaudio over Qt Multimedia is load-bearing here and not merely
  # stylistic: Qt has no loopback path at all, and loopback is how this spike
  # observes the far end.
  target_compile_definitions(zcomms_spike_a PRIVATE
    MA_NO_DECODING MA_NO_ENCODING MA_NO_GENERATION MA_NO_ENGINE MA_NO_NODE_GRAPH)
  if(MSVC)
    # /W3 rather than /W4: the SDK headers are included here. No /permissive-
    # for the same reason.
    target_compile_options(zcomms_spike_a PRIVATE /W3 /utf-8)
    # miniaudio's implementation unit is vendor code compiled once; its
    # warnings are noise we cannot act on.
    set_source_files_properties(src/miniaudio_impl.cpp PROPERTIES
      COMPILE_OPTIONS "/W0")
  endif()

  # The SDK's import library resolves sdk.dll at process start, before any code
  # of ours runs, so SetDllDirectory() would be too late. The runtime is copied
  # beside the executable instead. copy_directory_if_different keeps repeat
  # builds cheap despite the tree being ~300 MB.
  add_custom_command(TARGET zcomms_spike_a POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different
            "${ZOOM_SDK_DIR}/bin" "$<TARGET_FILE_DIR:zcomms_spike_a>"
    COMMENT "Staging Zoom SDK runtime beside the harness")
else()
  message(STATUS
    "Zoom SDK or miniaudio not found -- building core and tests only.\n"
    "   Expected: ${ZOOM_SDK_DIR}/lib/sdk.lib\n"
    "             ${MINIAUDIO_DIR}/miniaudio.h\n"
    "   The SDK is not redistributable and is gitignored (plan §3.5).")
endif()
