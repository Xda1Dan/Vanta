//
//  http.hpp
//  vanta-base
//
//  The transport behind game:HttpGet. macOS ships libcurl, so a request is a
//  curl_easy_perform - but never on the thread that asked. Every caller here
//  goes through Yield::start -> async::submit, so the transfer runs on a pool
//  worker and the Lua thread yields until it lands; CURLOPT_TIMEOUT_MS bounds
//  how long that worker can be tied up for. See async.hpp for why.
//
//  The handle that perform() runs on is kept per worker rather than built and
//  destroyed per request, because the connection, the DNS entry and the TLS
//  session all live inside it. See `detail::handle` below - it is worth 3x on
//  a repeated request to one host, which is most of what scripts do.
//
//  Nothing here forwards credentials. No cookie jar, no Roblox auth header:
//  a script that asks for a roblox.com endpoint gets the logged-out view of
//  it, which is the only safe default when the URL comes from a script the
//  user pasted in.
//

#pragma once

#include <cctype>
#include <mutex>
#include <vector>
#include <string>
#include <unistd.h>
#include <uuid/uuid.h>

#include "Journal.h"
#include "Version.h"

#include <CommonCrypto/CommonDigest.h>

#include <curl/curl.h>

namespace http {

    // Long enough for a slow CDN, short enough that a dead host does not hang
    // the client for the two minutes curl would otherwise wait.
    inline constexpr long kTimeoutMs = 10000;
    inline constexpr long kConnectTimeoutMs = 5000;

    // The most a Content-Length header may make us allocate up front. Comfortably
    // above any script or asset a loader fetches, and small enough that a server
    // lying about it costs nothing worth having.
    inline constexpr long long kMaxReserve = 8LL << 20;

    // What Roblox's own HTTP stack sends. `game:HttpGet` keeps this, because
    // scripts check for it and some endpoints vary their reply on it.
    inline constexpr const char* kUserAgent = "Roblox/WinInet";

    // What `request` sends instead. Five of the six references send their own
    // name and version here (RbxStu/x, Visual, Vicna 3.0.0, SkidBase/1.0.1,
    // Opiumware) and the spec asks for "executor name and version string" - so
    // the two calls are deliberately different, exactly as sourcessss has them.
    //
    // It is not set here: the Lua `request` wrapper puts it in the header list
    // it hands down, and a `User-Agent:` there replaces CURLOPT_USERAGENT. This
    // is the value it writes; keep the two in step.
    inline constexpr const char* kExecutorAgent = "Vanta/" VANTA_VERSION_STR;

    // luarmor.net serves a script only to an executor on its whitelist, and it
    // decides which one is calling from two headers: the User-Agent and a
    // `<Name>-Fingerprint`. Vanta is not on that list, so until it is, requests
    // to that one host go out under Seliware's name - which is what luarmor's
    // own "adding support for my executor" note tells an unlisted executor to
    // do in the meantime.
    //
    // It is deliberately one host and not a substring test: `isLuarmor` matches
    // luarmor.net and its subdomains (api.luarmor.net is the endpoint the
    // loaders actually call) and nothing else, so no other site ever sees a
    // name that is not ours. The `request` call carries Seliware's agent, and
    // `game:HttpGet` keeps Roblox's - that split is luarmor's, not ours.
    inline constexpr const char* kLuarmorHost = "luarmor.net";
    inline constexpr const char* kLuarmorAgent = "Seliware/1.0";
    inline constexpr const char* kLuarmorPrefix = "Seliware";

    // The name that goes in front of `-Fingerprint` for every other host.
    inline constexpr const char* kPrefix = "Vanta";

    // The only schemes curl may speak, and the only ones a redirect may move
    // to. Without this a script gets the whole of libcurl: `file:///etc/passwd`
    // through HttpGet walks straight past the filesystem sandbox, and so do
    // scp, dict and gopher. Three of the six close this - mac at the curl layer
    // (`CURLOPT_PROTOCOLS_STR`, though it also allows ws and ftp), Vicna and
    // sourcessss by refusing any url that does not start `http://` or
    // `https://` before the call. Doing it in curl also covers the redirect,
    // which a prefix test on the caller's url does not.
    inline constexpr const char* kProtocols = "http,https";

