#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

namespace tecflix::observability {

struct OtlpTask {
    std::string endpoint; ///< Ruta HTTP destino: "/v1/traces" o "/v1/metrics"
    std::string payload;  ///< Cuerpo serializado siguiendo el estándar OTLP/JSON
};

struct TraceContext {
    std::string traceId;
    std::string parentSpanId;
    bool valid = false;
};

struct LatencySummary {
    uint64_t count = 0;
    double p50 = 0.0;
    double p95 = 0.0;
    double p99 = 0.0;
};

class TelemetryManager {
public:
    static TelemetryManager& instance();

    TelemetryManager(const TelemetryManager&) = delete;
    TelemetryManager& operator=(const TelemetryManager&) = delete;

    void init(const std::string& serviceName,
              const std::string& otlpHost = "127.0.0.1",
              int otlpHttpPort = 4318);

    void shutdown();

    void recordSpan(const std::string& operationName,
                    const std::string& traceId,
                    const std::string& spanId,
                    uint64_t startTimeNanos,
                    uint64_t endTimeNanos,
                    int statusCode,
                    const std::map<std::string, std::string>& attributes = {},
                    const std::string& parentSpanId = "");

    void recordMetric(const std::string& metricName,
                      double value,
                      const std::string& unit = "1",
                      const std::map<std::string, std::string>& attributes = {});

    void recordCounter(const std::string& metricName,
                       uint64_t cumulativeValue,
                       const std::map<std::string, std::string>& attributes = {});

    LatencySummary recordHttpObservation(const std::string& operationName,
                                         int statusCode,
                                         double durationMs);

    static TraceContext parseTraceparent(const std::string& headerValue);
    static std::string formatTraceparent(const std::string& traceId,
                                         const std::string& spanId,
                                         bool sampled = true);
    static std::string currentTraceparent();

    static std::string generateHexId(size_t numBytes);
    static uint64_t nowNanos();

private:
    TelemetryManager() = default;
    ~TelemetryManager();

    void workerLoop();
    bool sendHttpPost(const std::string& endpoint, const std::string& jsonBody);
    static std::string escapeJson(const std::string& input);

    std::string serviceName_{"tecflix-service"};
    std::string host_{"127.0.0.1"};
    int port_{4318};

    std::atomic<bool> running_{false};
    std::mutex queueMutex_;
    std::condition_variable queueCv_;
    std::queue<OtlpTask> taskQueue_;
    std::thread workerThread_;

    std::mutex statsMutex_;
    std::map<std::string, uint64_t> requestCounters_;
    std::map<std::string, std::vector<double>> latencySamples_;
};

class ScopedSpan {
public:
    ScopedSpan(std::string operationName,
               std::string traceId = "",
               std::map<std::string, std::string> attributes = {},
               std::string parentSpanId = "");
    ~ScopedSpan();

    void setStatusCode(int code) { statusCode_ = code; }
    void addAttribute(const std::string& key, const std::string& value) { attributes_[key] = value; }

    const std::string& traceId() const { return traceId_; }
    const std::string& spanId() const { return spanId_; }

private:
    std::string operationName_;
    std::string traceId_;
    std::string spanId_;
    std::string parentSpanId_;
    uint64_t startNanos_;
    int statusCode_{200};
    std::map<std::string, std::string> attributes_;
};

} // namespace tecflix::observability