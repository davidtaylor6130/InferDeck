    server.Get(R"(^/api/inferdeck/v1/background/availability$)",
               wrap([deps](const httplib::Request& req,
                           httplib::Response& resp) {
        const std::int64_t uptime = deps.uptime_seconds
            ? deps.uptime_seconds() : 0;
        handle_background_availability(req, resp, deps.gw, uptime);
    }));

    server.Post(R"(^/api/inferdeck/v1/background/lease$)",
                wrap([deps](const httplib::Request& req,
                            httplib::Response& resp) {
        const std::int64_t uptime = deps.uptime_seconds
            ? deps.uptime_seconds() : 0;
        handle_acquire_background_lease(req, resp, deps.gw, uptime);
    }));

    server.Patch(R"(^/api/inferdeck/v1/background/lease/([0-9a-f]{32})$)",
                 wrap([deps](const httplib::Request& req,
                             httplib::Response& resp) {
        handle_renew_background_lease(
            req, resp, deps.gw, req.matches[1].str());
    }));

    server.Delete(R"(^/api/inferdeck/v1/background/lease/([0-9a-f]{32})$)",
                  wrap([deps](const httplib::Request& req,
                              httplib::Response& resp) {
        handle_release_background_lease(
            req, resp, deps.gw, req.matches[1].str());
    }));
