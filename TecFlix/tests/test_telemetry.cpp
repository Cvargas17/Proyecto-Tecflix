#include "TelemetryManager.hpp"
#include <chrono>
#include <iostream>
#include <thread>

#ifdef _WIN32
#include <winsock2.h> // Necesario en Windows para inicializar Winsock
#endif

int main() {
#ifdef _WIN32
    // Inicializa la librería de sockets de Windows (Winsock 2.2)
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        std::cerr << "Error inicializando Winsock\n";
        return 1;
    }
#endif

    // 1. Inicializa el gestor apuntando al Dashboard Aspire local (puerto OTLP HTTP 4318)
    auto& tm = tecflix::observability::TelemetryManager::instance();
    tm.init("tecflix-metadata-api-test", "127.0.0.1", 4318);

    {
        // 2. Simula una operación HTTP/DB usando RAII (mide latencia automáticamente al cerrar llave)
        tecflix::observability::ScopedSpan span("GET /api/videos", "", {{"component", "metadata"}});
        std::this_thread::sleep_for(std::chrono::milliseconds(45));
        span.setStatusCode(200);
    }

    // 3. Pausa breve para dar tiempo al hilo en segundo plano de enviar el paquete HTTP
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    tm.shutdown();

#ifdef _WIN32
    WSACleanup(); // Libera los recursos de Winsock al finalizar
#endif

    std::cout << "Telemetria enviada correctamente.\n";
    return 0;
}