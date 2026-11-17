#include "TelemetryManager.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <sstream>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
using otel_sock_t = SOCKET;
#define OTEL_CLOSE_SOCK closesocket
#define OTEL_INVALID_SOCK INVALID_SOCKET
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using otel_sock_t = int;
#define OTEL_CLOSE_SOCK close
#define OTEL_INVALID_SOCK (-1)
#endif

namespace tecflix::observability {

namespace {

struct ActiveSpanFrame {
    std::string traceId;
    std::string spanId;
};

thread_local std::vector<ActiveSpanFrame> tlsSpanStack;

bool isLowerHex(const std::string& s, size_t expectedLen) {
    if (s.size() != expectedLen) return false;
    bool allZero = true;
    for (unsigned char c : s) {
        if (!std::isxdigit(c)) return false;
        if (c != '0') allZero = false;
    }
    return !allZero;
}

std::string toLowerHex(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

double computePercentile(std::vector<double> sorted, double q) {
    if (sorted.empty()) return 0.0;
    std::sort(sorted.begin(), sorted.end());
    double pos = q * static_cast<double>(sorted.size() - 1);
    size_t idx = static_cast<size_t>(std::llround(pos));
    if (idx >= sorted.size()) idx = sorted.size() - 1;
    return sorted[idx];
}

} // namespace

TelemetryManager& TelemetryManager::instance() {
    static TelemetryManager inst;
    return inst;
}

TelemetryManager::~TelemetryManager() {
    shutdown();
}

void TelemetryManager::init(const std::string& serviceName, const std::string& otlpHost, int otlpHttpPort) {
    if (running_.exchange(true)) {
        return;
    }
#ifdef _WIN32
    WSADATA wsa{};
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    serviceName_ = serviceName;
    host_ = otlpHost;
    port_ = otlpHttpPort;
    workerThread_ = std::thread(&TelemetryManager::workerLoop, this);
}

void TelemetryManager::shutdown() {
    if (!running_.exchange(false)) {
        return;
    }
    queueCv_.notify_all();
    if (workerThread_.joinable()) {
        workerThread_.join();
    }
}

uint64_t TelemetryManager::nowNanos() {
    using namespace std::chrono;
    return static_cast<uint64_t>(
        duration_cast<nanoseconds>(system_clock::now().time_since_epoch()).count());
}

std::string TelemetryManager::generateHexId(size_t numBytes) {
    static thread_local std::mt19937_64 rng(std::random_device{}());
    std::uniform_int_distribution<uint32_t> dist(0, 255);
    std::string out;
    out.reserve(numBytes * 2);
    char buf[3];
    for (size_t i = 0; i < numBytes; ++i) {
        std::snprintf(buf, sizeof(buf), "%02x", dist(rng));
        out += buf;
    }
    return out;
}

TraceContext TelemetryManager::parseTraceparent(const std::string& headerValue) {
    // Formato W3C: 00-<32hex traceId>-<16hex parentSpanId>-<2hex flags>
    if (headerValue.size() < 55) return {};
    if (headerValue[2] != '-' || headerValue[35] != '-' || headerValue[52] != '-') return {};

    std::string version = toLowerHex(headerValue.substr(0, 2));
    std::string traceId = toLowerHex(headerValue.substr(3, 32));
    std::string parentId = toLowerHex(headerValue.substr(36, 16));
    std::string flags = toLowerHex(headerValue.substr(53, 2));

    if (version == "ff" || !std::isxdigit(static_cast<unsigned char>(version[0])) ||
        !std::isxdigit(static_cast<unsigned char>(version[1]))) {
        return {};
    }
    if (!isLowerHex(traceId, 32) || !isLowerHex(parentId, 16)) {
        return {};
    }
    if (!std::isxdigit(static_cast<unsigned char>(flags[0])) ||
        !std::isxdigit(static_cast<unsigned char>(flags[1]))) {
        return {};
    }

    return TraceContext{traceId, parentId, true};
}

std::string TelemetryManager::formatTraceparent(const std::string& traceId,
                                                const std::string& spanId,
                                                bool sampled) {
    if (!isLowerHex(traceId, 32) || !isLowerHex(spanId, 16)) return "";
    return "00-" + toLowerHex(traceId) + "-" + toLowerHex(spanId) + (sampled ? "-01" : "-00");
}

std::string TelemetryManager::currentTraceparent() {
    if (tlsSpanStack.empty()) return "";
    const auto& top = tlsSpanStack.back();
    return formatTraceparent(top.traceId, top.spanId, true);
}

std::string TelemetryManager::escapeJson(const std::string& input) {
    std::string out;
    out.reserve(input.size() + 8);
    for (unsigned char c : input) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:   out += static_cast<char>(c); break;
        }
    }
    return out;
}

