/**
 * @file test_user_api.cpp
 * @brief Suite de pruebas unitarias para DiskBTree y BTreeUserRepository.
 */

#include "BTreeUserRepository.hpp"
#include "TelemetryManager.hpp"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace tecflix::users;

static UserRecord makeUser(const std::string& username, uint64_t quotaBytes = 1024 * 1024) {
    UserRecord u;
    std::strncpy(u.username, username.c_str(), sizeof(u.username) - 1);
    std::strncpy(u.salt, "0123456789abcdef0123456789abcdef", sizeof(u.salt) - 1);
    std::strncpy(u.password_hash, "deadbeefcafebabe0123456789abcdef0123456789abcdef0123456789abcdef", sizeof(u.password_hash) - 1);
    u.quota_bytes = quotaBytes;
    u.used_bytes = 0;
    u.created_at = 1700000000;
    return u;
}

void testCrudAndQuotaInvariants(const fs::path& dir) {
    const std::string idxPath = (dir / "users_crud.idx").string();
    const std::string datPath = (dir / "users_crud.db").string();

    {
        BTreeUserRepository repo(idxPath, datPath);

        // 1. Inserción y rechazo de duplicados (insensible a mayúsculas)
        assert(repo.createUser(makeUser("Carlos_01", 5000)));
        assert(!repo.createUser(makeUser("carlos_01", 9999)));

        // 2. Consulta por username
        auto found = repo.findByUsername("Carlos_01");
        assert(found.has_value());
        assert(found->quota_bytes == 5000);
        assert(found->used_bytes == 0);
        assert(!repo.findByUsername("inexistente").has_value());

        // 3. Reserva y validación estricta de cuota inmutable
        assert(repo.reserveQuota("Carlos_01", 3000));
        assert(!repo.reserveQuota("Carlos_01", 2500)); // 3000 + 2500 > 5000 -> rechazado
        assert(repo.reserveQuota("Carlos_01", 2000));  // Exacto al límite -> permitido
        assert(!repo.reserveQuota("Carlos_01", 1));    // Cuota llena -> rechazado

        // 4. Liberación de cuota tras borrar video
        assert(repo.releaseQuota("Carlos_01", 1500));
        auto afterRelease = repo.findByUsername("Carlos_01");
        assert(afterRelease.has_value() && afterRelease->used_bytes == 3500);

        // 5. Actualización de credenciales sin alterar quota_bytes ni used_bytes
        assert(repo.updateCredentials("Carlos_01", "new_salt_hex", "new_hash_hex"));
        assert(!repo.updateCredentials("nadie", "a", "b"));
        auto updated = repo.findByUsername("Carlos_01");
        assert(updated.has_value());
        assert(std::string(updated->salt) == "new_salt_hex");
        assert(std::string(updated->password_hash) == "new_hash_hex");
        assert(updated->quota_bytes == 5000);
        assert(updated->used_bytes == 3500);
    }

    // 6. Persistencia tras reapertura desde disco
    {
        BTreeUserRepository reopened(idxPath, datPath);
        auto persisted = reopened.findByUsername("Carlos_01");
        assert(persisted.has_value());
        assert(persisted->quota_bytes == 5000);
        assert(persisted->used_bytes == 3500);
    }

    std::cout << "[PASS] testCrudAndQuotaInvariants\n";
}

void testBTreeSplitsAndRebalance(const fs::path& dir) {
    const std::string idxPath = (dir / "users_tree.idx").string();
    const std::string datPath = (dir / "users_tree.db").string();

    BTreeUserRepository repo(idxPath, datPath);

    std::vector<std::string> users = {
        "user_m", "user_d", "user_t", "user_a", "user_g",
        "user_p", "user_z", "user_b", "user_e", "user_k",
        "user_n", "user_r", "user_w", "user_c", "user_f",
        "user_h", "user_j", "user_l", "user_q", "user_v"
    };

    for (size_t i = 0; i < users.size(); ++i) {
        assert(repo.createUser(makeUser(users[i], (i + 1) * 1024)));
    }

    // Verificar búsqueda de todos los elementos tras múltiples divisiones de nodo
    for (size_t i = 0; i < users.size(); ++i) {
        auto u = repo.findByUsername(users[i]);
        assert(u.has_value());
        assert(u->quota_bytes == (i + 1) * 1024);
    }

    // Eliminar elementos en orden variado para ejercitar borrowFromPrev, borrowFromNext y merge
    assert(!repo.deleteUser("no_existe"));
    for (const auto& name : users) {
        assert(repo.deleteUser(name));
        assert(!repo.findByUsername(name).has_value());
    }

    std::cout << "[PASS] testBTreeSplitsAndRebalance\n";
}

int main() {
    tecflix::observability::TelemetryManager::instance().init("tecflix-user-unit-tests", "127.0.0.1", 4318);

    fs::path tempDir = fs::temp_directory_path() / "tecflix_btree_tests";
    std::error_code ec;
    fs::remove_all(tempDir, ec);
    fs::create_directories(tempDir, ec);

    testCrudAndQuotaInvariants(tempDir);
    testBTreeSplitsAndRebalance(tempDir);

    fs::remove_all(tempDir, ec);
    tecflix::observability::TelemetryManager::instance().shutdown();
    std::cout << "\nTodas las pruebas unitarias de User API (Arbol B) finalizaron exitosamente.\n";
    return 0;
}