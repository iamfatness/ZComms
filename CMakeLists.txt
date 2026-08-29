# ZComms
#
# Phase 0 plus the audio engine core. This is not a Phase 1 app tree: there is
# no shell, no UI, no packaging. What lives here is the engine (plan §6.1-6.3),
# the Zoom binding for it, and the Spike A harness that seeded both.
cmake_minimum_required(VERSION 3.20)
project(zcomms LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(ZOOM_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/zoom-sdk")
set(MINIAUDIO_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/miniaudio")

# ---------------------------------------------------------------------------
# speexdsp -- the acoustic echo canceller (plan §2: raw PCM into Zoom
# bypasses its AEC, so we carry our own). Vendored source, BSD-licensed,
# floating point, kiss FFT. Only the echo/preprocess units are built.
# ---------------------------------------------------------------------------
add_library(speexdsp STATIC
  third_party/speexdsp/libspeexdsp/mdf.c
  third_party/speexdsp/libspeexdsp/preprocess.c
  third_party/speexdsp/libspeexdsp/filterbank.c
  third_party/speexdsp/libspeexdsp/fftwrap.c
  third_party/speexdsp/libspeexdsp/kiss_fft.c
  third_party/speexdsp/libspeexdsp/kiss_fftr.c
)
target_include_directories(speexdsp PUBLIC third_party/speexdsp/include)
target_include_directories(speexdsp PRIVATE third_party/speexdsp/libspeexdsp)
target_compile_definitions(speexdsp PRIVATE
  FLOATING_POINT USE_KISS_FFT EXPORT= HAVE_STDINT_H)
if(MSVC)
  target_compile_options(speexdsp PRIVATE /W0 /utf-8)
  target_compile_definitions(speexdsp PRIVATE _USE_MATH_DEFINES _CRT_SECURE_NO_WARNINGS)
endif()

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
  src/audio/signal.cpp
  src/audio/correlator.cpp
  src/audio/generator.cpp
  src/audio/envelope.cpp
  src/audio/limiter.cpp
  src/audio/frame_accumulator.cpp
  src/audio/aec.cpp
  src/audio/devices.cpp
  src/audio/loopback.cpp
  src/audio/wav_sink.cpp
  src/audio/engine.cpp
  src/audio/miniaudio_impl.cpp
)
target_include_directories(zcomms_audio PUBLIC src/audio "${MINIAUDIO_DIR}")
target_link_libraries(zcomms_audio PUBLIC speexdsp)

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
  add_library(zcomms_zoom STATIC
    src/audio/mic_source.cpp
    src/zoom/zoom_client.cpp
    src/zoom/talkback_source.cpp
    src/zoom/talkback_channels.cpp
    src/zoom/roster.cpp
    src/zoom/breakout.cpp
    src/zoom/jwt.cpp
  )
  target_link_libraries(zcomms_zoom PUBLIC zcomms_audio "${ZOOM_SDK_DIR}/lib/sdk.lib")
  target_link_libraries(zcomms_zoom PRIVATE bcrypt)
  target_include_directories(zcomms_zoom PUBLIC "${ZOOM_SDK_DIR}/h" src/zoom)
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
# WebView2 -- the shell window's rendering engine. The panel is one HTML page;
# hosting it in a WebView2 window makes zcomms its own native window instead
# of a borrowed browser. SDK fetched at configure time from NuGet (loader is
# redistributable; nothing vendored, nothing committed).
# ---------------------------------------------------------------------------
set(WEBVIEW2_VERSION "1.0.2739.15")
set(WEBVIEW2_DIR "${CMAKE_BINARY_DIR}/webview2")
if(NOT EXISTS "${WEBVIEW2_DIR}/build/native/include/WebView2.h")
  message(STATUS "Fetching WebView2 SDK ${WEBVIEW2_VERSION}")
  file(DOWNLOAD
    "https://www.nuget.org/api/v2/package/Microsoft.Web.WebView2/${WEBVIEW2_VERSION}"
    "${CMAKE_BINARY_DIR}/webview2.nupkg" STATUS _wv2_dl)
  list(GET _wv2_dl 0 _wv2_rc)
  if(_wv2_rc EQUAL 0)
    file(ARCHIVE_EXTRACT INPUT "${CMAKE_BINARY_DIR}/webview2.nupkg"
         DESTINATION "${WEBVIEW2_DIR}")
  else()
    message(WARNING "WebView2 SDK download failed -- shell falls back to the browser window")
  endif()
endif()
if(EXISTS "${WEBVIEW2_DIR}/build/native/include/WebView2.h")
  set(ZCOMMS_HAVE_WEBVIEW2 TRUE)
else()
  set(ZCOMMS_HAVE_WEBVIEW2 FALSE)
endif()

# ---------------------------------------------------------------------------
# zcomms -- the talkback panel itself. Phase 1's first vertical slice.
# ---------------------------------------------------------------------------
if(ZCOMMS_HAVE_SDK)
  add_executable(zcomms src/app/main.cpp src/app/control_server.cpp
                        src/app/zoom_oauth.cpp src/app/version.rc)
  target_link_libraries(zcomms PRIVATE zcomms_zoom winmm ws2_32 shell32
                        winhttp crypt32 bcrypt)
  if(ZCOMMS_HAVE_WEBVIEW2)
    target_sources(zcomms PRIVATE src/app/shell_window.cpp)
    target_include_directories(zcomms PRIVATE
      "${WEBVIEW2_DIR}/build/native/include")
    target_link_libraries(zcomms PRIVATE
      "${WEBVIEW2_DIR}/build/native/x64/WebView2Loader.dll.lib")
    target_compile_definitions(zcomms PRIVATE ZCOMMS_HAVE_WEBVIEW2)
    add_custom_command(TARGET zcomms POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
              "${WEBVIEW2_DIR}/runtimes/win-x64/native/WebView2Loader.dll"
              "$<TARGET_FILE_DIR:zcomms>"
      COMMENT "Staging WebView2 loader beside zcomms")
  endif()
  if(MSVC)
    # /W3, SDK headers included transitively.
    target_compile_options(zcomms PRIVATE /W3 /utf-8)
  endif()
  add_custom_command(TARGET zcomms POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different
            "${ZOOM_SDK_DIR}/bin" "$<TARGET_FILE_DIR:zcomms>"
    COMMENT "Staging Zoom SDK runtime beside zcomms")
endif()

# ---------------------------------------------------------------------------
# zcomms-tap: end-to-end verifier's far end. Taps every playback endpoint and
# detects the --test-signal beep pattern. No SDK needed.
# ---------------------------------------------------------------------------
add_executable(zcomms-tap tools/tap-detect/main.cpp)
target_link_libraries(zcomms-tap PRIVATE zcomms_audio)
if(MSVC)
  target_compile_options(zcomms-tap PRIVATE /W4 /permissive- /utf-8)
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
  tests/audio/test_aec.cpp
)
target_link_libraries(zcomms_audio_tests PRIVATE zcomms_audio)
target_include_directories(zcomms_audio_tests PRIVATE tests/audio)
if(MSVC)
  target_compile_options(zcomms_audio_tests PRIVATE /W4 /permissive- /utf-8)
endif()
add_test(NAME zcomms_audio_tests COMMAND zcomms_audio_tests)

add_subdirectory(spikes/a-tx-latency)
