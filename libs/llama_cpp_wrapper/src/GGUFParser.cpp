/// @file GGUFParser.cpp
/// @brief GGUFParser implementation.

#include "llama_cpp/GGUFParser.hpp"
#include <fstream>
#include <cstring>

namespace inferdeck::core {

GGUFMetadata GGUFParser::Parse(const std::filesystem::path& path) {
    GGUFMetadata metadata;

    if (!IsValidGgufFile(path)) {
        return metadata;
    }

    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        return metadata;
    }

    // Read GGUF magic number
    uint32_t magic;
    file.read(reinterpret_cast<char*>(&magic), sizeof(magic));
    if (magic != GGUF_MAGIC) {
        return metadata; // Invalid GGUF
    }

    // Read version
    file.read(reinterpret_cast<char*>(&metadata.version), sizeof(metadata.version));

    // Read tensor count
    file.read(reinterpret_cast<char*>(&metadata.tensor_count), sizeof(metadata.tensor_count));

    // Read KV count
    file.read(reinterpret_cast<char*>(&metadata.kv_count), sizeof(metadata.kv_count));

    // Read KV pairs (for V1, we extract key info)
    for (uint64_t i = 0; i < metadata.kv_count; i++) {
        uint64_t key_len = 0;
        file.read(reinterpret_cast<char*>(&key_len), sizeof(key_len));

        std::string key(key_len, '\0');
        file.read(&key[0], key_len);

        uint32_t type = 0;
        file.read(reinterpret_cast<char*>(&type), sizeof(type));

        if (key == "general.architecture") {
            uint64_t str_len = 0;
            file.read(reinterpret_cast<char*>(&str_len), sizeof(str_len));
            std::string arch_name(str_len, '\0');
            file.read(&arch_name[0], str_len);
            metadata.architecture = ParseArchType(arch_name);
        } else if (key == "general.name" || key == "general.basename") {
            uint64_t str_len = 0;
            file.read(reinterpret_cast<char*>(&str_len), sizeof(str_len));
            metadata.model_name.resize(str_len);
            file.read(&metadata.model_name[0], str_len);
        } else if (key == "general.description") {
            uint64_t str_len = 0;
            file.read(reinterpret_cast<char*>(&str_len), sizeof(str_len));
            metadata.description.resize(str_len);
            file.read(&metadata.description[0], str_len);
        } else if (key == "general.quantization_version") {
            uint32_t quant;
            file.read(reinterpret_cast<char*>(&quant), sizeof(quant));
            metadata.quantization = ParseQuantType(quant);
        } else if (key == "llama.vocab_size" || key == "qwen2.vocab_size") {
            uint64_t val;
            file.read(reinterpret_cast<char*>(&val), sizeof(val));
            metadata.vocab_size = static_cast<uint32_t>(val);
        } else if (key == "llama.context_length" || key == "qwen2.context_length") {
            uint64_t val;
            file.read(reinterpret_cast<char*>(&val), sizeof(val));
            metadata.context_size = static_cast<int32_t>(val);
        } else if (key == "llama.embedding_length" || key == "qwen2.embedding_length") {
            uint64_t val;
            file.read(reinterpret_cast<char*>(&val), sizeof(val));
            metadata.embedding_size = static_cast<uint32_t>(val);
        } else if (key == "llama.block_count" || key == "qwen2.block_count") {
            uint64_t val;
            file.read(reinterpret_cast<char*>(&val), sizeof(val));
            metadata.block_count = static_cast<uint32_t>(val);
        } else {
            // Skip value based on type
            switch (type) {
                case 0: { bool val; file.read(reinterpret_cast<char*>(&val), sizeof(val)); } break;
                case 1: { uint64_t val; file.read(reinterpret_cast<char*>(&val), sizeof(val)); } break;
                case 2: { float val; file.read(reinterpret_cast<char*>(&val), sizeof(val)); } break;
                case 3: { double val; file.read(reinterpret_cast<char*>(&val), sizeof(val)); } break;
                case 4: { uint64_t len; file.read(reinterpret_cast<char*>(&len), sizeof(len));
                           file.seekg(len, std::ios::cur); } break;
                case 5: { uint64_t count; file.read(reinterpret_cast<char*>(&count), sizeof(count));
                           for (uint64_t j = 0; j < count; j++) { uint64_t len2; file.read(reinterpret_cast<char*>(&len2), sizeof(len2));
                           file.seekg(len2, std::ios::cur); } } break;
                case 6: { uint64_t count; file.read(reinterpret_cast<char*>(&count), sizeof(count));
                           for (uint64_t j = 0; j < count; j++) { bool val; file.read(reinterpret_cast<char*>(&val), sizeof(val)); } } break;
                case 7: { uint64_t count; file.read(reinterpret_cast<char*>(&count), sizeof(count));
                           for (uint64_t j = 0; j < count; j++) { uint32_t val; file.read(reinterpret_cast<char*>(&val), sizeof(val)); } } break;
                case 8: { uint64_t count; file.read(reinterpret_cast<char*>(&count), sizeof(count));
                           for (uint64_t j = 0; j < count; j++) { float val; file.read(reinterpret_cast<char*>(&val), sizeof(val)); } } break;
                case 9: { uint64_t count; file.read(reinterpret_cast<char*>(&count), sizeof(count));
                           for (uint64_t j = 0; j < count; j++) { double val; file.read(reinterpret_cast<char*>(&val), sizeof(val)); } } break;
                default: file.seekg(1024, std::ios::cur); break; // Skip unknown types
            }
        }
    }

