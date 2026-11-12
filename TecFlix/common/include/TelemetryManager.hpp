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

namespace tecflix::observability {

/**
 * @struct OtlpTask
 * @brief Representa un paquete de telemetría (traza o métrica) pendiente de envío al colector OTLP.
 */
struct OtlpTask {
    std::string endpoint; ///< Ruta HTTP destino: "/v1/traces" o "/v1/metrics"
    std::string payload;  ///< Cuerpo serializado siguiendo el estándar OTLP/JSON
};

/**
 * @class TelemetryManager
 * @brief Gestor centralizado de observabilidad OpenTelemetry (OTLP/HTTP) para todos los microservicios.
 *
 * Implementa el patrón Singleton con un hilo consumidor en segundo plano (Producer-Consumer)
 * para garantizar que el envío de trazas y métricas al Dashboard (Aspire / OTel Collector)
 * sea no bloqueante y no degrade la latencia de las APIs de Usuarios, Metadata o Streaming.
 */
class TelemetryManager {
public:
    /**
     * @brief Obtiene la instancia única del gestor de telemetría.
     * @return Referencia a la instancia global de TelemetryManager.
     */
    static TelemetryManager& instance();

    // Se prohíbe copia y asignación para garantizar una única instancia y manejo seguro de hilos.
    TelemetryManager(const TelemetryManager&) = delete;
    TelemetryManager& operator=(const TelemetryManager&) = delete;

    /**
     * @brief Inicializa el exportador OTLP y arranca el hilo de envío asíncrono.
     * @param serviceName Nombre identificador del componente (ej. "tecflix-metadata-api").
     * @param otlpHost Dirección IP o hostname del colector/dashboard (por defecto "127.0.0.1").
     * @param otlpHttpPort Puerto HTTP OTLP (por defecto 4318).
     */
    void init(const std::string& serviceName,
              const std::string& otlpHost = "127.0.0.1",
              int otlpHttpPort = 4318);

    /**
     * @brief Detiene ordenadamente el hilo exportador, vaciando las tareas pendientes.
     */
    void shutdown();

    /**
     * @brief Registra y encola una traza distribuida (Span) en formato OTLP.
     * @param operationName Nombre de la operación (ej. "POST /api/videos", "MongoDB.find").
     * @param traceId Identificador hexadecimal de 32 caracteres de la traza global.
     * @param spanId Identificador hexadecimal de 16 caracteres del span actual.
     * @param startTimeNanos Marca de tiempo de inicio en nanosegundos (Epoch).
     * @param endTimeNanos Marca de tiempo de finalización en nanosegundos (Epoch).
     * @param statusCode Código HTTP o estado de la operación (ej. 200, 404, 500).
     * @param attributes Mapa clave-valor con metadatos adicionales del span.
     */
    void recordSpan(const std::string& operationName,
                    const std::string& traceId,
                    const std::string& spanId,
                    uint64_t startTimeNanos,
                    uint64_t endTimeNanos,
                    int statusCode,
                    const std::map<std::string, std::string>& attributes = {});

    /**
     * @brief Registra y encola un punto de métrica numérica (Gauge/Counter) en formato OTLP.
     * @param metricName Nombre de la métrica (ej. "http.server.request.duration_ms", "system.errors.total").
     * @param value Valor numérico registrado.
     * @param unit Unidad de medida (ej. "ms", "1", "By").
     * @param attributes Etiquetas dimensionales (ej. {{"endpoint", "/api/videos"}}).
     */
    void recordMetric(const std::string& metricName,
                      double value,
                      const std::string& unit = "1",
                      const std::map<std::string, std::string>& attributes = {});

    /**
     * @brief Genera un identificador hexadecimal aleatorio válido para TraceId (16 bytes) o SpanId (8 bytes).
     * @param numBytes Cantidad de bytes (16 para TraceId, 8 para SpanId).
     * @return Cadena hexadecimal en minúsculas.
     */
    static std::string generateHexId(size_t numBytes);

    /**
     * @brief Obtiene el timestamp actual del reloj del sistema en nanosegundos.
     * @return Nanosegundos transcurridos desde Unix Epoch.
     */
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
};

/**
 * @class ScopedSpan
 * @brief Envoltura RAII (Resource Acquisition Is Initialization) para medir latencia automáticamente.
 *
 * Al instanciarse captura el tiempo inicial; al destruirse (al salir de una función o bloque)
 * calcula la duración exacta, emite el Span y registra la métrica de latencia en milisegundos.
 */
class ScopedSpan {
public:
    ScopedSpan(std::string operationName,
               std::string traceId = "",
               std::map<std::string, std::string> attributes = {});
    ~ScopedSpan();

    /**
     * @brief Permite actualizar el código de estado HTTP antes de que el Span se cierre.
     * @param code Código HTTP resultante (por defecto 200).
     */
    void setStatusCode(int code) { statusCode_ = code; }

    /**
     * @brief Añade un atributo dinámico durante la ejecución de la operación.
     */
    void addAttribute(const std::string& key, const std::string& value) { attributes_[key] = value; }

    const std::string& traceId() const { return traceId_; }

private:
    std::string operationName_;
    std::string traceId_;
    std::string spanId_;
    uint64_t startNanos_;
    int statusCode_{200};
    std::map<std::string, std::string> attributes_;
};

} // namespace tecflix::observability