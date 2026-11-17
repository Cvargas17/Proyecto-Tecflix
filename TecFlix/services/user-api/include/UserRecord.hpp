#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace tecflix::users {

constexpr uint64_t DEFAULT_MAX_QUOTA_BYTES = 1073741824ULL; // 1 GiB inmutable por defecto
constexpr int BTREE_ORDER = 4;                              // Grado mínimo t = 2 (máx. 3 claves, 4 hijos)
constexpr int BTREE_T = BTREE_ORDER / 2;
constexpr size_t USERNAME_MAX_LEN = 32;
constexpr size_t SALT_HEX_LEN = 33;
constexpr size_t HASH_HEX_LEN = 65;

struct UserRecord {
    char username[USERNAME_MAX_LEN];
    char salt[SALT_HEX_LEN];
    char password_hash[HASH_HEX_LEN];
    uint64_t quota_bytes;
    uint64_t used_bytes;
    int64_t created_at;

    UserRecord()
        : quota_bytes(DEFAULT_MAX_QUOTA_BYTES), used_bytes(0), created_at(0) {
        std::memset(username, 0, sizeof(username));
        std::memset(salt, 0, sizeof(salt));
        std::memset(password_hash, 0, sizeof(password_hash));
    }
};

struct IndexHeader {
    int64_t root_offset;
};

struct BTreeNode {
    uint8_t is_leaf;
    int32_t num_keys;
    char keys[BTREE_ORDER - 1][USERNAME_MAX_LEN];
    int64_t data_offsets[BTREE_ORDER - 1];
    int64_t children[BTREE_ORDER];

    BTreeNode() : is_leaf(1), num_keys(0) {
        std::memset(keys, 0, sizeof(keys));
        for (int i = 0; i < BTREE_ORDER; ++i) {
            children[i] = -1;
            if (i < BTREE_ORDER - 1) {
                data_offsets[i] = -1;
            }
        }
    }
};

} // namespace tecflix::users