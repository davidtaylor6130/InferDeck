#pragma once

#include <string_view>

#include <httplib.h>

#include "gateway/api_key_store.hpp"

namespace inferdeck::gateway {

void handle_create_api_key(const httplib::Request& req,
                           httplib::Response& resp,
                           ApiKeyStore& store);
void handle_list_api_keys(const httplib::Request& req,
                          httplib::Response& resp,
                          const ApiKeyStore& store);
void handle_update_api_key(const httplib::Request& req,
                           httplib::Response& resp,
                           ApiKeyStore& store,
                           std::string_view id);
void handle_revoke_api_key(const httplib::Request& req,
                           httplib::Response& resp,
                           ApiKeyStore& store,
                           std::string_view id);

}
