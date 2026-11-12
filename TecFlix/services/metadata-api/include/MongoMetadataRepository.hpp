#pragma once

#include "IMetadataRepository.hpp"
#include <memory>
#include <string>

// Declaraciones adelantadas del driver oficial mongocxx
namespace mongocxx {
inline namespace v_noabi {
class instance;
class pool;
} // namespace v_noabi
} // namespace mongocxx

namespace tecflix::metadata {

/**
 * @class MongoMetadataRepository
 * @brief Implementación concreta de IMetadataRepository respaldada por MongoDB.
 *
 * Decisiones de diseño (para defensa arquitectónica):
 * 1. Utiliza `mongocxx::pool` para manejar conexiones concurrentes desde el Thread Pool HTTP
 *    de manera segura (thread-safe) y evitar el overhead de abrir sockets TCP por cada consulta.
 * 2. Crea índices compuestos al inicializar (`ensureIndexes`) sobre `visibility`, `owner`, `uploaded`
 *    y `title` para evitar Collection Scans (COLLSCAN) en consultas paginadas.
 * 3. Instrumenta cada consulta a MongoDB con `ScopedSpan` de OpenTelemetry para medir latencia en el Dashboard.
 */
class MongoMetadataRepository : public IMetadataRepository {
public:
    /**
     * @brief Inicializa el pool de conexiones a MongoDB y garantiza los índices de la colección.
     * @param mongoUri URI de conexión (ej. "mongodb://localhost:27017").
     * @param dbName Nombre de la base de datos (por defecto "tecflix").
     * @param collectionName Nombre de la colección (por defecto "videos").
     */
    explicit MongoMetadataRepository(const std::string& mongoUri = "mongodb://127.0.0.1:27017",
                                     const std::string& dbName = "tecflix",
                                     const std::string& collectionName = "videos");

    ~MongoMetadataRepository() override;

    bool create(const VideoMetadata& video) override;
    std::optional<VideoMetadata> findById(const std::string& videoId) override;
    PaginatedVideos findPaginated(const VideoQueryFilter& filter) override;
    bool update(const VideoMetadata& video) override;
    bool updateDashStatus(const std::string& videoId, const std::string& dashStatus) override;
    bool remove(const std::string& videoId) override;

private:
    /**
     * @brief Crea índices compuestos en MongoDB si aún no existen.
     */
    void ensureIndexes();

    std::string dbName_;
    std::string collectionName_;
    std::unique_ptr<mongocxx::pool> pool_;
};

} // namespace tecflix::metadata