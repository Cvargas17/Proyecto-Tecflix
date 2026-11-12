/**
 * @file IMetadataRepository.hpp
 * @brief Definición del modelo de dominio `VideoMetadata`, estructuras de consulta paginada
 *        e interfaz abstracta `IMetadataRepository` para el microservicio de Metadata de TecFlix.
 * @details Desacopla la lógica de negocio (`MetadataService`) de la tecnología de almacenamiento
 *          subyacente (MongoDB en producción o repositorios simulados en memoria para pruebas unitarias).
 */

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace tecflix::metadata {

/**
 * @struct VideoMetadata
 * @brief Entidad de dominio que representa los metadatos completos y técnicos de un video en TecFlix.
 */
struct VideoMetadata {
    std::string id;                       ///< Identificador único del video (ej. "vid_001").
    std::string owner;                    ///< Nombre de usuario del propietario que subió el video.
    std::string title;                    ///< Título descriptivo del video.
    std::string topic;                    ///< Categoría o tema principal del contenido.
    std::vector<std::string> tags;        ///< Lista de etiquetas descriptivas asociadas.
    std::string visibility{"public"};     ///< Nivel de visibilidad ("public" o "private").
    std::string file;                     ///< Ruta o nombre del archivo original almacenado.
    std::string dash{"pending"};          ///< Estado de empaquetado MPEG-DASH ("pending", "processing", "ready", "error").
    int64_t uploaded{0};                  ///< Marca de tiempo Unix (en segundos) de creación del registro.

    /**
     * @brief Serializa la entidad a una cadena JSON incluyendo el campo calculado `canEdit`.
     * @param requesterUsername Usuario que solicita el recurso (para determinar si `owner == requesterUsername`).
     * @return Objeto JSON serializado con todos los atributos del video.
     */
    std::string toJson(const std::string& requesterUsername = "") const;
};

/**
 * @struct VideoQueryFilter
 * @brief Parámetros de filtrado, ordenamiento y paginación para consultas de catálogo.
 */
struct VideoQueryFilter {
    std::string requesterUsername;        ///< Usuario autenticado que realiza la consulta (vacío si es anónimo).
    std::string scope{"all"};             ///< Alcance de búsqueda: "all" (públicos + propios) o "mine" (solo propios).
    std::string sortBy{"uploaded"};       ///< Campo de ordenamiento ("uploaded" o "title").
    std::string order{"desc"};            ///< Dirección de ordenamiento ("asc" o "desc").
    std::string topicFilter;              ///< Filtro opcional por coincidencia exacta de tema.
    std::string tagFilter;                ///< Filtro opcional por presencia de una etiqueta específica.
    int page{1};                          ///< Número de página solicitada (base 1).
    int limit{12};                        ///< Cantidad máxima de elementos por página (rango 1..50).
};

/**
 * @struct PaginatedVideos
 * @brief Resultado estructurado de una consulta paginada sobre el catálogo de videos.
 */
struct PaginatedVideos {
    std::vector<VideoMetadata> items;     ///< Lista de videos correspondientes a la página actual.
    int page{1};                          ///< Página actual devuelta.
    int limit{12};                        ///< Límite de elementos por página aplicado.
    int totalPages{1};                    ///< Total de páginas disponibles según el filtro.
    int totalItems{0};                    ///< Cantidad total de registros que cumplen el filtro.
};

/**
 * @class IMetadataRepository
 * @brief Interfaz abstracta (contrato) para la persistencia de metadatos de videos.
 */
class IMetadataRepository {
public:
    virtual ~IMetadataRepository() = default;

    /**
     * @brief Inserta un nuevo registro de metadatos de video en el almacenamiento.
     * @param video Estructura `VideoMetadata` a persistir.
     * @return `true` si la inserción fue exitosa, `false` en caso de error o ID duplicado.
     */
    virtual bool create(const VideoMetadata& video) = 0;

    /**
     * @brief Busca un video por su identificador único.
     * @param videoId Identificador único del video.
     * @return `std::optional<VideoMetadata>` con la entidad si existe, o `std::nullopt` si no se encuentra.
     */
    virtual std::optional<VideoMetadata> findById(const std::string& videoId) = 0;

    /**
     * @brief Recupera una página de videos aplicando los criterios de filtrado y ordenamiento.
     * @param filter Parámetros de búsqueda, visibilidad, ordenamiento y paginación.
     * @return Estructura `PaginatedVideos` con los elementos y metadatos de paginación.
     */
    virtual PaginatedVideos findPaginated(const VideoQueryFilter& filter) = 0;

    /**
     * @brief Actualiza los campos mutables de un registro de video existente.
     * @param video Entidad `VideoMetadata` con los valores actualizados.
     * @return `true` si el registro fue actualizado, `false` si no existe o falló la operación.
     */
    virtual bool update(const VideoMetadata& video) = 0;

    /**
     * @brief Actualiza exclusivamente el estado de procesamiento MPEG-DASH de un video.
     * @param videoId Identificador único del video.
     * @param dashStatus Nuevo estado DASH ("pending", "processing", "ready", "error").
     * @return `true` si la actualización tuvo éxito, `false` en caso contrario.
     */
    virtual bool updateDashStatus(const std::string& videoId, const std::string& dashStatus) = 0;

    /**
     * @brief Elimina permanentemente un registro de metadatos por su identificador.
     * @param videoId Identificador único del video a eliminar.
     * @return `true` si el registro existía y fue eliminado, `false` en caso contrario.
     */
    virtual bool remove(const std::string& videoId) = 0;
};

} // namespace tecflix::metadata