#include "http.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>

#include "util.h"

namespace rf {
namespace http {
namespace {

constexpr size_t kMaxHeaderBytes = 64 * 1024;
constexpr size_t kMaxHeaderCount = 200;
constexpr int kPollSliceMs = 200;  // how often we re-check keep_going / stop

bool set_nonblocking(int fd, bool on) {
    int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0) return false;
    flags = on ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
    return ::fcntl(fd, F_SETFL, flags) == 0;
}

// Write everything or fail. Returns false on error or peer close.
bool write_all(int fd, const char* data, size_t n, int timeout_ms) {
    size_t sent = 0;
    while (sent < n) {
        ssize_t k = ::send(fd, data + sent, n - sent, MSG_NOSIGNAL);
        if (k > 0) { sent += static_cast<size_t>(k); continue; }
        if (k < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct pollfd p{fd, POLLOUT, 0};
            int r = ::poll(&p, 1, timeout_ms);
            if (r <= 0) return false;
            continue;
        }
        if (k < 0 && errno == EINTR) continue;
        return false;
    }
    return true;
}

// Buffered line/blob reader over a socket.
class SockReader {
public:
    SockReader(int fd, int timeout_ms, std::function<bool()> keep_going)
        : fd_(fd), timeout_ms_(timeout_ms), keep_going_(std::move(keep_going)) {}

    bool eof() const { return eof_ && pos_ >= len_; }
    bool aborted() const { return aborted_; }

    // Reads a CRLF- or LF-terminated line, without the terminator.
    bool read_line(std::string& out, size_t max_len) {
        out.clear();
        while (true) {
            for (size_t i = pos_; i < len_; ++i) {
                if (buf_[i] == '\n') {
                    out.append(buf_ + pos_, i - pos_);
                    pos_ = i + 1;
                    if (!out.empty() && out.back() == '\r') out.pop_back();
                    return true;
                }
            }
            out.append(buf_ + pos_, len_ - pos_);
            pos_ = len_ = 0;
            if (out.size() > max_len) return false;
            if (!fill()) return false;
        }
    }

    bool read_exact(size_t n, std::string& out) {
        out.clear();
        out.reserve(n);
        while (out.size() < n) {
            if (pos_ >= len_ && !fill()) return false;
            const size_t take = std::min(n - out.size(), len_ - pos_);
            out.append(buf_ + pos_, take);
            pos_ += take;
        }
        return true;
    }

    // Skips exactly n bytes, handing each block to sink as it arrives.
    bool pump(size_t n, const std::function<bool(const char*, size_t)>& sink) {
        size_t left = n;
        while (left > 0) {
            if (pos_ >= len_ && !fill()) return false;
            const size_t take = std::min(left, len_ - pos_);
            if (sink && !sink(buf_ + pos_, take)) { aborted_ = true; return false; }
            pos_ += take;
            left -= take;
        }
        return true;
    }

    // Reads until the peer closes, handing every block to sink.
    bool pump_to_eof(const std::function<bool(const char*, size_t)>& sink) {
        while (true) {
            if (pos_ >= len_ && !fill()) return eof_ && !aborted_;
            if (sink && !sink(buf_ + pos_, len_ - pos_)) { aborted_ = true; return false; }
            pos_ = len_ = 0;
        }
    }

private:
    bool fill() {
        if (eof_) return false;
        const int64_t deadline = mono_ms() + timeout_ms_;
        while (true) {
            if (keep_going_ && !keep_going_()) { aborted_ = true; return false; }
            struct pollfd p{fd_, POLLIN, 0};
            const int64_t left = deadline - mono_ms();
            if (left <= 0) return false;
            const int slice = static_cast<int>(std::min<int64_t>(left, kPollSliceMs));
            const int r = ::poll(&p, 1, slice);
            if (r == 0) continue;              // slice expired; re-check keep_going
            if (r < 0) { if (errno == EINTR) continue; return false; }
            const ssize_t k = ::recv(fd_, buf_, sizeof buf_, 0);
            if (k > 0) { pos_ = 0; len_ = static_cast<size_t>(k); return true; }
            if (k == 0) { eof_ = true; return false; }
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
            return false;
        }
    }

