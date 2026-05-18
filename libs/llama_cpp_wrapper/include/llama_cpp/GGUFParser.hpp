/// @file GGUFParser.hpp
/// @brief GGUF model file parser for InferDeck.
///
/// Reads GGUF model files and extracts metadata including:
/// - Model architecture type
/// - Quantization format (Q4_0, Q8_0, F16, F32, etc.)
/// - Vocabulary size
/// - Context size
/// - Number of layers

#pragma once

#include <string>
#include <cstdint>
#include <filesystem>
#include <optional>

namespace inferdeck::core {

/// GGUF magic number for version detection.
constexpr uint32_t GGUF_MAGIC = 0x4655474c; // "llm"

/// Quantization types supported by InferDeck.
enum class QuantType : uint32_t {
    Unknown = 0,
    Q4_0 = 1,
    Q4_1 = 2,
    Q8_0 = 8,
    Q5_0 = 9,
    Q5_1 = 10,
    Q8_0_old = 11,
    F16 = 12,
    F32 = 13,
    I8 = 14,
    I16 = 15,
    I32 = 16,
    I64 = 17,
    F64 = 18,
    Q8_K = 19,
    IQ4_NL = 20,
    IQ4_XS = 21,
    Q2_K = 22,
    Q3_K_M = 23,
    Q3_K_S = 24,
    Q3_K_L = 25,
    IQ2_XS = 26,
    IQ2_S = 27,
    Q4_K_M = 28,
    Q4_K_S = 29,
    Q5_K_M = 30,
    Q5_K_S = 31,
    Q6_K = 32,
    IQ1_M = 33,
    IQ1_S = 34,
    IQ4_K = 36,
    IQ8_K = 37,
    IQ8_K_old = 38
};

/// Model architecture types.
enum class ModelArch : uint32_t {
    Unknown = 0,
    LLAMA = 1,
    MPT = 2,
    BLOOM = 3,
    GPT2 = 4,
    GPTJ = 5,
    GPTNeoX = 6,
    Mamba = 7,
    QWEN2 = 8,
    PHI2 = 9,
    PLLaMA = 10,
    STARLIKES = 11,
    CHATGLM = 12,
    GEMMA = 13,
    T5 = 14,
    DEEPSEEK = 15
};

/// GGUF metadata for a loaded model.
struct GGUFMetadata {
    bool valid = false;
    uint32_t version = 0;
    uint64_t tensor_count = 0;
    uint64_t kv_count = 0;
    QuantType quantization = QuantType::Unknown;
    ModelArch architecture = ModelArch::Unknown;
    std::string model_name;
    std::string description;
    uint32_t vocab_size = 0;
    int32_t context_size = 0;
    uint32_t embedding_size = 0;
    uint32_t block_count = 0;
};

/// Parser for GGUF (GPT-Generated Unified Format) model files.
///
/// Reads GGUF headers and extracts model metadata without loading
/// the full model into memory. Used by LlamaEngine for auto-detection
/// of precision and model properties.
class GGUFParser {
public:
    /// Parse a GGUF file and extract metadata.
    /// @param path Path to the GGUF file.
    /// @return Parsed metadata, or empty if parsing fails.
    static GGUFMetadata Parse(const std::filesystem::path& path);

    /// Get the quantization type as a string identifier.
    /// @param quant The quantization type.
    /// @return String identifier (e.g., "Q4_0", "F16", "auto").
    static std::string QuantToString(QuantType quant);

    /// Get the model architecture as a string.
    /// @param arch The architecture type.
    /// @return String identifier (e.g., "llama", "qwen2").
    static std::string ArchToString(ModelArch arch);

    /// Check if a file exists and has a .gguf extension.
    /// @param path Path to check.
    /// @return True if file exists and ends with .gguf.
    static bool IsValidGgufFile(const std::filesystem::path& path);

private:
    GGUFParser() = default;
    ~GGUFParser() = default;
    GGUFParser(const GGUFParser&) = delete;
    GGUFParser& operator=(const GGUFParser&) = delete;

    static QuantType ParseQuantType(uint32_t value);
    static ModelArch ParseArchType(uint32_t value);
};

} // namespace inferdeck::core
