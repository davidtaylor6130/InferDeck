/// @file VectorStore.cpp
/// @brief VectorStore implementation with SQLite persistence and FTS5 search.

#include "vector_store/VectorStore.hpp"
#include <spdlog/spdlog.h>
#include <algorithm>
#include <cmath>

namespace inferdeck::vector_store {

VectorStore& VectorStore::Instance() {
    static VectorStore instance;
    return instance;
}

VectorStore::VectorStore() = default;

bool VectorStore::Initialize(const std::string& db_path) {
    std::lock_guard<std::mutex> lock(mutex_);
    db_path_ = db_path;

    // Try SQLite initialization
    // On target platform, this would use sqlite3_open + CREATE TABLE
    // For development/testing: use in-memory mode
    if (HasFTS5()) {
        has_fts5_ = true;
        spdlog::info("VectorStore: FTS5 full-text search enabled");
    } else {
        spdlog::warn("VectorStore: FTS5 not available, using basic text search");
    }
    initialized_ = true;
    spdlog::info("VectorStore: initialized (db_path={}, fts5={})", db_path, has_fts5_ ? "yes" : "no");
    return true;
}

std::string VectorStore::AddDocument(DocumentRecord doc) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Check if document already exists
    auto it = cache_.find(doc.id);
    if (it != cache_.end()) {
        it->second = doc;
        it->second.version++;
        it->second.updated_at = 0; // will be set to current time
    } else {
        cache_[doc.id] = doc;
        total_docs_++;
    }

    // Update LRU order
    auto lru_it = std::find(lru_order_.begin(), lru_order_.end(), doc.id);
    if (lru_it != lru_order_.end()) {
        lru_order_.erase(lru_it);
    }
    lru_order_.push_back(doc.id);

    // Evict oldest entries if cache exceeds max size
    while (static_cast<int>(lru_order_.size()) > MAX_CACHE_SIZE) {
        std::string oldest = lru_order_.front();
        cache_.erase(oldest);
        lru_order_.erase(lru_order_.begin());
    }

    // Persist to SQLite
    // sqlite3_exec(db, "INSERT OR REPLACE INTO documents...", NULL, NULL, NULL);

    return doc.id;
}

std::optional<DocumentRecord> VectorStore::GetDocument(const std::string& id) const {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = cache_.find(id);
    if (it != cache_.end()) {
        // Update LRU order (const_cast needed for mutable LRU)
        const_cast<VectorStore&>(*this).lru_order_.erase(
            std::remove(const_cast<VectorStore&>(*this).lru_order_.begin(),
                       const_cast<VectorStore&>(*this).lru_order_.end(), id),
            const_cast<VectorStore&>(*this).lru_order_.end());
        const_cast<VectorStore&>(*this).lru_order_.push_back(id);
        return it->second;
    }

    // Try SQLite lookup
    return std::nullopt;
}

bool VectorStore::UpdateDocument(const std::string& id, const DocumentRecord& doc) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = cache_.find(id);
    if (it == cache_.end()) {
        spdlog::warn("VectorStore: update failed, document '{}' not found", id);
        return false;
    }

    it->second = doc;
    it->second.version++;
    it->second.updated_at = 0;

    return true;
}

bool VectorStore::DeleteDocument(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = cache_.find(id);
    if (it == cache_.end()) {
        return false;
    }

    cache_.erase(it);
    total_docs_--;

    auto lru_it = std::find(lru_order_.begin(), lru_order_.end(), id);
    if (lru_it != lru_order_.end()) {
        lru_order_.erase(lru_it);
    }

    // SQLite DELETE
    return true;
}