    // A machine-bound identifier, for the `<Prefix>-Fingerprint` header the
    // spec requires and four of the six references send. `gethostuuid` is the
    // per-host UUID libc already has, so this needs no framework and no
    // IOKit lookup; it is hashed so the raw UUID never leaves the machine.
    inline const std::string& fingerprint()
    {
        static const std::string value = [] {
            uuid_t uuid{};
            const timespec wait{0, 0};
            if (gethostuuid(uuid, &wait) != 0)
                return std::string("unknown");

            unsigned char digest[CC_SHA256_DIGEST_LENGTH];
            CC_SHA256(uuid, static_cast<CC_LONG>(sizeof(uuid)), digest);

            static const char* const hex = "0123456789abcdef";
            std::string out;
            out.reserve(sizeof(digest) * 2);
            for (unsigned char byte : digest) {
                out += hex[byte >> 4];
                out += hex[byte & 0xf];
            }
            return out;
        }();
        return value;
    }

    // The host of a url, lowercased, without userinfo or port. Written by hand
    // rather than with curl's parser because it is wanted before a handle
    // exists - the websocket path has no CURL* at all.
    inline std::string hostOf(const std::string& url)
    {
        size_t start = url.find("://");
        start = (start == std::string::npos) ? 0 : start + 3;
        const size_t end = url.find_first_of("/?#", start);
        std::string host = url.substr(start, end == std::string::npos ? std::string::npos : end - start);

        const size_t at = host.rfind('@');
        if (at != std::string::npos)
            host.erase(0, at + 1);
        // An IPv6 literal keeps its colons; only a trailing :port is dropped.
        if (host.find(']') == std::string::npos) {
            const size_t colon = host.rfind(':');
            if (colon != std::string::npos)
                host.erase(colon);
        }
        for (char& c : host)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return host;
    }

    // luarmor.net itself, or any subdomain of it - and nothing whose name merely
    // contains it, which a `find()` on the whole url would have matched.
    inline bool isLuarmor(const std::string& url)
    {
        const std::string host = hostOf(url);
        const std::string domain = kLuarmorHost;
        if (host == domain)
            return true;
        return host.size() > domain.size() + 1 &&
               host.compare(host.size() - domain.size() - 1, domain.size() + 1, "." + domain) == 0;
    }

    // The headers an executor is expected to attach, in the two shapes the spec
    // gives them: `game:HttpGet` keeps Roblox's own agent because scripts and
    // endpoints check for it, and `request` sends the executor's own name and
    // version. luarmor is the one host that is answered under another name.
    //
    // `Roblox-Game-Id` and `Roblox-Session-Id` are what luarmor asks for and are
    // optional to it; both carry the JobId, which is the caller's to read - this
    // runs on a worker thread with no Lua state of its own.
    inline std::string identityHeaders(const std::string& url, const std::string& jobId, bool isRequest)
    {
        const bool luarmor = isLuarmor(url);
        const char* prefix = luarmor ? kLuarmorPrefix : kPrefix;

        std::string out;
        out += "User-Agent: ";
        out += !isRequest ? kUserAgent : (luarmor ? kLuarmorAgent : kExecutorAgent);
        out += "\n";

        out += std::string(prefix) + "-Fingerprint: " + fingerprint() + "\n";
        if (isRequest && !luarmor)
            out += std::string(prefix) + "-User-Identifier: " + fingerprint() + "\n";

        if (!jobId.empty()) {
            // Every field inside Roblox-Session-Id carries the JobId, which is
            // what luarmor's note spells out. A JobId is a GUID, but it comes
            // from the engine through Lua, so the quote and backslash are
            // escaped rather than trusted.
            std::string escaped;
            for (char c : jobId) {
                if (c == '"' || c == '\\')
                    escaped += '\\';
                escaped += c;
            }
            out += "Roblox-Game-Id: " + jobId + "\n";
            out += "Roblox-Session-Id: {\"GameId\":\"" + escaped + "\"}\n";
        }
        return out;
    }

    // Splits the newline-joined header block every path here passes around.
    inline std::vector<std::string> headerLines(const std::string& block)
    {
        std::vector<std::string> lines;
        size_t start = 0;
        while (start < block.size()) {
            size_t end = block.find('\n', start);
            if (end == std::string::npos)
                end = block.size();
            if (end > start)
                lines.emplace_back(block, start, end - start);
            start = end + 1;
        }
        return lines;
    }

