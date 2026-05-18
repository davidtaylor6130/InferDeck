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
