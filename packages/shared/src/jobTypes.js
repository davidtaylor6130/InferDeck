/**
 * Job runtime types internal to the scheduler.
 */
export function inferClientOrigin(headers) {
    const userAgent = headers["user-agent"] ?? headers["User-Agent"] ?? "";
    if (userAgent.includes("Open WebUI") || userAgent.includes("open-webui"))
        return "open_webui";
    if (userAgent.includes("opencode"))
        return "opencode";
    if (userAgent.includes("n8n"))
        return "n8n";
    if (userAgent.includes("ai-homelab-dashboard") || userAgent.includes("r9700"))
        return "dashboard";
    if (headers["x-api-key"] ?? headers["X-API-Key"])
        return "direct_api";
    return "script";
}
//# sourceMappingURL=jobTypes.js.map