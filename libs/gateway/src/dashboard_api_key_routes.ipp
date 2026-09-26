    server.Get(R"(^/api/inferdeck/v1/api-keys$)",
               wrap([deps](const httplib::Request& req,
                           httplib::Response& resp) {
        if (!deps.gw.api_keys) {
            write_error(resp, 503, "api_key_store_unavailable",
                        "API key store is unavailable");
            return;
        }
        handle_list_api_keys(req, resp, *deps.gw.api_keys);
    }));

    server.Post(R"(^/api/inferdeck/v1/api-keys$)",
                wrap([deps](const httplib::Request& req,
                            httplib::Response& resp) {
        if (!deps.gw.api_keys) {
            write_error(resp, 503, "api_key_store_unavailable",
                        "API key store is unavailable");
            return;
        }
        handle_create_api_key(req, resp, *deps.gw.api_keys);
    }));

    server.Patch(R"(^/api/inferdeck/v1/api-keys/([0-9a-f]{32})$)",
                 wrap([deps](const httplib::Request& req,
                             httplib::Response& resp) {
        if (!deps.gw.api_keys) {
            write_error(resp, 503, "api_key_store_unavailable",
                        "API key store is unavailable");
            return;
        }
        handle_update_api_key(req, resp, *deps.gw.api_keys,
                              req.matches[1].str());
    }));

    server.Delete(R"(^/api/inferdeck/v1/api-keys/([0-9a-f]{32})$)",
                  wrap([deps](const httplib::Request& req,
                              httplib::Response& resp) {
        if (!deps.gw.api_keys) {
            write_error(resp, 503, "api_key_store_unavailable",
                        "API key store is unavailable");
            return;
        }
        handle_revoke_api_key(req, resp, *deps.gw.api_keys,
                              req.matches[1].str());
    }));
