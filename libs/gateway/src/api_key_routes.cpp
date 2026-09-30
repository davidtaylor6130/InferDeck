#include "gateway/api_key_routes.hpp"

#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "gateway/routes.hpp"

namespace inferdeck::gateway {
namespace {

nlohmann::json api_key_json(const ApiKeyRecord& record) {
    nlohmann::json body = {
        {"id", record.id},
        {"name", record.name},
        {"prefix", record.prefix},
        {"priority", record.priority},
        {"createdAtUnixMs", record.created_at_unix_ms},
        {"updatedAtUnixMs", record.updated_at_unix_ms},
        {"revokedAtUnixMs", nullptr},
    };
    if (record.revoked_at_unix_ms) {
        body["revokedAtUnixMs"] = *record.revoked_at_unix_ms;
    }
    return body;
}

int error_status(foundation::ErrorCode code) {
    switch (code) {
    case foundation::ErrorCode::InvalidArgument:
        return 400;
    case foundation::ErrorCode::NotFound:
        return 404;
    case foundation::ErrorCode::Unavailable:
    case foundation::ErrorCode::IoError:
        return 503;
    default:
        return 500;
    }
}

void write_store_error(httplib::Response& resp,
                       const foundation::Error& error) {
    const int status = error_status(error.code);
    const std::string code = status == 400 ? "invalid_api_key" :
        status == 404 ? "api_key_not_found" :
        status == 503 ? "api_key_store_unavailable" : "api_key_error";
    write_error(resp, status, code, error.message,
                error.field.empty() ? nlohmann::json(nullptr) :
                                      nlohmann::json(error.field));
}

std::optional<nlohmann::json> parse_object(const httplib::Request& req,
                                           httplib::Response& resp) {
    const auto body = nlohmann::json::parse(req.body, nullptr, false);
    if (body.is_discarded() || !body.is_object()) {
        write_error(resp, 400, "invalid_api_key",
                    "request body must be a JSON object");
        return std::nullopt;
    }
    return body;
}

bool has_only(const nlohmann::json& body,
              std::initializer_list<std::string_view> names) {
    for (auto item = body.begin(); item != body.end(); ++item) {
        bool allowed = false;
        for (const std::string_view name : names) {
            if (item.key() == name) {
                allowed = true;
                break;
            }
        }
        if (!allowed) return false;
    }
    return true;
}

bool read_priority(const nlohmann::json& body,
                   std::optional<int>& priority) {
    if (!body.contains("priority")) return true;
    const auto& value = body["priority"];
    try {
        if (value.is_number_unsigned()) {
            const auto parsed = value.get<std::uint64_t>();
            if (parsed > 100) return false;
            priority = static_cast<int>(parsed);
            return true;
        }
        if (!value.is_number_integer()) return false;
        const auto parsed = value.get<std::int64_t>();
        if (parsed < -100 || parsed > 100) return false;
        priority = static_cast<int>(parsed);
        return true;
    } catch (...) {
        return false;
    }
}

}

void handle_create_api_key(const httplib::Request& req,
                           httplib::Response& resp,
                           ApiKeyStore& store) {
    const auto body = parse_object(req, resp);
    if (!body) return;
    std::optional<int> priority;
    if (!has_only(*body, {"name", "priority"}) ||
        !body->contains("name") || !(*body)["name"].is_string() ||
        !read_priority(*body, priority)) {
        write_error(resp, 400, "invalid_api_key",
                    "name must be a string and priority must be between -100 and 100");
        return;
    }
    const auto created = store.create(
        (*body)["name"].get<std::string>(), priority.value_or(0));
    if (!created) {
        write_store_error(resp, created.error());
        return;
    }
    nlohmann::json response = api_key_json(created->record);
    response["key"] = created->key;
    resp.set_header("Cache-Control", "no-store");
    resp.set_header("Pragma", "no-cache");
    write_json(resp, 201, response);
}

void handle_list_api_keys(const httplib::Request& req,
                          httplib::Response& resp,
                          const ApiKeyStore& store) {
    (void)req;
    const auto records = store.list();
    if (!records) {
        write_store_error(resp, records.error());
        return;
    }
    nlohmann::json keys = nlohmann::json::array();
    for (const auto& record : *records) keys.push_back(api_key_json(record));
    write_json(resp, 200, {{"apiKeys", std::move(keys)}});
}

void handle_update_api_key(const httplib::Request& req,
                           httplib::Response& resp,
                           ApiKeyStore& store,
                           std::string_view id) {
    const auto body = parse_object(req, resp);
    if (!body) return;
    std::optional<int> priority;
    if (body->empty() || !has_only(*body, {"name", "priority"}) ||
        (body->contains("name") && !(*body)["name"].is_string()) ||
        !read_priority(*body, priority)) {
        write_error(resp, 400, "invalid_api_key",
                    "name or integer priority is required");
        return;
    }
    std::optional<std::string> name;
    if (body->contains("name")) name = (*body)["name"].get<std::string>();
    const auto updated = store.update(id, std::move(name), priority);
    if (!updated) {
        write_store_error(resp, updated.error());
        return;
    }
    write_json(resp, 200, api_key_json(*updated));
}

void handle_revoke_api_key(const httplib::Request& req,
                           httplib::Response& resp,
                           ApiKeyStore& store,
                           std::string_view id) {
    (void)req;
    const auto revoked = store.revoke(id);
    if (!revoked) {
        write_store_error(resp, revoked.error());
        return;
    }
    resp.status = 204;
    resp.body.clear();
}

}