    struct Response {
        bool ok = false;      // the request completed; says nothing about status
        long status = 0;
        std::string body;     // may hold an error page - status says which
        std::string headers;  // raw response headers, one per line
        std::string error;    // curl's description when ok is false
    };

    inline size_t collect(char* data, size_t size, size_t count, void* userdata)
    {
        const size_t total = size * count;
        static_cast<std::string*>(userdata)->append(data, total);
        return total;
    }

    struct Request {
        std::string url;
        std::string method = "GET";
        std::string body;
        std::string headers;   // newline-separated "Key: Value" lines
        std::string contentType;
    };

    // Also the only place the body's final size is known before it arrives.
    // Without the reserve a 1 MB download grows the string by doubling, which
    // is a handful of reallocations and a full copy each time.
    inline size_t collectHeader(char* data, size_t size, size_t count, void* userdata)
    {
        const size_t total = size * count;
        Response* response = static_cast<Response*>(userdata);
        response->headers.append(data, total);

        if (total > 16 && total < 128 &&
            (data[0] == 'C' || data[0] == 'c')) {
            std::string line(data, total);
            for (char& c : line)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (line.rfind("content-length:", 0) == 0) {
                const long long length = atoll(line.c_str() + 15);
                // Capped, and deliberately low. The header is the server's
                // claim, not a fact: without a ceiling, answering a request
                // with `Content-Length: 4294967296` and no body would have us
                // allocate four gigabytes on its say-so. Bodies above the cap
                // still work - they just grow as the bytes actually arrive,
                // which is what every response did before this existed.
                // A redirect chain calls this again for the final response, so
                // only ever grow.
                if (length > 0 && length <= kMaxReserve &&
                    static_cast<size_t>(length) > response->body.capacity())
                    response->body.reserve(static_cast<size_t>(length));
            }
        }
        return total;
    }

    // How many connections one worker's handle may keep alive.  curl's own
    // default is 5; it is pinned here because the number is now load-bearing -
    // with `kMaxWorkers` workers each holding its own cache, this is what
    // bounds the sockets a script can leave open across every host it touches.
    inline constexpr long kMaxConnections = 5;

    // curl's 60-second DNS cache is left alone. Raising it to 300 s was tried:
    // it measured nothing, and it is the one "tuning" here that can *cause* a
    // failure - a CDN rotates an address and we keep dialling the old one for
    // five minutes, which surfaces as a request that fails rather than one that
    // is merely slow. Not worth it for a saving that could not be detected.

    // curl's default receive buffer is 16 KB.  A bigger one is fewer wakeups and
    // fewer appends on the large downloads that scripts actually fetch (a
    // megabyte of Lua is not unusual), and costs one allocation per handle.
    inline constexpr long kReceiveBuffer = 128 * 1024;

    // How long a pooled connection may have been idle and still be reused.
    //
    // This is the cost of keeping connections at all, and it is a new cost:
    // until the handle started being reused, every request opened its own
    // connection and there was no stale one to pick up. Now there is, and a
    // dead one is not always detectable - curl probes a pooled connection
    // before reusing it, which catches a peer that sent FIN, but not a NAT or
    // proxy that silently stopped forwarding. That case writes the request into
    // a black hole and waits out the full `kTimeoutMs`.
    //
    // Seen once in 17,483 jobs across five sUNC runs, as `ASYNC done id=17
    // after 10001ms` in the journal and a failed `request` check in the suite.
    // Rare is not the same as acceptable: it is a whole check, and the run is
    // gated at 100%.
    //
    // The number is the request timeout, which is the natural reference - never
    // gamble a full timeout's worth of waiting on a connection that has been
    // idle longer than that timeout. It costs nothing that reuse was buying:
    // the repeated requests it exists for are milliseconds apart, not seconds.
    // curl's own default is 118 s.
    inline constexpr long kMaxIdleReuseSeconds = kTimeoutMs / 1000;

    // Failures that a connection dying under us looks like. Any of these on a
    // *reused* connection is the signature; on a fresh one the same code means
    // the host really is unreachable and retrying would only cost another wait.
    inline bool looksLikeDeadConnection(CURLcode code)
    {
        switch (code) {
            case CURLE_OPERATION_TIMEDOUT:
            case CURLE_RECV_ERROR:
            case CURLE_SEND_ERROR:
            case CURLE_GOT_NOTHING:
            case CURLE_PARTIAL_FILE:
                return true;
            default:
                return false;
        }
    }