    int fd_;
    int timeout_ms_;
    std::function<bool()> keep_going_;
    char buf_[16384];
    size_t pos_ = 0, len_ = 0;
    bool eof_ = false;
    bool aborted_ = false;
};

bool parse_headers(SockReader& r, Headers& out, std::string* err) {
    size_t count = 0;
    std::string line;
    while (true) {
        if (!r.read_line(line, kMaxHeaderBytes)) {
            if (err) *err = "header read failed";
            return false;
        }
        if (line.empty()) return true;
        if (++count > kMaxHeaderCount) {
            if (err) *err = "too many headers";
            return false;
        }
        const size_t colon = line.find(':');
        if (colon == std::string::npos) {
            if (err) *err = "malformed header";
            return false;
        }
        out.add(trim(line.substr(0, colon)), trim(line.substr(colon + 1)));
    }
}

// Reads a chunked body, handing decoded blocks to sink.
bool read_chunked(SockReader& r, const std::function<bool(const char*, size_t)>& sink) {
    std::string line;
    while (true) {
        if (!r.read_line(line, 1024)) return false;
        const size_t semi = line.find(';');  // chunk extensions ignored
        if (semi != std::string::npos) line.resize(semi);
        char* end = nullptr;
        const unsigned long size = std::strtoul(trim(line).c_str(), &end, 16);
        if (end == nullptr || *end != '\0') return false;
        if (size == 0) {
            // Trailers, then the final blank line.
            while (r.read_line(line, kMaxHeaderBytes) && !line.empty()) {}
            return true;
        }
        if (!r.pump(size, sink)) return false;
        if (!r.read_line(line, 8) || !line.empty()) return false;  // CRLF after chunk
    }
}

}  // namespace

// --- Headers ----------------------------------------------------------------

void Headers::add(std::string name, std::string value) {
    items_.emplace_back(std::move(name), std::move(value));
}

void Headers::set(const std::string& name, std::string value) {
    for (auto& kv : items_) {
        if (iequals(kv.first, name)) { kv.second = std::move(value); return; }
    }
    items_.emplace_back(name, std::move(value));
}

bool Headers::has(const std::string& name) const {
    for (const auto& kv : items_)
        if (iequals(kv.first, name)) return true;
    return false;
}

std::string Headers::get(const std::string& name, const std::string& def) const {
    for (const auto& kv : items_)
        if (iequals(kv.first, name)) return kv.second;
    return def;
}

// --- Request ----------------------------------------------------------------

std::string Request::param(const std::string& name, const std::string& def) const {
    for (const auto& pair : split(query, '&')) {
        const size_t eq = pair.find('=');
        const std::string key = url_decode(eq == std::string::npos ? pair : pair.substr(0, eq));
        if (key == name)
            return eq == std::string::npos ? std::string() : url_decode(pair.substr(eq + 1));
    }
    return def;
}

bool Request::from_loopback() const {
    return starts_with(peer, "127.") || peer == "::1" || starts_with(peer, "::ffff:127.");
}

// --- helpers ----------------------------------------------------------------

const char* status_text(int status) {
    switch (status) {
        case 200: return "OK";
        case 201: return "Created";
        case 202: return "Accepted";
        case 204: return "No Content";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 408: return "Request Timeout";
        case 409: return "Conflict";
        case 413: return "Payload Too Large";
        case 422: return "Unprocessable Entity";
        case 429: return "Too Many Requests";
        case 499: return "Client Closed Request";
        case 500: return "Internal Server Error";
        case 502: return "Bad Gateway";
        case 503: return "Service Unavailable";
        case 504: return "Gateway Timeout";
        default:  return "Status";
    }
}

std::string url_decode(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '+') { out += ' '; continue; }
        if (s[i] == '%' && i + 2 < s.size()) {
            auto hex = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            const int hi = hex(s[i + 1]), lo = hex(s[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out += static_cast<char>(hi * 16 + lo);
                i += 2;
                continue;
            }
        }
        out += s[i];
    }
    return out;
}

