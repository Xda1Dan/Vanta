//
//  websocket.hpp
//  vanta-base
//
//  A WebSocket client. macOS ships libcurl 8.7 built without the ws/wss
//  protocols - `curl --version` lists neither - so the handshake and RFC 6455
//  framing are done here, over a connection curl opens in CONNECT_ONLY mode.
//  That still buys the whole TLS story for wss:// (SecureTransport), which the
//  sUNC suite explicitly checks for, without this file knowing anything about
//  certificates.
//
//  Each connection owns a reader thread. Messages it completes are queued for
//  the Lua side to drain, because firing a Roblox event from a foreign thread
//  is not something the engine tolerates.
//

#pragma once

#include <atomic>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <poll.h>

#include <CommonCrypto/CommonDigest.h>
#include <curl/curl.h>

#include "Http.h"
#include "Journal.h"
#include "Pool.h"
#include "lib/base64/include/base64.hpp"

namespace websocket {

    inline constexpr long kConnectTimeoutMs = 10000;

    // How long a single send may take before it is given up on. Without this
    // `sendAll` looped on CURLE_AGAIN for ever: a peer that stops reading, or a
    // socket that dies while a frame is half out, held whatever thread called
    // `send` or `Close` - and `Close` is reached from the Lua thread, which is
    // Roblox's scheduler. Every reference bounds its socket waits (the macOS one
    // flushes for 250 ms and then waits 5 s at most); this file had one loop
    // that did not.
    inline constexpr int kSendTimeoutMs = 5000;

    // The largest frame payload that will be assembled. RFC 6455 allows a
    // 63-bit length, so a peer announcing one had this reading and buffering
    // until the process ran out of memory. No real server sends more than this
    // in one frame.
    inline constexpr uint64_t kMaxFrameBytes = 64ull << 20;

    // The most the inbound queue may hold before the oldest messages are
    // dropped. Nothing else bounds it: a server can send faster than a Lua
    // thread polling once a frame can drain, and `pending` simply grew until
    // the process ran out of memory.
    //
    // None of the six references bounds this either - the macOS one enqueues
    // into a global YieldMap with no limit at all - so being the outlier here
    // is deliberate. It is the shape of hazard that only fires under load,
    // which is exactly the kind every reference can share and none of them
    // tests for.
    //
    // Dropping the OLDEST is the deliberate half. A queue this deep is one
    // nobody is draining, so its head is the stalest data on it; dropping the
    // newest instead would mean a socket that overflowed once never delivered
    // anything current again.
    inline constexpr size_t kMaxQueuedBytes = 32u << 20;

    // RFC 6455 section 5.2.
    enum Opcode : uint8_t {
        kContinuation = 0x0,
        kText = 0x1,
        kBinary = 0x2,
        kClose = 0x8,
        kPing = 0x9,
        kPong = 0xA,
    };

    struct Target {
        std::string scheme;   // http or https - what curl is asked to open
        std::string host;
        std::string port;
        std::string path;
        bool secure = false;
    };

    inline bool parse(const std::string& url, Target& out, std::string& error)
    {
        // ws/wss is what a caller passes; http/https is what a Location header
        // carries when a server redirects the upgrade, and the two are the same
        // thing as far as the transport is concerned.
        std::string rest;
        if (url.rfind("wss://", 0) == 0) {
            out.scheme = "https";
            out.secure = true;
            rest = url.substr(6);
        } else if (url.rfind("ws://", 0) == 0) {
            out.scheme = "http";
            rest = url.substr(5);
        } else if (url.rfind("https://", 0) == 0) {
            out.scheme = "https";
            out.secure = true;
            rest = url.substr(8);
        } else if (url.rfind("http://", 0) == 0) {
            out.scheme = "http";
            rest = url.substr(7);
        } else {
            error = "url must start with ws:// or wss://";
            return false;
        }

        const size_t slash = rest.find('/');
        std::string authority = slash == std::string::npos ? rest : rest.substr(0, slash);
        out.path = slash == std::string::npos ? "/" : rest.substr(slash);
        if (out.path.empty())
            out.path = "/";

        // An IPv6 literal is bracketed, so only a colon after the closing
        // bracket separates a port.
        const size_t colon = authority.rfind(':');
        const size_t bracket = authority.rfind(']');
        if (colon != std::string::npos && (bracket == std::string::npos || colon > bracket)) {
            out.port = authority.substr(colon + 1);
            authority = authority.substr(0, colon);
        } else {
            out.port = out.secure ? "443" : "80";
        }

        out.host = authority;
        if (out.host.empty()) {
            error = "url has no host";
            return false;
        }
        return true;
    }