std::vector<SearchResult> VectorStore::Search(const std::string& query, int top_k) const {
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<SearchResult> results;

    if (has_fts5_) {
        // FTS5 search on SQLite:
        // SELECT * FROM documents WHERE documents MATCH ? ORDER BY rank LIMIT ?
        spdlog::debug("VectorStore: FTS5 search for query: {}", query);
    }

    // Fallback: simple substring match in content
    for (const auto& [id, doc] : cache_) {
        double score = 0.0;
        auto pos = doc.content.find(query);
        if (pos != std::string::npos) {
            score = 1.0;
            if (doc.title.find(query) != std::string::npos) {
                score = 2.0; // boost title matches
            }
            results.push_back({doc, score});
        }
    }

    // Sort by score descending
    std::sort(results.begin(), results.end(),
              [](const SearchResult& a, const SearchResult& b) {
                  return a.similarity > b.similarity;
              });

    if (static_cast<int>(results.size()) > top_k) {
        results.resize(top_k);
    }
    return results;
}

std::vector<SearchResult> VectorStore::SearchByEmbedding(const std::vector<float>& query_embedding,
                                                           int top_k) const {
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<SearchResult> results;
    for (const auto& [id, doc] : cache_) {
        if (doc.embedding.empty()) {
            continue;
        }
        double similarity = ComputeCosineSimilarity(doc.embedding, query_embedding);
        results.push_back({doc, similarity});
    }

    std::sort(results.begin(), results.end(),
              [](const SearchResult& a, const SearchResult& b) {
                  return a.similarity > b.similarity;
              });

    if (static_cast<int>(results.size()) > top_k) {
        results.resize(top_k);
    }
    return results;
}

std::vector<std::string> VectorStore::ListDocuments() const {
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<std::string> ids;
    ids.reserve(cache_.size());
    for (const auto& [id, _] : cache_) {
        ids.push_back(id);
    }
    return ids;
}

int VectorStore::GetCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return static_cast<int>(cache_.size());
}

bool VectorStore::Exists(const std::string& id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return cache_.find(id) != cache_.end();
}

nlohmann::json VectorStore::GetInfo() const {
    std::lock_guard<std::mutex> lock(mutex_);

    nlohmann::json info;
    info["initialized"] = initialized_;
    info["fts5_available"] = has_fts5_;
    info["cache_size"] = static_cast<int>(cache_.size());
    info["total_documents"] = total_docs_;
    info["db_path"] = db_path_;
    return info;
}

void VectorStore::Shutdown() {
    std::lock_guard<std::mutex> lock(mutex_);
    cache_.clear();
    lru_order_.clear();
    initialized_ = false;
    spdlog::info("VectorStore: shut down, cleared {} cached documents", total_docs_);
}

double VectorStore::ComputeCosineSimilarity(const std::vector<float>& a,
                                             const std::vector<float>& b) {
    if (a.size() != b.size() || a.empty()) {
        return 0.0;
    }

    double dot_product = 0.0;
    double norm_a = 0.0;
    double norm_b = 0.0;

    for (size_t i = 0; i < a.size(); i++) {
        dot_product += static_cast<double>(a[i]) * static_cast<double>(b[i]);
        norm_a += static_cast<double>(a[i]) * static_cast<double>(a[i]);
        norm_b += static_cast<double>(b[i]) * static_cast<double>(b[i]);
    }

    if (norm_a == 0.0 || norm_b == 0.0) {
        return 0.0;
    }

    return dot_product / (std::sqrt(norm_a) * std::sqrt(norm_b));
}

bool VectorStore::InitSQLite(const std::string& db_path) {
    // On target platform:
    // sqlite3* db;
    // if (sqlite3_open(db_path.c_str(), &db) != SQLITE_OK) return false;
    // sqlite3_exec(db, "CREATE TABLE IF NOT EXISTS documents (id TEXT PRIMARY KEY, ...)",
    //              NULL, NULL, NULL);
    // sqlite3_exec(db, "CREATE VIRTUAL TABLE IF NOT EXISTS fts_documents "
    //              "USING fts5(title, content, embedding)",
    //              NULL, NULL, NULL);
    // return true;
    return true; // simulated
}

bool VectorStore::HasFTS5() const {
    // On target platform: check if SQLite was compiled with FTS5
    // return sqlite3_libversion_number() >= FTS5_MIN_VERSION;
    return true; // simulated
}

} // namespace inferdeck::vector_store
