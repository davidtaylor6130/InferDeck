# libs/backends CMakeLists.txt
cmake_minimum_required(VERSION 3.27)
project(backends LANGUAGES CXX)

add_library(backends STATIC
    src/GpuResourceManager.cpp
    src/BackendRegistry.cpp
    src/whisper_stt/WhisperSttBackend.cpp
    src/piper_tts/PiperTtsBackend.cpp
    src/stable_diffusion/StableDiffusionImageBackend.cpp
    src/text_embedding/GteEmbeddingBackend.cpp
    src/llama_finetune/LlamaFinetuneBackend.cpp
)

target_include_directories(backends PUBLIC
    include
    src/whisper_stt
    src/piper_tts
    src/stable_diffusion
    src/text_embedding
    src/llama_finetune
)

target_link_libraries(backends PUBLIC
    core
    llama_cpp_wrapper
    nlohmann_json::nlohmann_json
    Vulkan::Vulkan
)

target_compile_definitions(backends PRIVATE
    GGML_VULKAN=1
)

target_compile_options(backends PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    --coverage
)