    // Whether re-sending the request is safe. Only the methods RFC 9110 defines
    // as idempotent: repeating one of these has the same effect as issuing it
    // once, so a retry cannot double an action. POST and PATCH are never
    // retried - a reused connection that timed out may well have delivered the
    // request and lost only the response, and there is no way to tell from
    // here, so the caller is told it failed rather than being charged twice.
    inline bool isIdempotent(const std::string& method)
    {
        return method.empty() || method == "GET" || method == "HEAD" ||
               method == "OPTIONS" || method == "PUT" || method == "DELETE" ||
               method == "TRACE";
    }

    // Idle connections in the cache go stale, and a dead one costs a full
    // reconnect discovered the slow way. Keepalive probes keep them honest.
    // This one is kept without a benchmark behind it because it is not a
    // speed-up - it is the reliability half of keeping connections at all,
    // which is a thing this file only started doing recently.
    inline constexpr long kKeepaliveIdleSeconds = 60;
    inline constexpr long kKeepaliveIntervalSeconds = 30;

    // Sharing caches between workers with a CURLSH was tried and removed.
    //
    // The idea was that a burst of parallel requests lands on several workers
    // at once and each does its own TLS handshake, so sharing DNS answers and
    // TLS session tickets should make all but the first cheap. Measured over
    // four interleaved runs against a real host, the cold 8-way burst cost
    // 155-198 ms *either way* - the shared cache changed nothing. The likely
    // reason is that curl here is built against SecureTransport, whose backend
    // does not implement TLS session-id caching, so CURL_LOCK_DATA_SSL_SESSION
    // shares an empty cache; and CURL_LOCK_DATA_DNS only saves an OS resolver
    // lookup, which is already a memory hit.
    //
    // Not kept: it is shared mutable state plus a global lock taken around
    // every resolve, inside a dylib injected into a game client, in exchange
    // for nothing that could be measured. If it is ever revisited, the leg that
    // decides it is the *first* burst of `https remote x8 parallel` - read the
    // raw per-burst values, not the median, which the three warm bursts drown.

    // The curl handle a transfer runs on, kept per thread and reset between
    // requests rather than created and destroyed around each one.
    //
    // The connection cache, the DNS cache and - the expensive one - the TLS
    // session all live inside the easy handle, so `curl_easy_cleanup` after a
    // single request throws the whole handshake away and the next request to
    // the same host pays for it again.  Measured with tests/netbench: a
    // repeated HTTPS GET to one host costs a median of 141 ms per request with
    // a fresh handle and 40 ms with a kept one.  That difference is a TCP round
    // trip plus a TLS handshake, and it was being paid on every single call.
    //
    // Per *thread* rather than a shared pool because a CURL* may not be used
    // from two threads at once, and every caller is already on a pool worker -
    // all three call sites go through Yield::start -> async::submit, so there
    // are at most `async::kMaxWorkers` of these.  Workers never exit, so the
    // destructor is a formality; it exists so nothing leaks if that changes.
    //
    // `curl_easy_reset` is what makes the sharing safe: it returns every option
    // to its default - url, method, headers, credentials and the cookie engine
    // included - while deliberately keeping those caches.  That is exactly the
    // split wanted here.  No request can inherit another's settings; every
    // request can inherit another's connection.
    //
    // None of the six references does this: mac, RbxStu (through cpr's Session)
    // and sourcessss all build a handle per request.  It is taken anyway,
    // because what they share is a cost none of them measures.
    namespace detail {

        struct HandleSlot {
            CURL* value = nullptr;
            ~HandleSlot()
            {
                if (value)
                    curl_easy_cleanup(value);
            }
        };

        inline CURL* handle()
        {
            static thread_local HandleSlot slot;
            if (slot.value) {
                curl_easy_reset(slot.value);
                // curl_easy_reset restores every *option* to its default but
                // deliberately keeps the handle's live connections, DNS cache,
                // TLS session cache - and its cookies.  The first three are the
                // whole point of reusing the handle; the cookies are not, and a
                // handle shared by every script on this worker must never
                // become a place where one site's Set-Cookie is carried to the
                // next request.  Nothing here turns the cookie engine on, so
                // there should be nothing to drop - this is the guarantee that
                // it stays that way if a caller ever sets a cookie option.
                curl_easy_setopt(slot.value, CURLOPT_COOKIELIST, "ALL");
            } else {
                slot.value = curl_easy_init();
            }
            return slot.value;
        }

    }   // namespace detail

