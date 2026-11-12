/**
* @file VideoMetadata.cpp
 * @brief Implementación de la serialización JSON para la entidad `VideoMetadata`.
 */

#include "IMetadataRepository.hpp"
#include <sstream>

namespace tecflix::metadata {

    namespace {

        std::string escapeJsonStr(const std::string& input) {
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

    } // namespace

    std::string VideoMetadata::toJson(const std::string& requesterUsername) const {
        bool canEdit = (!requesterUsername.empty() && requesterUsername == owner);

        std::ostringstream oss;
        oss << "{"
            << "\"id\":\"" << escapeJsonStr(id) << "\","
            << "\"owner\":\"" << escapeJsonStr(owner) << "\","
            << "\"title\":\"" << escapeJsonStr(title) << "\","
            << "\"topic\":\"" << escapeJsonStr(topic) << "\","
            << "\"tags\":[";

        for (size_t i = 0; i < tags.size(); ++i) {
            if (i > 0) oss << ",";
            oss << "\"" << escapeJsonStr(tags[i]) << "\"";
        }

        oss << "],"
            << "\"visibility\":\"" << escapeJsonStr(visibility) << "\","
            << "\"file\":\"" << escapeJsonStr(file) << "\","
            << "\"dash\":\"" << escapeJsonStr(dash) << "\","
            << "\"uploaded\":" << uploaded << ","
            << "\"canEdit\":" << (canEdit ? "true" : "false")
            << "}";

        return oss.str();
    }

} // namespace tecflix::metadata