    // XOR a payload with its four-byte mask, eight bytes at a time.
    //
    // This is the only loop in this file that runs over every byte of every
    // frame - each direction, each message - so it is worth not doing one byte
    // at a time.  Endianness does not enter into it: the pattern is built
    // bytewise and the wide step is a multiple of four, so the mask phase at
    // any index is the same `i & 3` the tail loop uses.
    inline void applyMask(char* data, size_t length, const uint8_t mask[4])
    {
        uint8_t pattern[8];
        std::memcpy(pattern, mask, 4);
        std::memcpy(pattern + 4, mask, 4);
        uint64_t wide = 0;
        std::memcpy(&wide, pattern, sizeof(wide));

        size_t i = 0;
        for (; i + sizeof(wide) <= length; i += sizeof(wide)) {
            uint64_t chunk = 0;
            std::memcpy(&chunk, data + i, sizeof(chunk));
            chunk ^= wide;
            std::memcpy(data + i, &chunk, sizeof(chunk));
        }
        for (; i < length; i++)
            data[i] = static_cast<char>(data[i] ^ mask[i & 3]);
    }

    inline std::string acceptFor(const std::string& key)
    {
        static const char* const kGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
        const std::string combined = key + kGuid;
        unsigned char digest[CC_SHA1_DIGEST_LENGTH];
        CC_SHA1(combined.data(), static_cast<CC_LONG>(combined.size()), digest);
        return base64::to_base64(std::string(reinterpret_cast<char*>(digest), sizeof(digest)));
    }

    class Connection {
    public:
        ~Connection() { shutdown(); }

        // RFC 6455 leaves redirect handling to the client, and servers do use
        // it - echo.websocket.org answers a plain ws:// upgrade with a 301 to
        // its TLS endpoint. Following one is what makes ws:// work there at all.
        bool open(const std::string& url, std::string& error)
        {
            std::string current = url;
            for (int redirect = 0; redirect <= 3; redirect++) {
                std::string location;
                const Outcome outcome = attempt(current, error, location);
                if (outcome == Outcome::Connected)
                    return true;
                if (outcome == Outcome::Failed)
                    return false;

                if (location.empty()) {
                    error = "the server redirected without saying where to";
                    return false;
                }
                current = resolve(current, location);
                reset();
            }
            error = "too many redirects";
            return false;
        }

    private:
        enum class Outcome { Connected, Redirected, Failed };

        void reset()
        {
            if (handle) {
                curl_easy_cleanup(handle);
                handle = nullptr;
            }
            descriptor = -1;
            leftover.clear();
        }

        // A Location may be absolute or rooted at the host that sent it.
        static std::string resolve(const std::string& from, const std::string& location)
        {
            if (location.find("://") != std::string::npos)
                return location;

            Target base;
            std::string ignored;
            if (!parse(from, base, ignored))
                return location;

            const std::string scheme = base.secure ? "wss://" : "ws://";
            const bool defaultPort = base.port == (base.secure ? "443" : "80");
            std::string root = scheme + base.host + (defaultPort ? "" : ":" + base.port);
            return root + (location.empty() || location[0] == '/' ? location : "/" + location);
        }

