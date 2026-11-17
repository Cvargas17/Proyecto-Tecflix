/**
 * @file MetadataService.cpp
 * @brief Implementación de las reglas de negocio, invariantes de dominio y control de acceso.
 */

#include "MetadataService.hpp"
#include "TelemetryManager.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <sstream>

namespace tecflix::metadata {

namespace {

bool isValidHexVideoId(const std::string& id) {
    if (id.empty() || id.size() > 32) return false;
    return std::all_of(id.begin(), id.end(), [](unsigned char c) { return std::isxdigit(c) != 0; });
}

} // namespace

MetadataService::MetadataService(std::shared_ptr<IMetadataRepository> repository)
    : repo_(std::move(repository)) {}

bool MetadataService::isValidVisibility(const std::string& visibility) {
    return visibility == "public" || visibility == "private";
}

bool MetadataService::isValidDashStatus(const std::string& status) {
    return status == "pending" || status == "processing" || status == "ready" || status == "error";
}

std::string MetadataService::escapeJson(const std::string& input) {
    std::string out;
    out.reserve(input.size() + 4);
    for (unsigned char c : input) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:   out += static_cast<char>(c); break;
        }
    }
    return out;
}

ServiceResponse MetadataService::registerVideo(VideoMetadata video) {
    observability::ScopedSpan span("MetadataService.registerVideo");

    if (video.owner.empty()) {
        span.setStatusCode(401);
        return {401, "{\"error\":\"Authentication required: owner is missing\"}"};
    }

    if (!isValidHexVideoId(video.id)) {
        span.setStatusCode(400);
        return {400, "{\"error\":\"Invalid video id: expected 1..32 hexadecimal characters\"}"};
    }

    if (video.title.empty() || video.title.size() > 120) {
        span.setStatusCode(400);
        return {400, "{\"error\":\"Invalid title: must be between 1 and 120 characters\"}"};
    }

    if (std::isnan(video.duration) || video.duration < 0.0) {
        span.setStatusCode(400);
        return {400, "{\"error\":\"Invalid duration: must be >= 0\"}"};
    }

    if (video.sizeBytes < 0) {
        span.setStatusCode(400);
        return {400, "{\"error\":\"Invalid sizeBytes: must be >= 0\"}"};
    }

    if (video.visibility.empty()) {
        video.visibility = "public";
    } else if (!isValidVisibility(video.visibility)) {
        span.setStatusCode(400);
        return {400, "{\"error\":\"Invalid visibility. Expected 'public' or 'private'\"}"};
    }

    if (video.dash.empty()) {
        video.dash = "pending";
    } else if (!isValidDashStatus(video.dash)) {
        span.setStatusCode(400);
        return {400, "{\"error\":\"Invalid dash status\"}"};
    }

    if (repo_->findById(video.id).has_value()) {
        span.setStatusCode(409);
        return {409, "{\"error\":\"Video metadata with this ID already exists\"}"};
    }

    if (!repo_->create(video)) {
        span.setStatusCode(500);
        return {500, "{\"error\":\"Failed to persist video metadata\"}"};
    }

    span.setStatusCode(201);
    return {201, video.toJson(video.owner)};
}

ServiceResponse MetadataService::getVideoById(const std::string& videoId, const std::string& requesterUsername) {
    observability::ScopedSpan span("MetadataService.getVideoById", "", {{"video.id", videoId}});

    if (!isValidHexVideoId(videoId)) {
        span.setStatusCode(400);
        return {400, "{\"error\":\"Invalid video ID format\"}"};
    }

    auto found = repo_->findById(videoId);
    if (!found.has_value()) {
        span.setStatusCode(404);
        return {404, "{\"error\":\"Video not found\"}"};
    }

    if (found->visibility == "private" && found->owner != requesterUsername) {
        span.setStatusCode(403);
        return {403, "{\"error\":\"Access denied to private video\"}"};
    }

    span.setStatusCode(200);
    return {200, found->toJson(requesterUsername)};
}

ServiceResponse MetadataService::listVideos(VideoQueryFilter filter) {
    observability::ScopedSpan span("MetadataService.listVideos");

    if (filter.scope != "mine") {
        filter.scope = "all";
    }

    if (filter.scope == "mine" && filter.requesterUsername.empty()) {
        span.setStatusCode(401);
        return {401, "{\"error\":\"Authentication required for scope=mine\"}"};
    }

    if (filter.page < 1 || filter.limit < 1 || filter.limit > 50) {
        span.setStatusCode(400);
        return {400, "{\"error\":\"Invalid pagination bounds: page >= 1 and 1 <= limit <= 50\"}"};
    }

    PaginatedVideos pageResult = repo_->findPaginated(filter);

    std::ostringstream oss;
    oss << "{\"items\":[";
    for (size_t i = 0; i < pageResult.items.size(); ++i) {
        if (i > 0) oss << ",";
        oss << pageResult.items[i].toJson(filter.requesterUsername);
    }
    oss << "],\"page\":" << pageResult.page
        << ",\"limit\":" << pageResult.limit
        << ",\"totalPages\":" << pageResult.totalPages
        << ",\"totalItems\":" << pageResult.totalItems << "}";

    span.setStatusCode(200);
    return {200, oss.str()};
}

