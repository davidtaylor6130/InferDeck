cmake_minimum_required(VERSION 3.27)
project(InferDeck LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

option(INFERDECK_BUILD_TESTS "Build tests" ON)
option(INFERDECK_BUILD_WASM "Build WASM module" OFF)

set(CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake" ${CMAKE_MODULE_PATH})

find_package(Vulkan REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(spdlog REQUIRED)
find_package(fmt REQUIRED)
find_package(yaml-cpp REQUIRED)
find_package(unofficial-sqlite3 CONFIG REQUIRED)

if(INFERDECK_BUILD_TESTS)
    find_package(Catch2 REQUIRED)
endif()

if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/libs/third_party/llama.cpp/CMakeLists.txt")
    message(FATAL_ERROR "llama.cpp not found. Run: git submodule update --init --recursive")
endif()

set(LLAMA_VULKAN ON CACHE BOOL "Enable Vulkan support" FORCE)
set(LLAMA_BUILD_SHARED OFF CACHE BOOL "Build shared libraries" FORCE)
set(LLAMA_BUILD_STATIC ON CACHE BOOL "Build static libraries" FORCE)
set(LLAMA_BUILD_EXAMPLES OFF CACHE BOOL "Build examples" FORCE)
set(LLAMA_BUILD_TESTS OFF CACHE BOOL "Build llama.cpp tests" FORCE)
set(LLAMA_BUILD_SERVER OFF CACHE BOOL "Build server" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build shared libs" FORCE)

if(MSVC)
    add_compile_options(/utf-8 /Zc:char8_t-)
endif()

add_subdirectory(libs/third_party/llama.cpp)

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
