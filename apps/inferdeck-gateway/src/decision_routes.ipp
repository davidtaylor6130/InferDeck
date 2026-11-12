/// <summary>
/// Registers the OpenAI Decisions route and the provider status route.
/// Kept out of main.cpp to respect its size limit.
/// </summary>
void register_decision_routes(
    httplib::Server& server,
    const inferdeck::gateway::RouteWrapper& wrap,
    const inferdeck::gateway::GatewayDeps& deps) {
    using namespace inferdeck::gateway;
    const GatewayDeps* deps_ptr = &deps;
    server.Post(std::string(strict_openai_route(StrictOpenAIRoute::Decisions).pattern),
                wrap([deps_ptr](const httplib::Request& req, httplib::Response& resp) {
        handle_decisions(req, resp, *deps_ptr);
    }));
    server.Get(control_api_pattern("/providers"),
               wrap([](const httplib::Request&, httplib::Response& resp) {
        write_json(resp, 200, g_provider_report);
    }));
}
