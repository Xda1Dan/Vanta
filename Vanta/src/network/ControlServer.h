//
//  ControlServer.h
//  vanta-base
//
//  The GUI's control channel. The dylib binds a loopback HTTP server on
//  127.0.0.1:5553 and the app talks to it with ordinary requests:
//
//      GET  /status                 -> {ok, state, detail, game, armed, queued,
//                                       version, console}
//      POST /execute                -> body is Luau source; answers {status}
//      GET  /console?since=<seq>    -> {records:[{seq,name,message}]}
//      POST /console/input          -> answers an rconsoleinput prompt
//
//  Every route but OPTIONS requires `Authorization: Bearer <token>`. The token
//  is 32 random bytes in ~/Documents/Vanta/.vanta-token (mode 0600), written
//  before the listener starts. Loopback is not a boundary: any process on the
//  machine can reach 127.0.0.1, and a browser page can POST to it from a
//  cross-origin form. A wildcard CORS header used to make that a one-click
//  "run this script" gadget; it is gone, and the token is what closes the rest.
//
//  HTTP instead of a Unix socket: one listener, explicit request boundaries,
//  ordinary status codes, and no stale socket file to unlink after a crash.
//
#pragma once

#include <arpa/inet.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/stat.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>

#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "Console.h"
#include "Filesystem.h"
#include "Journal.h"
#include "Socket.h"
#include "Status.h"
#include "Version.h"

namespace control {

inline constexpr int kPort = 5553;
inline constexpr size_t kMaxRequest = 65536;
inline constexpr size_t kMaxBody = 16u << 20;
inline std::atomic<bool> running{false};

inline bool writeAll(int fd, const void* data, size_t size)
{
    const char* bytes = static_cast<const char*>(data);
    while (size > 0) {
        const ssize_t written = ::write(fd, bytes, size);
        if (written <= 0)
            return false;
        bytes += written;
        size -= static_cast<size_t>(written);
    }
    return true;
}

// One token per process. Never journal it - the journal is world-readable.
inline const std::string& authToken()
{
    static const std::string token = [] {
        unsigned char bytes[32];
        arc4random_buf(bytes, sizeof(bytes));
        static const char* digits = "0123456789abcdef";
        std::string out;
        out.reserve(sizeof(bytes) * 2);
        for (unsigned char byte : bytes) {
            out.push_back(digits[byte >> 4]);
            out.push_back(digits[byte & 0x0f]);
        }
        return out;
    }();
    return token;
}

inline std::string tokenPath()
{
    return (workspace::root().parent_path() / ".vanta-token").string();
}

// Written to a temporary file and renamed, so a UI polling while the new
// process starts never reads a truncated token.
inline void writeTokenFile()
{
    const std::string path = tokenPath();
    const std::string temporary = path + ".tmp";
    const std::string& token = authToken();
    const int fd = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        journal::writef("HTTP could not create %s (errno %d); the GUI cannot authenticate",
                        temporary.c_str(), errno);
        return;
    }
    // O_CREAT's mode is ignored when the file already exists.
    ::fchmod(fd, 0600);
    const bool wrote = writeAll(fd, token.data(), token.size());
    ::fsync(fd);
    ::close(fd);
    if (!wrote || ::rename(temporary.c_str(), path.c_str()) != 0) {
        journal::writef("HTTP could not write the token (errno %d)", errno);
        ::unlink(temporary.c_str());
    }
}

// True when the request carries our bearer token. Constant-time compare; the
// token is not a password but there is no reason to leak it through timing.
inline bool authorized(const std::string& head)
{
    std::string lower = head;
    for (char& c : lower)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    const std::string key = "authorization:";
    size_t at = lower.find(key);
    if (at == std::string::npos)
        return false;
    size_t i = at + key.size();
    while (i < lower.size() && (lower[i] == ' ' || lower[i] == '\t')) ++i;

    const std::string scheme = "bearer ";
    if (lower.compare(i, scheme.size(), scheme) != 0)
        return false;
    i += scheme.size();

    const size_t end = lower.find("\r\n", i);
    const std::string given = lower.substr(
        i, end == std::string::npos ? std::string::npos : end - i);
    const std::string& expected = authToken();
    if (given.size() != expected.size())
        return false;
    unsigned char diff = 0;
    for (size_t k = 0; k < given.size(); ++k)
        diff |= static_cast<unsigned char>(given[k] ^ expected[k]);
    return diff == 0;
}

