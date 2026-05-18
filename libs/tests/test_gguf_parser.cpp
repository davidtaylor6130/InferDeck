/// @file test_gguf_parser.cpp
/// @brief Unit tests for the GGUFParser module (interface validation).

#define CATCH_CONFIG_MAIN
#include <catch2/catch.hpp>
#include "llama_cpp/GGUFParser.hpp"

TEST_CASE("GGUFParser header loads correctly", "[gguf][parser]") {
    // Test that GGUF header structure is correctly sized
    static_assert(sizeof(gguf_meta_header) == 36);
    static_assert(sizeof(gguf_context) >= sizeof(gguf_meta_header));
}

TEST_CASE("GGUF quantization enum values", "[gguf][quant]") {
    REQUIRE(static_cast<int>(inferdeck::llama::Quantization::Q4_0) == 0);
    REQUIRE(static_cast<int>(inferdeck::llama::Quantization::Q4_1) == 1);
    REQUIRE(static_cast<int>(inferdeck::llama::Quantization::Q8_0) == 2);
    REQUIRE(static_cast<int>(inferdeck::llama::Quantization::F16) == 3);
    REQUIRE(static_cast<int>(inferdeck::llama::Quantization::F32) == 4);
    REQUIRE(static_cast<int>(inferdeck::llama::Quantization::Q2_K) == 5);
    REQUIRE(static_cast<int>(inferdeck::llama::Quantization::Q3_K) == 6);
    REQUIRE(static_cast<int>(inferdeck::llama::Quantization::Q4_K) == 7);
    REQUIRE(static_cast<int>(inferdeck::llama::Quantization::Q5_K) == 8);
    REQUIRE(static_cast<int>(inferdeck::llama::Quantization::Q6_K) == 9);
}

TEST_CASE("GGUF metadata defaults to zero", "[gguf][metadata]") {
    inferdeck::llama::GGUFMetadata metadata;
    REQUIRE(metadata.num_tensors == 0);
    REQUIRE(metadata.num_kv_pairs == 0);
    REQUIRE(metadata.quantization == inferdeck::llama::Quantization::Unknown);
    REQUIRE(metadata.precision == inferdeck::llama::Precision::Unknown);
    REQUIRE(metadata.context_length == 0);
    REQUIRE(metadata.embedding_length == 0);
}

TEST_CASE("Quantization to string conversion", "[gguf][string]") {
    REQUIRE(inferdeck::llama::QuantizationToString(inferdeck::llama::Quantization::Q4_0) == "Q4_0");
    REQUIRE(inferdeck::llama::QuantizationToString(inferdeck::llama::Quantization::Q8_0) == "Q8_0");
    REQUIRE(inferdeck::llama::QuantizationToString(inferdeck::llama::Quantization::F16) == "F16");
    REQUIRE(inferdeck::llama::QuantizationToString(inferdeck::llama::Quantization::F32) == "F32");
    REQUIRE(inferdeck::llama::QuantizationToString(inferdeck::llama::Quantization::Unknown) == "UNKNOWN");
}