void TelemetryManager::recordSpan(const std::string& operationName,
                                  const std::string& traceId,
                                  const std::string& spanId,
                                  uint64_t startTimeNanos,
                                  uint64_t endTimeNanos,
                                  int statusCode,
                                  const std::map<std::string, std::string>& attributes,
                                  const std::string& parentSpanId) {
    if (!running_.load()) return;

    std::ostringstream attrsJson;
    attrsJson << "{\"key\":\"http.status_code\",\"value\":{\"intValue\":" << statusCode << "}}";
    for (const auto& [k, v] : attributes) {
        attrsJson << ",{\"key\":\"" << escapeJson(k) << "\",\"value\":{\"stringValue\":\"" << escapeJson(v) << "\"}}";
    }

    int otlpStatus = (statusCode >= 500) ? 2 : 1;

    std::ostringstream payload;
    payload << "{\"resourceSpans\":[{\"resource\":{\"attributes\":["
            << "{\"key\":\"service.name\",\"value\":{\"stringValue\":\"" << escapeJson(serviceName_) << "\"}}"
            << "]},\"scopeSpans\":[{\"scope\":{\"name\":\"tecflix.otel\"},\"spans\":[{"
            << "\"traceId\":\"" << traceId << "\","
            << "\"spanId\":\"" << spanId << "\",";
    if (!parentSpanId.empty()) {
        payload << "\"parentSpanId\":\"" << escapeJson(parentSpanId) << "\",";
    }
    payload << "\"name\":\"" << escapeJson(operationName) << "\","
            << "\"kind\":2,"
            << "\"startTimeUnixNano\":\"" << startTimeNanos << "\","
            << "\"endTimeUnixNano\":\"" << endTimeNanos << "\","
            << "\"attributes\":[" << attrsJson.str() << "],"
            << "\"status\":{\"code\":" << otlpStatus << "}"
            << "}]}]}]}";

    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        if (taskQueue_.size() < 2048) {
            taskQueue_.push({"/v1/traces", payload.str()});
        }
    }
    queueCv_.notify_one();
}

void TelemetryManager::recordMetric(const std::string& metricName,
                                    double value,
                                    const std::string& unit,
                                    const std::map<std::string, std::string>& attributes) {
    if (!running_.load()) return;

    std::ostringstream attrsJson;
    bool first = true;
    for (const auto& [k, v] : attributes) {
        if (!first) attrsJson << ",";
        attrsJson << "{\"key\":\"" << escapeJson(k) << "\",\"value\":{\"stringValue\":\"" << escapeJson(v) << "\"}}";
        first = false;
    }

    uint64_t ts = nowNanos();
    std::ostringstream payload;
    payload << "{\"resourceMetrics\":[{\"resource\":{\"attributes\":["
            << "{\"key\":\"service.name\",\"value\":{\"stringValue\":\"" << escapeJson(serviceName_) << "\"}}"
            << "]},\"scopeMetrics\":[{\"scope\":{\"name\":\"tecflix.otel\"},\"metrics\":[{"
            << "\"name\":\"" << escapeJson(metricName) << "\","
            << "\"unit\":\"" << escapeJson(unit) << "\","
            << "\"gauge\":{\"dataPoints\":[{"
            << "\"asDouble\":" << value << ","
            << "\"timeUnixNano\":\"" << ts << "\","
            << "\"attributes\":[" << attrsJson.str() << "]"
            << "}]}"
            << "}]}]}]}";

    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        if (taskQueue_.size() < 2048) {
            taskQueue_.push({"/v1/metrics", payload.str()});
        }
    }
    queueCv_.notify_one();
}