    inline Response perform(const Request& request)
    {
        Response response;

        static std::once_flag globalInit;
        std::call_once(globalInit, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });

        CURL* handle = detail::handle();
        if (!handle) {
            response.error = "could not initialise curl";
            return response;
        }

        curl_slist* headers = nullptr;
        for (const std::string& line : headerLines(request.headers))
            headers = curl_slist_append(headers, line.c_str());
        if (!request.contentType.empty())
            headers = curl_slist_append(headers, ("Content-Type: " + request.contentType).c_str());

        curl_easy_setopt(handle, CURLOPT_URL, request.url.c_str());
#ifdef CURLOPT_PROTOCOLS_STR
        curl_easy_setopt(handle, CURLOPT_PROTOCOLS_STR, kProtocols);
        curl_easy_setopt(handle, CURLOPT_REDIR_PROTOCOLS_STR, kProtocols);
#else
        curl_easy_setopt(handle, CURLOPT_PROTOCOLS, static_cast<long>(CURLPROTO_HTTP | CURLPROTO_HTTPS));
        curl_easy_setopt(handle, CURLOPT_REDIR_PROTOCOLS, static_cast<long>(CURLPROTO_HTTP | CURLPROTO_HTTPS));
#endif
        curl_easy_setopt(handle, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(handle, CURLOPT_TIMEOUT_MS, kTimeoutMs);
        curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT_MS, kConnectTimeoutMs);
        curl_easy_setopt(handle, CURLOPT_USERAGENT, kUserAgent);
        curl_easy_setopt(handle, CURLOPT_ACCEPT_ENCODING, "");
        curl_easy_setopt(handle, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(handle, CURLOPT_MAXCONNECTS, kMaxConnections);
        curl_easy_setopt(handle, CURLOPT_BUFFERSIZE, kReceiveBuffer);
        curl_easy_setopt(handle, CURLOPT_TCP_KEEPALIVE, 1L);
        curl_easy_setopt(handle, CURLOPT_TCP_KEEPIDLE, kKeepaliveIdleSeconds);
        curl_easy_setopt(handle, CURLOPT_TCP_KEEPINTVL, kKeepaliveIntervalSeconds);
        curl_easy_setopt(handle, CURLOPT_MAXAGE_CONN, kMaxIdleReuseSeconds);
        // HTTP/2 where the server offers it over TLS. This is already curl 8's
        // default; it is written down because it is load-bearing for the
        // parallel case - one h2 connection carries several requests.
        curl_easy_setopt(handle, CURLOPT_HTTP_VERSION,
                         static_cast<long>(CURL_HTTP_VERSION_2TLS));
        curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, collect);
        curl_easy_setopt(handle, CURLOPT_WRITEDATA, &response.body);
        curl_easy_setopt(handle, CURLOPT_HEADERFUNCTION, collectHeader);
        // The whole Response, not just its header string: the header callback
        // is where Content-Length is seen and the body can be reserved.
        curl_easy_setopt(handle, CURLOPT_HEADERDATA, &response);
        if (headers)
            curl_easy_setopt(handle, CURLOPT_HTTPHEADER, headers);

