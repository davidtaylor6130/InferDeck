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

set(CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake" ${CMAKE_MODULE_PATH})

# --- Dependencies via vcpkg ---
find_package(nlohmann_json REQUIRED)
find_package(spdlog REQUIRED)
find_package(fmt REQUIRED)
find_package(yaml-cpp REQUIRED)
find_package(unofficial-sqlite3 CONFIG REQUIRED)

if(INFERDECK_BUILD_TESTS)
    find_package(Catch2 REQUIRED)
    find_package(httplib CONFIG REQUIRED)
endif()

# --- llama.cpp: internal Vulkan runtime ---
# InferDeck owns llama.cpp in-process; do not build or launch llama-server.exe.
set(LLAMA_BUILD_COMMON ON CACHE BOOL "" FORCE)
set(LLAMA_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_SERVER OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_UI OFF CACHE BOOL "" FORCE)
set(GGML_VULKAN OFF CACHE BOOL "" FORCE)
set(GGML_CUDA OFF CACHE BOOL "" FORCE)
set(GGML_HIP OFF CACHE BOOL "" FORCE)
set(GGML_SYCL OFF CACHE BOOL "" FORCE)
set(GGML_METAL OFF CACHE BOOL "" FORCE)
set(Vulkan_LIBRARY "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/lib/vulkan-1.lib" CACHE FILEPATH "" FORCE)
set(Vulkan_INCLUDE_DIR "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/include" CACHE PATH "" FORCE)
set(Vulkan_GLSLC_EXECUTABLE "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/tools/shaderc/glslc.exe" CACHE FILEPATH "" FORCE)
add_subdirectory(libs/third_party/llama.cpp EXCLUDE_FROM_ALL)
set(llama_cpp_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/libs/third_party/llama.cpp")
if(TARGET Vulkan::Vulkan)
    set_target_properties(Vulkan::Vulkan PROPERTIES
        IMPORTED_LOCATION "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/lib/vulkan-1.lib"
        IMPORTED_IMPLIB "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/lib/vulkan-1.lib"
    )
endif()
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
        LINK_LIBRARIES "ggml-base;${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/lib/vulkan-1.lib"
    )
endif()

# --- mtmd multimodal library (from llama.cpp) ---
set(LLAMA_INSTALL_VERSION "0.0.0" CACHE STRING "llama.cpp version")
add_subdirectory(${llama_cpp_SOURCE_DIR}/tools/mtmd EXCLUDE_FROM_ALL)

# --- Our libraries and app ---
add_subdirectory(libs/foundation)
add_subdirectory(libs/messaging)
add_subdirectory(libs/sampling)

if(INFERDECK_BUILD_TESTS)
    add_subdirectory(tests/integration)
endif()
add_subdirectory(libs/core)
add_subdirectory(libs/llama_cpp_wrapper)
add_subdirectory(libs/backends)
add_subdirectory(libs/vector_store)
add_subdirectory(apps/gateway-service)

if(INFERDECK_BUILD_TESTS)
    add_subdirectory(libs/tests)
endif()

install(TARGETS gateway-service
        RUNTIME DESTINATION .
        LIBRARY DESTINATION lib
        ARCHIVE DESTINATION lib/static)

install(DIRECTORY certs/ DESTINATION certs)
install(DIRECTORY config/ DESTINATION config)