inline void respond(int fd, int code, const std::string& body,
                    const char* type = "application/json")
{
    const char* phrase = code == 200 ? "OK"
                      : code == 204 ? "No Content"
                      : code == 400 ? "Bad Request"
                      : code == 404 ? "Not Found"
                      : code == 401 ? "Unauthorized"
                      : code == 405 ? "Method Not Allowed"
                      : code == 413 ? "Payload Too Large"
                      : code == 429 ? "Too Many Requests"
                      : "Error";
    std::string head = "HTTP/1.1 " + std::to_string(code) + " " + phrase + "\r\n";
    head += "Content-Type: ";
    head += type;
    head += "\r\nContent-Length: " + std::to_string(body.size()) + "\r\n";
    head += "Cache-Control: no-store\r\n";
    head += "X-Content-Type-Options: nosniff\r\n";
    head += "Connection: close\r\n\r\n";
    writeAll(fd, head.data(), head.size());
    if (!body.empty())
        writeAll(fd, body.data(), body.size());
}

inline size_t querySince(const std::string& path)
{
    const std::string key = "since=";
    const size_t at = path.find(key);
    if (at == std::string::npos)
        return 0;
    uint64_t value = 0;
    for (size_t i = at + key.size(); i < path.size(); ++i) {
        const char c = path[i];
        if (c < '0' || c > '9')
            break;
        value = value * 10 + static_cast<uint64_t>(c - '0');
    }
    return static_cast<size_t>(value);
}

inline bool enqueue(const std::string& script, size_t& queueDepth)
{
    {
        std::lock_guard<std::mutex> lock(execsock::g_mutex);
        if (execsock::g_queue.size() >= execsock::kMaxQueued) {
            queueDepth = execsock::g_queue.size();
            return false;
        }
        execsock::g_queue.push(script);
        queueDepth = execsock::g_queue.size();
    }
    execsock::g_arrived.notify_one();
    return true;
}

