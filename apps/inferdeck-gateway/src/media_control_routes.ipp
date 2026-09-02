std::optional<std::uint64_t> parse_media_route_integer(
    std::string_view text) {
    if (text.empty()) return std::nullopt;
    std::uint64_t value = 0;
    const auto [end, error] =
        std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

void register_media_control_routes(
    httplib::Server& server,
    const inferdeck::gateway::RouteWrapper& wrap,
    const inferdeck::gateway::GatewayDeps& deps) {
    using namespace inferdeck::gateway;

    const GatewayDeps* deps_ptr = &deps;
    server.Post(control_api_pattern("/media/images/generations"),
                wrap([deps_ptr](const httplib::Request& req,
                                httplib::Response& resp) {
        handle_image_generations(req, resp, *deps_ptr);
    }));
    server.Post(control_api_pattern("/media/audio/generations"),
                wrap([deps_ptr](const httplib::Request& req,
                                httplib::Response& resp) {
        handle_audio_generations(req, resp, *deps_ptr);
    }));
    server.Get(control_api_pattern("/media/jobs"),
               wrap([](const httplib::Request&,
                       httplib::Response& resp) {
        write_json(resp, 200, {{"jobs", media_jobs()}});
    }));
    server.Get(control_api_pattern(
                   "/media/jobs/([0-9]+)/outputs/([0-9]+)"),
               wrap([](const httplib::Request& req,
                       httplib::Response& resp) {
        const auto id = parse_media_route_integer(
            req.matches[1].str());
        const auto index = parse_media_route_integer(
            req.matches[2].str());
        if (!id || !index ||
            *index > std::numeric_limits<std::size_t>::max()) {
            write_error(resp, 400, "invalid_media_output",
                        "media job and output identifiers must be integers");
            return;
        }
        auto output = media_job_output(
            *id, static_cast<std::size_t>(*index));
        if (!output) {
            write_error(resp, 404, "media_output_not_found",
                        output.error().message);
            return;
        }
        resp.set_header("Content-Disposition", "inline");
        resp.set_header(
            "Cache-Control", "private, max-age=31536000, immutable");
        resp.set_content(
            std::move(output->body), output->content_type);
    }));
    server.Post(control_api_pattern("/media/jobs/([0-9]+)/cancel"),
                wrap([](const httplib::Request& req,
                        httplib::Response& resp) {
        const auto id = parse_media_route_integer(
            req.matches[1].str());
        if (!id) {
            write_error(resp, 400, "invalid_media_job",
                        "media job identifier must be an integer");
            return;
        }
        auto result = cancel_media_job(*id);
        if (!result) {
            write_error(
                resp,
                result.error().code ==
                        inferdeck::foundation::ErrorCode::NotFound
                    ? 404
                    : 409,
                "media_cancel_failed", result.error().message);
            return;
        }
        write_json(resp, 200, {{"ok", true}});
    }));
}