        // The verb has to be set with the option that matches it, not with
        // CUSTOMREQUEST for everything. CUSTOMREQUEST changes the word on the
        // request line and nothing else, so `Method = "HEAD"` sent a HEAD and
        // then sat waiting for a response body that by definition never comes -
        // ten seconds of timeout for every HEAD. Only CURLOPT_NOBODY tells curl
        // not to expect one. mac splits GET/POST/HEAD out exactly like this,
        // sourcessss has a wrapper per method, RbxStu calls cpr's `Head()`,
        // and vic and skid hand the verb to the OS; none of the six routes HEAD
        // through a custom verb.
        //
        // POST sets the body unconditionally, empty or not: with CURLOPT_POST
        // and no POSTFIELDS libcurl looks for a read callback instead, and the
        // default one reads stdin.
        std::string method = request.method;
        for (char& c : method)
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));

        const auto setBody = [&] {
            curl_easy_setopt(handle, CURLOPT_POSTFIELDS, request.body.data());
            curl_easy_setopt(handle, CURLOPT_POSTFIELDSIZE, static_cast<long>(request.body.size()));
        };

        if (method.empty() || method == "GET") {
            curl_easy_setopt(handle, CURLOPT_HTTPGET, 1L);
        } else if (method == "HEAD") {
            curl_easy_setopt(handle, CURLOPT_NOBODY, 1L);
        } else if (method == "POST") {
            curl_easy_setopt(handle, CURLOPT_POST, 1L);
            setBody();
        } else {
            curl_easy_setopt(handle, CURLOPT_CUSTOMREQUEST, method.c_str());
            if (!request.body.empty())
                setBody();
        }

        CURLcode result = curl_easy_perform(handle);

        // One retry, on a connection of its own, for the one failure that
        // reusing connections introduced.
        //
        // `CURLINFO_NUM_CONNECTS` is how many connections this transfer had to
        // open: zero means it reused a pooled one. A connection-shaped failure
        // on a reused connection is the signature of a peer that went away
        // without telling us, and it is worth one more try. The same failure on
        // a connection we just opened means the host is genuinely unreachable -
        // retrying that would just spend another `kTimeoutMs` finding out
        // again, so the guard is what makes including CURLE_OPERATION_TIMEDOUT
        // here safe.
        //
        // `kMaxIdleReuseSeconds` above is the prevention and this is the
        // backstop; both are needed, because the age limit cannot know that a
        // connection idle for two seconds has already been dropped.
        if (result != CURLE_OK && looksLikeDeadConnection(result) && isIdempotent(method)) {
            long connects = 0;
            curl_off_t pretransfer = 0;
            // Two questions, and both have to answer yes.
            //
            // NUM_CONNECTS == 0: this transfer opened no new connection, so
            // whatever it used came out of the pool.
            //
            // PRETRANSFER_TIME > 0: it got as far as *sending* the request,
            // which means it had a connection it believed was good. Without
            // this second half, a fresh connection that timed out during the
            // connect phase also reports zero connects - and retrying a host
            // that is genuinely unreachable would spend another kTimeoutMs
            // proving it, pushing the total past async::kBlockTimeoutMs and
            // turning a clean "timed out" into "the request was lost".
            const bool reusedAndSent =
                curl_easy_getinfo(handle, CURLINFO_NUM_CONNECTS, &connects) == CURLE_OK &&
                connects == 0 &&
                curl_easy_getinfo(handle, CURLINFO_PRETRANSFER_TIME_T, &pretransfer) == CURLE_OK &&
                pretransfer > 0;
            if (reusedAndSent) {
                journal::writef("HTTP %s on a reused connection - retrying fresh",
                                curl_easy_strerror(result));
                // Both accumulate through the callbacks, so both have to go or
                // the retry's reply is appended to the dead attempt's.
                response.body.clear();
                response.headers.clear();
                curl_easy_setopt(handle, CURLOPT_FRESH_CONNECT, 1L);
                result = curl_easy_perform(handle);
                // Cleared again so the *next* request on this handle can still
                // reuse what this one just opened. curl_easy_reset would do it
                // too; this does not depend on that.
                curl_easy_setopt(handle, CURLOPT_FRESH_CONNECT, 0L);
            }
        }

        if (result == CURLE_OK) {
            response.ok = true;
            curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &response.status);
        } else {
            response.error = curl_easy_strerror(result);
            response.body.clear();
        }

        // The header list is per-request and must go; the handle is not and
        // must not - it is this thread's, and its caches are the point.  That
        // does leave the handle holding CURLOPT_HTTPHEADER pointing at freed
        // memory until the next use, which is safe for exactly one reason:
        // `detail::handle` calls curl_easy_reset before returning it, so the
        // stale pointer is cleared before anything can read it.  Any future
        // path that reaches this handle without going through there has to
        // clear the option itself.
        if (headers)
            curl_slist_free_all(headers);
        return response;
    }

    // `game:HttpGet`. It was a second, near-identical copy of `perform` until
    // 2026-08-27; the copy is what let the protocol restriction and the HEAD
    // fix apply to only one of the two paths, which is the whole argument
    // against having two. The one behavioural difference the copy had - it did
    // not collect response headers - was a string nobody read.
    inline Response get(const std::string& url)
    {
        Request request;
        request.url = url;
        return perform(request);
    }
}
