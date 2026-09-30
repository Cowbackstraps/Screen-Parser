# Host-side offline build for the screen parser unit tests.
#
# This compiles ONLY the pure-logic modules (no OpenHarmony / MindSpore /
# window dependencies) against a minimal gtest shim, so the normalization,
# tokenizer, JSON and preprocessing logic can be verified on a dev machine:
#
#   cmake -S . -B build -G Ninja
#   cmake --build build
#   ctest --test-dir build --output-on-failure   (or run ./build/screenparser_tests)
#
# Inside the OpenHarmony source tree, test/BUILD.gn is used instead (real gtest).

cmake_minimum_required(VERSION 3.16)
project(screenparser_tests CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

set(SP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")
set(SP_SERVICES "${SP_ROOT}/services/screen_parser")

# Pure-logic sources under test (device-only modules are intentionally excluded).
set(SP_SOURCES
    "${SP_SERVICES}/common/src/json.cpp"
    "${SP_SERVICES}/common/src/screen_error.cpp"
    "${SP_SERVICES}/common/src/base64.cpp"
    "${SP_SERVICES}/common/src/service_config.cpp"
    "${SP_SERVICES}/schema/src/screen_schema.cpp"
    "${SP_SERVICES}/preprocess/src/image_preprocess.cpp"
    "${SP_SERVICES}/inference/src/json_constraint_decoder.cpp"
    "${SP_SERVICES}/inference/src/qwen_tokenizer.cpp"
    "${SP_SERVICES}/inference/src/http_message.cpp"
    "${SP_SERVICES}/inference/src/remote_vlm_engine.cpp"
    # MSLiteEngine compiles to its stub branch on host (SCREENPARSER_ENABLE_MSLITE
    # is off), but its pure multimodal helpers (EmbedTokens/BuildPromptEmbeds/
    # ArgMax) are always compiled and unit-tested here.
    "${SP_SERVICES}/inference/src/ms_lite_engine.cpp"
    "${SP_SERVICES}/ocr/src/core_vision_ocr_engine.cpp"
    "${SP_SERVICES}/capture/src/screen_capture.cpp"
    "${SP_SERVICES}/analyzer/src/screen_analyzer.cpp"
)

set(SP_TESTS
    "${CMAKE_CURRENT_SOURCE_DIR}/main.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/test_schema.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/test_analyzer.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/test_tokenizer.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/test_json_constraint.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/test_json_preprocess.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/test_ocr.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/test_remote_vlm.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/test_service_config.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/test_live_remote.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/test_mslite_logic.cpp"
)

add_executable(screenparser_tests ${SP_SOURCES} ${SP_TESTS})

target_include_directories(screenparser_tests PRIVATE
    # gtest shim first so <gtest/gtest.h> resolves to it on the host build.
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party"
    "${SP_SERVICES}/common/include"
    "${SP_SERVICES}/schema/include"
    "${SP_SERVICES}/preprocess/include"
    "${SP_SERVICES}/inference/include"
    "${SP_SERVICES}/ocr/include"
    "${SP_SERVICES}/capture/include"
    "${SP_SERVICES}/analyzer/include"
)

# Host builds exercise the REAL BSD-socket transport (SocketHttpTransport) over
# actual TCP against an in-process mock OpenAI server, so enable the same macro
# the device build uses. Winsock must be linked explicitly on Windows.
target_compile_definitions(screenparser_tests PRIVATE SCREENPARSER_ENABLE_REMOTE_VLM)
if(WIN32)
    target_link_libraries(screenparser_tests PRIVATE ws2_32)
endif()

if(MSVC)
    target_compile_options(screenparser_tests PRIVATE /W3 /utf-8)
else()
    target_compile_options(screenparser_tests PRIVATE -Wall -Wextra)
endif()

enable_testing()
add_test(NAME screenparser_tests COMMAND screenparser_tests)
