cmake_minimum_required(VERSION 3.27)
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" INFERDECK_VERSION LIMIT_COUNT 1)
if(NOT INFERDECK_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
    message(FATAL_ERROR "VERSION must contain one semantic version")
endif()
project(InferDeck VERSION ${INFERDECK_VERSION} LANGUAGES C CXX)
add_compile_definitions(INFERDECK_VERSION="${PROJECT_VERSION}")

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

option(INFERDECK_BUILD_TESTS "Build tests" OFF)
option(INFERDECK_BUILD_WASM "Build WASM module" OFF)

if(INFERDECK_BUILD_TESTS)
    enable_testing()
endif()

set(CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake" ${CMAKE_MODULE_PATH})

# --- Dependencies via vcpkg ---
find_package(nlohmann_json REQUIRED)
find_package(spdlog REQUIRED)
find_package(fmt REQUIRED)
find_package(yaml-cpp REQUIRED)
find_package(unofficial-sqlite3 CONFIG REQUIRED)
find_package(httplib CONFIG REQUIRED)

if(INFERDECK_BUILD_TESTS)
    find_package(Catch2 REQUIRED)
endif()

# --- llama.cpp: internal Vulkan runtime ---
# InferDeck owns llama.cpp in-process; do not build or launch llama-server.exe.
set(_llama_source_dir "${CMAKE_CURRENT_SOURCE_DIR}/libs/third_party/llama.cpp")

set(LLAMA_BUILD_COMMON ON CACHE BOOL "" FORCE)
set(LLAMA_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_SERVER OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_UI OFF CACHE BOOL "" FORCE)
set(GGML_VULKAN ON CACHE BOOL "" FORCE)
set(GGML_CUDA OFF CACHE BOOL "" FORCE)
set(GGML_HIP OFF CACHE BOOL "" FORCE)
set(GGML_SYCL OFF CACHE BOOL "" FORCE)
set(GGML_METAL OFF CACHE BOOL "" FORCE)

set(INFERDECK_VULKAN_SDK_VERSION "1.4.309.0" CACHE STRING "Required Vulkan SDK version")
set(INFERDECK_VULKAN_SDK_ROOT "" CACHE PATH "Validated Vulkan SDK root override")
if(INFERDECK_VULKAN_SDK_ROOT)
    set(_vulkan_root "${INFERDECK_VULKAN_SDK_ROOT}")
elseif(DEFINED ENV{VULKAN_SDK})
    set(_vulkan_root "$ENV{VULKAN_SDK}")
else()
    message(FATAL_ERROR "Set VULKAN_SDK or INFERDECK_VULKAN_SDK_ROOT to Vulkan SDK ${INFERDECK_VULKAN_SDK_VERSION}")
endif()
get_filename_component(_vulkan_root "${_vulkan_root}" ABSOLUTE)
get_filename_component(_vulkan_dir_name "${_vulkan_root}" NAME)
if(NOT _vulkan_dir_name STREQUAL INFERDECK_VULKAN_SDK_VERSION)
    message(FATAL_ERROR "Vulkan SDK ${INFERDECK_VULKAN_SDK_VERSION} required; received ${_vulkan_root}")
endif()
foreach(_vulkan_file IN ITEMS Include/vulkan/vulkan.h Lib/vulkan-1.lib Bin/glslc.exe)
    if(NOT EXISTS "${_vulkan_root}/${_vulkan_file}")
        message(FATAL_ERROR "Incomplete Vulkan SDK: ${_vulkan_root}/${_vulkan_file} is missing")
    endif()
endforeach()
set(Vulkan_LIBRARY "${_vulkan_root}/Lib/vulkan-1.lib" CACHE FILEPATH "" FORCE)
set(Vulkan_INCLUDE_DIR "${_vulkan_root}/Include" CACHE PATH "" FORCE)
set(Vulkan_GLSLC_EXECUTABLE "${_vulkan_root}/Bin/glslc.exe" CACHE FILEPATH "" FORCE)
set(Vulkan_FOUND ON CACHE INTERNAL "")
set(Vulkan_INCLUDE_DIRS "${_vulkan_root}/Include" CACHE INTERNAL "")
set(Vulkan_LIBRARIES "${_vulkan_root}/Lib/vulkan-1.lib" CACHE INTERNAL "")
set(Vulkan_VERSION "${INFERDECK_VULKAN_SDK_VERSION}" CACHE INTERNAL "")

# Create Vulkan::Vulkan imported target before llama.cpp subdirectory
if (NOT TARGET Vulkan::Vulkan)
    # Use SHARED IMPORTED instead of INTERFACE IMPORTED so the library gets linked
    add_library(Vulkan::Vulkan SHARED IMPORTED)
    set_target_properties(Vulkan::Vulkan PROPERTIES
        IMPORTED_IMPLIB "${_vulkan_root}/Lib/vulkan-1.lib"
        IMPORTED_LOCATION "${_vulkan_root}/Bin/vulkan-1.dll"
        INTERFACE_INCLUDE_DIRECTORIES "${_vulkan_root}/Include"
    )
endif()

set(GGML_NATIVE OFF CACHE BOOL "" FORCE)
set(GGML_AVX2 ON CACHE BOOL "" FORCE)
set(GGML_AVX512 OFF CACHE BOOL "" FORCE)
add_subdirectory(libs/third_party/llama.cpp EXCLUDE_FROM_ALL)
set(llama_cpp_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/libs/third_party/llama.cpp")
if(TARGET llama)
    set_target_properties(llama PROPERTIES
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
    )
endif()
if(TARGET ggml-vulkan)
    target_include_directories(ggml-vulkan PRIVATE
        ${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/libs/third_party/llama.cpp/ggml/include
    )
    set_target_properties(ggml-vulkan PROPERTIES
        LINK_LIBRARIES "ggml-base;Vulkan::Vulkan"
    )
endif()

# --- mtmd multimodal library (from llama.cpp) ---
set(LLAMA_INSTALL_VERSION "0.0.0" CACHE STRING "llama.cpp version")
add_subdirectory(${llama_cpp_SOURCE_DIR}/tools/mtmd EXCLUDE_FROM_ALL)

# --- Our libraries and app ---
add_subdirectory(libs/foundation)
add_subdirectory(libs/inference)
add_subdirectory(libs/model)
add_subdirectory(libs/native_runtimes)
add_subdirectory(libs/gateway)
add_subdirectory(libs/observability)
add_subdirectory(libs/optimize)
add_subdirectory(apps/benchmark-runner)

if(INFERDECK_BUILD_TESTS)
    add_subdirectory(tests/integration)
    add_subdirectory(tests/parity)
endif()
add_subdirectory(libs/llama_cpp_wrapper)
add_subdirectory(apps/inferdeck-gateway)
add_subdirectory(apps/model-tester)

install(TARGETS inferdeck-gateway
        RUNTIME DESTINATION .
        LIBRARY DESTINATION lib
        ARCHIVE DESTINATION lib/static)

install(FILES config/gateway.yml DESTINATION config)
install(DIRECTORY config/templates/ DESTINATION config/templates)
install(DIRECTORY apps/inferdeck-gateway/static/ DESTINATION static)
install(FILES data/pricing.json DESTINATION data)
install(DIRECTORY ops/ DESTINATION ops FILES_MATCHING PATTERN "*.ps1")
