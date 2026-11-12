/**
 * @file test_metadata_api.cpp
 * @brief Suite de pruebas unitarias para el microservicio de Metadata de TecFlix.
 * @details Valida las reglas de dominio de `MetadataService`, el enrutamiento HTTP y parsing
 *          JSON de `MetadataController`, la serialización de `VideoMetadata` y la generación
 *          de trazas/métricas en `TelemetryManager` mediante un repositorio simulado (`MockMetadataRepository`).
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

/**
 * @class MockMetadataRepository
 * @brief Implementación en memoria de `IMetadataRepository` para pruebas unitarias aisladas.
 * @details Simula almacenamiento, filtrado por visibilidad/tema/etiqueta, ordenamiento,
 *          paginación y fallos controlados de base de datos (`simulateDbFailure`).
 */
class MockMetadataRepository : public IMetadataRepository {
public:
    bool simulateDbFailure{false}; ///< Bandera para forzar fallos de persistencia (HTTP 500).

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
            // Filtrado por alcance ("mine" vs "all")
            if (filter.scope == "mine") {
                if (vid.owner != filter.requesterUsername) continue;
            } else {
                if (vid.visibility == "private" && vid.owner != filter.requesterUsername) continue;
            }

            // Filtrado opcional por tema
            if (!filter.topicFilter.empty() && vid.topic != filter.topicFilter) {
                continue;
            }

            // Filtrado opcional por etiqueta
            if (!filter.tagFilter.empty()) {
                bool hasTag = std::find(vid.tags.begin(), vid.tags.end(), filter.tagFilter) != vid.tags.end();
                if (!hasTag) continue;
            }

