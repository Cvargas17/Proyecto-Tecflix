/**
 * @file MetadataController.hpp
 * @brief Declaración del controlador REST HTTP para el microservicio de Metadata de TecFlix.
 * @details Traduce peticiones HTTP entrantes (`HttpRequest`) hacia invocaciones de `MetadataService`,
 *          extrayendo cabeceras de contexto (`X-User`, `X-Trace-Id`), parámetros de URL y campos JSON.
 */

#pragma once

#include "MetadataService.hpp"
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace tecflix::metadata {

/**
 * @struct HttpRequest
 * @brief Estructura ligera e independiente del servidor de red que modela una petición HTTP.
 */
struct HttpRequest {
    std::string method;                              ///< Verbo HTTP ("GET", "POST", "PUT", "PATCH", "DELETE").
    std::string path;                                ///< Ruta solicitada sin query string (ej. "/api/videos").
    std::map<std::string, std::string> queryParams;  ///< Mapa clave-valor de parámetros de consulta URL.
    std::map<std::string, std::string> headers;      ///< Mapa de cabeceras HTTP entrantes.
    std::string body;                                ///< Cuerpo de la petición en formato JSON.
};

/**
 * @class MetadataController
 * @brief Enrutador y adaptador HTTP para los endpoints REST de metadatos de video.
 */
class MetadataController {
public:
    /**
     * @brief Construye el controlador inyectando la capa de servicio de negocio.
     * @param service Puntero compartido a `MetadataService`.
     */
    explicit MetadataController(std::shared_ptr<MetadataService> service);

    /**
     * @brief Procesa y enruta una petición HTTP completa hacia el caso de uso correspondiente.
     * @param req Estructura `HttpRequest` con método, ruta, cabeceras, query params y cuerpo.
     * @return `ServiceResponse` con el código HTTP resultante y la respuesta JSON.
     */
    ServiceResponse handleRequest(const HttpRequest& req);

private:
    static std::string getHeaderIgnoreCase(const std::map<std::string, std::string>& headers,
                                           const std::string& key);
    static std::string extractJsonString(const std::string& json, const std::string& key);
    static std::vector<std::string> extractJsonStringArray(const std::string& json, const std::string& key);
    static bool hasJsonKey(const std::string& json, const std::string& key);

    std::shared_ptr<MetadataService> service_; ///< Instancia de la capa de lógica de negocio.
};

} // namespace tecflix::metadata