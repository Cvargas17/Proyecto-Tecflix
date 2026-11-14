#include "crow_all.h"
#include "DiskBTree.h"
#include <string>
#include <cstring>

int main() {
    crow::SimpleApp app;
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
        std::string uname = json["username"].s();
        std::string pass = json["password"].s();

        std::strncpy(newUser.username, uname.c_str(), sizeof(newUser.username) - 1);
        newUser.username[sizeof(newUser.username) - 1] = '\0';

        std::strncpy(newUser.password, pass.c_str(), sizeof(newUser.password) - 1);
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

    // 3. ACTUALIZAR USUARIO (PUT)
    CROW_ROUTE(app, "/api/users/<int>").methods(crow::HTTPMethod::Put)
    ([&btree](const crow::request& req, int userId) {
        auto json = crow::json::load(req.body);
        if (!json) {
            return crow::response(400, "Invalid JSON payload");
        }

        std::string newUsername = json.has("username") ? std::string(json["username"].s()) : "";
        std::string newPassword = json.has("password") ? std::string(json["password"].s()) : "";

        if (btree.updateUser(userId, newUsername, newPassword)) {
            return crow::response(200, "User updated successfully");
        } else {
            return crow::response(404, "User not found");
        }
    });

    // 4. ELIMINAR USUARIO (DELETE)
    CROW_ROUTE(app, "/api/users/<int>").methods(crow::HTTPMethod::Delete)
    ([&btree](int userId) {
        if (btree.deleteUser(userId)) {
            return crow::response(200, "User deleted successfully");
        } else {
            return crow::response(404, "User not found");
        }
    });

    // 5. LÓGICA DE NEGOCIO: SUBIR VIDEO Y VALIDAR CUOTA (POST)
    CROW_ROUTE(app, "/api/users/<int>/videos").methods(crow::HTTPMethod::Post)
    ([&btree](const crow::request& req, int userId) {
        auto json = crow::json::load(req.body);
        if (!json || !json.has("video_size_bytes")) {
            return crow::response(400, "Missing video_size_bytes");
        }

        size_t videoSize = json["video_size_bytes"].i();

        if (btree.verifyQuotaAndAdd(userId, videoSize)) {
            return crow::response(200, "Video upload authorized and quota updated");
        } else {
            return crow::response(403, "Forbidden: Quota exceeded or user not found");
        }
    });

    app.port(8080).multithreaded().run();
    return 0;
}