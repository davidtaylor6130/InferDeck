/// @file Documents.hpp
/// @brief /v1/documents/* route handlers for RAG document management.

#pragma once

#include <httplib.h>

namespace inferdeck::gateway::routes {

/// Handle GET /v1/documents
void HandleDocumentsList(const httplib::Request& req, httplib::Response& resp);

/// Handle POST /v1/documents
void HandleDocumentsCreate(const httplib::Request& req, httplib::Response& resp);

/// Handle GET /v1/documents/{id}
void HandleDocumentsGet(const httplib::Request& req, httplib::Response& resp);

/// Handle DELETE /v1/documents/{id}
void HandleDocumentsDelete(const httplib::Request& req, httplib::Response& resp);

/// Handle POST /v1/documents/search
void HandleDocumentsSearch(const httplib::Request& req, httplib::Response& resp);

/// Validate document create request.
std::string ValidateDocumentCreate(const nlohmann::json& body);

} // namespace inferdeck::gateway::routes