bool parse_endpoint(const std::string& endpoint, std::string& host, uint16_t& port,
                    uint16_t default_port) {
    std::string s = trim(endpoint);
    if (s.empty()) return false;
    if (starts_with(s, "http://")) s = s.substr(7);
    else if (starts_with(s, "https://")) return false;  // TLS is out of scope
    const size_t slash = s.find('/');
    if (slash != std::string::npos) s.resize(slash);
    port = default_port;
    if (!s.empty() && s.front() == '[') {  // [::1]:8080
        const size_t close = s.find(']');
        if (close == std::string::npos) return false;
        host = s.substr(1, close - 1);
        if (close + 1 < s.size() && s[close + 1] == ':')
            port = static_cast<uint16_t>(std::atoi(s.c_str() + close + 2));
    } else {
        const size_t colon = s.rfind(':');
        if (colon != std::string::npos) {
            host = s.substr(0, colon);
            port = static_cast<uint16_t>(std::atoi(s.c_str() + colon + 1));
        } else {
            host = s;
        }
    }
    return !host.empty() && port != 0;
}

void init_process() {
    ::signal(SIGPIPE, SIG_IGN);
}

// --- Responder --------------------------------------------------------------

namespace {

class ResponderImpl : public Responder {
public:
    ResponderImpl(int fd, bool keep_alive, int timeout_ms)
        : fd_(fd), keep_alive_(keep_alive), timeout_ms_(timeout_ms) {}

    void send(int status, const std::string& content_type,
              const std::string& body) override {
        if (responded_) return;
        responded_ = true;
        std::string head = build_status(status);
        head += "Content-Type: " + content_type + "\r\n";
        head += "Content-Length: " + std::to_string(body.size()) + "\r\n";
        head += keep_alive_ ? "Connection: keep-alive\r\n" : "Connection: close\r\n";
        head += "\r\n";
        alive_ = write_all(fd_, head.data(), head.size(), timeout_ms_) &&
                 write_all(fd_, body.data(), body.size(), timeout_ms_);
    }

    void send_json(int status, const std::string& json_text) override {
        send(status, "application/json", json_text);
    }

    void send_error(int status, const std::string& type,
                    const std::string& message) override {
        std::string escaped;
        for (char c : message) {
            if (c == '"' || c == '\\') { escaped += '\\'; escaped += c; }
            else if (c == '\n') escaped += "\\n";
            else if (static_cast<unsigned char>(c) < 0x20) continue;
            else escaped += c;
        }
        send_json(status, "{\"error\":{\"message\":\"" + escaped + "\",\"type\":\"" +
                              type + "\",\"code\":" + std::to_string(status) + "}}");
    }

    bool begin(int status, const Headers& headers) override {
        if (responded_) return false;
        responded_ = true;
        streaming_ = true;
        keep_alive_ = false;  // a stream owns the connection to its end
        std::string head = build_status(status);
        for (const auto& kv : headers.items())
            head += kv.first + ": " + kv.second + "\r\n";
        if (!headers.has("Transfer-Encoding") && !headers.has("Content-Length"))
            head += "Transfer-Encoding: chunked\r\n";
        chunked_ = !headers.has("Content-Length");
        head += "Connection: close\r\n\r\n";
        alive_ = write_all(fd_, head.data(), head.size(), timeout_ms_);
        return alive_;
    }

    bool write(const char* data, size_t n) override {
        if (!alive_ || !streaming_ || n == 0) return alive_;
        if (chunked_) {
            char hdr[32];
            const int hn = std::snprintf(hdr, sizeof hdr, "%zx\r\n", n);
            alive_ = write_all(fd_, hdr, static_cast<size_t>(hn), timeout_ms_) &&
                     write_all(fd_, data, n, timeout_ms_) &&
                     write_all(fd_, "\r\n", 2, timeout_ms_);
        } else {
            alive_ = write_all(fd_, data, n, timeout_ms_);
        }
        return alive_;
    }

    void end() override {
        if (!streaming_ || ended_) return;
        ended_ = true;
        if (alive_ && chunked_) write_all(fd_, "0\r\n\r\n", 5, timeout_ms_);
    }

    bool sse(const std::string& event, const std::string& data) override {
        std::string frame;
        if (!event.empty()) frame = "event: " + event + "\n";
        // Per the SSE grammar every line of the payload needs its own "data:".
        size_t start = 0;
        while (start <= data.size()) {
            const size_t nl = data.find('\n', start);
            const size_t end = (nl == std::string::npos) ? data.size() : nl;
            frame += "data: " + data.substr(start, end - start) + "\n";
            if (nl == std::string::npos) break;
            start = nl + 1;
        }
        frame += "\n";
        return write(frame.data(), frame.size());
    }

