# Spike A -- TX latency harness (plan §9).
#
# Phase 0. Configured from the root CMakeLists, and consumes the shared engine
# targets rather than carrying its own copies -- plan §9 said this harness
# seeds §6.1 rather than being throwaway, and the pacer, the frame ring and the
# virtual mic have since moved into src/audio where the product uses them.
#
# What remains here is genuinely spike-only: the probe signal, the correlator,
# the capture timebase and the measurement itself.

# ---------------------------------------------------------------------------
# Measurement core: pure computation, no SDK and no audio device, which is what
# lets the tests exercise the whole signal/correlation/timebase chain on a
# machine with no meeting and no sound card.
# ---------------------------------------------------------------------------
# signal, correlator and generator promoted to src/audio (zcomms_audio).
add_library(spike_a_core STATIC
  src/stats.cpp
  src/timebase.cpp
  src/probe.cpp
)
target_include_directories(spike_a_core PUBLIC src)
target_link_libraries(spike_a_core PUBLIC zcomms_audio)
if(MSVC)
  target_compile_options(spike_a_core PRIVATE /W4 /permissive- /utf-8)
endif()

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
add_test(NAME spike_a_tests COMMAND spike_a_tests)

# ---------------------------------------------------------------------------
# The harness itself. Needs the SDK, which is gitignored and may be absent --
# a normal state for a fresh clone, not a failure.
#
# NOT APPLE: this links zoom_client/talkback_source/jwt (Windows-only, see
# the root CMakeLists) plus winmm/bcrypt directly, so it cannot build on
# Apple even now that ZCOMMS_HAVE_SDK can be TRUE there.
# ---------------------------------------------------------------------------
if(ZCOMMS_HAVE_SDK AND NOT APPLE)
  # zoom_client, talkback_source and jwt were promoted to src/zoom (the
  # product's zcomms_zoom library); the spike consumes them from there.
  # loopback promoted to src/audio alongside the rest of the device layer.
  add_executable(zcomms_spike_a
    src/main.cpp
    src/config.cpp
  )
  target_link_libraries(zcomms_spike_a PRIVATE spike_a_core zcomms_zoom winmm bcrypt)
  if(MSVC)
    # /W3 and no /permissive-: the Zoom SDK headers are included here.
    target_compile_options(zcomms_spike_a PRIVATE /W3 /utf-8)
  endif()

  # The SDK's import library resolves sdk.dll at process start, before any of
  # our code runs, so SetDllDirectory() would be too late. The runtime is
  # staged beside the executable instead; copy_directory_if_different keeps
  # repeat builds cheap despite the tree being ~300 MB.
  add_custom_command(TARGET zcomms_spike_a POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different
            "${ZOOM_SDK_DIR}/bin" "$<TARGET_FILE_DIR:zcomms_spike_a>"
    COMMENT "Staging Zoom SDK runtime beside the harness")
endif()
