/// @file VectorStore.hpp
/// @brief In-memory and SQLite file-backed vector store for RAG documents.

#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <mutex>
#include <optional>
#include <nlohmann/json.hpp>

namespace inferdeck::vector_store {

struct DocumentRecord {
    std::string id;
    std::string title;
    std::string content;
    std::vector<float> embedding;
    std::unordered_map<std::string, std::string> metadata;
    int version;
    double created_at;
    double updated_at;
};

struct SearchResult {
    DocumentRecord document;
    double similarity;
};

class VectorStore {
public:
    static VectorStore& Instance();

    VectorStore(const VectorStore&) = delete;
    VectorStore& operator=(const VectorStore&) = delete;

    /// Initialize the vector store with SQLite file path.
    bool Initialize(const std::string& db_path = "data/vector_store.db");

    /// Add or update a document.
    std::string AddDocument(DocumentRecord doc);

    /// Get a document by ID.
    std::optional<DocumentRecord> GetDocument(const std::string& id) const;

    /// Update an existing document.
    bool UpdateDocument(const std::string& id, const DocumentRecord& doc);

    /// Delete a document.
    bool DeleteDocument(const std::string& id);

    /// Search documents by text query (FTS5).
    std::vector<SearchResult> Search(const std::string& query, int top_k = 10) const;

    /// Search by embedding similarity.
    std::vector<SearchResult> SearchByEmbedding(const std::vector<float>& embedding,
                                                  int top_k = 10) const;

    /// List all document IDs.
    std::vector<std::string> ListDocuments() const;

    /// Get document count.
    int GetCount() const;

    /// Check if a document exists.
    bool Exists(const std::string& id) const;

    /// Get store info as JSON.
    nlohmann::json GetInfo() const;

    /// Shutdown and close database.
    void Shutdown();

    /// Compute cosine similarity between two vectors.
    static double ComputeCosineSimilarity(const std::vector<float>& a, const std::vector<float>& b);

private:
    VectorStore();
    ~VectorStore() = default;

    bool InitSQLite(const std::string& db_path);
    bool HasFTS5() const;

    mutable std::mutex mutex_;
    std::unordered_map<std::string, DocumentRecord> cache_;
    std::string db_path_;
    bool initialized_{false};
    bool has_fts5_{false};
    int total_docs_{0};

    // LRU cache metadata
    mutable std::vector<std::string> lru_order_;
    static constexpr int MAX_CACHE_SIZE = 1000;
};

} // namespace inferdeck::vector_store