        Outcome attempt(const std::string& url, std::string& error, std::string& location)
        {
            Target target;
            if (!parse(url, target, error))
                return Outcome::Failed;

            handle = curl_easy_init();
            if (!handle) {
                error = "could not create a curl handle";
                return Outcome::Failed;
            }

            const std::string endpoint = target.scheme + "://" + target.host + ":" + target.port + target.path;
            curl_easy_setopt(handle, CURLOPT_URL, endpoint.c_str());
            curl_easy_setopt(handle, CURLOPT_CONNECT_ONLY, 1L);
            curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT_MS, kConnectTimeoutMs);
            curl_easy_setopt(handle, CURLOPT_NOSIGNAL, 1L);
            // Without this curl offers h2 in the TLS handshake and servers take
            // it, leaving the peer expecting HTTP/2 frames while this code is
            // writing an HTTP/1.1 upgrade request - which reads to the server as
            // garbage and gets the connection closed with no reply at all.
            // WebSocket is defined over HTTP/1.1, so ALPN has nothing to offer.
            curl_easy_setopt(handle, CURLOPT_SSL_ENABLE_ALPN, 0L);

            CURLcode code = curl_easy_perform(handle);
            if (code != CURLE_OK) {
                error = std::string("could not connect: ") + curl_easy_strerror(code);
                return Outcome::Failed;
            }

            curl_socket_t socket = CURL_SOCKET_BAD;
            if (curl_easy_getinfo(handle, CURLINFO_ACTIVESOCKET, &socket) != CURLE_OK ||
                socket == CURL_SOCKET_BAD) {
                error = "curl did not hand back a socket";
                return Outcome::Failed;
            }
            descriptor = static_cast<int>(socket);

            const Outcome outcome = handshake(target, error, location);
            if (outcome != Outcome::Connected)
                return outcome;

            connected = true;
            reader = std::thread([this] { read(); });
            return Outcome::Connected;
        }

    public:
        bool send(const std::string& message)
        {
            if (!connected)
                return false;
            return writeFrame(kText, message);
        }

        // Asks the reader to finish. Does no I/O, so it is safe to call from
        // anywhere - including the frame the game is being torn down on.
        void stop()
        {
            finished = true;
        }

        // Sends a close frame so the peer sees a clean shutdown, then tears the
        // connection down. Safe to call twice; the second one is a no-op.
        void close()
        {
            if (connected.exchange(false)) {
                // 1000, "normal closure", big-endian, per RFC 6455 section 7.4.
                const char payload[2] = { 0x03, static_cast<char>(0xE8) };
                writeFrame(kClose, std::string(payload, sizeof(payload)));
            }
            finished = true;
        }

        std::vector<std::string> drain()
        {
            std::lock_guard<std::mutex> lock(queueMutex);
            std::vector<std::string> messages;
            messages.reserve(pending.size());
            queuedBytes = 0;
            // Moved, not copied: these are the caller's now, and a queue of
            // 64 KB messages copied under the lock is the reader thread
            // blocked for the length of the copy.
            for (std::string& message : pending)
                messages.push_back(std::move(message));
            pending.clear();
            return messages;
        }

        bool done() const { return finished.load(); }

        void shutdown()
        {
            finished = true;
            connected = false;
            if (reader.joinable())
                reader.join();
            if (handle) {
                curl_easy_cleanup(handle);
                handle = nullptr;
            }
        }

        bool sendAll(const char* data, size_t length, std::string& error)
        {
            const auto deadline =
                std::chrono::steady_clock::now() + std::chrono::milliseconds(kSendTimeoutMs);
            size_t total = 0;
            while (total < length) {
                if (!handle) {
                    error = "the connection is gone";
                    return false;
                }
                size_t written = 0;
                CURLcode code;
                {
                    std::lock_guard<std::mutex> lock(transportMutex);
                    code = curl_easy_send(handle, data + total, length - total, &written);
                }
                if (code == CURLE_AGAIN) {
                    if (std::chrono::steady_clock::now() > deadline) {
                        error = "the peer stopped reading";
                        return false;
                    }
                    waitWritable(20);
                    continue;
                }
                if (code != CURLE_OK) {
                    error = curl_easy_strerror(code);
                    return false;
                }
                total += written;
            }
            return true;
        }