    metadata.valid = (metadata.architecture != ModelArch::Unknown);
    return metadata;
}

QuantType GGUFParser::ParseQuantType(uint32_t value) {
    return static_cast<QuantType>(value);
}

ModelArch GGUFParser::ParseArchType(uint32_t value) {
    return static_cast<ModelArch>(value);
}

std::string GGUFParser::QuantToString(QuantType quant) {
    switch (quant) {
        case QuantType::Q4_0:     return "q4_0";
        case QuantType::Q4_1:     return "q4_1";
        case QuantType::Q8_0:     return "q8_0";
        case QuantType::Q5_0:     return "q5_0";
        case QuantType::Q5_1:     return "q5_1";
        case QuantType::F16:      return "f16";
        case QuantType::F32:      return "f32";
        case QuantType::Q8_K:     return "q8_k";
        case QuantType::Q2_K:     return "q2_k";
        case QuantType::Q3_K_M:   return "q3_k_m";
        case QuantType::Q3_K_S:   return "q3_k_s";
        case QuantType::Q3_K_L:   return "q3_k_l";
        case QuantType::Q4_K_M:   return "q4_k_m";
        case QuantType::Q4_K_S:   return "q4_k_s";
        case QuantType::Q5_K_M:   return "q5_k_m";
        case QuantType::Q5_K_S:   return "q5_k_s";
        case QuantType::Q6_K:     return "q6_k";
        case QuantType::IQ1_M:    return "iq1_m";
        case QuantType::IQ1_S:    return "iq1_s";
        case QuantType::IQ4_K:    return "iq4_k";
        case QuantType::IQ8_K:    return "iq8_k";
        default:                  return "unknown";
    }
}

std::string GGUFParser::ArchToString(ModelArch arch) {
    switch (arch) {
        case ModelArch::LLAMA:    return "llama";
        case ModelArch::QWEN2:    return "qwen2";
        case ModelArch::PHI2:     return "phi2";
        case ModelArch::GEMMA:    return "gemma";
        case ModelArch::MPT:      return "mpt";
        case ModelArch::BLOOM:    return "bloom";
        case ModelArch::GPT2:     return "gpt2";
        case ModelArch::GPTJ:     return "gptj";
        case ModelArch::GPTNeoX:  return "gptneox";
        case ModelArch::Mamba:    return "mamba";
        case ModelArch::CHATGLM:  return "chatglm";
        case ModelArch::T5:       return "t5";
        case ModelArch::DEEPSEEK: return "deepseek";
        default:                  return "unknown";
    }
}

bool GGUFParser::IsValidGgufFile(const std::filesystem::path& path) {
    if (!std::filesystem::exists(path)) {
        return false;
    }
    return path.extension() == ".gguf";
}

} // namespace inferdeck::core
