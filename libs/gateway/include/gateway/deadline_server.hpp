#pragma once

#include <httplib.h>

#include <algorithm>
#include <chrono>
#include <memory>
#include <string>
#include <string_view>

namespace inferdeck::gateway {

inline bool is_transcription_upload_prefix(std::string_view data) {
    constexpr std::string_view target = "POST /v1/audio/transcriptions";
    return data.size() >= target.size() && data.substr(0, target.size()) == target;
}

class UploadDeadlineStream final : public httplib::Stream {
public:
    UploadDeadlineStream(std::unique_ptr<httplib::Stream> inner,
                         std::chrono::milliseconds short_deadline)
        : inner_(std::move(inner)), short_deadline_(short_deadline) {}

    bool is_readable() const override { return inner_->is_readable(); }
    bool wait_readable() const override { return inner_->wait_readable(); }
    bool wait_writable() const override { return inner_->wait_writable(); }
    bool is_peer_alive() const override { return inner_->is_peer_alive(); }
    ssize_t read(char* ptr, size_t size) override {
        if (!upload_ && !sniffed_ && inner_->duration() > short_deadline_.count()) {
            error_ = httplib::Error::Timeout;
            return -1;
        }
        const ssize_t count = inner_->read(ptr, size);
        if (count > 0 && !sniffed_) {
            head_.append(ptr, static_cast<size_t>(count));
            constexpr size_t needed = sizeof("POST /v1/audio/transcriptions") - 1;
            if (head_.size() >= needed || head_.find('\n') != std::string::npos) {
                sniffed_ = true;
                upload_ = is_transcription_upload_prefix(head_);
                head_.clear();
            }
        }
        if (count > 0 && sniffed_ && !upload_ && inner_->duration() > short_deadline_.count()) {
            error_ = httplib::Error::Timeout;
            return -1;
        }
        if (count < 0) error_ = inner_->get_error();
        return count;
    }
    ssize_t write(const char* ptr, size_t size) override { return inner_->write(ptr, size); }
    void get_remote_ip_and_port(std::string& ip, int& port) const override {
        inner_->get_remote_ip_and_port(ip, port);
    }
    void get_local_ip_and_port(std::string& ip, int& port) const override {
        inner_->get_local_ip_and_port(ip, port);
    }
    socket_t socket() const override { return inner_->socket(); }
    time_t duration() const override { return inner_->duration(); }
    void set_read_timeout(time_t sec, time_t usec = 0) override { inner_->set_read_timeout(sec, usec); }

private:
    std::unique_ptr<httplib::Stream> inner_;
    std::chrono::milliseconds short_deadline_;
    std::string head_;
    bool sniffed_{false};
    bool upload_{false};
};

class DeadlineServer final : public httplib::Server {
public:
    explicit DeadlineServer(std::chrono::milliseconds request_read_deadline,
                            std::chrono::milliseconds upload_read_deadline = std::chrono::milliseconds{0})
        : request_read_deadline_(request_read_deadline),
          upload_read_deadline_(std::max(request_read_deadline, upload_read_deadline)) {}

protected:
    bool process_and_close_socket(socket_t socket) override {
        std::string remote_address;
        int remote_port = 0;
        httplib::detail::get_remote_ip_and_port(socket, remote_address, remote_port);
        std::string local_address;
        int local_port = 0;
        httplib::detail::get_local_ip_and_port(socket, local_address, local_port);

        bool websocket_upgraded = false;
        const auto result = httplib::detail::process_server_socket_core(
            svr_sock_, socket, keep_alive_max_count_, keep_alive_timeout_sec_,
            [&](bool close_connection, bool& connection_closed) {
                UploadDeadlineStream stream(
                    std::make_unique<httplib::detail::SocketStream>(
                        socket, read_timeout_sec_, read_timeout_usec_,
                        write_timeout_sec_, write_timeout_usec_,
                        static_cast<time_t>(upload_read_deadline_.count()),
                        std::chrono::steady_clock::now()),
                    request_read_deadline_);
                return process_request(
                    stream, remote_address, remote_port, local_address, local_port,
                    close_connection, connection_closed,
                    [](httplib::Request& request) {
                        if (!request.has_header("Content-Length") &&
                            !request.has_header("Transfer-Encoding")) {
                            request.set_header("Content-Length", "0");
                        }
                    }, &websocket_upgraded);
            });
        drain_and_close(socket);
        return result;
    }

private:
    static void drain_and_close(socket_t socket) noexcept {
#ifdef _WIN32
        ::shutdown(socket, SD_SEND);
#else
        ::shutdown(socket, SHUT_WR);
#endif
        char buffer[CPPHTTPLIB_RECV_BUFSIZ];
        std::size_t total = 0;
        const auto deadline = std::chrono::steady_clock::now() +
            std::chrono::milliseconds{100};
        while (total < 1024ULL * 1024ULL) {
            const auto remaining = std::chrono::duration_cast<std::chrono::microseconds>(
                deadline - std::chrono::steady_clock::now()).count();
            if (remaining <= 0) break;
            if (httplib::detail::select_read(
                    socket, 0, static_cast<time_t>(remaining)) <= 0) break;
            const auto count = httplib::detail::read_socket(
                socket, buffer, sizeof(buffer), CPPHTTPLIB_RECV_FLAGS);
            if (count <= 0) break;
            total += static_cast<std::size_t>(count);
        }
        httplib::detail::shutdown_socket(socket);
        httplib::detail::close_socket(socket);
    }

    std::chrono::milliseconds request_read_deadline_;
    std::chrono::milliseconds upload_read_deadline_;
};

}
