// Direct WSS latency diagnostic. No listening port or local proxy.
#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <curl/curl.h>
#include <curl/websockets.h>

using Clock = std::chrono::steady_clock;
struct Edge {
    CURL* curl = nullptr;
    curl_slist* headers = nullptr;
    curl_slist* resolve = nullptr;
    curl_socket_t fd = CURL_SOCKET_BAD;
    std::string ray, colo;
    static size_t header(char* bytes, size_t size, size_t count, void* opaque) {
        auto& self = *static_cast<Edge*>(opaque);
        std::string line(bytes, size * count);
        auto colon = line.find(':');
        if (colon != std::string::npos) {
            auto key = line.substr(0, colon);
            for (char& c : key) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
            auto value = line.substr(colon + 1);
            auto first = value.find_first_not_of(" \t"), last = value.find_last_not_of(" \t\r\n");
            value = first == std::string::npos ? "" : value.substr(first, last - first + 1);
            if (key == "cf-ray") self.ray = value;
            if (key == "x-edge-colo") self.colo = value;
        }
        return size * count;
    }
    Edge(const std::string& url, const std::string& file, const std::string& ip) {
        if (url.rfind("wss://", 0) != 0 || url.find_first_of("\r\n") != std::string::npos)
            throw std::runtime_error("edge URL must use wss://");
        auto authority_end = url.find('/', 6);
        if (url.substr(6, authority_end == std::string::npos ? std::string::npos : authority_end - 6).find('@') != std::string::npos ||
            url.find_first_of("?#") != std::string::npos) throw std::runtime_error("edge credentials must be supplied by token file only");
        std::ifstream input(file);
        std::string token; std::getline(input, token);
        if (!token.empty() && token.back() == '\r') token.pop_back();
        if (!input || token.size() < 32 || token.size() > 256 ||
            token.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_") != std::string::npos)
            throw std::runtime_error("cannot read valid edge token file");
        curl = curl_easy_init();
        if (!curl) throw std::runtime_error("curl initialization failed");
        try {
        headers = curl_slist_append(headers, ("Authorization: Bearer " + token).c_str());
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_PROXY, "");
        curl_easy_setopt(curl, CURLOPT_NOPROXY, "*");
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
        curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
        curl_easy_setopt(curl, CURLOPT_TCP_NODELAY, 1L);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 10000L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 15000L);
        curl_easy_setopt(curl, CURLOPT_CONNECT_ONLY, 2L);
        curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header);
        curl_easy_setopt(curl, CURLOPT_HEADERDATA, this);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, +[](char*, size_t size, size_t count, void*) { return size * count; });
        if (!ip.empty()) {
            unsigned char address[16];
            if (inet_pton(AF_INET, ip.c_str(), address) != 1 && inet_pton(AF_INET6, ip.c_str(), address) != 1)
                throw std::runtime_error("edge IP must be a literal IP address");
            CURLU* parsed = curl_url(); char *host = nullptr, *port = nullptr;
            if (!parsed || curl_url_set(parsed, CURLUPART_URL, url.c_str(), CURLU_DISALLOW_USER) ||
                curl_url_get(parsed, CURLUPART_HOST, &host, 0)) throw std::runtime_error("invalid edge URL");
            curl_url_get(parsed, CURLUPART_PORT, &port, CURLU_DEFAULT_PORT);
            std::string mapping = std::string(host) + ":" + (port ? port : "443") + ":" + ip;
            curl_free(host); curl_free(port); curl_url_cleanup(parsed);
            resolve = curl_slist_append(resolve, mapping.c_str());
            curl_easy_setopt(curl, CURLOPT_RESOLVE, resolve);
        }
        auto result = curl_easy_perform(curl);
        if (result != CURLE_OK) {
            long status = 0; curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
            throw std::runtime_error("edge handshake failed: " + std::string(curl_easy_strerror(result)) + " HTTP=" + std::to_string(status));
        }
        curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 0L);
        if (curl_easy_getinfo(curl, CURLINFO_ACTIVESOCKET, &fd) != CURLE_OK || fd == CURL_SOCKET_BAD)
            throw std::runtime_error("edge socket unavailable");
        } catch (...) {
            curl_easy_cleanup(curl); curl_slist_free_all(headers); curl_slist_free_all(resolve);
            throw;
        }
    }
    ~Edge() { if (curl) curl_easy_cleanup(curl); curl_slist_free_all(headers); curl_slist_free_all(resolve); }
    void wait(short events, Clock::time_point end) {
        for (;;) {
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - Clock::now()).count();
            if (ms <= 0) throw std::runtime_error("edge I/O timeout");
            pollfd descriptor{fd, events, 0}; int n = poll(&descriptor, 1, static_cast<int>(ms));
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) throw std::runtime_error("edge I/O timeout");
            return;
        }
    }
    void probe(size_t count) {
        std::vector<double> samples;
        for (size_t i = 0; i <= count; ++i) {
            unsigned char value = static_cast<unsigned char>(i), response = 0;
            auto before = Clock::now(), deadline = before + std::chrono::seconds(15);
            size_t sent = 0;
            while (!sent) {
                auto result = curl_ws_send(curl, &value, 1, &sent, 0, CURLWS_BINARY);
                if (result == CURLE_AGAIN) wait(POLLOUT, deadline);
                else if (result != CURLE_OK) throw std::runtime_error("edge probe write failed");
            }
            for (;;) {
                size_t received = 0; const curl_ws_frame* meta = nullptr;
                auto result = curl_ws_recv(curl, &response, 1, &received, &meta);
                if (result == CURLE_AGAIN) { wait(POLLIN, deadline); continue; }
                if (result != CURLE_OK || (meta && (meta->flags & CURLWS_CLOSE))) throw std::runtime_error("edge probe closed");
                if (meta && (meta->flags & (CURLWS_PING | CURLWS_PONG))) continue;
                if (received != 1 || response != value || meta->bytesleft) throw std::runtime_error("edge probe mismatch");
                break;
            }
            if (i) samples.push_back(std::chrono::duration<double, std::milli>(Clock::now() - before).count());
        }
        std::sort(samples.begin(), samples.end());
        std::cout << "edge_echo samples=" << samples.size() << " colo=" << colo << " ray=" << ray
                  << " p50_ms=" << (samples[(count-1)/2] + samples[count/2])/2
                  << " p95_ms=" << samples[(count*95+99)/100-1] << " max_ms=" << samples.back() << '\n';
    }

};

int main(int argc, char** argv) {
    try {
        if (argc < 4 || argc > 6 || std::string(argv[1]) != "--probe")
            throw std::runtime_error("usage: edge --probe wss://host/path token-file [IP|-] [probe-count]");
        if (curl_global_init(CURL_GLOBAL_DEFAULT)) throw std::runtime_error("curl initialization failed");
        Edge edge(argv[2], argv[3], argc > 4 && std::string(argv[4]) != "-" ? argv[4] : "");
        if (std::string(argv[1]) == "--probe") {
            int count = argc > 5 ? std::stoi(argv[5]) : 50;
            if (count < 1 || count > 1000) throw std::runtime_error("invalid probe count");
            edge.probe(static_cast<size_t>(count));
        }
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