void TelemetryManager::recordCounter(const std::string& metricName,
                                     uint64_t cumulativeValue,
                                     const std::map<std::string, std::string>& attributes) {
    if (!running_.load()) return;

    std::ostringstream attrsJson;
    bool first = true;
    for (const auto& [k, v] : attributes) {
        if (!first) attrsJson << ",";
        attrsJson << "{\"key\":\"" << escapeJson(k) << "\",\"value\":{\"stringValue\":\"" << escapeJson(v) << "\"}}";
        first = false;
    }

    uint64_t ts = nowNanos();
    std::ostringstream payload;
    payload << "{\"resourceMetrics\":[{\"resource\":{\"attributes\":["
            << "{\"key\":\"service.name\",\"value\":{\"stringValue\":\"" << escapeJson(serviceName_) << "\"}}"
            << "]},\"scopeMetrics\":[{\"scope\":{\"name\":\"tecflix.otel\"},\"metrics\":[{"
            << "\"name\":\"" << escapeJson(metricName) << "\","
            << "\"unit\":\"1\","
            << "\"sum\":{\"aggregationTemporality\":2,\"isMonotonic\":true,\"dataPoints\":[{"
            << "\"asInt\":\"" << cumulativeValue << "\","
            << "\"timeUnixNano\":\"" << ts << "\","
            << "\"attributes\":[" << attrsJson.str() << "]"
            << "}]}"
            << "}]}]}]}";

    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        if (taskQueue_.size() < 2048) {
            taskQueue_.push({"/v1/metrics", payload.str()});
        }
    }
    queueCv_.notify_one();
}

LatencySummary TelemetryManager::recordHttpObservation(const std::string& operationName,
                                                       int statusCode,
                                                       double durationMs) {
    std::lock_guard<std::mutex> lock(statsMutex_);
    std::string counterKey = operationName + "|" + std::to_string(statusCode);
    uint64_t totalCount = ++requestCounters_[counterKey];

    auto& samples = latencySamples_[operationName];
    if (samples.size() >= 1024) {
        samples.erase(samples.begin());
    }
    samples.push_back(durationMs);

    return LatencySummary{
        totalCount,
        computePercentile(samples, 0.50),
        computePercentile(samples, 0.95),
        computePercentile(samples, 0.99)
    };
}

void TelemetryManager::workerLoop() {
    while (true) {
        OtlpTask task;
        {
            std::unique_lock<std::mutex> lock(queueMutex_);
            queueCv_.wait(lock, [this] { return !taskQueue_.empty() || !running_.load(); });
            if (!running_.load() && taskQueue_.empty()) {
                break;
            }
            task = std::move(taskQueue_.front());
            taskQueue_.pop();
        }
        sendHttpPost(task.endpoint, task.payload);
    }
}

