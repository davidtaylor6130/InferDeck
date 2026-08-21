#ifdef _WIN32

namespace {

std::wstring widen(const std::string& value) {
    if (value.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), size);
    return result;
}

struct InternetHandle {
    HINTERNET value{nullptr};
    InternetHandle() = default;
    InternetHandle(const InternetHandle&) = delete;
    InternetHandle& operator=(const InternetHandle&) = delete;
    InternetHandle(InternetHandle&& other) noexcept : value(other.value) { other.value = nullptr; }
    InternetHandle& operator=(InternetHandle&& other) noexcept {
        if (this != &other) {
            if (value) WinHttpCloseHandle(value);
            value = other.value;
            other.value = nullptr;
        }
        return *this;
    }
    ~InternetHandle() { if (value) WinHttpCloseHandle(value); }
};

struct OpenRequest {
    InternetHandle session;
    InternetHandle connection;
    InternetHandle request;
};

Result<OpenRequest> open_request(const std::string& url, const std::string& token,
                                 const std::optional<std::uint64_t>& range) {
    const std::wstring wide_url = widen(url);
    URL_COMPONENTS components{};
    components.dwStructSize = sizeof(components);
    components.dwHostNameLength = static_cast<DWORD>(-1);
    components.dwUrlPathLength = static_cast<DWORD>(-1);
    components.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(wide_url.c_str(), 0, 0, &components)) {
        return Err<OpenRequest>(ErrorCode::InvalidArgument, "invalid model source URL");
    }
    OpenRequest handles;
    handles.session.value = WinHttpOpen(L"InferDeck/2.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!handles.session.value) return Err<OpenRequest>(ErrorCode::Unavailable, "cannot initialize Windows HTTP");
    if (!WinHttpSetTimeouts(handles.session.value, 10000, 10000, 30000, 30000)) {
        return Err<OpenRequest>(ErrorCode::Unavailable,
                                "cannot configure model source timeouts");
    }
    std::wstring host(components.lpszHostName, components.dwHostNameLength);
    handles.connection.value = WinHttpConnect(handles.session.value, host.c_str(), components.nPort, 0);
    if (!handles.connection.value) return Err<OpenRequest>(ErrorCode::Unavailable, "cannot connect to model source");
    std::wstring path(components.lpszUrlPath, components.dwUrlPathLength);
    if (components.dwExtraInfoLength) path.append(components.lpszExtraInfo, components.dwExtraInfoLength);
    handles.request.value = WinHttpOpenRequest(
        handles.connection.value, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        components.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
    if (!handles.request.value) return Err<OpenRequest>(ErrorCode::Unavailable, "cannot create model source request");
    std::wstring headers = L"Accept: application/json\r\n";
    if (!token.empty()) headers += L"Authorization: Bearer " + widen(token) + L"\r\n";
    if (range && *range > 0) headers += L"Range: bytes=" + std::to_wstring(*range) + L"-\r\n";
    if (!WinHttpSendRequest(handles.request.value, headers.c_str(), static_cast<DWORD>(-1),
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(handles.request.value, nullptr)) {
        return Err<OpenRequest>(ErrorCode::Unavailable, "model source request failed");
    }
    return Ok(std::move(handles));
}

DWORD status_code(HINTERNET request) {
    DWORD status = 0;
    DWORD size = sizeof(status);
    WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
    return status;
}

class WinHttpModelStoreTransport final : public IModelStoreTransport {
public:
    Result<nlohmann::json> get_json(const std::string& url, const std::string& token) override {
        auto handles = open_request(url, token, std::nullopt);
        if (!handles) return Err<nlohmann::json>(handles.error().code, handles.error().message);
        const DWORD status = status_code(handles->request.value);
        if (status < 200 || status >= 300) {
            return Err<nlohmann::json>(status == 404 ? ErrorCode::NotFound : ErrorCode::Unavailable,
                                       "model source responded " + std::to_string(status));
        }
        std::string body;
        std::array<char, 64 * 1024> buffer{};
        while (true) {
            DWORD received = 0;
            if (!WinHttpReadData(handles->request.value, buffer.data(), static_cast<DWORD>(buffer.size()), &received)) {
                return Err<nlohmann::json>(ErrorCode::IoError, "cannot read model source response");
            }
            if (received == 0) break;
            body.append(buffer.data(), received);
            if (body.size() > 32 * 1024 * 1024) return Err<nlohmann::json>(ErrorCode::InvalidArgument, "model metadata is too large");
        }
        try {
            return Ok(nlohmann::json::parse(body));
        } catch (const std::exception& error) {
            return Err<nlohmann::json>(ErrorCode::ParseError, error.what());
        }
    }

    Result<void> download(const std::string& url, const std::string& token,
                          const std::filesystem::path& destination, std::uint64_t offset,
                          const std::function<bool(std::uint64_t)>& progress) override {
        auto handles = open_request(url, token, offset);
        if (!handles) return Err<void>(handles.error().code, handles.error().message);
        const DWORD status = status_code(handles->request.value);
        if (status != 200 && status != 206) {
            return Err<void>(status == 404 ? ErrorCode::NotFound : ErrorCode::Unavailable,
                             "model download responded " + std::to_string(status));
        }
        if (offset > 0 && status == 200) offset = 0;
        std::ofstream output(destination, std::ios::binary |
                            (offset > 0 ? std::ios::app : std::ios::trunc));
        if (!output) return Err<void>(ErrorCode::IoError, "cannot open partial model artifact");
        std::vector<char> buffer(1024 * 1024);
        std::uint64_t total = offset;
        while (true) {
            DWORD received = 0;
            if (!WinHttpReadData(handles->request.value, buffer.data(), static_cast<DWORD>(buffer.size()), &received)) {
                return Err<void>(ErrorCode::IoError, "model download read failed");
            }
            if (received == 0) break;
            output.write(buffer.data(), received);
            if (!output) return Err<void>(ErrorCode::IoError, "model download write failed");
            total += received;
            if (!progress(total)) return Err<void>(ErrorCode::Cancelled, "cancelled");
        }
        output.flush();
        if (!output) return Err<void>(ErrorCode::IoError, "cannot flush partial model artifact");
        return Ok();
    }
};

}

std::unique_ptr<IModelStoreTransport> make_native_model_store_transport() {
    return std::make_unique<WinHttpModelStoreTransport>();
}

#else

namespace {

class UnavailableModelStoreTransport final : public IModelStoreTransport {
public:
    Result<nlohmann::json> get_json(const std::string&, const std::string&) override {
        return Err<nlohmann::json>(ErrorCode::Unavailable, "model store network transport requires Windows");
    }
    Result<void> download(const std::string&, const std::string&, const std::filesystem::path&,
                          std::uint64_t, const std::function<bool(std::uint64_t)>&) override {
        return Err<void>(ErrorCode::Unavailable, "model store network transport requires Windows");
    }
};

}

std::unique_ptr<IModelStoreTransport> make_native_model_store_transport() {
    return std::make_unique<UnavailableModelStoreTransport>();
}

#endif
