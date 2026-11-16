#include "TelemetryManager.hpp"

#include <cstdio>
#include <random>
#include <sstream>

#ifdef _WIN32
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

TelemetryManager& TelemetryManager::instance() {
    static TelemetryManager inst;
    return inst;
}

TelemetryManager::~TelemetryManager() {
    shutdown();
}

void TelemetryManager::init(const std::string& serviceName, const std::string& otlpHost, int otlpHttpPort) {
    if (running_.exchange(true)) {
        return; // Ya se encuentra inicializado
    }
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
                                  const std::map<std::string, std::string>& attributes) {
    if (!running_.load()) return;

    // Construcción de atributos en formato estándar OTLP KeyValue
    std::ostringstream attrsJson;
    attrsJson << "{\"key\":\"http.status_code\",\"value\":{\"intValue\":" << statusCode << "}}";
    for (const auto& [k, v] : attributes) {
        attrsJson << ",{\"key\":\"" << escapeJson(k) << "\",\"value\":{\"stringValue\":\"" << escapeJson(v) << "\"}}";
    }

    // Código de estado OTLP: 1 = OK, 2 = ERROR (cuando status HTTP >= 500)
    int otlpStatus = (statusCode >= 500) ? 2 : 1;

    std::ostringstream payload;
    payload << "{\"resourceSpans\":[{\"resource\":{\"attributes\":["
            << "{\"key\":\"service.name\",\"value\":{\"stringValue\":\"" << escapeJson(serviceName_) << "\"}}"
            << "]},\"scopeSpans\":[{\"scope\":{\"name\":\"tecflix.otel\"},\"spans\":[{"
            << "\"traceId\":\"" << traceId << "\","
            << "\"spanId\":\"" << spanId << "\","
            << "\"name\":\"" << escapeJson(operationName) << "\","
            << "\"kind\":2," // SPAN_KIND_SERVER
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
    otel_sock_t sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == OTEL_INVALID_SOCK) return false;

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port_));
    if (inet_pton(AF_INET, host_.c_str(), &addr.sin_addr) <= 0) {
        OTEL_CLOSE_SOCK(sock);
        return false;
    }

    if (connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        OTEL_CLOSE_SOCK(sock);
        return false;
    }

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

    // Señaliza fin del envío HTTP para que el servidor procese el POST completo sin TCP RST
#ifdef _WIN32
    ::shutdown(sock, SD_SEND);
#else
    ::shutdown(sock, SHUT_WR);
#endif

    // Lee la respuesta HTTP (ej. HTTP/1.1 200 OK) antes de cerrar el socket
    char respBuf[256];
    recv(sock, respBuf, sizeof(respBuf) - 1, 0);

    OTEL_CLOSE_SOCK(sock);
    return true;
}

// --- Implementación RAII de ScopedSpan ---

ScopedSpan::ScopedSpan(std::string operationName,
                       std::string traceId,
                       std::map<std::string, std::string> attributes)
    : operationName_(std::move(operationName)),
      traceId_(traceId.empty() ? TelemetryManager::generateHexId(16) : std::move(traceId)),
      spanId_(TelemetryManager::generateHexId(8)),
      startNanos_(TelemetryManager::nowNanos()),
      attributes_(std::move(attributes)) {}

ScopedSpan::~ScopedSpan() {
    uint64_t endNanos = TelemetryManager::nowNanos();
    double durationMs = static_cast<double>(endNanos - startNanos_) / 1'000'000.0;

    auto& tm = TelemetryManager::instance();
    tm.recordSpan(operationName_, traceId_, spanId_, startNanos_, endNanos, statusCode_, attributes_);
    tm.recordMetric("http.server.request.duration_ms", durationMs, "ms", {{"operation", operationName_}});

    if (statusCode_ >= 400) {
        tm.recordMetric("system.errors.total", 1.0, "1",
                        {{"operation", operationName_}, {"status_code", std::to_string(statusCode_)}});
    }
}

} // namespace tecflix::observability