bool TelemetryManager::sendHttpPost(const std::string& endpoint, const std::string& jsonBody) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    addrinfo* res = nullptr;
    const std::string portStr = std::to_string(port_);
    if (getaddrinfo(host_.c_str(), portStr.c_str(), &hints, &res) != 0 || !res) {
        return false;
    }

    otel_sock_t sock = OTEL_INVALID_SOCK;
    for (addrinfo* rp = res; rp != nullptr; rp = rp->ai_next) {
        sock = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (sock == OTEL_INVALID_SOCK) continue;
        if (connect(sock, rp->ai_addr, static_cast<int>(rp->ai_addrlen)) == 0) {
            break;
        }
        OTEL_CLOSE_SOCK(sock);
        sock = OTEL_INVALID_SOCK;
    }
    freeaddrinfo(res);

    if (sock == OTEL_INVALID_SOCK) return false;

    std::ostringstream req;
    req << "POST " << endpoint << " HTTP/1.1\r\n"
        << "Host: " << host_ << ":" << port_ << "\r\n"
        << "Content-Type: application/json\r\n"
        << "Content-Length: " << jsonBody.size() << "\r\n"
        << "Connection: close\r\n\r\n"
        << jsonBody;

    std::string raw = req.str();
    size_t sentTotal = 0;
    while (sentTotal < raw.size()) {
        int sent = send(sock, raw.data() + sentTotal, static_cast<int>(raw.size() - sentTotal), 0);
        if (sent <= 0) {
            OTEL_CLOSE_SOCK(sock);
            return false;
        }
        sentTotal += static_cast<size_t>(sent);
    }

#ifdef _WIN32
    ::shutdown(sock, SD_SEND);
#else
    ::shutdown(sock, SHUT_WR);
#endif

    char respBuf[256];
    recv(sock, respBuf, sizeof(respBuf) - 1, 0);

    OTEL_CLOSE_SOCK(sock);
    return true;
}

// --- Implementación RAII de ScopedSpan ---

ScopedSpan::ScopedSpan(std::string operationName,
                       std::string traceId,
                       std::map<std::string, std::string> attributes,
                       std::string parentSpanId)
    : operationName_(std::move(operationName)),
      spanId_(TelemetryManager::generateHexId(8)),
      startNanos_(TelemetryManager::nowNanos()),
      attributes_(std::move(attributes)) {
    if (!traceId.empty()) {
        traceId_ = std::move(traceId);
        parentSpanId_ = std::move(parentSpanId);
    } else if (!tlsSpanStack.empty()) {
        traceId_ = tlsSpanStack.back().traceId;
        parentSpanId_ = tlsSpanStack.back().spanId;
    } else {
        traceId_ = TelemetryManager::generateHexId(16);
        parentSpanId_ = std::move(parentSpanId);
    }
    tlsSpanStack.push_back(ActiveSpanFrame{traceId_, spanId_});
}

ScopedSpan::~ScopedSpan() {
    if (!tlsSpanStack.empty()) {
        tlsSpanStack.pop_back();
    }

    uint64_t endNanos = TelemetryManager::nowNanos();
    double durationMs = static_cast<double>(endNanos - startNanos_) / 1'000'000.0;

    auto& tm = TelemetryManager::instance();
    tm.recordSpan(operationName_, traceId_, spanId_, startNanos_, endNanos, statusCode_, attributes_, parentSpanId_);

    std::string statusStr = std::to_string(statusCode_);
    auto summary = tm.recordHttpObservation(operationName_, statusCode_, durationMs);

    tm.recordCounter("http.server.requests.total", summary.count,
                     {{"operation", operationName_}, {"http.status_code", statusStr}});
    tm.recordMetric("http.server.request.duration_ms", durationMs, "ms",
                    {{"operation", operationName_}});
    tm.recordMetric("http.server.request.duration_ms.p50", summary.p50, "ms",
                    {{"operation", operationName_}});
    tm.recordMetric("http.server.request.duration_ms.p95", summary.p95, "ms",
                    {{"operation", operationName_}});
    tm.recordMetric("http.server.request.duration_ms.p99", summary.p99, "ms",
                    {{"operation", operationName_}});

    if (statusCode_ >= 400) {
        tm.recordMetric("system.errors.total", 1.0, "1",
                        {{"operation", operationName_}, {"status_code", statusStr}});
    }
}

} // namespace tecflix::observability