/**
 * @file test_metadata_api.cpp
 * @brief Suite de pruebas unitarias para el microservicio de Metadata de TecFlix.
 */

#include "IMetadataRepository.hpp"
#include "MetadataController.hpp"
#include "MetadataService.hpp"
#include "TelemetryManager.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace tecflix::metadata;
using namespace tecflix::observability;

class MockMetadataRepository : public IMetadataRepository {
public:
    bool simulateDbFailure{false};

    bool create(const VideoMetadata& video) override {
        if (simulateDbFailure) return false;
        if (storage_.count(video.id)) return false;
        storage_[video.id] = video;
        return true;
    }

    std::optional<VideoMetadata> findById(const std::string& videoId) override {
        auto it = storage_.find(videoId);
        if (it == storage_.end()) return std::nullopt;
        return it->second;
    }

    PaginatedVideos findPaginated(const VideoQueryFilter& filter) override {
        std::vector<VideoMetadata> filtered;

        for (const auto& [id, vid] : storage_) {
            if (filter.scope == "mine") {
                if (vid.owner != filter.requesterUsername) continue;
            } else {
                if (vid.visibility == "private" && vid.owner != filter.requesterUsername) continue;
            }

            if (!filter.topicFilter.empty() && vid.topic != filter.topicFilter) {
                continue;
            }

            if (!filter.tagFilter.empty()) {
                bool hasTag = std::find(vid.tags.begin(), vid.tags.end(), filter.tagFilter) != vid.tags.end();
                if (!hasTag) continue;
            }

            filtered.push_back(vid);
        }

        bool asc = (filter.order == "asc");
        std::sort(filtered.begin(), filtered.end(), [&](const VideoMetadata& a, const VideoMetadata& b) {
            if (filter.sortBy == "title") {
                return asc ? (a.title < b.title) : (a.title > b.title);
            }
            if (filter.sortBy == "owner" || filter.sortBy == "author") {
                return asc ? (a.owner < b.owner) : (a.owner > b.owner);
            }
            return asc ? (a.uploaded < b.uploaded) : (a.uploaded > b.uploaded);
        });

        int totalItems = static_cast<int>(filtered.size());
        int totalPages = std::max(1, static_cast<int>(std::ceil(static_cast<double>(totalItems) / filter.limit)));
        int page = std::clamp(filter.page, 1, totalPages);

        size_t startIdx = static_cast<size_t>((page - 1) * filter.limit);
        size_t endIdx = std::min(filtered.size(), startIdx + static_cast<size_t>(filter.limit));

        std::vector<VideoMetadata> pageItems;
        if (startIdx < filtered.size()) {
            pageItems.assign(filtered.begin() + startIdx, filtered.begin() + endIdx);
        }

        return PaginatedVideos{pageItems, page, filter.limit, totalPages, totalItems};
    }

    bool update(const VideoMetadata& video) override {
        if (simulateDbFailure) return false;
        if (!storage_.count(video.id)) return false;
        storage_[video.id] = video;
        return true;
    }

    bool updateDashStatus(const std::string& videoId, const std::string& dashStatus) override {
        if (simulateDbFailure) return false;
        auto it = storage_.find(videoId);
        if (it == storage_.end()) return false;
        it->second.dash = dashStatus;
        return true;
    }

    bool remove(const std::string& videoId) override {
        if (simulateDbFailure) return false;
        return storage_.erase(videoId) > 0;
    }

private:
    std::map<std::string, VideoMetadata> storage_;
};

