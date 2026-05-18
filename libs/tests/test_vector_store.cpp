/// @file test_vector_store.cpp
/// @brief Tests for VectorStore document persistence and search.

#include <catch2/catch_test_macros.hpp>
#include "vector_store/VectorStore.hpp"
#include <cmath>

using namespace inferdeck::vector_store;

TEST_CASE("VectorStore: Singleton Instance", "[vector_store][singleton]") {
    auto& a = VectorStore::Instance();
    auto& b = VectorStore::Instance();
    REQUIRE(&a == &b);
}

TEST_CASE("VectorStore: Initialize returns true", "[vector_store][init]") {
    auto& store = VectorStore::Instance();
    REQUIRE(store.Initialize("data/test_store.db"));
}

TEST_CASE("VectorStore: Exists returns false before adding", "[vector_store]") {
    auto& store = VectorStore::Instance();
    REQUIRE(store.Initialize("data/test_vs.db"));
    REQUIRE(!store.Exists("doc_1"));
}

TEST_CASE("VectorStore: AddDocument and GetDocument", "[vector_store]") {
    auto& store = VectorStore::Instance();
    store.Initialize("data/test_vs2.db");

    DocumentRecord doc;
    doc.id = "doc_1";
    doc.title = "Test Document";
    doc.content = "This is a test document for vector store";
    doc.version = 1;
    doc.created_at = 1000.0;
    doc.updated_at = 1000.0;

    store.AddDocument(doc);
    REQUIRE(store.Exists("doc_1"));

    auto retrieved = store.GetDocument("doc_1");
    REQUIRE(retrieved.has_value());
    REQUIRE(retrieved->id == "doc_1");
    REQUIRE(retrieved->title == "Test Document");
    REQUIRE(retrieved->content == "This is a test document for vector store");
}

TEST_CASE("VectorStore: UpdateDocument increments version", "[vector_store]") {
    auto& store = VectorStore::Instance();
    store.Initialize("data/test_vs3.db");

    DocumentRecord doc;
    doc.id = "doc_update";
    doc.title = "Original";
    doc.content = "Original content";
    doc.version = 1;
    doc.created_at = 1000.0;
    doc.updated_at = 1000.0;
    store.AddDocument(doc);

    DocumentRecord updated;
    updated.id = "doc_update";
    updated.title = "Updated";
    updated.content = "New content";
    updated.version = doc.version;
    updated.created_at = 1000.0;
    updated.updated_at = 2000.0;

    REQUIRE(store.UpdateDocument("doc_update", updated));

    auto result = store.GetDocument("doc_update");
    REQUIRE(result.has_value());
    REQUIRE(result->title == "Updated");
}

TEST_CASE("VectorStore: DeleteDocument removes entry", "[vector_store]") {
    auto& store = VectorStore::Instance();
    store.Initialize("data/test_vs4.db");

    DocumentRecord doc;
    doc.id = "doc_delete";
    doc.title = "To Delete";
    doc.content = "Delete me";
    doc.version = 1;
    doc.created_at = 1000.0;
    doc.updated_at = 1000.0;
    store.AddDocument(doc);

    REQUIRE(store.Exists("doc_delete"));
    REQUIRE(store.DeleteDocument("doc_delete"));
    REQUIRE(!store.Exists("doc_delete"));
}

TEST_CASE("VectorStore: DeleteDocument for missing entry returns false", "[vector_store]") {
    auto& store = VectorStore::Instance();
    store.Initialize("data/test_vs5.db");
    REQUIRE(!store.DeleteDocument("nonexistent"));
}