    bool alive() const override { return alive_; }
    bool responded() const override { return responded_; }
    bool keep_alive() const { return keep_alive_ && alive_ && !streaming_; }

private:
    std::string build_status(int status) const {
        char buf[64];
        std::snprintf(buf, sizeof buf, "HTTP/1.1 %d %s\r\n", status, status_text(status));
        return buf;
    }

    int fd_;
    bool keep_alive_;
    int timeout_ms_;
    bool responded_ = false;
    bool streaming_ = false;
    bool chunked_ = true;
    bool ended_ = false;
    bool alive_ = true;
};

}  // namespace

// --- Server -----------------------------------------------------------------

struct Server::Impl {
    Handler handler;
    int worker_threads = 32;
    size_t max_body = 32u * 1024 * 1024;
    int listen_fd = -1;
    uint16_t bound_port = 0;
    int wake_pipe[2] = {-1, -1};
    std::atomic<bool> running{false};

    std::mutex mu;
    std::condition_variable cv;
    std::deque<std::pair<int, std::string>> queue;  // fd, peer
    std::vector<std::thread> workers;

    ~Impl() {
        if (listen_fd >= 0) ::close(listen_fd);
        if (wake_pipe[0] >= 0) ::close(wake_pipe[0]);
        if (wake_pipe[1] >= 0) ::close(wake_pipe[1]);
    }

    void serve_connection(int fd, const std::string& peer);
};

void Server::Impl::serve_connection(int fd, const std::string& peer) {
    int one = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);

    bool keep_alive = true;
    while (keep_alive && running.load()) {
        SockReader reader(fd, 120000, [this] { return running.load(); });

        std::string line;
        if (!reader.read_line(line, kMaxHeaderBytes) || line.empty()) break;

        Request req;
        req.peer = peer;
        {
            const size_t s1 = line.find(' ');
            if (s1 == std::string::npos) break;
            const size_t s2 = line.find(' ', s1 + 1);
            if (s2 == std::string::npos) break;
            req.method = line.substr(0, s1);
            req.target = line.substr(s1 + 1, s2 - s1 - 1);
            const std::string version = line.substr(s2 + 1);
            keep_alive = (version == "HTTP/1.1");
        }
        const size_t qm = req.target.find('?');
        req.path = url_decode(qm == std::string::npos ? req.target : req.target.substr(0, qm));
        req.query = qm == std::string::npos ? std::string() : req.target.substr(qm + 1);

        std::string err;
        if (!parse_headers(reader, req.headers, &err)) break;

        const std::string conn = lower(req.headers.get("Connection"));
        if (conn.find("close") != std::string::npos) keep_alive = false;

        // Body.
        if (iequals(req.headers.get("Transfer-Encoding"), "chunked")) {
            bool over = false;
            const bool ok = read_chunked(reader, [&](const char* d, size_t n) {
                if (req.body.size() + n > max_body) { over = true; return false; }
                req.body.append(d, n);
                return true;
            });
            if (over) {
                ResponderImpl r(fd, false, 15000);
                r.send_error(413, "payload_too_large", "request body exceeds limit");
                break;
            }
            if (!ok) break;
        } else if (req.headers.has("Content-Length")) {
            const unsigned long long len =
                std::strtoull(req.headers.get("Content-Length").c_str(), nullptr, 10);
            if (len > max_body) {
                ResponderImpl r(fd, false, 15000);
                r.send_error(413, "payload_too_large", "request body exceeds limit");
                break;
            }
            if (len > 0 && !reader.read_exact(static_cast<size_t>(len), req.body)) break;
        }

        ResponderImpl responder(fd, keep_alive, 60000);
        if (handler) {
            handler(req, responder);
        } else {
            responder.send_error(500, "no_handler", "server has no handler installed");
        }
        responder.end();

        if (!responder.responded()) {
            responder.send_error(500, "no_response", "handler produced no response");
        }
        keep_alive = responder.keep_alive();
    }
    ::close(fd);
}

