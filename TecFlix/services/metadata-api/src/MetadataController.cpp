/**
 * @file MetadataController.cpp
 * @brief Implementación del enrutamiento HTTP REST, propagación W3C y validación de parámetros.
 */

#include "MetadataController.hpp"
#include "TelemetryManager.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>

namespace tecflix::metadata {

namespace {

bool parseStrictInt(const std::string& raw, int& out) {
    if (raw.empty()) return false;
    size_t idx = 0;
    if (raw[0] == '-' || raw[0] == '+') idx = 1;
    if (idx >= raw.size()) return false;
    for (size_t i = idx; i < raw.size(); ++i) {
        if (!std::isdigit(static_cast<unsigned char>(raw[i]))) return false;
    }
    try {
        out = std::stoi(raw);
        return true;
    } catch (...) {
        return false;
    }
}

bool isValidSortBy(const std::string& s) {
    return s == "uploaded" || s == "title" || s == "author" || s == "owner";
}

bool isValidOrder(const std::string& o) {
    return o == "asc" || o == "desc";
}

} // namespace

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

double MetadataController::extractJsonNumber(const std::string& json, const std::string& key, double defaultVal) {
    std::string needle = "\"" + key + "\"";
    size_t pos = json.find(needle);
    if (pos == std::string::npos) return defaultVal;

    size_t colon = json.find(':', pos + needle.size());
    if (colon == std::string::npos) return defaultVal;

    size_t start = colon + 1;
    while (start < json.size() && std::isspace(static_cast<unsigned char>(json[start]))) {
        ++start;
    }
    if (start >= json.size()) return defaultVal;

    size_t end = start;
    while (end < json.size() &&
           (std::isdigit(static_cast<unsigned char>(json[end])) || json[end] == '.' || json[end] == '-')) {
        ++end;
    }
    if (end == start) return defaultVal;

    try {
        return std::stod(json.substr(start, end - start));
    } catch (...) {
        return defaultVal;
    }
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
    std::string traceparent = getHeaderIgnoreCase(req.headers, "traceparent");
    auto w3cCtx = observability::TelemetryManager::parseTraceparent(traceparent);
    std::string incomingTraceId = w3cCtx.valid ? w3cCtx.traceId : getHeaderIgnoreCase(req.headers, "X-Trace-Id");

    std::string routePattern = req.path;
    if (req.path.rfind("/api/videos/", 0) == 0) {
        routePattern = "/api/videos/{id}";
    }

    observability::ScopedSpan span(req.method + " " + routePattern,
                                   incomingTraceId,
                                   {{"http.method", req.method}, {"http.route", routePattern}},
                                   w3cCtx.parentSpanId);

    std::string requester = getHeaderIgnoreCase(req.headers, "X-User");
    ServiceResponse response{404, "{\"error\":\"Endpoint not found\"}"};

    if (req.method == "GET" && req.path == "/health") {
        response = {200, "{\"status\":\"healthy\",\"service\":\"tecflix-metadata-api\"}"};
    } else if (req.method == "GET" && req.path == "/api/videos") {
        VideoQueryFilter filter;
        filter.requesterUsername = requester;

        if (req.queryParams.count("scope")) {
            const std::string& sc = req.queryParams.at("scope");
            if (sc != "all" && sc != "mine" && sc != "public") {
                span.setStatusCode(400);
                return {400, "{\"error\":\"Invalid scope. Expected 'all' or 'mine'\"}"};
            }
            filter.scope = (sc == "mine") ? "mine" : "all";
        }
        if (req.queryParams.count("sortBy")) {
            const std::string& sb = req.queryParams.at("sortBy");
            if (!isValidSortBy(sb)) {
                span.setStatusCode(400);
                return {400, "{\"error\":\"Invalid sortBy. Expected uploaded, title, author, or owner\"}"};
            }
            filter.sortBy = sb;
        }
        if (req.queryParams.count("order")) {
            const std::string& ord = req.queryParams.at("order");
            if (!isValidOrder(ord)) {
                span.setStatusCode(400);
                return {400, "{\"error\":\"Invalid order. Expected 'asc' or 'desc'\"}"};
            }
            filter.order = ord;
        }
        if (req.queryParams.count("topic")) filter.topicFilter = req.queryParams.at("topic");
        if (req.queryParams.count("tag")) filter.tagFilter = req.queryParams.at("tag");

        if (req.queryParams.count("page")) {
            int parsedPage = 0;
            if (!parseStrictInt(req.queryParams.at("page"), parsedPage) || parsedPage < 1) {
                span.setStatusCode(400);
                return {400, "{\"error\":\"Invalid page parameter: must be an integer >= 1\"}"};
            }
            filter.page = parsedPage;
        }
        if (req.queryParams.count("limit")) {
            int parsedLimit = 0;
            if (!parseStrictInt(req.queryParams.at("limit"), parsedLimit) || parsedLimit < 1 || parsedLimit > 50) {
                span.setStatusCode(400);
                return {400, "{\"error\":\"Invalid limit parameter: must be between 1 and 50\"}"};
            }
            filter.limit = parsedLimit;
        }

        response = service_->listVideos(filter);
    } else if (req.method == "POST" && req.path == "/api/videos") {
        std::string bodyOwner = extractJsonString(req.body, "owner");
        std::string effectiveOwner = requester.empty() ? bodyOwner : requester;
        if (effectiveOwner.empty()) {
            span.setStatusCode(401);
            return {401, "{\"error\":\"Authentication required to create video metadata\"}"};
        }

        VideoMetadata video;
        video.id = extractJsonString(req.body, "id");
        video.owner = effectiveOwner;
        video.title = extractJsonString(req.body, "title");
        video.topic = extractJsonString(req.body, "topic");
        video.tags = extractJsonStringArray(req.body, "tags");
        video.visibility = extractJsonString(req.body, "visibility");
        video.file = extractJsonString(req.body, "file");
        video.dash = extractJsonString(req.body, "dash");
        video.duration = extractJsonNumber(req.body, "duration", 0.0);
        video.sizeBytes = static_cast<int64_t>(extractJsonNumber(req.body, "sizeBytes", 0.0));
        video.uploaded = static_cast<int64_t>(
            std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
        response = service_->registerVideo(std::move(video));
    } else if (req.path.rfind("/api/videos/", 0) == 0) {
        std::string sub = req.path.substr(12);
        size_t slashPos = sub.find('/');

        if (slashPos == std::string::npos) {
            std::string videoId = sub;
            if (req.method == "GET") {
                response = service_->getVideoById(videoId, requester);
            } else if (req.method == "PUT" || req.method == "PATCH") {
                bool hasTitle = hasJsonKey(req.body, "title");
                std::string newTitle = extractJsonString(req.body, "title");
                if (hasTitle && (newTitle.empty() || newTitle.size() > 120)) {
                    span.setStatusCode(400);
                    return {400, "{\"error\":\"Title must be between 1 and 120 characters\"}"};
                }

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