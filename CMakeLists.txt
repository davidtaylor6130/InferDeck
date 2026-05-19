cmake_minimum_required(VERSION 3.27)
project(InferDeck LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

option(INFERDECK_BUILD_TESTS "Build tests" OFF)
option(INFERDECK_BUILD_WASM "Build WASM module" OFF)

set(CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake" ${CMAKE_MODULE_PATH})

# --- Dependencies via vcpkg ---
find_package(nlohmann_json REQUIRED)
find_package(spdlog REQUIRED)
find_package(fmt REQUIRED)
find_package(yaml-cpp REQUIRED)
find_package(unofficial-sqlite3 CONFIG REQUIRED)

if(INFERDECK_BUILD_TESTS)
    find_package(Catch2 REQUIRED)
endif()

# --- llama.cpp: FetchContent for headers only ---
# We use the official pre-built llama-server binary at runtime.
# FetchContent gives us the headers we need for our wrapper types.
include(FetchContent)
FetchContent_Declare(
    llama_cpp
    GIT_REPOSITORY https://github.com/ggml-org/llama.cpp.git
    GIT_TAG        b9222
    SOURCE_SUBDIR  OFF
)
FetchContent_GetProperties(llama_cpp)
if(NOT llama_cpp_POPULATED)
    FetchContent_Populate(llama_cpp)
endif()

# --- Our libraries and app ---
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