void testVideoRegistration() {
    auto repo = std::make_shared<MockMetadataRepository>();
    auto service = std::make_shared<MetadataService>(repo);
    MetadataController controller(service);

    // 1. Registro exitoso con ID hexadecimal, duración y tamaño en bytes
    HttpRequest createReq{
        "POST",
        "/api/videos",
        {},
        {{"X-User", "fabrizio"}, {"traceparent", "00-1234567890abcdef1234567890abcdef-1234567890abcdef-01"}},
        R"({"id":"a00001","title":"Arquitectura Microservicios","topic":"Sistemas","tags":["c++","docker"],"visibility":"public","file":"a00001.mp4","duration":125.5,"sizeBytes":10485760})"
    };
    ServiceResponse res = controller.handleRequest(createReq);
    assert(res.statusCode == 201);
    assert(res.jsonBody.find("\"id\":\"a00001\"") != std::string::npos);
    assert(res.jsonBody.find("\"duration\":125.5") != std::string::npos);
    assert(res.jsonBody.find("\"sizeBytes\":10485760") != std::string::npos);
    assert(res.jsonBody.find("\"canEdit\":true") != std::string::npos);

    // 2. Conflicto 409 al intentar registrar un ID duplicado
    assert(controller.handleRequest(createReq).statusCode == 409);

    // 3. Registro con valores por defecto
    HttpRequest defaultReq{
        "POST",
        "/api/videos",
        {},
        {{"X-User", "ana"}},
        R"({"id":"a00002","title":"Tutorial OpenTelemetry"})"
    };
    ServiceResponse resDefault = controller.handleRequest(defaultReq);
    assert(resDefault.statusCode == 201);
    assert(resDefault.jsonBody.find("\"visibility\":\"public\"") != std::string::npos);
    assert(resDefault.jsonBody.find("\"dash\":\"pending\"") != std::string::npos);

    // 4. Error 401 sin usuario autenticado
    HttpRequest unauthReq{"POST", "/api/videos", {}, {}, R"({"id":"a00003","title":"Sin Auth"})"};
    assert(controller.handleRequest(unauthReq).statusCode == 401);

    // 5. Error 400 por falta de campos obligatorios o ID no hexadecimal
    assert(controller.handleRequest({"POST", "/api/videos", {}, {{"X-User", "fabrizio"}}, R"({"id":"a00003"})"}).statusCode == 400);
    assert(controller.handleRequest({"POST", "/api/videos", {}, {{"X-User", "fabrizio"}}, R"({"id":"no_hex!","title":"Test"})"}).statusCode == 400);

    // 6. Error 400 por título > 120 caracteres, duración negativa o tamaño negativo
    std::string longTitle(125, 'X');
    assert(controller.handleRequest({"POST", "/api/videos", {}, {{"X-User", "fabrizio"}},
                                     "{\"id\":\"a00004\",\"title\":\"" + longTitle + "\"}"}).statusCode == 400);
    assert(controller.handleRequest({"POST", "/api/videos", {}, {{"X-User", "fabrizio"}},
                                     R"({"id":"a00004","title":"Duracion Negativa","duration":-5.0})"}).statusCode == 400);
    assert(controller.handleRequest({"POST", "/api/videos", {}, {{"X-User", "fabrizio"}},
                                     R"({"id":"a00004","title":"Size Negativo","sizeBytes":-10})"}).statusCode == 400);

    // 7. Error 400 por visibilidad inválida o estado DASH inválido
    assert(controller.handleRequest({"POST", "/api/videos", {}, {{"X-User", "fabrizio"}},
                                     R"({"id":"a00004","title":"Test","visibility":"unlisted"})"}).statusCode == 400);
    VideoMetadata invalidDash;
    invalidDash.id = "a00005";
    invalidDash.owner = "fabrizio";
    invalidDash.title = "Test";
    invalidDash.dash = "unknown_state";
    assert(service->registerVideo(invalidDash).statusCode == 400);

    // 8. Error 500 por fallo simulado de persistencia
    repo->simulateDbFailure = true;
    assert(controller.handleRequest({"POST", "/api/videos", {}, {{"X-User", "fabrizio"}},
                                     R"({"id":"a00006","title":"Fallo DB"})"}).statusCode == 500);
    repo->simulateDbFailure = false;

    std::cout << "[PASS] testVideoRegistration\n";
}

