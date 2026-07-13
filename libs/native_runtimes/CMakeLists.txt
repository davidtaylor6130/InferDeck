add_library(native_runtimes STATIC
    src/runtime_factories.cpp
    src/png.cpp
)

target_include_directories(native_runtimes PUBLIC include)
target_link_libraries(native_runtimes PUBLIC model foundation)
target_compile_features(native_runtimes PUBLIC cxx_std_23)

set(INFERDECK_STABLE_DIFFUSION_ROOT "" CACHE PATH "stable-diffusion.cpp source tree")
set(INFERDECK_WHISPER_ROOT "" CACHE PATH "whisper.cpp source tree")
set(INFERDECK_SHERPA_ONNX_ROOT "" CACHE PATH "sherpa-onnx install prefix")

if(EXISTS "${INFERDECK_STABLE_DIFFUSION_ROOT}/CMakeLists.txt")
    set(SD_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(SD_VULKAN ON CACHE BOOL "" FORCE)
    set(SD_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    add_subdirectory("${INFERDECK_STABLE_DIFFUSION_ROOT}"
                     "${CMAKE_BINARY_DIR}/native/stable-diffusion" EXCLUDE_FROM_ALL)
    target_sources(native_runtimes PRIVATE src/stable_diffusion_backend.cpp)
    target_link_libraries(native_runtimes PRIVATE stable-diffusion)
    target_compile_definitions(native_runtimes PUBLIC INFERDECK_HAS_STABLE_DIFFUSION_CPP=1)
endif()

if(EXISTS "${INFERDECK_WHISPER_ROOT}/CMakeLists.txt")
    set(WHISPER_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(WHISPER_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(WHISPER_BUILD_SERVER OFF CACHE BOOL "" FORCE)
    add_subdirectory("${INFERDECK_WHISPER_ROOT}"
                     "${CMAKE_BINARY_DIR}/native/whisper" EXCLUDE_FROM_ALL)
    target_sources(native_runtimes PRIVATE src/whisper_backend.cpp)
    target_link_libraries(native_runtimes PRIVATE whisper)
    target_compile_definitions(native_runtimes PUBLIC INFERDECK_HAS_WHISPER_CPP=1)
endif()

find_path(INFERDECK_SHERPA_ONNX_INCLUDE sherpa-onnx/c-api/c-api.h
          HINTS "${INFERDECK_SHERPA_ONNX_ROOT}" PATH_SUFFIXES include)
find_library(INFERDECK_SHERPA_ONNX_LIBRARY NAMES sherpa-onnx-c-api
             HINTS "${INFERDECK_SHERPA_ONNX_ROOT}" PATH_SUFFIXES lib lib/Release)
if(INFERDECK_SHERPA_ONNX_INCLUDE AND INFERDECK_SHERPA_ONNX_LIBRARY)
    target_sources(native_runtimes PRIVATE src/sherpa_tts_backend.cpp)
    target_include_directories(native_runtimes PRIVATE ${INFERDECK_SHERPA_ONNX_INCLUDE})
    target_link_libraries(native_runtimes PRIVATE ${INFERDECK_SHERPA_ONNX_LIBRARY})
    target_compile_definitions(native_runtimes PUBLIC INFERDECK_HAS_SHERPA_ONNX=1)
endif()

if(MSVC)
    target_compile_options(native_runtimes PRIVATE /W4 /utf-8 /FS)
else()
    target_compile_options(native_runtimes PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(INFERDECK_BUILD_TESTS)
    add_executable(native_runtime_tests tests/test_png.cpp)
    target_link_libraries(native_runtime_tests PRIVATE native_runtimes Catch2::Catch2WithMain)
    add_test(NAME native_runtime_tests COMMAND native_runtime_tests)
    set_tests_properties(native_runtime_tests PROPERTIES LABELS "unit")
endif()