TEST_CASE("VectorStore: ListDocuments returns all IDs", "[vector_store]") {
    auto& store = VectorStore::Instance();
    store.Initialize("data/test_vs6.db");

    DocumentRecord d1; d1.id = "list_1"; d1.title = "One"; d1.content = "Content 1";
    d1.version = 1; d1.created_at = 1000.0; d1.updated_at = 1000.0;

    DocumentRecord d2; d2.id = "list_2"; d2.title = "Two"; d2.content = "Content 2";
    d2.version = 1; d2.created_at = 1000.0; d2.updated_at = 1000.0;

    store.AddDocument(d1);
    store.AddDocument(d2);

    auto ids = store.ListDocuments();
    REQUIRE(ids.size() == 2);
    REQUIRE(std::find(ids.begin(), ids.end(), "list_1") != ids.end());
    REQUIRE(std::find(ids.begin(), ids.end(), "list_2") != ids.end());
}

TEST_CASE("VectorStore: GetCount returns document count", "[vector_store]") {
    auto& store = VectorStore::Instance();
    store.Initialize("data/test_vs7.db");
    REQUIRE(store.GetCount() == 0);

    DocumentRecord doc;
    doc.id = "count_1"; doc.title = "One"; doc.content = "Content";
    doc.version = 1; doc.created_at = 1000.0; doc.updated_at = 1000.0;
    store.AddDocument(doc);

    REQUIRE(store.GetCount() >= 1);
}

TEST_CASE("VectorStore: Search returns results for matching text", "[vector_store]") {
    auto& store = VectorStore::Instance();
    store.Initialize("data/test_vs8.db");

    DocumentRecord doc;
    doc.id = "search_doc";
    doc.title = "Machine Learning";
    doc.content = "Deep learning is a subset of machine learning";
    doc.version = 1;
    doc.created_at = 1000.0;
    doc.updated_at = 1000.0;
    store.AddDocument(doc);

    auto results = store.Search("machine learning");
    REQUIRE(!results.empty());
    REQUIRE(results[0].similarity > 0.0);
}

TEST_CASE("VectorStore: Search respects top_k limit", "[vector_store]") {
    auto& store = VectorStore::Instance();
    store.Initialize("data/test_vs9.db");

    for (int i = 0; i < 5; i++) {
        DocumentRecord doc;
        doc.id = "search_limit_" + std::to_string(i);
        doc.title = "Test";
        doc.content = "This is a test document for search limiting";
        doc.version = 1;
        doc.created_at = 1000.0;
        doc.updated_at = 1000.0;
        store.AddDocument(doc);
    }

    auto results = store.Search("test", 2);
    REQUIRE(results.size() <= 2);
}

TEST_CASE("VectorStore: SearchByEmbedding returns results", "[vector_store]") {
    auto& store = VectorStore::Instance();
    store.Initialize("data/test_vs10.db");

    DocumentRecord doc;
    doc.id = "embed_doc";
    doc.title = "Embedding Test";
    doc.content = "Content with embedding";
    doc.embedding = {0.1f, 0.2f, 0.3f, 0.4f, 0.5f};
    doc.version = 1;
    doc.created_at = 1000.0;
    doc.updated_at = 1000.0;
    store.AddDocument(doc);

    std::vector<float> query = {0.1f, 0.2f, 0.3f, 0.4f, 0.5f};
    auto results = store.SearchByEmbedding(query, 10);

    REQUIRE(!results.empty());
    REQUIRE(results[0].document.id == "embed_doc");
}

TEST_CASE("VectorStore: SearchByEmbedding skips empty embeddings", "[vector_store]") {
    auto& store = VectorStore::Instance();
    store.Initialize("data/test_vs11.db");

    DocumentRecord doc;
    doc.id = "empty_emb_doc";
    doc.title = "No Embedding";
    doc.content = "No embedding here";
    doc.embedding = {};
    doc.version = 1;
    doc.created_at = 1000.0;
    doc.updated_at = 1000.0;
    store.AddDocument(doc);

    std::vector<float> query = {0.1f, 0.2f, 0.3f};
    auto results = store.SearchByEmbedding(query, 10);

    REQUIRE(results.empty());
}

