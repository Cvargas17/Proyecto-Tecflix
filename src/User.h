#pragma once
#include <cstddef>
#include <iostream>

// Constantes de negocio
constexpr size_t MAX_QUOTA_BYTES = 1073741824; // 1 GB por defecto

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

// Parámetros del Árbol B en disco
constexpr int BTREE_ORDER = 4;

// Nodo del Árbol B para guardar en binario
struct BTreeNode {
    bool is_leaf;
    int num_keys;
    int keys[BTREE_ORDER - 1];              // IDs de los usuarios
    std::streampos data_offsets[BTREE_ORDER - 1]; // Puntero al archivo users.dat
    std::streampos children[BTREE_ORDER];         // Puntero al archivo index.bin

    BTreeNode() : is_leaf(true), num_keys(0) {
        for (int i = 0; i < BTREE_ORDER; ++i) {
            children[i] = -1; // -1 indica null/vacío en disco
            if (i < BTREE_ORDER - 1) {
                keys[i] = 0;
                data_offsets[i] = -1;
            }
        }
    }
};