void testQueryAndPrivacyRules() {
    auto repo = std::make_shared<MockMetadataRepository>();
    auto service = std::make_shared<MetadataService>(repo);
    MetadataController controller(service);

    controller.handleRequest({"POST", "/api/videos", {}, {{"X-User", "fabrizio"}},
                              R"({"id":"b00001","title":"Alpha C++","topic":"Programacion","tags":["cpp"],"visibility":"public","duration":60})"});
    controller.handleRequest({"POST", "/api/videos", {}, {{"X-User", "fabrizio"}},
                              R"({"id":"b00002","title":"Borrador Privado","topic":"Programacion","tags":["interno"],"visibility":"private","duration":90})"});
    controller.handleRequest({"POST", "/api/videos", {}, {{"X-User", "carlos"}},
                              R"({"id":"b00003","title":"Redes Avanzadas","topic":"Redes","tags":["tcp"],"visibility":"public","duration":240})"});

    // 1. Consulta de video público por usuario anónimo -> 200 OK y canEdit:false
    ServiceResponse pubRes = controller.handleRequest({"GET", "/api/videos/b00001", {}, {}, ""});
    assert(pubRes.statusCode == 200);
    assert(pubRes.jsonBody.find("\"canEdit\":false") != std::string::npos);

    // 2. Consulta de video privado por otro usuario -> 403 Forbidden
    assert(controller.handleRequest({"GET", "/api/videos/b00002", {}, {{"X-User", "carlos"}}, ""}).statusCode == 403);

    // 3. Consulta de video privado por su dueño -> 200 OK y canEdit:true
    ServiceResponse privOwner = controller.handleRequest({"GET", "/api/videos/b00002", {}, {{"X-User", "fabrizio"}}, ""});
    assert(privOwner.statusCode == 200);
    assert(privOwner.jsonBody.find("\"canEdit\":true") != std::string::npos);

    // 4. Consulta de video inexistente (404) e ID inválido (400)
    assert(controller.handleRequest({"GET", "/api/videos/deadbeef", {}, {}, ""}).statusCode == 404);
    assert(service->getVideoById("", "fabrizio").statusCode == 400);
    assert(service->getVideoById("not-hex", "fabrizio").statusCode == 400);

    // 5. Validaciones estrictas de parámetros de consulta en GET /api/videos -> 400
    assert(controller.handleRequest({"GET", "/api/videos", {{"page", "0"}}, {}, ""}).statusCode == 400);
    assert(controller.handleRequest({"GET", "/api/videos", {{"limit", "100"}}, {}, ""}).statusCode == 400);
    assert(controller.handleRequest({"GET", "/api/videos", {{"sortBy", "invalid"}}, {}, ""}).statusCode == 400);
    assert(controller.handleRequest({"GET", "/api/videos", {{"order", "sideways"}}, {}, ""}).statusCode == 400);
    assert(controller.handleRequest({"GET", "/api/videos", {{"scope", "invalid"}}, {}, ""}).statusCode == 400);

    // 6. Listado scope="mine" sin autenticación -> 401 Unauthorized
    assert(controller.handleRequest({"GET", "/api/videos", {{"scope", "mine"}}, {}, ""}).statusCode == 401);

    // 7. Listado scope="mine" autenticado como fabrizio -> devuelve 2 videos
    ServiceResponse mineRes = controller.handleRequest(
        {"GET", "/api/videos", {{"scope", "mine"}, {"sortBy", "title"}, {"order", "asc"}}, {{"X-User", "fabrizio"}}, ""});
    assert(mineRes.statusCode == 200);
    assert(mineRes.jsonBody.find("\"totalItems\":2") != std::string::npos);

    // 8. Listado público con filtro por topic y tag
    ServiceResponse filteredRes = controller.handleRequest(
        {"GET", "/api/videos", {{"scope", "all"}, {"topic", "Redes"}, {"tag", "tcp"}, {"page", "1"}, {"limit", "10"}}, {}, ""});
    assert(filteredRes.statusCode == 200);
    assert(filteredRes.jsonBody.find("\"totalItems\":1") != std::string::npos);
    assert(filteredRes.jsonBody.find("\"id\":\"b00003\"") != std::string::npos);

    // 9. Ordenamiento ascendente por autor ("author" -> carlos antes que fabrizio)
    ServiceResponse sortedByAuthor = controller.handleRequest(
        {"GET", "/api/videos", {{"scope", "all"}, {"sortBy", "author"}, {"order", "asc"}}, {}, ""});
    assert(sortedByAuthor.statusCode == 200);
    size_t posCarlos = sortedByAuthor.jsonBody.find("\"owner\":\"carlos\"");
    size_t posFabrizio = sortedByAuthor.jsonBody.find("\"owner\":\"fabrizio\"");
    assert(posCarlos != std::string::npos && posFabrizio != std::string::npos && posCarlos < posFabrizio);

    std::cout << "[PASS] testQueryAndPrivacyRules\n";
}

