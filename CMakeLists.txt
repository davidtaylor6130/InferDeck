cmake_minimum_required(VERSION 3.27)
project(InferDeck LANGUAGES C CXX)

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

add_subdirectory(apps/inferdeck-gateway)

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

# Set up Vulkan BEFORE adding llama.cpp subdirectory
set(Vulkan_LIBRARY "C:/VulkanSDK/1.4.309.0/Lib/vulkan-1.lib" CACHE FILEPATH "" FORCE)
set(Vulkan_INCLUDE_DIR "C:/VulkanSDK/1.4.309.0/Include" CACHE PATH "" FORCE)
set(Vulkan_GLSLC_EXECUTABLE "C:/VulkanSDK/1.4.309.0/Bin/glslc.exe" CACHE FILEPATH "" FORCE)
set(Vulkan_FOUND ON CACHE INTERNAL "")
set(Vulkan_INCLUDE_DIRS "C:/VulkanSDK/1.4.309.0/Include" CACHE INTERNAL "")
set(Vulkan_LIBRARIES "C:/VulkanSDK/1.4.309.0/Lib/vulkan-1.lib" CACHE INTERNAL "")
set(Vulkan_VERSION "1.4" CACHE INTERNAL "")

# Create Vulkan::Vulkan imported target before llama.cpp subdirectory
if (NOT TARGET Vulkan::Vulkan)
    # Use SHARED IMPORTED instead of INTERFACE IMPORTED so the library gets linked
    add_library(Vulkan::Vulkan SHARED IMPORTED)
    set_target_properties(Vulkan::Vulkan PROPERTIES
        IMPORTED_IMPLIB "C:/VulkanSDK/1.4.309.0/Lib/vulkan-1.lib"
        IMPORTED_LOCATION "C:/VulkanSDK/1.4.309.0/Bin/vulkan-1.dll"
        INTERFACE_INCLUDE_DIRECTORIES "C:/VulkanSDK/1.4.309.0/Include"
    )
endif()

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
add_subdirectory(libs/model)
add_subdirectory(libs/gateway)
add_subdirectory(libs/observability)
add_subdirectory(libs/optimize)
add_subdirectory(apps/benchmark-runner)

if(INFERDECK_BUILD_TESTS)
    add_subdirectory(tests/integration)
    add_subdirectory(tests/parity)
endif()
add_subdirectory(libs/llama_cpp_wrapper)
add_subdirectory(apps/model-tester)

install(TARGETS inferdeck-gateway
        RUNTIME DESTINATION .
        LIBRARY DESTINATION lib
        ARCHIVE DESTINATION lib/static)

install(DIRECTORY config/ DESTINATION config)