        // Blocks until the socket has something to say or the timeout expires.
        // curl may already hold decrypted bytes that never reach the socket, so
        // callers drain with recv until CURLE_AGAIN before coming back here.
        void waitReadable(int timeoutMs) const { waitFor(POLLIN, timeoutMs); }
        void waitWritable(int timeoutMs) const { waitFor(POLLOUT, timeoutMs); }

        void waitFor(short events, int timeoutMs) const
        {
            if (descriptor < 0) {
                // poll() ignores a negative fd, so this would otherwise return
                // straight away and turn every caller's retry loop into a spin.
                std::this_thread::sleep_for(std::chrono::milliseconds(timeoutMs));
                return;
            }
            pollfd descriptorSet{};
            descriptorSet.fd = descriptor;
            descriptorSet.events = events;
            poll(&descriptorSet, 1, timeoutMs);
        }

        // Returns false only on a real failure; no data available is CURLE_AGAIN
        // and comes back as true with nothing appended.
        bool receive(std::string& buffer, bool& again)
        {
            // 4096 stays.  IXWebSocket reads a whole frame at a time and beat
            // this file on a 64 KB round trip (0.31 ms against 0.78 ms), so a
            // 64 KB buffer was tried here - and moved the number not at all,
            // because curl_easy_recv already hands back whatever the socket
            // holds and the syscall count was never the cost.  The rest of that
            // gap is the copies takeFrame makes assembling a message, which is
            // a real but much larger change.  Not worth it: at 64 B, the size
            // scripts actually send, this file is already level with IX
            // (0.08 ms), and it sustains 110 MB/s on the throughput leg that
            // IX failed outright.
            char chunk[4096];
            size_t read = 0;
            CURLcode code;
            {
                std::lock_guard<std::mutex> lock(transportMutex);
                code = curl_easy_recv(handle, chunk, sizeof(chunk), &read);
            }
            again = (code == CURLE_AGAIN);
            if (again)
                return true;
            lastReceive = code;
            if (code != CURLE_OK || read == 0)
                return false;
            buffer.append(chunk, read);
            return true;
        }

        Outcome handshake(const Target& target, std::string& error, std::string& location)
        {
            unsigned char nonce[16];
            arc4random_buf(nonce, sizeof(nonce));
            const std::string key = base64::to_base64(std::string(reinterpret_cast<char*>(nonce), sizeof(nonce)));

            std::string request = "GET " + target.path + " HTTP/1.1\r\n";
            request += "Host: " + target.host;
            if (target.port != (target.secure ? "443" : "80"))
                request += ":" + target.port;
            request += "\r\n";
            request += "Upgrade: websocket\r\n";
            request += "Connection: Upgrade\r\n";
            request += "Sec-WebSocket-Key: " + key + "\r\n";
            request += "Sec-WebSocket-Version: 13\r\n";
            // Same identity the HTTP paths send, so a host that whitelists an
            // executor sees one answer however the script reaches it. No JobId
            // here: the handshake is built on a worker with no Lua state, and
            // Roblox-Game-Id is optional to the one host that reads it.
            for (const std::string& line : http::headerLines(
                     http::identityHeaders(target.scheme + "://" + target.host, "", true)))
                request += line + "\r\n";
            request += "Origin: " + target.scheme + "://" + target.host + "\r\n";
            request += "\r\n";

            if (!sendAll(request.data(), request.size(), error)) {
                error = "handshake request failed: " + error;
                return Outcome::Failed;
            }

            std::string response;
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kConnectTimeoutMs);
            while (response.find("\r\n\r\n") == std::string::npos) {
                if (std::chrono::steady_clock::now() > deadline) {
                    error = "the server never finished the handshake";
                    return Outcome::Failed;
                }
                bool again = false;
                if (!receive(response, again)) {
                    error = std::string("the connection dropped during the handshake (") +
                            curl_easy_strerror(lastReceive) + ", " +
                            std::to_string(response.size()) + " bytes read)";
                    if (!response.empty())
                        error += ": " + response.substr(0, 120);
                    return Outcome::Failed;
                }
                if (again)
                    waitReadable(50);
            }

