/**
 * @file MetadataController.cpp
 * @brief Implementación del enrutamiento HTTP REST y extracción JSON de `MetadataController`.
 */

#include "MetadataController.hpp"
#include "TelemetryManager.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>

namespace tecflix::metadata {

MetadataController::MetadataController(std::shared_ptr<MetadataService> service)
    : service_(std::move(service)) {}

std::string MetadataController::getHeaderIgnoreCase(const std::map<std::string, std::string>& headers,
                                                    const std::string& key) {
    std::string target = key;
    std::transform(target.begin(), target.end(), target.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    for (const auto& [k, v] : headers) {
        std::string lowerK = k;
        std::transform(lowerK.begin(), lowerK.end(), lowerK.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (lowerK == target) {
            return v;
        }
    }
    return "";
}

bool MetadataController::hasJsonKey(const std::string& json, const std::string& key) {
    return json.find("\"" + key + "\"") != std::string::npos;
}

std::string MetadataController::extractJsonString(const std::string& json, const std::string& key) {
    std::string needle = "\"" + key + "\"";
    size_t pos = json.find(needle);
    if (pos == std::string::npos) return "";

    size_t colon = json.find(':', pos + needle.size());
    if (colon == std::string::npos) return "";

    size_t firstQuote = json.find('"', colon + 1);
    if (firstQuote == std::string::npos) return "";

    std::string result;
    for (size_t i = firstQuote + 1; i < json.size(); ++i) {
        if (json[i] == '\\' && i + 1 < json.size()) {
            char next = json[++i];
            if (next == 'n') result += '\n';
            else if (next == 't') result += '\t';
            else result += next;
        } else if (json[i] == '"') {
            return result;
        } else {
            result += json[i];
        }
    }
    return "";
}

std::vector<std::string> MetadataController::extractJsonStringArray(const std::string& json, const std::string& key) {
    std::vector<std::string> items;
    std::string needle = "\"" + key + "\"";
    size_t pos = json.find(needle);
    if (pos == std::string::npos) return items;

    size_t openBracket = json.find('[', pos + needle.size());
    if (openBracket == std::string::npos) return items;

    size_t closeBracket = json.find(']', openBracket + 1);
    if (closeBracket == std::string::npos) return items;

    std::string arrContent = json.substr(openBracket + 1, closeBracket - openBracket - 1);
    size_t idx = 0;
    while (idx < arrContent.size()) {
        size_t q1 = arrContent.find('"', idx);
        if (q1 == std::string::npos) break;
        size_t q2 = arrContent.find('"', q1 + 1);
        if (q2 == std::string::npos) break;
        items.push_back(arrContent.substr(q1 + 1, q2 - q1 - 1));
        idx = q2 + 1;
    }
    return items;
}

ServiceResponse MetadataController::handleRequest(const HttpRequest& req) {
    // Propagación de traza distribuida recibida desde el API Gateway
    std::string incomingTraceId = getHeaderIgnoreCase(req.headers, "X-Trace-Id");
    observability::ScopedSpan span(req.method + " " + req.path, incomingTraceId,
                                   {{"http.method", req.method}, {"http.route", req.path}});

    std::string requester = getHeaderIgnoreCase(req.headers, "X-User");
    ServiceResponse response{404, "{\"error\":\"Endpoint not found\"}"};

    // 1. Verificación de salud del microservicio
    if (req.method == "GET" && req.path == "/health") {
        response = {200, "{\"status\":\"healthy\",\"service\":\"tecflix-metadata-api\"}"};
    }
    // 2. Consulta paginada y filtrada del catálogo
    else if (req.method == "GET" && req.path == "/api/videos") {
        VideoQueryFilter filter;
        filter.requesterUsername = requester;
        if (req.queryParams.count("scope")) filter.scope = req.queryParams.at("scope");
        if (req.queryParams.count("sortBy")) filter.sortBy = req.queryParams.at("sortBy");
        if (req.queryParams.count("order")) filter.order = req.queryParams.at("order");
        if (req.queryParams.count("topic")) filter.topicFilter = req.queryParams.at("topic");
        if (req.queryParams.count("tag")) filter.tagFilter = req.queryParams.at("tag");
        if (req.queryParams.count("page")) {
            try { filter.page = std::stoi(req.queryParams.at("page")); } catch (...) {}
        }
        if (req.queryParams.count("limit")) {
            try { filter.limit = std::stoi(req.queryParams.at("limit")); } catch (...) {}
        }
        response = service_->listVideos(filter);
    }
    // 3. Registro de metadatos de un video nuevo
    else if (req.method == "POST" && req.path == "/api/videos") {
        VideoMetadata video;
        video.id = extractJsonString(req.body, "id");
        video.owner = requester.empty() ? extractJsonString(req.body, "owner") : requester;
        video.title = extractJsonString(req.body, "title");
        video.topic = extractJsonString(req.body, "topic");
        video.tags = extractJsonStringArray(req.body, "tags");
        video.visibility = extractJsonString(req.body, "visibility");
        video.file = extractJsonString(req.body, "file");
        video.dash = extractJsonString(req.body, "dash");
        video.uploaded = static_cast<int64_t>(
            std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
        response = service_->registerVideo(std::move(video));
    }
    // 4. Rutas parametrizadas por identificador: /api/videos/{id} y /api/videos/{id}/dash
    else if (req.path.rfind("/api/videos/", 0) == 0) {
        std::string sub = req.path.substr(12); // Extrae el segmento posterior a "/api/videos/"
        size_t slashPos = sub.find('/');

        if (slashPos == std::string::npos) {
            std::string videoId = sub;
            if (req.method == "GET") {
                response = service_->getVideoById(videoId, requester);
            } else if (req.method == "PUT" || req.method == "PATCH") {
                std::string newTitle = extractJsonString(req.body, "title");
                std::string newVisibility = extractJsonString(req.body, "visibility");
                std::string newTopic = extractJsonString(req.body, "topic");
                bool updateTopic = hasJsonKey(req.body, "topic");
                std::vector<std::string> newTags = extractJsonStringArray(req.body, "tags");
                bool updateTags = hasJsonKey(req.body, "tags");

                response = service_->updateVideoMetadata(
                    videoId, requester, newTitle, newVisibility, newTopic, updateTopic, newTags, updateTags);
            } else if (req.method == "DELETE") {
                response = service_->deleteVideo(videoId, requester);
            }
        } else {
            std::string videoId = sub.substr(0, slashPos);
            std::string action = sub.substr(slashPos + 1);
            if (req.method == "PATCH" && action == "dash") {
                std::string status = extractJsonString(req.body, "dash");
                response = service_->updateDashState(videoId, status);
            }
        }
    }

    span.setStatusCode(response.statusCode);
    return response;
}

} // namespace tecflix::metadata