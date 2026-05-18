cmake_minimum_required(VERSION 3.27)
project(InferDeck LANGUAGES CXX)

# C++23 standard
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Output directories
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# Options
option(INFERDECK_BUILD_TESTS "Build tests" ON)
option(INFERDECK_BUILD_WASM "Build WASM module" OFF)

# Fetch llama.cpp as a git submodule (if not present, clone it)
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/libs/third_party/llama.cpp/CMakeLists.txt")
    message(STATUS "llama.cpp not found, cloning as git submodule...")
    execute_process(
        COMMAND git submodule update --init --recursive
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        RESULT_VARIABLE GIT_RESULT
    )
    if(NOT GIT_RESULT EQUAL 0)
        message(WARNING "Failed to clone llama.cpp submodule. Please run: git submodule update --init --recursive")
    endif()
endif()

# Add subdirectories
add_subdirectory(libs/core)
add_subdirectory(libs/llama_cpp_wrapper)
add_subdirectory(apps/gateway-service)

# Tests
if(INFERDECK_BUILD_TESTS)
    add_subdirectory(libs/tests)
endif()

# Install rules
install(TARGETS gateway-service
        RUNTIME DESTINATION .
        LIBRARY DESTINATION lib
        ARCHIVE DESTINATION lib/static)

install(DIRECTORY certs/ DESTINATION certs)
install(DIRECTORY config/ DESTINATION config)
install(DIRECTORY libs/third_party/llama.cpp DESTINATION libs/third_party/llama.cpp
         FILES_MATCHING PATTERN "*.h" PATTERN "*.hpp" PATTERN "CMakeLists.txt")
