std::string mime_type(const fs::path& path) {
    const auto ext = path.extension().string();
    if (ext == ".html") return "text/html; charset=utf-8";
    if (ext == ".js") return "text/javascript; charset=utf-8";
    if (ext == ".css") return "text/css; charset=utf-8";
    if (ext == ".json") return "application/json";
    if (ext == ".svg") return "image/svg+xml";
    if (ext == ".png") return "image/png";
    if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
    if (ext == ".ico") return "image/x-icon";
    if (ext == ".woff2") return "font/woff2";
    return "application/octet-stream";
}

std::string read_file(const fs::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) return {};
    std::ostringstream buf;
    buf << f.rdbuf();
    return buf.str();
}

fs::path executable_dir() {
#ifdef _WIN32
    char module_path[MAX_PATH]{};
    if (GetModuleFileNameA(nullptr, module_path, static_cast<DWORD>(sizeof(module_path))) > 0) {
        return fs::path(module_path).parent_path();
    }
#endif
    return fs::current_path();
}

fs::path find_dashboard_static_dir() {
    std::vector<fs::path> candidates = {
        executable_dir() / "static",
        fs::current_path() / "apps" / "inferdeck-gateway" / "static",
        fs::current_path() / "static",
        executable_dir() / "dashboard"
    };
    for (const auto& candidate : candidates) {
        std::error_code ec;
        if (fs::exists(candidate / "index.html", ec)) return candidate;
    }
    return candidates.front();
}

void write_dashboard_file(httplib::Response& resp, const fs::path& static_dir, const std::string& request_path) {
    fs::path relative = request_path == "/" ? fs::path("index.html") : fs::path(request_path.substr(1));
    std::error_code ec;
    fs::path target = fs::weakly_canonical(static_dir / relative, ec);
    fs::path root = fs::weakly_canonical(static_dir, ec);
    if (ec || !inferdeck::foundation::is_path_within(root, target) ||
        !fs::exists(target, ec) || fs::is_directory(target, ec)) {
        target = static_dir / "index.html";
    }
    const auto body = read_file(target);
    if (body.empty()) {
        resp.status = 404;
        resp.set_content("InferDeck dashboard has not been built. Run npm run build in apps/dashboard.", "text/plain");
        return;
    }
    resp.status = 200;
    resp.set_content(body, mime_type(target));
}
