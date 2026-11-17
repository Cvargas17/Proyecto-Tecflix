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
    std::string owner;                    ///< Nombre de usuario (autor) que subió el video.
    std::string title;                    ///< Título descriptivo del video.
    std::string topic;                    ///< Categoría o tema principal del contenido.
    std::vector<std::string> tags;        ///< Lista de etiquetas descriptivas asociadas.
    std::string visibility{"public"};     ///< Nivel de visibilidad ("public" o "private").
    std::string file;                     ///< Ruta o nombre del archivo original almacenado.
    std::string dash{"pending"};          ///< Estado de empaquetado MPEG-DASH ("pending", "processing", "ready", "error").
    double duration{0.0};                 ///< Duración total del video en segundos.
    int64_t sizeBytes{0};                 ///< Tamaño del archivo original en bytes (para control de cuota).
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
    std::string sortBy{"uploaded"};       ///< Campo de ordenamiento ("uploaded", "title", "owner" o "author").
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

    virtual bool create(const VideoMetadata& video) = 0;
    virtual std::optional<VideoMetadata> findById(const std::string& videoId) = 0;
    virtual PaginatedVideos findPaginated(const VideoQueryFilter& filter) = 0;
    virtual bool update(const VideoMetadata& video) = 0;
    virtual bool updateDashStatus(const std::string& videoId, const std::string& dashStatus) = 0;
    virtual bool remove(const std::string& videoId) = 0;
};

} // namespace tecflix::metadata