Server::Server() : impl_(new Impl) {}
Server::~Server() { stop(); }

void Server::set_handler(Handler h) { impl_->handler = std::move(h); }
void Server::set_worker_threads(int n) { impl_->worker_threads = n > 0 ? n : 1; }
void Server::set_max_body_bytes(size_t n) { impl_->max_body = n; }
uint16_t Server::port() const { return impl_->bound_port; }

bool Server::listen(const std::string& bind_addr, uint16_t port, std::string* err) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        if (err) *err = std::string("socket: ") + std::strerror(errno);
        return false;
    }
    int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (::inet_pton(AF_INET, bind_addr.c_str(), &addr.sin_addr) != 1) {
        if (err) *err = "invalid bind address: " + bind_addr;
        ::close(fd);
        return false;
    }
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) {
        if (err) *err = "bind " + bind_addr + ":" + std::to_string(port) + ": " +
                        std::strerror(errno);
        ::close(fd);
        return false;
    }
    if (::listen(fd, 128) != 0) {
        if (err) *err = std::string("listen: ") + std::strerror(errno);
        ::close(fd);
        return false;
    }
    socklen_t len = sizeof addr;
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) == 0)
        impl_->bound_port = ntohs(addr.sin_port);

    if (::pipe(impl_->wake_pipe) != 0) {
        if (err) *err = std::string("pipe: ") + std::strerror(errno);
        ::close(fd);
        return false;
    }
    set_nonblocking(impl_->wake_pipe[0], true);
    impl_->listen_fd = fd;
    return true;
}

void Server::run() {
    Impl& s = *impl_;
    if (s.listen_fd < 0) return;
    s.running.store(true);

    for (int i = 0; i < s.worker_threads; ++i) {
        s.workers.emplace_back([&s] {
            while (true) {
                std::pair<int, std::string> conn{-1, {}};
                {
                    std::unique_lock<std::mutex> lock(s.mu);
                    s.cv.wait(lock, [&s] { return !s.queue.empty() || !s.running.load(); });
                    if (!s.running.load() && s.queue.empty()) return;
                    conn = s.queue.front();
                    s.queue.pop_front();
                }
                if (conn.first >= 0) s.serve_connection(conn.first, conn.second);
            }
        });
    }

    while (s.running.load()) {
        struct pollfd fds[2];
        fds[0] = {s.listen_fd, POLLIN, 0};
        fds[1] = {s.wake_pipe[0], POLLIN, 0};
        const int r = ::poll(fds, 2, kPollSliceMs);
        if (r < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (fds[1].revents & POLLIN) break;  // stop() was called
        if (!(fds[0].revents & POLLIN)) continue;

        sockaddr_in peer{};
        socklen_t plen = sizeof peer;
        const int cfd = ::accept(s.listen_fd, reinterpret_cast<sockaddr*>(&peer), &plen);
        if (cfd < 0) continue;

        char ip[INET_ADDRSTRLEN] = {0};
        ::inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof ip);
        {
            std::lock_guard<std::mutex> lock(s.mu);
            s.queue.emplace_back(cfd, std::string(ip));
        }
        s.cv.notify_one();
    }

    s.running.store(false);
    s.cv.notify_all();
    for (auto& t : s.workers)
        if (t.joinable()) t.join();
    s.workers.clear();
}

void Server::stop() {
    if (!impl_ || !impl_->running.exchange(false)) return;
    if (impl_->wake_pipe[1] >= 0) {
        const char b = 'x';
        ssize_t ignored = ::write(impl_->wake_pipe[1], &b, 1);
        (void)ignored;
    }
    impl_->cv.notify_all();
}

// --- client -----------------------------------------------------------------

namespace {

int connect_with_timeout(const std::string& host, uint16_t port, int timeout_ms,
                         std::string* err) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    const std::string port_str = std::to_string(port);
    const int gai = ::getaddrinfo(host.c_str(), port_str.c_str(), &hints, &res);
    if (gai != 0 || !res) {
        if (err) *err = "resolve " + host + ": " + ::gai_strerror(gai);
        return -1;
    }