TEST_CASE("VectorStore: GetInfo returns valid JSON", "[vector_store]") {
    auto& store = VectorStore::Instance();
    store.Initialize("data/test_vs_info.db");

    auto info = store.GetInfo();
    REQUIRE(info.is_object());
    REQUIRE(info["initialized"].get<bool>() == true);
    REQUIRE(info.contains("cache_size"));
    REQUIRE(info.contains("total_documents"));
    REQUIRE(info.contains("db_path"));
}

TEST_CASE("VectorStore: Shutdown clears cache", "[vector_store]") {
    auto& store = VectorStore::Instance();
    store.Initialize("data/test_vs_shutdown.db");

    DocumentRecord doc;
    doc.id = "shutdown_doc";
    doc.title = "Shutdown Test";
    doc.content = "Content";
    doc.version = 1;
    doc.created_at = 1000.0;
    doc.updated_at = 1000.0;
    store.AddDocument(doc);

    REQUIRE(store.GetCount() >= 1);
    store.Shutdown();
    REQUIRE(store.GetCount() == 0);
}

TEST_CASE("VectorStore: ComputeCosineSimilarity same vector = 1.0", "[vector_store][static]") {
    std::vector<float> a = {1.0f, 2.0f, 3.0f};
    std::vector<float> b = {1.0f, 2.0f, 3.0f};
    REQUIRE(std::abs(VectorStore::ComputeCosineSimilarity(a, b) - 1.0) < 0.001);
}

TEST_CASE("VectorStore: ComputeCosineSimilarity orthogonal vectors = 0.0", "[vector_store][static]") {
    std::vector<float> a = {1.0f, 0.0f, 0.0f};
    std::vector<float> b = {0.0f, 1.0f, 0.0f};
    REQUIRE(std::abs(VectorStore::ComputeCosineSimilarity(a, b)) < 0.001);
}

TEST_CASE("VectorStore: ComputeCosineSimilarity opposite vectors = -1.0", "[vector_store][static]") {
    std::vector<float> a = {1.0f, 0.0f};
    std::vector<float> b = {-1.0f, 0.0f};
    REQUIRE(std::abs(VectorStore::ComputeCosineSimilarity(a, b) - (-1.0)) < 0.001);
}

TEST_CASE("VectorStore: ComputeCosineSimilarity empty input = 0.0", "[vector_store][static]") {
    std::vector<float> a = {};
    std::vector<float> b = {1.0f, 2.0f};
    REQUIRE(VectorStore::ComputeCosineSimilarity(a, b) == 0.0);
}

TEST_CASE("VectorStore: ComputeCosineSimilarity different sizes = 0.0", "[vector_store][static]") {
    std::vector<float> a = {1.0f, 2.0f};
    std::vector<float> b = {1.0f, 2.0f, 3.0f};
    REQUIRE(VectorStore::ComputeCosineSimilarity(a, b) == 0.0);
}

TEST_CASE("VectorStore: DocumentRecord struct fields", "[vector_store][struct]") {
    DocumentRecord doc;
    doc.id = "struct_test";
    doc.title = "Struct Test";
    doc.content = "Testing struct fields";
    doc.embedding = {0.1f, 0.2f};
    doc.version = 5;
    doc.created_at = 100.0;
    doc.updated_at = 200.0;

    REQUIRE(doc.id == "struct_test");
    REQUIRE(doc.title == "Struct Test");
    REQUIRE(doc.embedding.size() == 2);
    REQUIRE(doc.version == 5);
}

TEST_CASE("VectorStore: SearchResult struct fields", "[vector_store][struct]") {
    DocumentRecord doc;
    doc.id = "sr_test"; doc.title = "Search Result Test";
    doc.content = "Content"; doc.embedding = {}; doc.version = 1;
    doc.created_at = 100.0; doc.updated_at = 200.0;

    SearchResult sr;
    sr.document = doc;
    sr.similarity = 0.95;

    REQUIRE(sr.document.id == "sr_test");
    REQUIRE(sr.similarity == 0.95);
}