void testUpdateAndDeleteAuthorization() {
    auto repo = std::make_shared<MockMetadataRepository>();
    auto service = std::make_shared<MetadataService>(repo);
    MetadataController controller(service);

    controller.handleRequest({"POST", "/api/videos", {}, {{"X-User", "fabrizio"}},
                              R"({"id":"c00010","title":"Titulo Original","topic":"DB","tags":["mongo"],"visibility":"public"})"});

    // 1. Permisos de edición (401, 403, 404, 400)
    assert(controller.handleRequest({"PUT", "/api/videos/c00010", {}, {}, R"({"title":"Hack"})"}).statusCode == 401);
    assert(controller.handleRequest({"PUT", "/api/videos/c00010", {}, {{"X-User", "intruso"}}, R"({"title":"Hack"})"}).statusCode == 403);
    assert(controller.handleRequest({"PUT", "/api/videos/c00999", {}, {{"X-User", "fabrizio"}}, R"({"title":"Nuevo"})"}).statusCode == 404);
    assert(controller.handleRequest({"PATCH", "/api/videos/c00010", {}, {{"X-User", "fabrizio"}}, R"({"visibility":"secreto"})"}).statusCode == 400);

    // 2. Edición válida por el dueño -> 200 OK
    ServiceResponse editRes = controller.handleRequest(
        {"PUT", "/api/videos/c00010", {}, {{"X-User", "fabrizio"}},
         R"({"title":"Titulo Actualizado","visibility":"private","topic":"NoSQL","tags":["mongodb","cpp"]})"});
    assert(editRes.statusCode == 200);
    assert(editRes.jsonBody.find("\"title\":\"Titulo Actualizado\"") != std::string::npos);
    assert(editRes.jsonBody.find("\"visibility\":\"private\"") != std::string::npos);

    // 3. Actualización de estado MPEG-DASH (PATCH /api/videos/{id}/dash)
    assert(controller.handleRequest({"PATCH", "/api/videos/c00010/dash", {}, {}, R"({"dash":"invalid"})"}).statusCode == 400);
    assert(controller.handleRequest({"PATCH", "/api/videos/deadbeef/dash", {}, {}, R"({"dash":"ready"})"}).statusCode == 404);
    ServiceResponse dashRes = controller.handleRequest({"PATCH", "/api/videos/c00010/dash", {}, {}, R"({"dash":"ready"})"});
    assert(dashRes.statusCode == 200);
    assert(dashRes.jsonBody.find("\"dash\":\"ready\"") != std::string::npos);

    // 4. Borrado con validaciones de sesión y propiedad
    assert(controller.handleRequest({"DELETE", "/api/videos/c00010", {}, {}, ""}).statusCode == 401);
    assert(controller.handleRequest({"DELETE", "/api/videos/c00010", {}, {{"X-User", "otro"}}, ""}).statusCode == 403);
    assert(controller.handleRequest({"DELETE", "/api/videos/deadbeef", {}, {{"X-User", "fabrizio"}}, ""}).statusCode == 404);

    // 5. Borrado exitoso por el propietario -> 200 OK
    ServiceResponse delRes = controller.handleRequest({"DELETE", "/api/videos/c00010", {}, {{"X-User", "fabrizio"}}, ""});
    assert(delRes.statusCode == 200);
    assert(delRes.jsonBody.find("\"deleted\":true") != std::string::npos);

    // 6. Healthcheck y endpoint desconocido
    assert(controller.handleRequest({"GET", "/health", {}, {}, ""}).statusCode == 200);
    assert(controller.handleRequest({"GET", "/ruta/inexistente", {}, {}, ""}).statusCode == 404);

    std::cout << "[PASS] testUpdateAndDeleteAuthorization\n";
}

int main() {
    TelemetryManager::instance().init("tecflix-metadata-unit-tests", "127.0.0.1", 4318);

    testVideoRegistration();
    testQueryAndPrivacyRules();
    testUpdateAndDeleteAuthorization();

    TelemetryManager::instance().shutdown();
    std::cout << "\nTodas las pruebas unitarias de Metadata API finalizaron exitosamente.\n";
    return 0;
}