ServiceResponse MetadataService::updateVideoMetadata(const std::string& videoId,
                                                     const std::string& requesterUsername,
                                                     const std::string& newTitle,
                                                     const std::string& newVisibility,
                                                     const std::string& newTopic,
                                                     bool updateTopic,
                                                     const std::vector<std::string>& newTags,
                                                     bool updateTags) {
    observability::ScopedSpan span("MetadataService.updateVideoMetadata", "", {{"video.id", videoId}});

    if (requesterUsername.empty()) {
        span.setStatusCode(401);
        return {401, "{\"error\":\"Authentication required\"}"};
    }

    if (!isValidHexVideoId(videoId)) {
        span.setStatusCode(400);
        return {400, "{\"error\":\"Invalid video ID format\"}"};
    }

    auto found = repo_->findById(videoId);
    if (!found.has_value()) {
        span.setStatusCode(404);
        return {404, "{\"error\":\"Video not found\"}"};
    }

    if (found->owner != requesterUsername) {
        span.setStatusCode(403);
        return {403, "{\"error\":\"Only the owner can edit this video\"}"};
    }

    VideoMetadata updated = *found;

    if (!newTitle.empty()) {
        if (newTitle.size() > 120) {
            span.setStatusCode(400);
            return {400, "{\"error\":\"Title cannot exceed 120 characters\"}"};
        }
        updated.title = newTitle;
    }

    if (!newVisibility.empty()) {
        if (!isValidVisibility(newVisibility)) {
            span.setStatusCode(400);
            return {400, "{\"error\":\"Invalid visibility. Expected 'public' or 'private'\"}"};
        }
        updated.visibility = newVisibility;
    }

    if (updateTopic) {
        updated.topic = newTopic;
    }

    if (updateTags) {
        updated.tags = newTags;
    }

    if (!repo_->update(updated)) {
        span.setStatusCode(500);
        return {500, "{\"error\":\"Failed to update video metadata\"}"};
    }

    span.setStatusCode(200);
    return {200, updated.toJson(requesterUsername)};
}

ServiceResponse MetadataService::updateDashState(const std::string& videoId, const std::string& dashStatus) {
    observability::ScopedSpan span("MetadataService.updateDashState", "", {{"video.id", videoId}});

    if (!isValidHexVideoId(videoId) || !isValidDashStatus(dashStatus)) {
        span.setStatusCode(400);
        return {400, "{\"error\":\"Invalid videoId or dash status\"}"};
    }

    if (!repo_->updateDashStatus(videoId, dashStatus)) {
        span.setStatusCode(404);
        return {404, "{\"error\":\"Video not found or update failed\"}"};
    }

    span.setStatusCode(200);
    return {200, "{\"id\":\"" + escapeJson(videoId) + "\",\"dash\":\"" + escapeJson(dashStatus) + "\"}"};
}

ServiceResponse MetadataService::deleteVideo(const std::string& videoId, const std::string& requesterUsername) {
    observability::ScopedSpan span("MetadataService.deleteVideo", "", {{"video.id", videoId}});

    if (requesterUsername.empty()) {
        span.setStatusCode(401);
        return {401, "{\"error\":\"Authentication required\"}"};
    }

    if (!isValidHexVideoId(videoId)) {
        span.setStatusCode(400);
        return {400, "{\"error\":\"Invalid video ID format\"}"};
    }

    auto found = repo_->findById(videoId);
    if (!found.has_value()) {
        span.setStatusCode(404);
        return {404, "{\"error\":\"Video not found\"}"};
    }

    if (found->owner != requesterUsername) {
        span.setStatusCode(403);
        return {403, "{\"error\":\"Only the owner can delete this video\"}"};
    }

    if (!repo_->remove(videoId)) {
        span.setStatusCode(500);
        return {500, "{\"error\":\"Failed to delete video metadata\"}"};
    }

    span.setStatusCode(200);
    return {200, "{\"deleted\":true,\"id\":\"" + escapeJson(videoId) + "\"}"};
}

} // namespace tecflix::metadata