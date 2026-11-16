/**
 * @file MetadataService.cpp
 * @brief Implementación de las reglas de negocio y control de acceso de `MetadataService`.
 */

#include "MetadataService.hpp"
#include "TelemetryManager.hpp"

#include <algorithm>
#include <sstream>

namespace tecflix::metadata {

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

    // Validación de campos mínimos indispensables
    if (video.id.empty() || video.owner.empty() || video.title.empty()) {
        span.setStatusCode(400);
        return {400, "{\"error\":\"Missing required fields: id, owner, or title\"}"};
    }

    // Asignación por defecto y validación de visibilidad
    if (video.visibility.empty()) {
        video.visibility = "public";
    } else if (!isValidVisibility(video.visibility)) {
        span.setStatusCode(400);
        return {400, "{\"error\":\"Invalid visibility. Expected 'public' or 'private'\"}"};
    }

    // Asignación por defecto y validación de estado de empaquetado DASH
    if (video.dash.empty()) {
        video.dash = "pending";
    } else if (!isValidDashStatus(video.dash)) {
        span.setStatusCode(400);
        return {400, "{\"error\":\"Invalid dash status\"}"};
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

    if (videoId.empty()) {
        span.setStatusCode(400);
        return {400, "{\"error\":\"Video ID is required\"}"};
    }

    auto found = repo_->findById(videoId);
    if (!found.has_value()) {
        span.setStatusCode(404);
        return {404, "{\"error\":\"Video not found\"}"};
    }

    // Control de privacidad: solo el propietario puede consultar videos marcados como "private"
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

    // El alcance "mine" exige que el usuario haya sido autenticado previamente
    if (filter.scope == "mine" && filter.requesterUsername.empty()) {
        span.setStatusCode(401);
        return {401, "{\"error\":\"Authentication required for scope=mine\"}"};
    }

    // Normalización de límites de paginación para evitar consultas abusivas
    filter.page = std::max(1, filter.page);
    filter.limit = std::clamp(filter.limit, 1, 50);

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

    auto found = repo_->findById(videoId);
    if (!found.has_value()) {
        span.setStatusCode(404);
        return {404, "{\"error\":\"Video not found\"}"};
    }

    // Autorización estricta: únicamente el propietario puede modificar metadatos
    if (found->owner != requesterUsername) {
        span.setStatusCode(403);
        return {403, "{\"error\":\"Only the owner can edit this video\"}"};
    }

    VideoMetadata updated = *found;

    if (!newTitle.empty()) {
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

    if (videoId.empty() || !isValidDashStatus(dashStatus)) {
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

    auto found = repo_->findById(videoId);
    if (!found.has_value()) {
        span.setStatusCode(404);
        return {404, "{\"error\":\"Video not found\"}"};
    }

    // Autorización estricta: únicamente el propietario puede eliminar el recurso
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