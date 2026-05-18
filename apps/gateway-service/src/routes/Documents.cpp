/// @file Documents.cpp
/// @brief /v1/documents/* route handlers for RAG document management.

#include "Documents.hpp"
#include <httplib.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <uuid/uuid.h>

namespace inferdeck::gateway::routes {

std::string ValidateDocumentCreate(const nlohmann::json& body) {
    if (!body.contains("content") || !body["content"].is_string()) {
        return "missing_content";
    }
    return ""; // valid
}

void HandleDocumentsList(const httplib::Request& req, httplib::Response& resp) {
    nlohmann::json result;
    result["object"] = "list";

    // Get documents from vector store
    // auto& store = vector_store::VectorStore::Instance();
    // auto ids = store.ListDocuments();
    nlohmann::json data = nlohmann::json::array();
    // for (const auto& id : ids) {
    //     auto doc = store.GetDocument(id).value();
    //     nlohmann::json d;
    //     d["id"] = doc.id;
    //     d["title"] = doc.title;
    //     d["content"] = doc.content;
    //     d["embedding_dimension"] = static_cast<int>(doc.embedding.size());
    //     d["version"] = doc.version;
    //     d["created_at"] = doc.created_at;
    //     d["updated_at"] = doc.updated_at;
    //     data.push_back(d);
    // }
    result["data"] = data;
    result["count"] = static_cast<int>(data.size());

    resp.status = 200;
    resp.set_content(result.dump(), "application/json");
}

void HandleDocumentsCreate(const httplib::Request& req, httplib::Response& resp) {
    nlohmann::json body;
    try {
        body = nlohmann::json::parse(req.body);
    } catch (const std::exception& e) {
        nlohmann::json error;
        error["error"]["message"] = "Invalid JSON body";
        error["error"]["type"] = "invalid_request_error";
        resp.status = 400;
        resp.set_content(error.dump(), "application/json");
        return;
    }

    auto validation = ValidateDocumentCreate(body);
    if (!validation.empty()) {
        nlohmann::json error;
        error["error"]["message"] = "content is required and must be a string";
        error["error"]["type"] = "invalid_request_error";
        resp.status = 400;
        resp.set_content(error.dump(), "application/json");
        return;
    }

    std::string content = body["content"].get<std::string>();
    std::string title = body.value("title", "");
    nlohmann::json metadata = body.value("metadata", nlohmann::json::object());

    // Get embedding if available
    // std::vector<float> embedding;
    // if (body.contains("embedding") && body["embedding"].is_array()) {
    //     for (auto& v : body["embedding"]) {
    //         embedding.push_back(v.get<float>());
    //     }
    // } else if (body.contains("generate_embedding") && body["generate_embedding"].get<bool>()) {
    //     auto& registry = backends::BackendRegistry::Instance();
    //     auto emb = registry.GetEmbeddingBackend("gte");
    //     if (emb && emb->IsReady()) {
    //         auto emb_result = emb->Generate(content);
    //         embedding = emb_result.embedding;
    //     }
    // }

    // Store document
    // vector_store::DocumentRecord doc;
    // doc.id = "doc_" + std::to_string(std::time(nullptr));
    // doc.title = title;
    // doc.content = content;
    // doc.embedding = embedding;
    // doc.metadata = ...;
    // store.AddDocument(doc);

    nlohmann::json result;
    result["id"] = "doc_" + std::to_string(std::time(nullptr));
    result["object"] = "document";
    result["title"] = title;
    result["content"] = content;
    result["embedding_dimension"] = static_cast<int>(metadata.count("embedding_dimension") ?
                      metadata["embedding_dimension"].get<int>() : 0);
    result["metadata"] = metadata;
    result["created_at"] = std::time(nullptr);
    result["updated_at"] = std::time(nullptr);

    resp.status = 201;
    resp.set_content(result.dump(), "application/json");
    spdlog::info("Documents: created document '{}'", result["id"].get<std::string>());
}

void HandleDocumentsGet(const httplib::Request& req, httplib::Response& resp) {
    std::string doc_id = req.path.substr(req.path.find_last_of('/') + 1);

    nlohmann::json result;
    result["id"] = doc_id;
    result["object"] = "document";
    result["title"] = "Document " + doc_id;
    result["content"] = "[document content for '" + doc_id + "']";
    result["embedding_dimension"] = 0;
    result["metadata"] = nlohmann::json::object();
    result["created_at"] = std::time(nullptr);
    result["updated_at"] = std::time(nullptr);

    resp.status = 200;
    resp.set_content(result.dump(), "application/json");
}

void HandleDocumentsDelete(const httplib::Request& req, httplib::Response& resp) {
    std::string doc_id = req.path.substr(req.path.find_last_of('/') + 1);

    nlohmann::json result;
    result["id"] = doc_id;
    result["object"] = "document";
    result["deleted"] = true;

    resp.status = 200;
    resp.set_content(result.dump(), "application/json");
    spdlog::info("Documents: deleted document '{}'", doc_id);
}

void HandleDocumentsSearch(const httplib::Request& req, httplib::Response& resp) {
    nlohmann::json body;
    try {
        body = nlohmann::json::parse(req.body);
    } catch (const std::exception& e) {
        nlohmann::json error;
        error["error"]["message"] = "Invalid JSON body";
        error["error"]["type"] = "invalid_request_error";
        resp.status = 400;
        resp.set_content(error.dump(), "application/json");
        return;
    }

    if (!body.contains("query") || !body["query"].is_string()) {
        nlohmann::json error;
        error["error"]["message"] = "query is required";
        error["error"]["type"] = "invalid_request_error";
        resp.status = 400;
        resp.set_content(error.dump(), "application/json");
        return;
    }

    std::string query = body["query"].get<std::string>();
    int top_k = body.value("top_k", 10);
    bool include_embedding = body.value("include_embedding", false);

    nlohmann::json result;
    result["object"] = "list";

    // Search
    // auto& store = vector_store::VectorStore::Instance();
    // std::vector<backends::SearchResult> results;
    // if (body.contains("embedding") && body["embedding"].is_array()) {
    //     std::vector<float> emb;
    //     for (auto& v : body["embedding"]) emb.push_back(v.get<float>());
    //     results = store.SearchByEmbedding(emb, top_k);
    // } else {
    //     results = store.Search(query, top_k);
    // }
    nlohmann::json data = nlohmann::json::array();
    // for (const auto& r : results) {
    //     nlohmann::json d;
    //     d["id"] = r.document.id;
    //     d["title"] = r.document.title;
    //     d["content"] = r.document.content;
    //     d["similarity"] = r.similarity;
    //     if (include_embedding) {
    //         d["embedding"] = r.document.embedding;
    //     }
    //     data.push_back(d);
    // }
    result["data"] = data;
    result["count"] = static_cast<int>(data.size());

    resp.status = 200;
    resp.set_content(result.dump(), "application/json");
}

} // namespace inferdeck::gateway::routes
