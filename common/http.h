// RouteFlow — dependency-free HTTP/1.1 server and client, with streaming.
//
// Streaming is not an add-on here: the router proxies token streams and must
// measure time-to-first-token, so both sides expose chunk-level callbacks
// rather than only buffered bodies.
//
// POSIX sockets. Linux first (ARCHITECTURE §3).
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace rf {
namespace http {

// --- headers ----------------------------------------------------------------

class Headers {
public:
    void add(std::string name, std::string value);
    void set(const std::string& name, std::string value);  // replaces existing
    bool has(const std::string& name) const;
    std::string get(const std::string& name, const std::string& def = std::string()) const;
    const std::vector<std::pair<std::string, std::string>>& items() const { return items_; }
    void clear() { items_.clear(); }

private:
    std::vector<std::pair<std::string, std::string>> items_;  // names case-insensitive
};

// --- server -----------------------------------------------------------------

struct Request {
    std::string method;
    std::string target;  // raw request target, e.g. "/v1/models?x=1"
    std::string path;    // decoded path
    std::string query;   // raw query string
    Headers headers;
    std::string body;
    std::string peer;    // client address, for logging and localhost checks

    std::string param(const std::string& name, const std::string& def = std::string()) const;
    bool from_loopback() const;
};

// Response writer. Either a single buffered reply, or a stream: begin() then
// write() repeatedly then end().
class Responder {
public:
    virtual ~Responder() = default;

    virtual void send(int status, const std::string& content_type,
                      const std::string& body) = 0;
    virtual void send_json(int status, const std::string& json_text) = 0;
    // Convenience: {"error":{"message":...,"type":...}} in the OpenAI shape.
    virtual void send_error(int status, const std::string& type,
                            const std::string& message) = 0;

    // Chunked streaming. Returns false once the peer is gone.
    virtual bool begin(int status, const Headers& headers) = 0;
    virtual bool write(const char* data, size_t n) = 0;
    bool write(const std::string& s) { return write(s.data(), s.size()); }
    virtual void end() = 0;

    // Server-sent event: "event: <name>\ndata: <payload>\n\n" (name optional).
    virtual bool sse(const std::string& event, const std::string& data) = 0;

    virtual bool alive() const = 0;
    virtual bool responded() const = 0;
};

using Handler = std::function<void(const Request&, Responder&)>;

class Server {
public:
    Server();
    ~Server();
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    void set_handler(Handler h);
    void set_worker_threads(int n);      // default: 32
    void set_max_body_bytes(size_t n);   // default: 32 MiB

    // bind_addr "127.0.0.1" or "0.0.0.0". port 0 picks a free port.
    bool listen(const std::string& bind_addr, uint16_t port, std::string* err);
    uint16_t port() const;

    void run();   // blocks until stop()
    void stop();  // safe from any thread, including a signal handler

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// --- client -----------------------------------------------------------------

struct ClientRequest {
    std::string host = "127.0.0.1";
    uint16_t port = 0;
    std::string method = "GET";
    std::string path = "/";
    Headers headers;
    std::string body;

    int connect_timeout_ms = 3000;
    int read_timeout_ms = 600000;  // long: a cold model load can take minutes

    // Streaming hooks. Both optional; returning false aborts the transfer.
    // When on_chunk is set the body is not buffered into ClientResponse.
    std::function<bool(int status, const Headers&)> on_headers;
    std::function<bool(const char* data, size_t n)> on_chunk;

    // Polled during the transfer; returning false aborts (client disconnected).
    std::function<bool()> keep_going;
};

struct ClientResponse {
    int status = 0;
    Headers headers;
    std::string body;
    bool aborted = false;  // a callback or keep_going stopped us
};

bool perform(const ClientRequest& req, ClientResponse& out, std::string* err);

// --- helpers ----------------------------------------------------------------

const char* status_text(int status);
std::string url_decode(const std::string& s);

// Parses "host:port" / "http://host:port" / bare host. Returns false if empty.
bool parse_endpoint(const std::string& endpoint, std::string& host, uint16_t& port,
                    uint16_t default_port);

// Ignores SIGPIPE. Call once at startup in every executable.
void init_process();

}  // namespace http
}  // namespace rf
