add_library(native_runtimes STATIC
    src/runtime_factories.cpp
    src/png.cpp
)

target_include_directories(native_runtimes PUBLIC include)
target_link_libraries(native_runtimes PUBLIC model foundation)
target_compile_features(native_runtimes PUBLIC cxx_std_23)

if(WIN32)
    target_sources(native_runtimes PRIVATE src/windows_sapi_backend.cpp)
    target_link_libraries(native_runtimes PRIVATE sapi ole32)
    target_compile_definitions(native_runtimes PUBLIC INFERDECK_HAS_WINDOWS_SAPI=1)
endif()

set(INFERDECK_WHISPER_ROOT "${CMAKE_SOURCE_DIR}/runtime/whisper.cpp-src" CACHE PATH "whisper.cpp source tree")
option(INFERDECK_REQUIRE_WHISPER_CPP "Fail configuration when whisper.cpp is unavailable" OFF)
set(INFERDECK_SHERPA_ONNX_ROOT "" CACHE PATH "sherpa-onnx install prefix")
option(INFERDECK_REQUIRE_SHERPA_ONNX "Fail configuration when sherpa-onnx is unavailable" OFF)
set(INFERDECK_SHERPA_ONNX_RUNTIME_DIR "${INFERDECK_SHERPA_ONNX_ROOT}/lib" CACHE PATH "sherpa-onnx runtime library directory")

