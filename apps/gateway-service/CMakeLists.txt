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
    ${CMAKE_CURRENT_SOURCE_DIR}/../../libs/core/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../libs/llama_cpp_wrapper/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../libs/backends/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../libs/vector_store/include
    ${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/include
)

target_link_libraries(gateway-service PRIVATE
    core
    llama_cpp_wrapper
    backends
    vector_store
    yaml-cpp::yaml-cpp
    nlohmann_json::nlohmann_json
    spdlog::spdlog
    fmt::fmt
    ws2_32
    crypt32
)

if(MSVC)
    target_compile_options(gateway-service PRIVATE /W4 /utf-8)
else()
    target_compile_options(gateway-service PRIVATE -Wall -Wextra -Wpedantic)
endif()

install(TARGETS gateway-service
        RUNTIME DESTINATION .)
