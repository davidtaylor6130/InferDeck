/// @file test_documents.cpp
/// @brief Unit tests for Documents route handlers.

#define CATCH_CONFIG_MAIN
#include <catch2/catch.hpp>
#include "routes/Documents.hpp"
#include <nlohmann/json.hpp>

TEST_CASE("ValidateDocumentCreate accepts valid input", "[route][document]") {
    nlohmann::json body;
    body["content"] = "Document content";

    std::string error = inferdeck::gateway::routes::ValidateDocumentCreate(body);
    REQUIRE(error.empty());
}

TEST_CASE("ValidateDocumentCreate rejects missing content", "[route][document]") {
    nlohmann::json body;

    std::string error = inferdeck::gateway::routes::ValidateDocumentCreate(body);
    REQUIRE(!error.empty());
    REQUIRE(error.find("content") != std::string::npos);
}

TEST_CASE("ValidateDocumentCreate rejects non-string content", "[route][document]") {
    nlohmann::json body;
    body["content"] = 123;

    std::string error = inferdeck::gateway::routes::ValidateDocumentCreate(body);
    REQUIRE(!error.empty());
}

TEST_CASE("HandleDocumentsList returns valid response", "[route][document]") {
    httplib::Request req;
    httplib::Response resp;

    inferdeck::gateway::routes::HandleDocumentsList(req, resp);

    REQUIRE(resp.status == 200);
    nlohmann::json j = nlohmann::json::parse(resp.body);
    REQUIRE(j["object"] == "list");
    REQUIRE(j.contains("data"));
    REQUIRE(j.contains("count"));
    REQUIRE(j["count"].get<int>() >= 0);
}

TEST_CASE("HandleDocumentsCreate accepts valid request", "[route][document]") {
    httplib::Request req;
    nlohmann::json body;
    body["content"] = "Test document content";
    body["title"] = "Test Title";
    nlohmann::json meta;
    meta["embedding_dimension"] = 768;
    body["metadata"] = meta;
    req.body = body.dump();

    httplib::Response resp;
    inferdeck::gateway::routes::HandleDocumentsCreate(req, resp);

    REQUIRE(resp.status == 201);
    nlohmann::json j = nlohmann::json::parse(resp.body);
    REQUIRE(j["object"] == "document");
    REQUIRE(j.contains("id"));
    REQUIRE(j["title"] == "Test Title");
    REQUIRE(j["content"] == "Test document content");
}

TEST_CASE("HandleDocumentsCreate rejects invalid JSON", "[route][document]") {
    httplib::Request req;
    req.body = "not json";

    httplib::Response resp;
    inferdeck::gateway::routes::HandleDocumentsCreate(req, resp);

    REQUIRE(resp.status == 400);
}

TEST_CASE("HandleDocumentsCreate rejects missing content", "[route][document]") {
    httplib::Request req;
    nlohmann::json body;
    body["title"] = "No content";
    req.body = body.dump();

    httplib::Response resp;
    inferdeck::gateway::routes::HandleDocumentsCreate(req, resp);

    REQUIRE(resp.status == 400);
}

TEST_CASE("HandleDocumentsGet returns document", "[route][document]") {
    httplib::Request req;
    req.path = "/v1/documents/doc_123";

    httplib::Response resp;
    inferdeck::gateway::routes::HandleDocumentsGet(req, resp);

    REQUIRE(resp.status == 200);
    nlohmann::json j = nlohmann::json::parse(resp.body);
    REQUIRE(j["id"] == "doc_123");
    REQUIRE(j["object"] == "document");
    REQUIRE(j.contains("title"));
    REQUIRE(j.contains("content"));
}

TEST_CASE("HandleDocumentsDelete returns deleted confirmation", "[route][document]") {
    httplib::Request req;
    req.path = "/v1/documents/doc_456";

    httplib::Response resp;
    inferdeck::gateway::routes::HandleDocumentsDelete(req, resp);

    REQUIRE(resp.status == 200);
    nlohmann::json j = nlohmann::json::parse(resp.body);
    REQUIRE(j["id"] == "doc_456");
    REQUIRE(j["deleted"] == true);
}

TEST_CASE("HandleDocumentsSearch accepts valid query", "[route][document]") {
    httplib::Request req;
    nlohmann::json body;
    body["query"] = "test search";
    body["top_k"] = 5;
    req.body = body.dump();

    httplib::Response resp;
    inferdeck::gateway::routes::HandleDocumentsSearch(req, resp);

    REQUIRE(resp.status == 200);
    nlohmann::json j = nlohmann::json::parse(resp.body);
    REQUIRE(j["object"] == "list");
    REQUIRE(j.contains("data"));
    REQUIRE(j.contains("count"));
}

TEST_CASE("HandleDocumentsSearch rejects missing query", "[route][document]") {
    httplib::Request req;
    nlohmann::json body;
    body["top_k"] = 10;
    req.body = body.dump();

    httplib::Response resp;
    inferdeck::gateway::routes::HandleDocumentsSearch(req, resp);

    REQUIRE(resp.status == 400);
}

TEST_CASE("HandleDocumentsSearch rejects invalid JSON", "[route][document]") {
    httplib::Request req;
    req.body = "bad";

    httplib::Response resp;
    inferdeck::gateway::routes::HandleDocumentsSearch(req, resp);

    REQUIRE(resp.status == 400);
}