if(EXISTS "${INFERDECK_STABLE_DIFFUSION_ROOT}/CMakeLists.txt")
    set(SD_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(SD_WEBP OFF CACHE BOOL "" FORCE)
    set(SD_WEBM OFF CACHE BOOL "" FORCE)
    set(SD_VULKAN ON CACHE BOOL "" FORCE)
    set(SD_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    add_subdirectory("${INFERDECK_STABLE_DIFFUSION_ROOT}"
                     "${CMAKE_BINARY_DIR}/native/stable-diffusion" EXCLUDE_FROM_ALL)
    target_include_directories(stable-diffusion BEFORE PRIVATE
        "${CMAKE_SOURCE_DIR}/libs/third_party/llama.cpp")
    target_sources(native_runtimes PRIVATE src/stable_diffusion_backend.cpp)
    target_link_libraries(native_runtimes PRIVATE stable-diffusion)
    target_compile_definitions(native_runtimes PUBLIC INFERDECK_HAS_STABLE_DIFFUSION_CPP=1)
    if(MSVC)
        target_compile_options(stable-diffusion PRIVATE /bigobj)
    endif()
endif()

if(EXISTS "${INFERDECK_ACESTEP_ROOT}/src/pipeline-synth.cpp")
    find_package(Threads REQUIRED)

    add_library(inferdeck-acestep-yyjson STATIC
        "${INFERDECK_ACESTEP_ROOT}/vendor/yyjson/yyjson.c")
    target_include_directories(inferdeck-acestep-yyjson PUBLIC
        "${INFERDECK_ACESTEP_ROOT}/vendor/yyjson")

    add_library(inferdeck-acestep-core STATIC
        "${INFERDECK_ACESTEP_ROOT}/src/request.cpp"
        "${INFERDECK_ACESTEP_ROOT}/src/model-store.cpp"
        "${INFERDECK_ACESTEP_ROOT}/src/pipeline-lm.cpp"
        "${INFERDECK_ACESTEP_ROOT}/src/pipeline-synth.cpp"
        "${INFERDECK_ACESTEP_ROOT}/src/pipeline-synth-ops.cpp"
        "${INFERDECK_ACESTEP_ROOT}/src/pipeline-understand.cpp")
    target_include_directories(inferdeck-acestep-core PRIVATE
        "${INFERDECK_ACESTEP_ROOT}"
        "${INFERDECK_ACESTEP_ROOT}/src"
        "${CMAKE_SOURCE_DIR}/libs/third_party/llama.cpp/ggml/include")
    target_link_libraries(inferdeck-acestep-core PRIVATE
        inferdeck-acestep-yyjson
        ggml
        Threads::Threads)
    foreach(INFERDECK_ACESTEP_GGML_BACKEND IN ITEMS
            ggml-base ggml-cpu ggml-vulkan)
        if(TARGET ${INFERDECK_ACESTEP_GGML_BACKEND})
            target_link_libraries(inferdeck-acestep-core PRIVATE
                ${INFERDECK_ACESTEP_GGML_BACKEND})
        endif()
    endforeach()
    target_compile_features(inferdeck-acestep-core PUBLIC cxx_std_17)

    if(MSVC)
        target_compile_definitions(inferdeck-acestep-core PRIVATE
            _CRT_SECURE_NO_WARNINGS)
        target_compile_options(inferdeck-acestep-yyjson PRIVATE /W0 /utf-8 /FS)
        target_compile_options(inferdeck-acestep-core PRIVATE
            /W4 /wd4100 /wd4505 /utf-8 /FS)
    else()
        target_compile_options(inferdeck-acestep-yyjson PRIVATE -w)
        target_compile_options(inferdeck-acestep-core PRIVATE
            -Wall -Wextra -Wshadow -Wconversion
            -Wno-unused-parameter -Wno-unused-function -Wno-sign-conversion)
    endif()

    target_sources(native_runtimes PRIVATE src/ace_step_backend.cpp)
    target_include_directories(native_runtimes PRIVATE
        "${INFERDECK_ACESTEP_ROOT}"
        "${INFERDECK_ACESTEP_ROOT}/src"
        "${INFERDECK_ACESTEP_ROOT}/vendor/yyjson")
    target_link_libraries(native_runtimes PRIVATE inferdeck-acestep-core)
    target_compile_definitions(native_runtimes PUBLIC
        INFERDECK_HAS_ACESTEP_CPP=1)
    if(MSVC)
        set_source_files_properties(src/ace_step_backend.cpp PROPERTIES
            COMPILE_OPTIONS "/wd4505;/wd4996")
    endif()
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
elseif(INFERDECK_REQUIRE_WHISPER_CPP)
    message(FATAL_ERROR "whisper.cpp is required but INFERDECK_WHISPER_ROOT is unavailable: ${INFERDECK_WHISPER_ROOT}")
endif()

find_path(INFERDECK_SHERPA_ONNX_INCLUDE sherpa-onnx/c-api/c-api.h
          HINTS "${INFERDECK_SHERPA_ONNX_ROOT}" PATH_SUFFIXES include)
find_library(INFERDECK_SHERPA_ONNX_LIBRARY NAMES sherpa-onnx-c-api
             HINTS "${INFERDECK_SHERPA_ONNX_ROOT}" PATH_SUFFIXES lib lib/Release)
if(INFERDECK_SHERPA_ONNX_INCLUDE AND INFERDECK_SHERPA_ONNX_LIBRARY)
    target_sources(native_runtimes PRIVATE
        src/sherpa_asr_backend.cpp
        src/sherpa_tts_backend.cpp)
    target_include_directories(native_runtimes PRIVATE ${INFERDECK_SHERPA_ONNX_INCLUDE})
    target_link_libraries(native_runtimes PRIVATE ${INFERDECK_SHERPA_ONNX_LIBRARY})
    target_compile_definitions(native_runtimes PUBLIC INFERDECK_HAS_SHERPA_ONNX=1)
elseif(INFERDECK_REQUIRE_SHERPA_ONNX)
    message(FATAL_ERROR "sherpa-onnx is required but headers or libraries are unavailable under: ${INFERDECK_SHERPA_ONNX_ROOT}")
endif()

if(MSVC)
    target_compile_options(native_runtimes PRIVATE /W4 /utf-8 /FS)
else()
    target_compile_options(native_runtimes PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(INFERDECK_BUILD_TESTS)
    add_executable(native_runtime_tests tests/test_png.cpp)
    if(WIN32)
        target_sources(native_runtime_tests PRIVATE tests/test_windows_sapi.cpp)
    endif()
    if(EXISTS "${INFERDECK_WHISPER_ROOT}/CMakeLists.txt")
        target_sources(native_runtime_tests PRIVATE tests/test_whisper.cpp)
    endif()
    if(INFERDECK_SHERPA_ONNX_INCLUDE AND INFERDECK_SHERPA_ONNX_LIBRARY)
        target_sources(native_runtime_tests PRIVATE
            tests/test_sherpa_asr.cpp
            tests/test_sherpa_tts.cpp)
        if(WIN32)
            add_custom_command(TARGET native_runtime_tests POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "${INFERDECK_SHERPA_ONNX_RUNTIME_DIR}/sherpa-onnx-c-api.dll"
                        "${INFERDECK_SHERPA_ONNX_RUNTIME_DIR}/onnxruntime.dll"
                        "${INFERDECK_SHERPA_ONNX_RUNTIME_DIR}/onnxruntime_providers_shared.dll"
                        "$<TARGET_FILE_DIR:native_runtime_tests>"
                COMMENT "Copying sherpa-onnx runtime libraries for native tests")
        endif()
    endif()
    if(EXISTS "${INFERDECK_ACESTEP_ROOT}/src/pipeline-synth.cpp")
        target_sources(native_runtime_tests PRIVATE tests/test_ace_step.cpp)
    endif()
    target_link_libraries(native_runtime_tests PRIVATE native_runtimes Catch2::Catch2WithMain)
    add_test(NAME native_runtime_tests COMMAND native_runtime_tests)
    set_tests_properties(native_runtime_tests PROPERTIES LABELS "unit")
endif()
