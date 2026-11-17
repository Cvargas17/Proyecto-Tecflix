#include "TelemetryManager.hpp"
#include <cassert>
#include <chrono>
#include <iostream>
#include <thread>

#ifdef _WIN32
#include <winsock2.h>
#endif

using namespace tecflix::observability;

int main() {
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        std::cerr << "Error inicializando Winsock\n";
        return 1;
    }
#endif

    // 1. Validar parsing y formateo W3C traceparent
    const std::string validTp = "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01";
    auto ctx = TelemetryManager::parseTraceparent(validTp);
    assert(ctx.valid);
    assert(ctx.traceId == "4bf92f3577b34da6a3ce929d0e0e4736");
    assert(ctx.parentSpanId == "00f067aa0ba902b7");
    assert(TelemetryManager::formatTraceparent(ctx.traceId, ctx.parentSpanId, true) == validTp);
    assert(!TelemetryManager::parseTraceparent("invalid-header").valid);

    // 2. Inicializar TelemetryManager
    auto& tm = TelemetryManager::instance();
    tm.init("tecflix-telemetry-unit-test", "127.0.0.1", 4318);

    // 3. Validar propagación de contexto entre spans padre e hijo
    {
        ScopedSpan parentSpan("GET /api/videos", ctx.traceId, {{"component", "http"}}, ctx.parentSpanId);
        std::string activeTp = TelemetryManager::currentTraceparent();
        assert(activeTp.find(ctx.traceId) != std::string::npos);

        {
            ScopedSpan childSpan("MongoDB.findPaginated");
            assert(childSpan.traceId() == ctx.traceId);
            std::this_thread::sleep_for(std::chrono::milliseconds(15));
            childSpan.setStatusCode(200);
        }

        parentSpan.addAttribute("http.status_code", "200");
        parentSpan.setStatusCode(200);
    }

    // 4. Validar métricas de error y percentiles p50/p95/p99
    {
        ScopedSpan errSpan("POST /api/upload");
        errSpan.setStatusCode(413);
    }
    auto summary = tm.recordHttpObservation("GET /api/videos", 200, 12.5);
    assert(summary.count >= 2);
    assert(summary.p99 >= summary.p95 && summary.p95 >= summary.p50);

    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    tm.shutdown();

#ifdef _WIN32
    WSACleanup();
#endif

    std::cout << "[PASS] Telemetria W3C y metricas verificadas correctamente.\n";
    return 0;
}