/**
 * @file MetadataService.hpp
 * @brief Declaración de la capa de lógica de negocio para el microservicio de Metadata de TecFlix.
 * @details Encapsula las reglas de dominio, validación de campos de entrada, políticas de visibilidad
 *          (público vs. privado) y control de propiedad (`owner`) antes de delegar la persistencia
 *          a la interfaz `IMetadataRepository`.
 */

#pragma once

#include "IMetadataRepository.hpp"
#include <memory>
#include <string>
#include <vector>

namespace tecflix::metadata {

/**
 * @struct ServiceResponse
 * @brief Representa el resultado estándar de una operación de negocio.
 * @details Desacopla la lógica de dominio del transporte HTTP al devolver un código de estado
 *          semántico (ej. 200, 201, 400, 403, 404) junto con el cuerpo serializado en JSON.
 */
struct ServiceResponse {
    int statusCode{200};   ///< Código de estado HTTP equivalente resultante de la operación.
    std::string jsonBody;  ///< Carga útil serializada en formato JSON lista para enviarse al cliente.
};

/**
 * @class MetadataService
 * @brief Servicio de dominio encargado de gestionar el ciclo de vida de los metadatos de video.
 *
 * Responsabilidades principales:
 * - Validar integridad de datos en creación y actualización (campos requeridos, valores permitidos).
 * - Aplicar autorización basada en propiedad (`owner`) para consultas privadas, edición y borrado.
 * - Normalizar parámetros de paginación y filtrado antes de consultar el repositorio.
 * - Instrumentar cada operación mediante spans de OpenTelemetry (`ScopedSpan`).
 */
class MetadataService {
public:
    /**
     * @brief Construye una instancia del servicio inyectando el repositorio de persistencia.
     * @param repository Puntero compartido a una implementación de `IMetadataRepository` (MongoDB o Mock).
     */
    explicit MetadataService(std::shared_ptr<IMetadataRepository> repository);

    /**
     * @brief Valida y registra los metadatos de un nuevo video en el catálogo.
     * @param video Estructura `VideoMetadata` con los datos iniciales y metadatos técnicos.
     * @return `ServiceResponse` con código `201 Created` y el JSON del video creado,
     *         `400 Bad Request` si faltan campos obligatorios o los valores son inválidos,
     *         o `500 Internal Server Error` si falla la persistencia.
     */
    ServiceResponse registerVideo(VideoMetadata video);

    /**
     * @brief Recupera los metadatos detallados de un video verificando permisos de acceso.
     * @param videoId Identificador único del video solicitado.
     * @param requesterUsername Nombre del usuario autenticado que realiza la consulta (vacío si es anónimo).
     * @return `ServiceResponse` con código `200 OK` si es público o pertenece al solicitante,
     *         `400 Bad Request` si el ID está vacío, `403 Forbidden` si es privado de otro usuario,
     *         o `404 Not Found` si no existe.
     */
    ServiceResponse getVideoById(const std::string& videoId, const std::string& requesterUsername);

    /**
     * @brief Obtiene una lista paginada y filtrada de videos según el alcance y criterios solicitados.
     * @param filter Parámetros de búsqueda (`scope`, `page`, `limit`, `sortBy`, `order`, `topicFilter`, `tagFilter`).
     * @return `ServiceResponse` con código `200 OK` y el arreglo paginado en JSON,
     *         o `401 Unauthorized` si se solicita `scope="mine"` sin autenticación.
     */
    ServiceResponse listVideos(VideoQueryFilter filter);

    /**
     * @brief Actualiza parcialmente los metadatos editables de un video existente.
     * @param videoId Identificador único del video a modificar.
     * @param requesterUsername Usuario autenticado que solicita la modificación.
     * @param newTitle Nuevo título del video (se ignora si está vacío).
     * @param newVisibility Nueva visibilidad (`"public"` o `"private"`, se ignora si está vacío).
     * @param newTopic Nuevo tema o categoría asignada.
     * @param updateTopic Bandera que indica si debe sobrescribirse el campo `topic`.
     * @param newTags Nuevo vector de etiquetas descriptivas.
     * @param updateTags Bandera que indica si debe sobrescribirse el arreglo `tags`.
     * @return `ServiceResponse` con `200 OK` y el recurso actualizado, `400 Bad Request`,
     *         `401 Unauthorized`, `403 Forbidden` si no es el propietario, o `404 Not Found`.
     */
    ServiceResponse updateVideoMetadata(const std::string& videoId,
                                        const std::string& requesterUsername,
                                        const std::string& newTitle,
                                        const std::string& newVisibility,
                                        const std::string& newTopic,
                                        bool updateTopic,
                                        const std::vector<std::string>& newTags,
                                        bool updateTags);

    /**
     * @brief Actualiza el estado del flujo de transcodificación y empaquetado MPEG-DASH de un video.
     * @param videoId Identificador único del video.
     * @param dashStatus Nuevo estado DASH (`"pending"`, `"processing"`, `"ready"` o `"error"`).
     * @return `ServiceResponse` con `200 OK` si se actualizó, `400 Bad Request` si el estado es inválido,
     *         o `404 Not Found` si el video no existe.
     */
    ServiceResponse updateDashState(const std::string& videoId, const std::string& dashStatus);

    /**
     * @brief Elimina permanentemente el registro de metadatos de un video.
     * @param videoId Identificador único del video a eliminar.
     * @param requesterUsername Usuario autenticado que solicita el borrado.
     * @return `ServiceResponse` con `200 OK` si se eliminó, `401 Unauthorized`,
     *         `403 Forbidden` si el solicitante no es el dueño, o `404 Not Found`.
     */
    ServiceResponse deleteVideo(const std::string& videoId, const std::string& requesterUsername);

private:
    /**
     * @brief Verifica si la cadena de visibilidad corresponde a `"public"` o `"private"`.
     * @param visibility Cadena de visibilidad a validar.
     * @return `true` si es un valor válido, `false` en caso contrario.
     */
    static bool isValidVisibility(const std::string& visibility);

    /**
     * @brief Verifica si el estado DASH pertenece al conjunto permitido del ciclo de procesamiento.
     * @param status Cadena de estado DASH a validar.
     * @return `true` si es `"pending"`, `"processing"`, `"ready"` o `"error"`.
     */
    static bool isValidDashStatus(const std::string& status);

    /**
     * @brief Escapa caracteres especiales para construir cadenas JSON seguras.
     * @param input Cadena original sin escapar.
     * @return Cadena escapada apta para incluirse dentro de comillas JSON.
     */
    static std::string escapeJson(const std::string& input);

    std::shared_ptr<IMetadataRepository> repo_; ///< Repositorio inyectado para operaciones CRUD.
};

} // namespace tecflix::metadata