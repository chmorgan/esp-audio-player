cmake_minimum_required(VERSION 3.18)

project(esp_audio_player_host_tests LANGUAGES C CXX)

include(CTest)
include(FetchContent)

set(HELIX_MP3_REVISION "9a12eee090cff1a7c04a60afa14d9adc4f5ccc1b")

FetchContent_Declare(
    libhelix_mp3
    GIT_REPOSITORY https://github.com/chmorgan/libhelix-mp3.git
    GIT_TAG ${HELIX_MP3_REVISION}
    SOURCE_SUBDIR src)
FetchContent_MakeAvailable(libhelix_mp3)

get_filename_component(REPOSITORY_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)

add_executable(audio_decode_host_test
    audio_decode_test.cpp
    "${REPOSITORY_ROOT}/audio_mp3.cpp"
    "${REPOSITORY_ROOT}/audio_stream_io.cpp"
    "${REPOSITORY_ROOT}/audio_wav.cpp")

target_compile_features(audio_decode_host_test PRIVATE cxx_std_20)
target_compile_definitions(audio_decode_host_test PRIVATE
    CONFIG_AUDIO_PLAYER_LOG_LEVEL=0)
target_include_directories(audio_decode_host_test PRIVATE
    stubs
    "${REPOSITORY_ROOT}"
    "${REPOSITORY_ROOT}/include")
target_link_libraries(audio_decode_host_test PRIVATE helixmp3)

add_test(
    NAME audio_decode_host_test
    COMMAND audio_decode_host_test
            "${REPOSITORY_ROOT}/examples/esp_box_mp3/main/sample.mp3")
