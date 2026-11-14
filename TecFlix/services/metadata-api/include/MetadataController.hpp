/**
* @file MetadataController.hpp
 * @brief Declaración del controlador REST HTTP para el microservicio de Metadata de TecFlix.
 */

#pragma once

#include "MetadataService.hpp"
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace tecflix::metadata {

    struct HttpRequest {
        std::string method;
        std::string path;
        std::map<std::string, std::string> queryParams;
        std::map<std::string, std::string> headers;
        std::string body;
    };

    class MetadataController {
    public:
        explicit MetadataController(std::shared_ptr<MetadataService> service);
        ServiceResponse handleRequest(const HttpRequest& req);

    private:
        static std::string getHeaderIgnoreCase(const std::map<std::string, std::string>& headers,
                                               const std::string& key);
        static std::string extractJsonString(const std::string& json, const std::string& key);
        static double extractJsonNumber(const std::string& json, const std::string& key, double defaultVal = 0.0);
        static std::vector<std::string> extractJsonStringArray(const std::string& json, const std::string& key);
        static bool hasJsonKey(const std::string& json, const std::string& key);

        std::shared_ptr<MetadataService> service_;
    };

} // namespace tecflix::metadata