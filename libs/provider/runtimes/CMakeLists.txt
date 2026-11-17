# InferDeck's own runtimes packaged as v2 provider DLLs (docs/providers.md).
#
# Main (shared-ggml) build:
#   - windows_sapi and vllm_radiance, which do not use ggml, build as real DLLs
#     into ${CMAKE_BINARY_DIR}/providers.
#   - With INFERDECK_BUILD_TESTS, every ggml runtime also builds against a stub
#     of its C API into ${CMAKE_BINARY_DIR}/provider-stubs, and ctest runs
#     inferdeck-provider-check on each DLL. No GPU is touched.
# Static-ggml build (INFERDECK_BUILD_RUNTIME_PROVIDERS=ON, BUILD_SHARED_LIBS=OFF):
#   - every ggml runtime builds as a real DLL with a private, static
#     llama.cpp/ggml inside, so it can be upgraded without touching the gateway.

option(INFERDECK_BUILD_RUNTIME_PROVIDERS
       "Build the ggml runtimes as provider DLLs with static ggml (needs BUILD_SHARED_LIBS=OFF)" OFF)

set(_native "${CMAKE_SOURCE_DIR}/libs/native_runtimes")

# inferdeck_native_provider(<target> <runtime> <factory> [SOURCES ...] [LIBRARIES ...] [INCLUDES ...] [OUTPUT_DIRECTORY dir])
function(inferdeck_native_provider target runtime factory)
    cmake_parse_arguments(ARG "" "OUTPUT_DIRECTORY" "SOURCES;LIBRARIES;INCLUDES" ${ARGN})
    inferdeck_add_provider(${target} RUNTIME ${runtime} OUTPUT_DIRECTORY "${ARG_OUTPUT_DIRECTORY}"
        SOURCES native_provider.cpp ${ARG_SOURCES}
        LIBRARIES model foundation ${ARG_LIBRARIES})
    target_include_directories(${target} PRIVATE "${_native}/include" ${ARG_INCLUDES})
    target_compile_definitions(${target} PRIVATE
        INFERDECK_PROVIDER_RUNTIME="${runtime}"
        INFERDECK_PROVIDER_FACTORY=${factory}
        INFERDECK_PROVIDER_VERSION="${PROJECT_VERSION}")
    if(MSVC)
        target_compile_options(${target} PRIVATE /wd4100 /wd4505)
    endif()
endfunction()

function(inferdeck_provider_check_test name target)
    if(INFERDECK_BUILD_TESTS)
        add_test(NAME ${name} COMMAND inferdeck-provider-check $<TARGET_FILE:${target}> ${ARGN}
                 WORKING_DIRECTORY $<TARGET_FILE_DIR:inferdeck-provider-check>)
        set_tests_properties(${name} PROPERTIES LABELS "unit" TIMEOUT 120)
    endif()
endfunction()

if(INFERDECK_BUILD_RUNTIME_PROVIDERS)
    if(BUILD_SHARED_LIBS)
        message(FATAL_ERROR
            "INFERDECK_BUILD_RUNTIME_PROVIDERS needs BUILD_SHARED_LIBS=OFF so llama.cpp/ggml is linked "
            "statically into each provider DLL. Windows resolves a second ggml.dll to the gateway's copy.")
    endif()
    set(_runtime_targets "")
    inferdeck_add_provider(inferdeck-provider-llama_cpp RUNTIME llama_cpp
        SOURCES llama_cpp_provider.cpp LIBRARIES llama_cpp_wrapper)
    target_compile_definitions(inferdeck-provider-llama_cpp PRIVATE INFERDECK_PROVIDER_VERSION="${PROJECT_VERSION}")
    list(APPEND _runtime_targets inferdeck-provider-llama_cpp)
    inferdeck_add_provider(inferdeck-provider-jev_style RUNTIME jev_style
        SOURCES jev_style_provider.cpp LIBRARIES decision_runtime llama)
    target_include_directories(inferdeck-provider-jev_style PRIVATE "${CMAKE_SOURCE_DIR}/libs/third_party/llama.cpp/include")
    target_compile_definitions(inferdeck-provider-jev_style PRIVATE INFERDECK_PROVIDER_VERSION="${PROJECT_VERSION}")
    list(APPEND _runtime_targets inferdeck-provider-jev_style)
    get_target_property(_defines native_runtimes INTERFACE_COMPILE_DEFINITIONS)
    foreach(entry IN ITEMS
            "stable_diffusion_cpp;make_stable_diffusion_backend;INFERDECK_HAS_STABLE_DIFFUSION_CPP"
            "ltx_video_cpp;make_ltx_video_backend;INFERDECK_HAS_LTX_VIDEO_CPP"
            "ace_step_cpp;make_ace_step_backend;INFERDECK_HAS_ACESTEP_CPP"
            "whisper_cpp;make_whisper_backend;INFERDECK_HAS_WHISPER_CPP")
        list(GET entry 0 runtime)
        list(GET entry 1 factory)
        list(GET entry 2 marker)
        if("${_defines}" MATCHES "${marker}")
            inferdeck_native_provider(inferdeck-provider-${runtime} ${runtime} ${factory} LIBRARIES native_runtimes)
            list(APPEND _runtime_targets inferdeck-provider-${runtime})
        endif()
    endforeach()
    add_custom_target(inferdeck-runtime-providers DEPENDS ${_runtime_targets})