    int fd = -1;
    for (addrinfo* ai = res; ai; ai = ai->ai_next) {
        fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;
        set_nonblocking(fd, true);
        int rc = ::connect(fd, ai->ai_addr, ai->ai_addrlen);
        if (rc == 0) break;
        if (errno != EINPROGRESS) { ::close(fd); fd = -1; continue; }

        struct pollfd p{fd, POLLOUT, 0};
        rc = ::poll(&p, 1, timeout_ms);
        if (rc <= 0) {
            if (err) *err = rc == 0 ? "connect timeout" : "connect poll failed";
            ::close(fd);
            fd = -1;
            continue;
        }
        int soerr = 0;
        socklen_t slen = sizeof soerr;
        ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &slen);
        if (soerr != 0) {
            if (err) *err = std::string("connect: ") + std::strerror(soerr);
            ::close(fd);
            fd = -1;
            continue;
        }
        break;
    }
    ::freeaddrinfo(res);
    if (fd < 0 && err && err->empty()) *err = "connect failed";
    if (fd >= 0) {
        int one = 1;
        ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    }
    return fd;
}

struct FdGuard {
    int fd;
    explicit FdGuard(int f) : fd(f) {}
    ~FdGuard() { if (fd >= 0) ::close(fd); }
};

}  // namespace

bool perform(const ClientRequest& req, ClientResponse& out, std::string* err) {
    if (err) err->clear();
    if (req.port == 0) {
        if (err) *err = "no port";
        return false;
    }

    const int fd = connect_with_timeout(req.host, req.port, req.connect_timeout_ms, err);
    if (fd < 0) return false;
    FdGuard guard(fd);

    std::string head = req.method + " " + req.path + " HTTP/1.1\r\n";
    head += "Host: " + req.host + ":" + std::to_string(req.port) + "\r\n";
    for (const auto& kv : req.headers.items())
        head += kv.first + ": " + kv.second + "\r\n";
    if (!req.headers.has("Content-Length") && !req.headers.has("Transfer-Encoding"))
        head += "Content-Length: " + std::to_string(req.body.size()) + "\r\n";
    if (!req.headers.has("Connection")) head += "Connection: close\r\n";
    if (!req.headers.has("Accept-Encoding")) head += "Accept-Encoding: identity\r\n";
    head += "\r\n";

    if (!write_all(fd, head.data(), head.size(), req.connect_timeout_ms) ||
        (!req.body.empty() &&
         !write_all(fd, req.body.data(), req.body.size(), req.read_timeout_ms))) {
        if (err) *err = "send failed";
        return false;
    }

    SockReader reader(fd, req.read_timeout_ms, req.keep_going);

    std::string line;
    if (!reader.read_line(line, kMaxHeaderBytes)) {
        out.aborted = reader.aborted();
        if (err) *err = out.aborted ? "aborted" : "no response";
        return false;
    }
    {
        const size_t s1 = line.find(' ');
        if (s1 == std::string::npos) {
            if (err) *err = "malformed status line";
            return false;
        }
        out.status = std::atoi(line.c_str() + s1 + 1);
    }
    if (!parse_headers(reader, out.headers, err)) return false;

    if (req.on_headers && !req.on_headers(out.status, out.headers)) {
        out.aborted = true;
        return true;  // caller chose to stop; status and headers are valid
    }

    auto sink = [&](const char* d, size_t n) -> bool {
        if (req.on_chunk) return req.on_chunk(d, n);
        out.body.append(d, n);
        return true;
    };

    const bool has_body = !(req.method == "HEAD" || out.status == 204 || out.status == 304);
    bool ok = true;
    if (has_body) {
        if (iequals(out.headers.get("Transfer-Encoding"), "chunked")) {
            ok = read_chunked(reader, sink);
        } else if (out.headers.has("Content-Length")) {
            const unsigned long long len =
                std::strtoull(out.headers.get("Content-Length").c_str(), nullptr, 10);
            ok = len == 0 || reader.pump(static_cast<size_t>(len), sink);
        } else {
            ok = reader.pump_to_eof(sink);
        }
    }

    if (!ok) {
        out.aborted = reader.aborted();
        if (!out.aborted) {
            if (err) *err = "body read failed";
            return false;
        }
    }
    return true;
}

}  // namespace http
}  // namespace rf
