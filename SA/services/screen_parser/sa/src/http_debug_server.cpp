/*
 * Copyright (c) 2026 OpenHarmony Contributors
 * Licensed under the Apache License, Version 2.0 (the "License");
 */

#include "http_debug_server.h"

#include <cstring>

#include "json.h"

#if defined(SCREENPARSER_ENABLE_HTTP_DEBUG)
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>
#include <strings.h>
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sstream>
#endif

namespace OHOS {
namespace ScreenParser {

#if defined(SCREENPARSER_ENABLE_HTTP_DEBUG)

namespace {

std::string GuessContentType(const std::string &path) {
    size_t dot = path.find_last_of('.');
    std::string ext = dot == std::string::npos ? "" : path.substr(dot);
    if (ext == ".html") return "text/html; charset=utf-8";
    if (ext == ".js") return "application/javascript; charset=utf-8";
    if (ext == ".css") return "text/css; charset=utf-8";
    if (ext == ".json") return "application/json; charset=utf-8";
    if (ext == ".png") return "image/png";
    if (ext == ".svg") return "image/svg+xml";
    return "application/octet-stream";
}

void SendAll(int fd, const std::string &data) {
    size_t sent = 0;
    while (sent < data.size()) {
        ssize_t n = ::send(fd, data.data() + sent, data.size() - sent, 0);
        if (n <= 0) {
            break;
        }
        sent += static_cast<size_t>(n);
    }
}

void SendResponse(int fd, int status, const std::string &statusText, const std::string &contentType,
                  const std::string &body) {
    std::ostringstream head;
    head << "HTTP/1.1 " << status << " " << statusText << "\r\n"
         << "Content-Type: " << contentType << "\r\n"
         << "Content-Length: " << body.size() << "\r\n"
         << "Cache-Control: no-store\r\n"
         << "X-Content-Type-Options: nosniff\r\n"
         << "Content-Security-Policy: default-src 'self'; img-src 'self' data:; "
            "style-src 'self' 'unsafe-inline'; script-src 'self'\r\n"
         << "Connection: close\r\n\r\n";
    std::string response = head.str() + body;
    SendAll(fd, response);
}

void SendJson(int fd, int status, const std::string &statusText, const std::string &body) {
    SendResponse(fd, status, statusText, "application/json; charset=utf-8", body);
}

// Returns true when the analysis JSON carries an "error" field.
bool IsErrorPayload(const std::string &body) {
    json::Value parsed;
    std::string err;
    if (!json::Value::Parse(body, parsed, err) || !parsed.is_object()) {
        return false;
    }
    return parsed.contains("error");
}

}  // namespace

HttpDebugServer::HttpDebugServer(IScreenParser *service, std::string staticDir)
    : service_(service), staticDir_(std::move(staticDir)) {}

HttpDebugServer::~HttpDebugServer() {
    Stop();
}

bool HttpDebugServer::Start(const std::string &host, int port) {
    if (running_.load()) {
        return true;
    }
    listenFd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listenFd_ < 0) {
        return false;
    }
    int opt = 1;
    ::setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    addr.sin_addr.s_addr = inet_addr(host.c_str());
    if (::bind(listenFd_, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
        ::close(listenFd_);
        listenFd_ = -1;
        return false;
    }
    if (::listen(listenFd_, 8) < 0) {
        ::close(listenFd_);
        listenFd_ = -1;
        return false;
    }
    running_.store(true);
    thread_ = std::thread([this]() { AcceptLoop(); });
    return true;
}

void HttpDebugServer::Stop() {
    if (!running_.exchange(false)) {
        return;
    }
    if (listenFd_ >= 0) {
        ::shutdown(listenFd_, SHUT_RDWR);
        ::close(listenFd_);
        listenFd_ = -1;
    }
    if (thread_.joinable()) {
        thread_.join();
    }
}

void HttpDebugServer::AcceptLoop() {
    while (running_.load()) {
        int clientFd = ::accept(listenFd_, nullptr, nullptr);
        if (clientFd < 0) {
            if (!running_.load()) {
                break;
            }
            continue;
        }
        HandleClient(clientFd);
        ::close(clientFd);
    }
}

void HttpDebugServer::HandleClient(int clientFd) {
    std::string request;
    char buf[4096];
    // Read headers.
    while (request.find("\r\n\r\n") == std::string::npos) {
        ssize_t n = ::recv(clientFd, buf, sizeof(buf), 0);
        if (n <= 0) {
            return;
        }
        request.append(buf, static_cast<size_t>(n));
        if (request.size() > 64 * 1024) {
            SendJson(clientFd, 431, "Request Header Fields Too Large", "{\"error\":\"header too large\"}");
            return;
        }
    }
    size_t headerEnd = request.find("\r\n\r\n");
    std::string headerPart = request.substr(0, headerEnd);
    std::string body = request.substr(headerEnd + 4);

    std::istringstream headerStream(headerPart);
    std::string method, path, version;
    headerStream >> method >> path >> version;

    // Read remaining body per Content-Length.
    size_t contentLength = 0;
    std::string line;
    while (std::getline(headerStream, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        size_t colon = line.find(':');
        if (colon == std::string::npos) {
            continue;
        }
        std::string key = line.substr(0, colon);
        std::string value = line.substr(colon + 1);
        while (!value.empty() && (value.front() == ' ')) {
            value.erase(value.begin());
        }
        if (strcasecmp(key.c_str(), "Content-Length") == 0) {
            contentLength = static_cast<size_t>(std::strtoul(value.c_str(), nullptr, 10));
        }
    }
    contentLength = std::min<size_t>(contentLength, 64 * 1024);
    while (body.size() < contentLength) {
        ssize_t n = ::recv(clientFd, buf, sizeof(buf), 0);
        if (n <= 0) {
            break;
        }
        body.append(buf, static_cast<size_t>(n));
    }

    if (path == "/api/status" && method == "GET") {
        SendJson(clientFd, 200, "OK", service_ ? service_->GetStatus() : std::string("{}"));
        return;
    }
    if (path == "/api/analyze" && method == "POST") {
        std::string result = service_ ? service_->AnalyzeSync(0) : std::string();
        if (IsErrorPayload(result)) {
            SendJson(clientFd, 502, "Bad Gateway", result);
        } else {
            SendJson(clientFd, 200, "OK", result);
        }
        return;
    }
    if (path == "/api/ocr" && method == "POST") {
        std::string result = service_ ? service_->RecognizeText(0) : std::string();
        if (IsErrorPayload(result)) {
            SendJson(clientFd, 502, "Bad Gateway", result);
        } else {
            SendJson(clientFd, 200, "OK", result);
        }
        return;
    }

    // Static file serving (GET only).
    if (method != "GET") {
        SendJson(clientFd, 404, "Not Found", "{\"error\":\"接口不存在\"}");
        return;
    }
    std::string relative = (path == "/" || path.empty()) ? "/index.html" : path;
    size_t query = relative.find('?');
    if (query != std::string::npos) {
        relative = relative.substr(0, query);
    }
    if (relative.find("..") != std::string::npos) {
        SendJson(clientFd, 404, "Not Found", "{\"error\":\"资源不存在\"}");
        return;
    }
    std::string fullPath = staticDir_ + relative;
    std::ifstream file(fullPath, std::ios::binary);
    if (!file) {
        SendJson(clientFd, 404, "Not Found", "{\"error\":\"资源不存在\"}");
        return;
    }
    std::stringstream fileBuffer;
    fileBuffer << file.rdbuf();
    SendResponse(clientFd, 200, "OK", GuessContentType(fullPath), fileBuffer.str());
}

#else  // !SCREENPARSER_ENABLE_HTTP_DEBUG

HttpDebugServer::HttpDebugServer(IScreenParser *service, std::string staticDir)
    : service_(service), staticDir_(std::move(staticDir)) {}
HttpDebugServer::~HttpDebugServer() = default;
bool HttpDebugServer::Start(const std::string &, int) { return false; }
void HttpDebugServer::Stop() {}
void HttpDebugServer::AcceptLoop() {}
void HttpDebugServer::HandleClient(int) {}

#endif

}  // namespace ScreenParser
}  // namespace OHOS