endif()

if(WIN32)
    inferdeck_native_provider(inferdeck-provider-windows_sapi windows_sapi make_windows_sapi_backend
        SOURCES "${_native}/src/windows_sapi_backend.cpp"
        LIBRARIES sapi ole32 windowscodecs mfplat mfreadwrite mfuuid)
    target_compile_definitions(inferdeck-provider-windows_sapi PRIVATE INFERDECK_HAS_WINDOWS_SAPI=1)
    inferdeck_provider_check_test(provider_dll_windows_sapi inferdeck-provider-windows_sapi)
endif()

if(TARGET vllm_radiance_wrapper)
    inferdeck_add_provider(inferdeck-provider-vllm_radiance RUNTIME vllm_radiance
        SOURCES vllm_radiance_provider.cpp LIBRARIES vllm_radiance_wrapper)
    target_compile_definitions(inferdeck-provider-vllm_radiance PRIVATE INFERDECK_PROVIDER_VERSION="${PROJECT_VERSION}")
    inferdeck_provider_check_test(provider_dll_vllm_radiance inferdeck-provider-vllm_radiance --no-load
        --artifact python_root=${INFERDECK_PYTHON312_ROOT})
endif()

if(NOT INFERDECK_BUILD_TESTS OR INFERDECK_BUILD_RUNTIME_PROVIDERS)
    return()
endif()

# Stub-backed DLLs: the real backend source, a fake native C API.
set(_stub_dir "${CMAKE_BINARY_DIR}/provider-stubs")
set(_fixture "$<TARGET_FILE:inferdeck-provider-check>")
set(_ggml_include "${CMAKE_SOURCE_DIR}/libs/third_party/llama.cpp/ggml/include")

if(EXISTS "${INFERDECK_STABLE_DIFFUSION_ROOT}/include/stable-diffusion.h")
    inferdeck_native_provider(inferdeck-provider-stub-stable_diffusion_cpp stable_diffusion_cpp
        make_stable_diffusion_backend OUTPUT_DIRECTORY "${_stub_dir}"
        SOURCES "${_native}/src/stable_diffusion_backend.cpp" stubs/stable_diffusion_stub.cpp
        INCLUDES "${INFERDECK_STABLE_DIFFUSION_ROOT}/include")
    inferdeck_provider_check_test(provider_dll_stable_diffusion_cpp inferdeck-provider-stub-stable_diffusion_cpp
        --model ${_fixture})
    inferdeck_native_provider(inferdeck-provider-stub-ltx_video_cpp ltx_video_cpp
        make_ltx_video_backend OUTPUT_DIRECTORY "${_stub_dir}"
        SOURCES "${_native}/src/ltx_video_backend.cpp" stubs/stable_diffusion_stub.cpp
        INCLUDES "${INFERDECK_STABLE_DIFFUSION_ROOT}/include")
    if(MSVC)
        target_compile_options(inferdeck-provider-stub-ltx_video_cpp PRIVATE /EHsc-)
    endif()
    inferdeck_provider_check_test(provider_dll_ltx_video_cpp inferdeck-provider-stub-ltx_video_cpp
        --artifact diffusion_model=${_fixture} --artifact llm=${_fixture}
        --artifact embeddings_connectors=${_fixture} --artifact vae=${_fixture})
endif()

if(EXISTS "${INFERDECK_ACESTEP_ROOT}/src/pipeline-synth.cpp")
    inferdeck_native_provider(inferdeck-provider-stub-ace_step_cpp ace_step_cpp
        make_ace_step_backend OUTPUT_DIRECTORY "${_stub_dir}"
        SOURCES "${_native}/src/ace_step_backend.cpp" stubs/ace_step_stub.cpp
        INCLUDES "${INFERDECK_ACESTEP_ROOT}" "${INFERDECK_ACESTEP_ROOT}/src"
                 "${INFERDECK_ACESTEP_ROOT}/vendor/yyjson" "${_ggml_include}")
    target_compile_definitions(inferdeck-provider-stub-ace_step_cpp PRIVATE _CRT_SECURE_NO_WARNINGS)
    if(MSVC)
        target_compile_options(inferdeck-provider-stub-ace_step_cpp PRIVATE /wd4996)
    endif()
    inferdeck_provider_check_test(provider_dll_ace_step_cpp inferdeck-provider-stub-ace_step_cpp
        --artifact text_encoder=${_fixture} --artifact dit=${_fixture} --artifact vae=${_fixture})
endif()

if(EXISTS "${INFERDECK_WHISPER_ROOT}/include/whisper.h")
    inferdeck_native_provider(inferdeck-provider-stub-whisper_cpp whisper_cpp
        make_whisper_backend OUTPUT_DIRECTORY "${_stub_dir}"
        SOURCES "${_native}/src/whisper_backend.cpp" stubs/whisper_stub.cpp
        INCLUDES "${INFERDECK_WHISPER_ROOT}/include" "${_ggml_include}")
    inferdeck_provider_check_test(provider_dll_whisper_cpp inferdeck-provider-stub-whisper_cpp
        --artifact model=${_fixture} --artifact provider=cpu)
endif()