            filtered.push_back(vid);
        }

        // Ordenamiento por título o fecha de subida
        bool asc = (filter.order == "asc");
        std::sort(filtered.begin(), filtered.end(), [&](const VideoMetadata& a, const VideoMetadata& b) {
            if (filter.sortBy == "title") {
                return asc ? (a.title < b.title) : (a.title > b.title);
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

/**
 * @brief Prueba el registro de videos válidos e inválidos (campos faltantes, visibilidad inválida, fallo BD).
 */
void testVideoRegistration() {
    auto repo = std::make_shared<MockMetadataRepository>();
    auto service = std::make_shared<MetadataService>(repo);
    MetadataController controller(service);

    // 1. Registro exitoso mediante controlador REST
    HttpRequest createReq{
        "POST",
        "/api/videos",
        {},
        {{"X-User", "fabrizio"}, {"X-Trace-Id", "1234567890abcdef1234567890abcdef"}},
        R"({"id":"vid_001","title":"Arquitectura Microservicios","topic":"Sistemas","tags":["c++","docker"],"visibility":"public","file":"vid_001.mp4"})"
    };
    ServiceResponse res = controller.handleRequest(createReq);
    assert(res.statusCode == 201);
    assert(res.jsonBody.find("\"id\":\"vid_001\"") != std::string::npos);
    assert(res.jsonBody.find("\"canEdit\":true") != std::string::npos);

    // 2. Registro con valores por defecto (sin visibility ni dash explícitos)
    HttpRequest defaultReq{
        "POST",
        "/api/videos",
        {},
        {{"X-User", "ana"}},
        R"({"id":"vid_002","title":"Tutorial OpenTelemetry"})"
    };
    ServiceResponse resDefault = controller.handleRequest(defaultReq);
    assert(resDefault.statusCode == 201);
    assert(resDefault.jsonBody.find("\"visibility\":\"public\"") != std::string::npos);
    assert(resDefault.jsonBody.find("\"dash\":\"pending\"") != std::string::npos);

    // 3. Error 400 por falta de campos obligatorios
    HttpRequest badReq{"POST", "/api/videos", {}, {{"X-User", "fabrizio"}}, R"({"id":"vid_003"})"};
    assert(controller.handleRequest(badReq).statusCode == 400);

    // 4. Error 400 por visibilidad inválida
    HttpRequest badVisReq{"POST", "/api/videos", {}, {{"X-User", "fabrizio"}},
                          R"({"id":"vid_004","title":"Test","visibility":"unlisted"})"};
    assert(controller.handleRequest(badVisReq).statusCode == 400);

    // 5. Error 400 por estado DASH inválido
    VideoMetadata invalidDash;
    invalidDash.id = "vid_005";
    invalidDash.owner = "fabrizio";
    invalidDash.title = "Test";
    invalidDash.dash = "unknown_state";
    assert(service->registerVideo(invalidDash).statusCode == 400);

    // 6. Error 500 por fallo simulado de persistencia
    repo->simulateDbFailure = true;
    HttpRequest dbFailReq{"POST", "/api/videos", {}, {{"X-User", "fabrizio"}},
                          R"({"id":"vid_006","title":"Fallo DB"})"};
    assert(controller.handleRequest(dbFailReq).statusCode == 500);
    repo->simulateDbFailure = false;

    std::cout << "[PASS] testVideoRegistration\n";
}

/**
 * @brief Prueba consultas individuales, permisos de privacidad (403), listados paginados y filtros.
 */
void testQueryAndPrivacyRules() {
    auto repo = std::make_shared<MockMetadataRepository>();
    auto service = std::make_shared<MetadataService>(repo);
    MetadataController controller(service);

    // Poblar catálogo con videos públicos y privados
    controller.handleRequest({"POST", "/api/videos", {}, {{"X-User", "fabrizio"}},
                              R"({"id":"v1","title":"Alpha C++","topic":"Programacion","tags":["cpp"],"visibility":"public"})"});
    controller.handleRequest({"POST", "/api/videos", {}, {{"X-User", "fabrizio"}},
                              R"({"id":"v2","title":"Borrador Privado","topic":"Programacion","tags":["interno"],"visibility":"private"})"});
    controller.handleRequest({"POST", "/api/videos", {}, {{"X-User", "carlos"}},
                              R"({"id":"v3","title":"Redes Avanzadas","topic":"Redes","tags":["tcp"],"visibility":"public"})"});

    // 1. Consulta de video público por usuario anónimo -> 200 OK y canEdit:false
    ServiceResponse pubRes = controller.handleRequest({"GET", "/api/videos/v1", {}, {}, ""});
    assert(pubRes.statusCode == 200);
    assert(pubRes.jsonBody.find("\"canEdit\":false") != std::string::npos);

    // 2. Consulta de video privado por otro usuario -> 403 Forbidden
    ServiceResponse privDenied = controller.handleRequest({"GET", "/api/videos/v2", {}, {{"X-User", "carlos"}}, ""});
    assert(privDenied.statusCode == 403);

    // 3. Consulta de video privado por su dueño -> 200 OK y canEdit:true
    ServiceResponse privOwner = controller.handleRequest({"GET", "/api/videos/v2", {}, {{"X-User", "fabrizio"}}, ""});
    assert(privOwner.statusCode == 200);
    assert(privOwner.jsonBody.find("\"canEdit\":true") != std::string::npos);

    // 4. Consulta de video inexistente -> 404 Not Found
    assert(controller.handleRequest({"GET", "/api/videos/no_existe", {}, {}, ""}).statusCode == 404);

    // 5. Consulta con ID vacío -> 400 Bad Request
    assert(service->getVideoById("", "fabrizio").statusCode == 400);

    // 6. Listado scope="mine" sin autenticación -> 401 Unauthorized
    assert(controller.handleRequest({"GET", "/api/videos", {{"scope", "mine"}}, {}, ""}).statusCode == 401);

    // 7. Listado scope="mine" autenticado como fabrizio -> devuelve 2 videos (v1 y v2)
    ServiceResponse mineRes = controller.handleRequest(
        {"GET", "/api/videos", {{"scope", "mine"}, {"sortBy", "title"}, {"order", "asc"}}, {{"X-User", "fabrizio"}}, ""});
    assert(mineRes.statusCode == 200);
    assert(mineRes.jsonBody.find("\"totalItems\":2") != std::string::npos);

    // 8. Listado público con filtro por topic y tag
    ServiceResponse filteredRes = controller.handleRequest(
        {"GET", "/api/videos", {{"scope", "all"}, {"topic", "Redes"}, {"tag", "tcp"}, {"page", "1"}, {"limit", "10"}}, {}, ""});
    assert(filteredRes.statusCode == 200);
    assert(filteredRes.jsonBody.find("\"totalItems\":1") != std::string::npos);
    assert(filteredRes.jsonBody.find("\"id\":\"v3\"") != std::string::npos);

    std::cout << "[PASS] testQueryAndPrivacyRules\n";
}

/**
 * @brief Prueba actualización de metadatos, estado DASH, borrado y validación de autoría (401/403/404).
 */
void testUpdateAndDeleteAuthorization() {
    auto repo = std::make_shared<MockMetadataRepository>();
    auto service = std::make_shared<MetadataService>(repo);
    MetadataController controller(service);

    controller.handleRequest({"POST", "/api/videos", {}, {{"X-User", "fabrizio"}},
                              R"({"id":"v10","title":"Titulo Original","topic":"DB","tags":["mongo"],"visibility":"public"})"});

    // 1. Intentar editar sin sesión -> 401 Unauthorized
    assert(controller.handleRequest({"PUT", "/api/videos/v10", {}, {}, R"({"title":"Hack"})"}).statusCode == 401);

    // 2. Intentar editar siendo otro usuario -> 403 Forbidden
    assert(controller.handleRequest({"PUT", "/api/videos/v10", {}, {{"X-User", "intruso"}}, R"({"title":"Hack"})"}).statusCode == 403);

    // 3. Editar video inexistente -> 404 Not Found
    assert(controller.handleRequest({"PUT", "/api/videos/v999", {}, {{"X-User", "fabrizio"}}, R"({"title":"Nuevo"})"}).statusCode == 404);

    // 4. Editar con visibilidad inválida -> 400 Bad Request
    assert(controller.handleRequest({"PATCH", "/api/videos/v10", {}, {{"X-User", "fabrizio"}}, R"({"visibility":"secreto"})"}).statusCode == 400);

    // 5. Edición válida por el dueño -> 200 OK
    ServiceResponse editRes = controller.handleRequest(
        {"PUT", "/api/videos/v10", {}, {{"X-User", "fabrizio"}},
         R"({"title":"Titulo Actualizado","visibility":"private","topic":"NoSQL","tags":["mongodb","cpp"]})"});
    assert(editRes.statusCode == 200);
    assert(editRes.jsonBody.find("\"title\":\"Titulo Actualizado\"") != std::string::npos);
    assert(editRes.jsonBody.find("\"visibility\":\"private\"") != std::string::npos);
    assert(editRes.jsonBody.find("\"topic\":\"NoSQL\"") != std::string::npos);

    // 6. Actualización de estado MPEG-DASH (PATCH /api/videos/{id}/dash)
    assert(controller.handleRequest({"PATCH", "/api/videos/v10/dash", {}, {}, R"({"dash":"invalid"})"}).statusCode == 400);
    assert(controller.handleRequest({"PATCH", "/api/videos/no_existe/dash", {}, {}, R"({"dash":"ready"})"}).statusCode == 404);
    ServiceResponse dashRes = controller.handleRequest({"PATCH", "/api/videos/v10/dash", {}, {}, R"({"dash":"ready"})"});
    assert(dashRes.statusCode == 200);
    assert(dashRes.jsonBody.find("\"dash\":\"ready\"") != std::string::npos);

    // 7. Borrado con validaciones de sesión y propiedad
    assert(controller.handleRequest({"DELETE", "/api/videos/v10", {}, {}, ""}).statusCode == 401);
    assert(controller.handleRequest({"DELETE", "/api/videos/v10", {}, {{"X-User", "otro"}}, ""}).statusCode == 403);
    assert(controller.handleRequest({"DELETE", "/api/videos/no_existe", {}, {{"X-User", "fabrizio"}}, ""}).statusCode == 404);

    // 8. Borrado exitoso por el propietario -> 200 OK
    ServiceResponse delRes = controller.handleRequest({"DELETE", "/api/videos/v10", {}, {{"X-User", "fabrizio"}}, ""});
    assert(delRes.statusCode == 200);
    assert(delRes.jsonBody.find("\"deleted\":true") != std::string::npos);

    // 9. Healthcheck y endpoint desconocido
    assert(controller.handleRequest({"GET", "/health", {}, {}, ""}).statusCode == 200);
    assert(controller.handleRequest({"GET", "/ruta/inexistente", {}, {}, ""}).statusCode == 404);

    std::cout << "[PASS] testUpdateAndDeleteAuthorization\n";
}

/**
 * @brief Punto de entrada de la suite de pruebas unitarias del microservicio de Metadata.
 */
int main() {
    // Inicializa telemetría hacia localhost (no bloqueante si el dashboard no está levantado en CI)
    TelemetryManager::instance().init("tecflix-metadata-unit-tests", "127.0.0.1", 4318);

    testVideoRegistration();
    testQueryAndPrivacyRules();
    testUpdateAndDeleteAuthorization();

    TelemetryManager::instance().shutdown();
    std::cout << "\nTodas las pruebas unitarias de Metadata API finalizaron exitosamente.\n";
    return 0;
}