inline void handleClient(int fd)
{
    std::string request;
    char chunk[4096];
    while (request.find("\r\n\r\n") == std::string::npos
           && request.size() < kMaxRequest) {
        const ssize_t n = ::recv(fd, chunk, sizeof(chunk), 0);
        if (n <= 0) {
            ::close(fd);
            return;
        }
        request.append(chunk, static_cast<size_t>(n));
    }

    const size_t end = request.find("\r\n\r\n");
    if (end == std::string::npos) {
        respond(fd, 400, "{\"error\":\"bad request\"}");
        ::close(fd);
        return;
    }

    const std::string head = request.substr(0, end);
    const size_t lineEnd = head.find("\r\n");
    const std::string line = head.substr(0, lineEnd == std::string::npos
                                             ? head.size() : lineEnd);
    const size_t firstSpace = line.find(' ');
    const size_t secondSpace = firstSpace == std::string::npos
                             ? std::string::npos : line.find(' ', firstSpace + 1);
    if (firstSpace == std::string::npos || secondSpace == std::string::npos) {
        respond(fd, 400, "{\"error\":\"bad request line\"}");
        ::close(fd);
        return;
    }

    const std::string method = line.substr(0, firstSpace);
    const std::string path = line.substr(firstSpace + 1, secondSpace - firstSpace - 1);

    // Preflight is answered before the token check so a browser learns there
    // is no CORS grant at all, rather than reading a 401 as an auth prompt.
    if (method == "OPTIONS") {
        respond(fd, 204, "");
        ::close(fd);
        return;
    }
    if (!authorized(head)) {
        journal::writef("HTTP rejected an unauthenticated %s %s", method.c_str(), path.c_str());
        respond(fd, 401, "{\"error\":\"unauthorized\"}");
        ::close(fd);
        return;
    }

    size_t contentLength = 0;
    {
        std::string lower = head;
        for (char& c : lower)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        const std::string key = "content-length:";
        const size_t at = lower.find(key);
        if (at != std::string::npos) {
            size_t i = at + key.size();
            while (i < lower.size() && (lower[i] == ' ' || lower[i] == '\t')) ++i;
            while (i < lower.size() && lower[i] >= '0' && lower[i] <= '9') {
                contentLength = contentLength * 10
                              + static_cast<size_t>(lower[i] - '0');
                ++i;
            }
        }
    }

    if (contentLength > kMaxBody) {
        respond(fd, 413, "{\"error\":\"script too large\"}");
        ::close(fd);
        return;
    }

    std::string body = request.substr(end + 4);
    while (body.size() < contentLength) {
        const ssize_t n = ::recv(fd, chunk,
                                 std::min(sizeof(chunk), contentLength - body.size()), 0);
        if (n <= 0) {
            ::close(fd);
            return;
        }
        body.append(chunk, static_cast<size_t>(n));
    }
    body.resize(contentLength);

    if (method == "GET" && path == "/status") {
        size_t depth = 0;
        {
            std::lock_guard<std::mutex> lock(execsock::g_mutex);
            depth = execsock::g_queue.size();
        }
        depth += runtime::queuedScripts.load(std::memory_order_acquire);

        // The listener is up before the environment is, so the state field,
        // not the 200, is what tells the GUI whether a script will run now,
        // later, or never. `game` is the last answer the bootstrap gave to
        // game:IsLoaded() + LocalPlayer on Heartbeat.
        std::string json = "{\"ok\":true,\"state\":\"";
        json += runtime::name(runtime::state());
        json += "\",\"detail\":\"";
        json += console::escape(runtime::detail());
        json += "\",\"game\":";
        json += runtime::gameReady.load(std::memory_order_acquire) ? "true" : "false";
        json += ",\"armed\":";
        json += runtime::pumpArmed.load(std::memory_order_acquire) ? "true" : "false";
        json += ",\"queued\":";
        json += std::to_string(depth);
        json += ",\"console\":";
        json += std::to_string(console::lastSeq());
        json += ",\"version\":\"";
        json += console::escape(VANTA_VERSION_STR);
        json += "\"}";
        respond(fd, 200, json);
    } else if (method == "POST" && path == "/execute") {
        if (body.empty()) {
            respond(fd, 400, "{\"error\":\"empty script\"}");
        } else {
            size_t depth = 0;
            if (!enqueue(body, depth)) {
                respond(fd, 429, "{\"error\":\"queue full\",\"queued\":"
                       + std::to_string(depth) + "}");
            } else {
                journal::writef("HTTP queued %zu byte script", body.size());
                respond(fd, 200, "{\"status\":\"queued\",\"bytes\":"
                       + std::to_string(body.size()) + "}");
            }
        }
    } else if (method == "GET" && path.rfind("/console", 0) == 0) {
        respond(fd, 200, console::recordsJson(querySince(path)));
    } else if (method == "POST" && path == "/console/input") {
        std::ofstream file(console::inputPath(), std::ios::binary | std::ios::trunc);
        if (!file) {
            respond(fd, 500, "{\"error\":\"could not open input file\"}");
        } else {
            file << body;
            respond(fd, 200, "{\"status\":\"ok\"}");
        }
    } else {
        respond(fd, 404, "{\"error\":\"not found\"}");
    }

    ::close(fd);
}

inline void listener()
{
    const int server = ::socket(AF_INET, SOCK_STREAM, 0);
    if (server < 0) {
        journal::writef("HTTP could not create a socket (errno %d)", errno);
        return;
    }
    int on = 1;
    ::setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

    sockaddr_in address{};
    address.sin_len = sizeof(address);
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<uint16_t>(kPort));
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::bind(server, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
        journal::writef("HTTP could not bind 127.0.0.1:%d (errno %d)", kPort, errno);
        ::close(server);
        return;
    }
    if (::listen(server, 32) < 0) {
        journal::writef("HTTP could not listen (errno %d)", errno);
        ::close(server);
        return;
    }

    running.store(true, std::memory_order_release);
    journal::writef("HTTP listening on 127.0.0.1:%d", kPort);
    printf("[vanta] control server on http://127.0.0.1:%d\n", kPort);
    fflush(stdout);

    while (true) {
        const int client = ::accept(server, nullptr, nullptr);
        if (client < 0) {
            if (errno == EINTR || errno == ECONNABORTED)
                continue;
            if (errno == EMFILE || errno == ENFILE || errno == ENOBUFS
                || errno == ENOMEM) {
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                continue;
            }
            journal::writef("HTTP stopped listening (errno %d)", errno);
            ::close(server);
            return;
        }
        int onClient = 1;
        ::setsockopt(client, SOL_SOCKET, SO_NOSIGPIPE, &onClient, sizeof(onClient));
        timeval deadline{5, 0};
        ::setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &deadline, sizeof(deadline));
        ::setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &deadline, sizeof(deadline));
        std::thread(handleClient, client).detach();
    }
}

inline void start()
{
    writeTokenFile();
    std::thread(listener).detach();
}

}  // namespace control
