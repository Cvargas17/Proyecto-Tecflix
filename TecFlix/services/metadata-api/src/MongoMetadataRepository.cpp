#include "MongoMetadataRepository.hpp"
#include "TelemetryManager.hpp"

#include <bsoncxx/builder/stream/array.hpp>
#include <bsoncxx/builder/stream/document.hpp>
#include <bsoncxx/builder/stream/helpers.hpp>
#include <bsoncxx/json.hpp>
#include <mongocxx/client.hpp>
#include <mongocxx/exception/exception.hpp>
#include <mongocxx/instance.hpp>
#include <mongocxx/options/find.hpp>
#include <mongocxx/pool.hpp>
#include <mongocxx/uri.hpp>

#include <algorithm>
#include <cmath>
#include <iostream>

namespace tecflix::metadata {

namespace {

using bsoncxx::builder::stream::array;
using bsoncxx::builder::stream::close_array;
using bsoncxx::builder::stream::close_document;
using bsoncxx::builder::stream::document;
using bsoncxx::builder::stream::finalize;
using bsoncxx::builder::stream::open_array;
using bsoncxx::builder::stream::open_document;

/**
 * @brief Garantiza que `mongocxx::instance` se inicialice exactamente una vez en todo el proceso.
 */
void initMongoDriverOnce() {
    static mongocxx::instance instance{};
    (void)instance;
}

/**
 * @brief Convierte un documento BSON recuperado de MongoDB a una entidad VideoMetadata.
 */
VideoMetadata fromBson(const bsoncxx::document::view& doc) {
    VideoMetadata v;
    if (doc["_id"]) v.id = std::string(doc["_id"].get_string().value);
    if (doc["owner"]) v.owner = std::string(doc["owner"].get_string().value);
    if (doc["title"]) v.title = std::string(doc["title"].get_string().value);
    if (doc["topic"]) v.topic = std::string(doc["topic"].get_string().value);
    if (doc["visibility"]) v.visibility = std::string(doc["visibility"].get_string().value);
    if (doc["file"]) v.file = std::string(doc["file"].get_string().value);
    if (doc["uploaded"]) v.uploaded = doc["uploaded"].get_int64().value;
    if (doc["dash"]) v.dash = std::string(doc["dash"].get_string().value);

    if (doc["tags"] && doc["tags"].type() == bsoncxx::type::k_array) {
        for (const auto& elem : doc["tags"].get_array().value) {
            if (elem.type() == bsoncxx::type::k_string) {
                v.tags.emplace_back(elem.get_string().value);
            }
        }
    }

    if (doc["meta"] && doc["meta"].type() == bsoncxx::type::k_document) {
        auto m = doc["meta"].get_document().value;
        if (m["filename"]) v.meta.filename = std::string(m["filename"].get_string().value);
        if (m["size"]) v.meta.size = static_cast<uint64_t>(m["size"].get_int64().value);
        if (m["mime"]) v.meta.mime = std::string(m["mime"].get_string().value);
        if (m["duration"]) v.meta.duration = m["duration"].get_double().value;
        if (m["width"]) v.meta.width = m["width"].get_int32().value;
        if (m["height"]) v.meta.height = m["height"].get_int32().value;
        if (m["vcodec"]) v.meta.vcodec = std::string(m["vcodec"].get_string().value);
        if (m["acodec"]) v.meta.acodec = std::string(m["acodec"].get_string().value);
        if (m["brand"]) v.meta.brand = std::string(m["brand"].get_string().value);
        if (m["tracks"]) v.meta.tracks = m["tracks"].get_int32().value;
        if (m["created"]) v.meta.created = m["created"].get_int64().value;
        if (m["bitrate"]) v.meta.bitrate = m["bitrate"].get_int64().value;
    }
    return v;
}

} // namespace

MongoMetadataRepository::MongoMetadataRepository(const std::string& mongoUri,
                                                 const std::string& dbName,
                                                 const std::string& collectionName)
    : dbName_(dbName), collectionName_(collectionName) {
    initMongoDriverOnce();
    mongocxx::uri uri{mongoUri};
    pool_ = std::make_unique<mongocxx::pool>(uri);
    ensureIndexes();
}

MongoMetadataRepository::~MongoMetadataRepository() = default;

void MongoMetadataRepository::ensureIndexes() {
    try {
        auto client = pool_->acquire();
        auto coll = (*client)[dbName_][collectionName_];

        // Índice compuesto para listar videos públicos ordenados por fecha de carga
        coll.create_index(document{} << "visibility" << 1 << "uploaded" << -1 << finalize);
        // Índice compuesto para consultar los videos de un usuario específico
        coll.create_index(document{} << "owner" << 1 << "uploaded" << -1 << finalize);
        // Índices simples para ordenamiento por título y filtrado por tema/etiquetas
        coll.create_index(document{} << "title" << 1 << finalize);
        coll.create_index(document{} << "topic" << 1 << finalize);
        coll.create_index(document{} << "tags" << 1 << finalize);
    } catch (const mongocxx::exception& e) {
        std::cerr << "[MongoMetadataRepository] Warning creating indexes: " << e.what() << "\n";
    }
}

bool MongoMetadataRepository::create(const VideoMetadata& v) {
    observability::ScopedSpan span("MongoDB.createVideo", "", {{"video.id", v.id}});
    try {
        auto client = pool_->acquire();
        auto coll = (*client)[dbName_][collectionName_];

        array tagsArr;
        for (const auto& t : v.tags) {
            tagsArr << t;
        }

        auto doc = document{}
                   << "_id" << v.id
                   << "owner" << v.owner
                   << "title" << v.title
                   << "topic" << v.topic
                   << "tags" << tagsArr
                   << "visibility" << v.visibility
                   << "file" << v.file
                   << "uploaded" << v.uploaded
                   << "dash" << v.dash
                   << "meta" << open_document
                       << "filename" << v.meta.filename
                       << "size" << static_cast<int64_t>(v.meta.size)
                       << "mime" << v.meta.mime
                       << "duration" << v.meta.duration
                       << "width" << v.meta.width
                       << "height" << v.meta.height
                       << "vcodec" << v.meta.vcodec
                       << "acodec" << v.meta.acodec
                       << "brand" << v.meta.brand
                       << "tracks" << v.meta.tracks
                       << "created" << v.meta.created
                       << "bitrate" << v.meta.bitrate
                   << close_document
                   << finalize;

        auto res = coll.insert_one(doc.view());
        bool ok = res.has_value();
        span.setStatusCode(ok ? 201 : 500);
        return ok;
    } catch (const mongocxx::exception& e) {
        span.setStatusCode(500);
        std::cerr << "[MongoMetadataRepository::create] Error: " << e.what() << "\n";
        return false;
    }
}

std::optional<VideoMetadata> MongoMetadataRepository::findById(const std::string& videoId) {
    observability::ScopedSpan span("MongoDB.findVideoById", "", {{"video.id", videoId}});
    try {
        auto client = pool_->acquire();
        auto coll = (*client)[dbName_][collectionName_];

        auto doc = coll.find_one(document{} << "_id" << videoId << finalize);
        if (!doc) {
            span.setStatusCode(404);
            return std::nullopt;
        }
        span.setStatusCode(200);
        return fromBson(doc->view());
    } catch (const mongocxx::exception& e) {
        span.setStatusCode(500);
        std::cerr << "[MongoMetadataRepository::findById] Error: " << e.what() << "\n";
        return std::nullopt;
    }
}

PaginatedVideos MongoMetadataRepository::findPaginated(const VideoQueryFilter& filter) {
    observability::ScopedSpan span("MongoDB.findPaginated");
    PaginatedVideos result;
    result.page = std::max(1, filter.page);
    result.limit = std::clamp(filter.limit, 1, 50);

    try {
        auto client = pool_->acquire();
        auto coll = (*client)[dbName_][collectionName_];

        // 1. Construcción del filtro de visibilidad y propiedad
        document queryBuilder;
        if (filter.scope == "mine") {
            queryBuilder << "owner" << filter.requesterUsername;
        } else {
            // scope == "all": videos públicos OR videos propios privados
            if (filter.requesterUsername.empty()) {
                queryBuilder << "visibility" << "public";
            } else {
                queryBuilder << "$or" << open_array
                             << open_document << "visibility" << "public" << close_document
                             << open_document << "owner" << filter.requesterUsername << close_document
                             << close_array;
            }
        }

        if (!filter.topicFilter.empty()) {
            queryBuilder << "topic" << filter.topicFilter;
        }
        if (!filter.tagFilter.empty()) {
            queryBuilder << "tags" << filter.tagFilter;
        }

        auto queryView = queryBuilder << finalize;

        // 2. Conteo total para metadatos de paginación
        result.totalItems = coll.count_documents(queryView.view());
        result.totalPages = static_cast<int>(
            std::ceil(static_cast<double>(result.totalItems) / static_cast<double>(result.limit)));

        // 3. Validación estricta del campo de ordenamiento (prevención de inyección)
        std::string sortField = "uploaded";
        if (filter.sortBy == "title" || filter.sortBy == "owner" || filter.sortBy == "uploaded") {
            sortField = filter.sortBy;
        }
        int sortDir = (filter.order == "asc") ? 1 : -1;

        mongocxx::options::find opts;
        opts.sort(document{} << sortField << sortDir << finalize);
        opts.skip(static_cast<int64_t>(result.page - 1) * result.limit);
        opts.limit(result.limit);

        auto cursor = coll.find(queryView.view(), opts);
        for (const auto& doc : cursor) {
            result.items.push_back(fromBson(doc));
        }

        span.setStatusCode(200);
    } catch (const mongocxx::exception& e) {
        span.setStatusCode(500);
        std::cerr << "[MongoMetadataRepository::findPaginated] Error: " << e.what() << "\n";
    }
    return result;
}

bool MongoMetadataRepository::update(const VideoMetadata& video) {
    observability::ScopedSpan span("MongoDB.updateVideo", "", {{"video.id", video.id}});
    try {
        auto client = pool_->acquire();
        auto coll = (*client)[dbName_][collectionName_];

        array tagsArr;
        for (const auto& t : video.tags) {
            tagsArr << t;
        }

        auto updateDoc = document{}
                         << "$set" << open_document
                             << "title" << video.title
                             << "visibility" << video.visibility
                             << "topic" << video.topic
                             << "tags" << tagsArr
                         << close_document
                         << finalize;

        auto res = coll.update_one(document{} << "_id" << video.id << finalize, updateDoc.view());
        bool ok = res && res->matched_count() > 0;
        span.setStatusCode(ok ? 200 : 404);
        return ok;
    } catch (const mongocxx::exception& e) {
        span.setStatusCode(500);
        std::cerr << "[MongoMetadataRepository::update] Error: " << e.what() << "\n";
        return false;
    }
}

bool MongoMetadataRepository::updateDashStatus(const std::string& videoId, const std::string& dashStatus) {
    observability::ScopedSpan span("MongoDB.updateDashStatus", "", {{"video.id", videoId}});
    try {
        auto client = pool_->acquire();
        auto coll = (*client)[dbName_][collectionName_];

        auto updateDoc = document{}
                         << "$set" << open_document << "dash" << dashStatus << close_document
                         << finalize;

        auto res = coll.update_one(document{} << "_id" << videoId << finalize, updateDoc.view());
        bool ok = res && res->matched_count() > 0;
        span.setStatusCode(ok ? 200 : 404);
        return ok;
    } catch (const mongocxx::exception& e) {
        span.setStatusCode(500);
        return false;
    }
}

bool MongoMetadataRepository::remove(const std::string& videoId) {
    observability::ScopedSpan span("MongoDB.removeVideo", "", {{"video.id", videoId}});
    try {
        auto client = pool_->acquire();
        auto coll = (*client)[dbName_][collectionName_];

        auto res = coll.delete_one(document{} << "_id" << videoId << finalize);
        bool ok = res && res->deleted_count() > 0;
        span.setStatusCode(ok ? 200 : 404);
        return ok;
    } catch (const mongocxx::exception& e) {
        span.setStatusCode(500);
        return false;
    }
}

} // namespace tecflix::metadata