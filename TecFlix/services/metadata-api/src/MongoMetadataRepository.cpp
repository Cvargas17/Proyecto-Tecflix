/**
 * @file MongoMetadataRepository.cpp
 * @brief Implementación en MongoDB del contrato `IMetadataRepository` usando `mongocxx`.
 */

#include "MongoMetadataRepository.hpp"
#include "TelemetryManager.hpp"

#include <algorithm>
#include <cmath>

#include <bsoncxx/builder/basic/array.hpp>
#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>
#include <bsoncxx/types.hpp>
#include <mongocxx/client.hpp>
#include <mongocxx/collection.hpp>
#include <mongocxx/database.hpp>
#include <mongocxx/exception/exception.hpp>
#include <mongocxx/options/find.hpp>

using bsoncxx::builder::basic::kvp;
using bsoncxx::builder::basic::make_array;
using bsoncxx::builder::basic::make_document;

namespace tecflix::metadata {

MongoMetadataRepository::MongoMetadataRepository(const std::string& mongoUri,
                                                 const std::string& dbName,
                                                 const std::string& collectionName)
    : uri_(mongocxx::uri{mongoUri}),
      pool_(std::make_unique<mongocxx::pool>(uri_)),
      dbName_(dbName),
      collectionName_(collectionName) {
    ensureIndexes();
}

void MongoMetadataRepository::ensureIndexes() {
    try {
        auto client = pool_->acquire();
        auto collection = (*client)[dbName_][collectionName_];
        collection.create_index(make_document(kvp("owner", 1), kvp("uploaded", -1)));
        collection.create_index(make_document(kvp("visibility", 1), kvp("topic", 1), kvp("uploaded", -1)));
        collection.create_index(make_document(kvp("tags", 1)));
    } catch (const mongocxx::exception&) {
        // La creación de índices se reintentará al estabilizarse la conexión
    }
}

VideoMetadata MongoMetadataRepository::documentToVideo(const bsoncxx::document::view& doc) {
    VideoMetadata video;
    if (doc["_id"] && doc["_id"].type() == bsoncxx::type::k_string) {
        video.id = std::string(doc["_id"].get_string().value);
    }
    if (doc["owner"] && doc["owner"].type() == bsoncxx::type::k_string) {
        video.owner = std::string(doc["owner"].get_string().value);
    }
    if (doc["title"] && doc["title"].type() == bsoncxx::type::k_string) {
        video.title = std::string(doc["title"].get_string().value);
    }
    if (doc["topic"] && doc["topic"].type() == bsoncxx::type::k_string) {
        video.topic = std::string(doc["topic"].get_string().value);
    }
    if (doc["tags"] && doc["tags"].type() == bsoncxx::type::k_array) {
        for (const auto& elem : doc["tags"].get_array().value) {
            if (elem.type() == bsoncxx::type::k_string) {
                video.tags.emplace_back(elem.get_string().value);
            }
        }
    }
    if (doc["visibility"] && doc["visibility"].type() == bsoncxx::type::k_string) {
        video.visibility = std::string(doc["visibility"].get_string().value);
    }
    if (doc["file"] && doc["file"].type() == bsoncxx::type::k_string) {
        video.file = std::string(doc["file"].get_string().value);
    }
    if (doc["dash"] && doc["dash"].type() == bsoncxx::type::k_string) {
        video.dash = std::string(doc["dash"].get_string().value);
    }
    if (doc["duration"]) {
        if (doc["duration"].type() == bsoncxx::type::k_double) {
            video.duration = doc["duration"].get_double().value;
        } else if (doc["duration"].type() == bsoncxx::type::k_int32) {
            video.duration = static_cast<double>(doc["duration"].get_int32().value);
        } else if (doc["duration"].type() == bsoncxx::type::k_int64) {
            video.duration = static_cast<double>(doc["duration"].get_int64().value);
        }
    }
    if (doc["sizeBytes"] && doc["sizeBytes"].type() == bsoncxx::type::k_int64) {
        video.sizeBytes = doc["sizeBytes"].get_int64().value;
    } else if (doc["sizeBytes"] && doc["sizeBytes"].type() == bsoncxx::type::k_int32) {
        video.sizeBytes = doc["sizeBytes"].get_int32().value;
    }
    if (doc["uploaded"] && doc["uploaded"].type() == bsoncxx::type::k_int64) {
        video.uploaded = doc["uploaded"].get_int64().value;
    } else if (doc["uploaded"] && doc["uploaded"].type() == bsoncxx::type::k_int32) {
        video.uploaded = doc["uploaded"].get_int32().value;
    }
    return video;
}

bool MongoMetadataRepository::create(const VideoMetadata& video) {
    observability::ScopedSpan span("MongoDB.insertOne", "", {{"db.system", "mongodb"}, {"db.operation", "insert"}});
    try {
        auto client = pool_->acquire();
        auto collection = (*client)[dbName_][collectionName_];

        bsoncxx::builder::basic::array tagsArray;
        for (const auto& tag : video.tags) {
            tagsArray.append(tag);
        }

        auto doc = make_document(
            kvp("_id", video.id),
            kvp("owner", video.owner),
            kvp("title", video.title),
            kvp("topic", video.topic),
            kvp("tags", tagsArray.extract()),
            kvp("visibility", video.visibility),
            kvp("file", video.file),
            kvp("dash", video.dash),
            kvp("duration", video.duration),
            kvp("sizeBytes", video.sizeBytes),
            kvp("uploaded", video.uploaded)
        );

        auto res = collection.insert_one(doc.view());
        bool ok = res && res->result().inserted_count() == 1;
        span.setStatusCode(ok ? 200 : 500);
        return ok;
    } catch (const mongocxx::exception&) {
        span.setStatusCode(500);
        return false;
    }
}

std::optional<VideoMetadata> MongoMetadataRepository::findById(const std::string& videoId) {
    observability::ScopedSpan span("MongoDB.findOne", "", {{"db.system", "mongodb"}, {"db.operation", "find"}});
    try {
        auto client = pool_->acquire();
        auto collection = (*client)[dbName_][collectionName_];

        auto maybeDoc = collection.find_one(make_document(kvp("_id", videoId)));
        if (!maybeDoc) {
            span.setStatusCode(404);
            return std::nullopt;
        }
        span.setStatusCode(200);
        return documentToVideo(maybeDoc->view());
    } catch (const mongocxx::exception&) {
        span.setStatusCode(500);
        return std::nullopt;
    }
}

PaginatedVideos MongoMetadataRepository::findPaginated(const VideoQueryFilter& filter) {
    observability::ScopedSpan span("MongoDB.findPaginated", "", {{"db.system", "mongodb"}, {"db.operation", "find"}});
    PaginatedVideos result;
    result.page = std::max(1, filter.page);
    result.limit = std::clamp(filter.limit, 1, 50);

    try {
        auto client = pool_->acquire();
        auto collection = (*client)[dbName_][collectionName_];

        bsoncxx::builder::basic::document queryBuilder;
        if (filter.scope == "mine") {
            queryBuilder.append(kvp("owner", filter.requesterUsername));
        } else if (!filter.requesterUsername.empty()) {
            queryBuilder.append(kvp("$or", make_array(
                make_document(kvp("visibility", "public")),
                make_document(kvp("owner", filter.requesterUsername))
            )));
        } else {
            queryBuilder.append(kvp("visibility", "public"));
        }

        if (!filter.topicFilter.empty()) {
            queryBuilder.append(kvp("topic", filter.topicFilter));
        }
        if (!filter.tagFilter.empty()) {
            queryBuilder.append(kvp("tags", filter.tagFilter));
        }

        auto queryView = queryBuilder.view();
        int64_t totalCount = collection.count_documents(queryView);
        result.totalItems = static_cast<int>(totalCount);
        result.totalPages = std::max(1, static_cast<int>(std::ceil(static_cast<double>(totalCount) / result.limit)));
        if (result.page > result.totalPages) {
            result.page = result.totalPages;
        }

        std::string sortField = "uploaded";
        if (filter.sortBy == "title") {
            sortField = "title";
        } else if (filter.sortBy == "owner" || filter.sortBy == "author") {
            sortField = "owner";
        }
        int sortDir = (filter.order == "asc") ? 1 : -1;

        mongocxx::options::find findOpts;
        findOpts.sort(make_document(kvp(sortField, sortDir)));
        findOpts.skip(static_cast<int64_t>((result.page - 1) * result.limit));
        findOpts.limit(result.limit);

        auto cursor = collection.find(queryView, findOpts);
        for (const auto& doc : cursor) {
            result.items.push_back(documentToVideo(doc));
        }

        span.setStatusCode(200);
        return result;
    } catch (const mongocxx::exception&) {
        span.setStatusCode(500);
        return result;
    }
}

bool MongoMetadataRepository::update(const VideoMetadata& video) {
    observability::ScopedSpan span("MongoDB.updateOne", "", {{"db.system", "mongodb"}, {"db.operation", "update"}});
    try {
        auto client = pool_->acquire();
        auto collection = (*client)[dbName_][collectionName_];

        bsoncxx::builder::basic::array tagsArray;
        for (const auto& tag : video.tags) {
            tagsArray.append(tag);
        }

        auto updateDoc = make_document(kvp("$set", make_document(
            kvp("title", video.title),
            kvp("topic", video.topic),
            kvp("tags", tagsArray.extract()),
            kvp("visibility", video.visibility),
            kvp("duration", video.duration),
            kvp("sizeBytes", video.sizeBytes)
        )));

        auto res = collection.update_one(make_document(kvp("_id", video.id)), updateDoc.view());
        bool ok = res && res->matched_count() == 1;
        span.setStatusCode(ok ? 200 : 404);
        return ok;
    } catch (const mongocxx::exception&) {
        span.setStatusCode(500);
        return false;
    }
}

bool MongoMetadataRepository::updateDashStatus(const std::string& videoId, const std::string& dashStatus) {
    observability::ScopedSpan span("MongoDB.updateDash", "", {{"db.system", "mongodb"}, {"db.operation", "update"}});
    try {
        auto client = pool_->acquire();
        auto collection = (*client)[dbName_][collectionName_];

        auto updateDoc = make_document(kvp("$set", make_document(kvp("dash", dashStatus))));
        auto res = collection.update_one(make_document(kvp("_id", videoId)), updateDoc.view());
        bool ok = res && res->matched_count() == 1;
        span.setStatusCode(ok ? 200 : 404);
        return ok;
    } catch (const mongocxx::exception&) {
        span.setStatusCode(500);
        return false;
    }
}

bool MongoMetadataRepository::remove(const std::string& videoId) {
    observability::ScopedSpan span("MongoDB.deleteOne", "", {{"db.system", "mongodb"}, {"db.operation", "delete"}});
    try {
        auto client = pool_->acquire();
        auto collection = (*client)[dbName_][collectionName_];

        auto res = collection.delete_one(make_document(kvp("_id", videoId)));
        bool ok = res && res->deleted_count() == 1;
        span.setStatusCode(ok ? 200 : 404);
        return ok;
    } catch (const mongocxx::exception&) {
        span.setStatusCode(500);
        return false;
    }
}

} // namespace tecflix::metadata