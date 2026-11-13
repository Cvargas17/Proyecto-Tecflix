#include "./crow_all.h" // Framework Crow
#include "DiskBTree.h"
#include <string>

int main() {
    crow::SimpleApp app;
    
    // Instanciar el gestor del Árbol B
    DiskBTree btree("index.bin", "users.dat");

    // 1. REGISTRAR USUARIO (POST)
    CROW_ROUTE(app, "/api/users").methods(crow::HTTPMethod::Post)
    ([&btree](const crow::request& req) {
        auto json = crow::json::load(req.body);
        if (!json || !json.has("id") || !json.has("username") || !json.has("password")) {
            return crow::response(400, "Invalid JSON payload");
        }

        User newUser;
        newUser.id = json["id"].i();
        // Convertimos explícitamente a std::string antes de usar c_str()
        strncpy(newUser.username, std::string(json["username"].s()).c_str(), sizeof(newUser.username) - 1);
        strncpy(newUser.password, std::string(json["password"].s()).c_str(), sizeof(newUser.password) - 1);
        // Aseguramos que la cadena termine en el carácter nulo por seguridad
        newUser.username[sizeof(newUser.username) - 1] = '\0';
        newUser.password[sizeof(newUser.password) - 1] = '\0';
        
        if (btree.insertUser(newUser)) {
            return crow::response(201, "User registered successfully");
        } else {
            return crow::response(409, "User ID already exists");
        }
    });

    // 2. CONSULTAR USUARIO (GET)
    CROW_ROUTE(app, "/api/users/<int>").methods(crow::HTTPMethod::Get)
    ([&btree](int userId) {
        auto userOpt = btree.getUser(userId);
        if (!userOpt) {
            return crow::response(404, "User not found");
        }

        crow::json::wvalue res;
        res["id"] = userOpt->id;
        res["username"] = userOpt->username;
        res["quota_used"] = userOpt->quota_used;
        res["max_quota"] = userOpt->max_quota;

        return crow::response(200, res);
    });

    // 3. LÓGICA DE NEGOCIO: SUBIR VIDEO (POST)
    CROW_ROUTE(app, "/api/users/<int>/videos").methods(crow::HTTPMethod::Post)
    ([&btree](const crow::request& req, int userId) {
        auto json = crow::json::load(req.body);
        if (!json || !json.has("video_size_bytes")) {
            return crow::response(400, "Missing video_size_bytes");
        }

        size_t videoSize = json["video_size_bytes"].i();

        // El BTree maneja internamente la verificación y el bloqueo del archivo
        bool success = btree.verifyQuotaAndAdd(userId, videoSize);
        
        if (success) {
            return crow::response(200, "Video upload authorized and quota updated");
        } else {
            return crow::response(403, "Forbidden: Quota exceeded or user not found");
        }
    });

    // Configurar y lanzar servidor en el puerto 8080 con múltiples hilos
    app.port(8080).multithreaded().run();

    return 0;
}