#pragma once

#include <httplib.h>

#include <chrono>

namespace inferdeck::gateway {

class DeadlineServer final : public httplib::Server {
public:
    explicit DeadlineServer(std::chrono::milliseconds request_read_deadline)
        : request_read_deadline_(request_read_deadline) {}

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
                httplib::detail::SocketStream stream(
                    socket, read_timeout_sec_, read_timeout_usec_,
                    write_timeout_sec_, write_timeout_usec_,
                    static_cast<time_t>(request_read_deadline_.count()),
                    std::chrono::steady_clock::now());
                return process_request(
                    stream, remote_address, remote_port, local_address, local_port,
                    close_connection, connection_closed, nullptr, &websocket_upgraded);
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
};

}