            const size_t headerEnd = response.find("\r\n\r\n");
            const std::string head = response.substr(0, headerEnd);
            const std::string status = head.substr(0, head.find("\r\n"));

            std::string lowered = head;
            for (char& c : lowered)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

            auto header = [&](const char* name) {
                const size_t at = lowered.find(name);
                if (at == std::string::npos)
                    return std::string();
                size_t start = head.find(':', at) + 1;
                while (start < head.size() && head[start] == ' ')
                    start++;
                const size_t end = head.find("\r\n", start);
                return head.substr(start, end - start);
            };

            if (head.find(" 101") == std::string::npos) {
                if (status.find(" 30") != std::string::npos) {
                    location = header("location:");
                    return Outcome::Redirected;
                }
                error = "the server refused to upgrade: " + status;
                return Outcome::Failed;
            }

            // The accept header is what proves the peer speaks WebSocket rather
            // than being a proxy that echoed a 101 back.
            const std::string accept = header("sec-websocket-accept:");
            if (accept.empty()) {
                error = "the server did not send Sec-WebSocket-Accept";
                return Outcome::Failed;
            }
            if (accept != acceptFor(key)) {
                error = "Sec-WebSocket-Accept did not match the key we sent";
                return Outcome::Failed;
            }

            // Anything after the header block is already frame data.
            leftover = response.substr(headerEnd + 4);
            return Outcome::Connected;
        }

        bool writeFrame(uint8_t opcode, const std::string& payload)
        {
            const size_t length = payload.size();
            std::string frame;
            // Two header bytes, at most eight of length, four of mask. Reserved
            // once so appending the payload cannot reallocate mid-frame.
            frame.reserve(14 + length);
            frame += static_cast<char>(0x80 | opcode);   // FIN, no reserved bits

            // Every client frame is masked; the length field grows with the
            // payload rather than being fixed width.
            if (length < 126) {
                frame += static_cast<char>(0x80 | length);
            } else if (length <= 0xFFFF) {
                frame += static_cast<char>(0x80 | 126);
                frame += static_cast<char>((length >> 8) & 0xFF);
                frame += static_cast<char>(length & 0xFF);
            } else {
                frame += static_cast<char>(0x80 | 127);
                for (int shift = 56; shift >= 0; shift -= 8)
                    frame += static_cast<char>((length >> shift) & 0xFF);
            }

            uint8_t mask[4];
            arc4random_buf(mask, sizeof(mask));
            frame.append(reinterpret_cast<char*>(mask), sizeof(mask));

            const size_t start = frame.size();
            frame.append(payload);
            applyMask(frame.data() + start, length, mask);

            std::string error;
            return sendAll(frame.data(), frame.size(), error);
        }

        // Pulls one frame out of `buffer` starting at `consumed` if a whole one
        // is there, and advances `consumed` past it. Returns false when more
        // bytes are needed.
        //
        // It reads through an offset rather than erasing the front, because
        // erasing is a memmove of every byte still in the buffer and it used to
        // happen once per frame - a single read landing twenty frames paid it
        // twenty times. read() reclaims the dead prefix once per cycle instead.
        bool takeFrame(std::string& buffer, size_t& consumed, uint8_t& opcode, bool& final,
                       std::string& payload)
        {
            const char* const base = buffer.data() + consumed;
            const size_t available = buffer.size() - consumed;
            if (available < 2)
                return false;

            const uint8_t first = static_cast<uint8_t>(base[0]);
            const uint8_t second = static_cast<uint8_t>(base[1]);
            final = (first & 0x80) != 0;
            opcode = first & 0x0F;

            const bool masked = (second & 0x80) != 0;
            uint64_t length = second & 0x7F;
            size_t offset = 2;

            if (length == 126) {
                if (available < offset + 2)
                    return false;
                length = (static_cast<uint8_t>(base[offset]) << 8) | static_cast<uint8_t>(base[offset + 1]);
                offset += 2;
            } else if (length == 127) {
                if (available < offset + 8)
                    return false;
                length = 0;
                for (int i = 0; i < 8; i++)
                    length = (length << 8) | static_cast<uint8_t>(base[offset + i]);
                offset += 8;
            }

            uint8_t mask[4] = {0, 0, 0, 0};
            if (masked) {
                if (available < offset + 4)
                    return false;
                std::memcpy(mask, base + offset, sizeof(mask));
                offset += 4;
            }

            if (length > kMaxFrameBytes) {
                // Nothing legitimate announces this. Say so by claiming the
                // frame as a close, which is what read() acts on.
                opcode = kClose;
                final = true;
                payload.clear();
                buffer.clear();
                consumed = 0;
                return true;
            }

            if (available < offset + length)
                return false;

            payload.assign(base + offset, static_cast<size_t>(length));
            if (masked)
                applyMask(payload.data(), payload.size(), mask);

            consumed += offset + static_cast<size_t>(length);
            return true;
        }

        void read()
        {
            std::string buffer = std::move(leftover);
            size_t consumed = 0;
            std::string message;
            // Hoisted out of the loop so its capacity survives from frame to
            // frame instead of being reallocated for each one.
            std::string payload;
            uint8_t messageOpcode = kText;

            while (!finished.load()) {
                uint8_t opcode = 0;
                bool final = false;

                while (takeFrame(buffer, consumed, opcode, final, payload)) {
                    if (opcode == kClose) {
                        connected = false;
                        finished = true;
                        return;
                    }
                    if (opcode == kPing) {
                        writeFrame(kPong, payload);
                        continue;
                    }
                    if (opcode == kPong)
                        continue;

                    // A message can arrive split across frames; only the first
                    // carries the opcode saying what it is.
                    if (opcode != kContinuation) {
                        message.clear();
                        messageOpcode = opcode;
                    }
                    // The overwhelmingly common case is one frame per message,
                    // and then this is a move rather than a copy of the whole
                    // payload. Only a fragmented message pays an append.
                    if (message.empty())
                        message = std::move(payload);
                    else
                        message += payload;

                    if (final) {
                        (void)messageOpcode;   // text and binary both reach Lua as strings
                        // Size read before the move - afterwards it is 0.
                        const size_t added = message.size();
                        std::lock_guard<std::mutex> lock(queueMutex);
                        pending.push_back(std::move(message));
                        message.clear();
                        queuedBytes += added;
                        // Never drop the message just queued, however large it
                        // is: a single frame over the cap would otherwise be
                        // delivered to nobody.
                        while (queuedBytes > kMaxQueuedBytes && pending.size() > 1) {
                            queuedBytes -= pending.front().size();
                            pending.pop_front();
                            droppedMessages++;
                        }
                        if (droppedMessages && !warnedOverflow) {
                            warnedOverflow = true;
                            journal::writef("WS inbound queue over %zu bytes - "
                                            "dropping the oldest; nothing is draining it",
                                            kMaxQueuedBytes);
                        }
                    }
                }

                // Reclaim what has been read. The win is that this happens
                // once per read cycle instead of once per frame - a read that
                // lands twenty frames used to pay twenty memmoves of the whole
                // remainder. Compacting every cycle rather than at some
                // high-water mark keeps the buffer no bigger than it needs to
                // be, which matters more than the memmove: a 64 KB threshold
                // was tried and only grew the buffer for no measurable gain.
                if (consumed == buffer.size()) {
                    buffer.clear();
                    consumed = 0;
                } else if (consumed > 0) {
                    buffer.erase(0, consumed);
                    consumed = 0;
                }

                bool again = false;
                if (!receive(buffer, again)) {
                    connected = false;
                    finished = true;
                    return;
                }
                if (again)
                    waitReadable(50);
            }
        }

        CURL* handle = nullptr;
        int descriptor = -1;
        std::string leftover;
        std::thread reader;
        std::mutex transportMutex;
        std::mutex queueMutex;
        std::deque<std::string> pending;
        // Guarded by queueMutex, with `pending`.
        size_t queuedBytes = 0;
        size_t droppedMessages = 0;
        bool warnedOverflow = false;
        CURLcode lastReceive = CURLE_OK;
        std::atomic<bool> connected{false};
        std::atomic<bool> finished{false};
    };

    // Connections are addressed from Lua by an integer, so nothing about a
    // std::shared_ptr has to survive a trip through the VM.
    inline std::mutex registryMutex;
    inline std::unordered_map<int, std::shared_ptr<Connection>> connections;
    inline int nextId = 1;

    inline int add(std::shared_ptr<Connection> connection)
    {
        std::lock_guard<std::mutex> lock(registryMutex);
        const int id = nextId++;
        connections[id] = std::move(connection);
        return id;
    }

    inline std::shared_ptr<Connection> find(int id)
    {
        std::lock_guard<std::mutex> lock(registryMutex);
        auto found = connections.find(id);
        return found == connections.end() ? nullptr : found->second;
    }

    // Closing a socket means writing a close frame and joining a reader thread,
    // and both of those are waits. They are done on a worker so that neither the
    // Lua thread (`ws:Close()`) nor the game-change path (`closeAll` below) ever
    // waits on the network. The macOS reference hands its socket to a detached
    // thread at exactly these two points for the same reason.
    inline pool::Pool& reapers()
    {
        static pool::Pool instance(4);
        return instance;
    }

    inline void reap(std::shared_ptr<Connection> connection)
    {
        if (!connection)
            return;
        // Tell the reader to stop before handing it over, so the wait the
        // worker then does is a short one however busy the pool is.
        connection->stop();
        // The lambda takes a *copy* on purpose. Moving it in would mean that a
        // pool that could not queue the job destroys the only reference inside
        // run(), on this thread - which is both the join we were avoiding and a
        // dangling pointer for the fallback below.
        if (!reapers().run([connection]() mutable {
                connection->close();
                connection.reset();   // joins the reader here, not on the caller
            })) {
            // No worker to hand it to. Closing here is still better than
            // leaking the socket.
            connection->close();
        }
    }

    inline void remove(int id)
    {
        std::shared_ptr<Connection> connection;
        {
            std::lock_guard<std::mutex> lock(registryMutex);
            auto found = connections.find(id);
            if (found == connections.end())
                return;
            connection = found->second;
            connections.erase(found);
        }
        reap(std::move(connection));
    }

    // Every socket belongs to the game that opened it: its OnMessage handlers
    // are registry refs in that game's Lua state, so a socket that outlives a
    // teleport delivers into a state that no longer exists. The reference
    // executor closes them all on the way out of a DataModel for exactly this
    // reason (`A-Actual-Mac-Executor.../src/hooks/hooks.hpp`, TeleportSuccess).
    inline void closeAll()
    {
        std::unordered_map<int, std::shared_ptr<Connection>> taken;
        {
            std::lock_guard<std::mutex> lock(registryMutex);
            taken.swap(connections);
        }
        if (!taken.empty())
            journal::writef("WS closing %zu socket(s) for the game change", taken.size());
        for (auto& entry : taken)
            reap(std::move(entry.second));
    }
}
