# apps/gateway-service CMakeLists.txt
cmake_minimum_required(VERSION 3.27)
project(gateway-service LANGUAGES CXX)

add_executable(gateway-service
    src/main.cpp
    src/Server.cpp
    src/routes/ChatCompletions.cpp
    src/routes/Completions.cpp
    src/routes/Models.cpp
    src/routes/Embeddings.cpp
    src/routes/Health.cpp
    src/routes/Metrics.cpp
    src/routes/AudioTranscriptions.cpp
    src/routes/AudioSpeech.cpp
    src/routes/Images.cpp
    src/routes/Documents.cpp
    src/routes/FineTuningJobs.cpp
    src/config/ConfigLoader.cpp
)

target_include_directories(gateway-service PRIVATE
    src
    ../core/include
    ../llama_cpp_wrapper/include
    ../backends/include
    ../vector_store/include
)

target_link_libraries(gateway-service PRIVATE
    core
    llama_cpp_wrapper
    backends
    vector_store
    cpp-httplib::httplib
    yaml-cpp
    nlohmann_json::nlohmann_json
    spdlog::spdlog
    fmt::fmt
)

# TLS support
if(MSVC)
    target_compile_definitions(gateway-service PRIVATE
        CPPHTTPLIB_OPENSSL_SUPPORT
        OPENSSL_ISOPENSSL
    )
    target_link_libraries(gateway-service PRIVATE
        openssl::crypto
        openssl::ssl
    )
endif()

# Compiler warnings
target_compile_options(gateway-service PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    -Wconversion
    -Wsign-conversion
)

# Install
install(TARGETS gateway-service
        RUNTIME DESTINATION .)
