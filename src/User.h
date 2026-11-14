#pragma once
#include <cstddef>
#include <cstdint>

constexpr size_t MAX_QUOTA_BYTES = 1073741824; // 1 GB inmodificable
constexpr int BTREE_ORDER = 4;                 // Grado mínimo t = 2 (máx. 3 claves por nodo)
constexpr int BTREE_T = BTREE_ORDER / 2;

struct User {
    int id;
    char username[32];
    char password[64];
    size_t quota_used;
    size_t max_quota;

    User() : id(0), quota_used(0), max_quota(MAX_QUOTA_BYTES) {
        username[0] = '\0';
        password[0] = '\0';
    }
};

// Cabecera al inicio de index.bin para persistir el offset de la raíz actual
struct IndexHeader {
    int64_t root_offset;
};

// Nodo del Árbol B almacenado en disco
struct BTreeNode {
    bool is_leaf;
    int num_keys;
    int keys[BTREE_ORDER - 1];
    int64_t data_offsets[BTREE_ORDER - 1];
    int64_t children[BTREE_ORDER];

    BTreeNode() : is_leaf(true), num_keys(0) {
        for (int i = 0; i < BTREE_ORDER; ++i) {
            children[i] = -1;
            if (i < BTREE_ORDER - 1) {
                keys[i] = 0;
                data_offsets[i] = -1;
            }
        }
    }
};