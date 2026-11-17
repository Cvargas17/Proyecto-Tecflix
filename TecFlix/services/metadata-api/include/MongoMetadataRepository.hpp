/**
* @file MongoMetadataRepository.hpp
 * @brief Declaración del repositorio persistente en MongoDB para el microservicio de Metadata.
 */

#pragma once

#include "IMetadataRepository.hpp"

#include <memory>
#include <optional>
#include <string>

#include <bsoncxx/document/view.hpp>
#include <mongocxx/pool.hpp>
#include <mongocxx/uri.hpp>

namespace tecflix::metadata {

    /**
     * @class MongoMetadataRepository
     * @brief Implementación en MongoDB de `IMetadataRepository` utilizando pool de conexiones seguro ante hilos.
     */
    class MongoMetadataRepository : public IMetadataRepository {
    public:
        MongoMetadataRepository(const std::string& mongoUri,
                                const std::string& dbName = "tecflix",
                                const std::string& collectionName = "videos");
        ~MongoMetadataRepository() override = default;

        bool create(const VideoMetadata& video) override;
        std::optional<VideoMetadata> findById(const std::string& videoId) override;
        PaginatedVideos findPaginated(const VideoQueryFilter& filter) override;
        bool update(const VideoMetadata& video) override;
        bool updateDashStatus(const std::string& videoId, const std::string& dashStatus) override;
        bool remove(const std::string& videoId) override;

    private:
        void ensureIndexes();
        static VideoMetadata documentToVideo(const bsoncxx::document::view& doc);

        mongocxx::uri uri_;
        std::unique_ptr<mongocxx::pool> pool_;
        std::string dbName_;
        std::string collectionName_;
    };

} // namespace tecflix::metadata