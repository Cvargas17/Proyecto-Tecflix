#pragma once
#include "User.h"
#include <fstream>
#include <shared_mutex>
#include <optional>

class DiskBTree {
private:
    std::string indexFile;
    std::string dataFile;
    std::streampos rootOffset;
    
    // Mutex de lectores-escritores para proteger los archivos
    mutable std::shared_mutex rw_mutex;

    // Métodos internos de lectura/escritura a nivel de bytes
    void writeNode(std::streampos offset, const BTreeNode& node);
    BTreeNode readNode(std::streampos offset) const;
    std::streampos allocateNode();
    std::streampos appendUser(const User& user);
    void updateUserOnDisk(std::streampos offset, const User& user);

    // Lógica recursiva del Árbol B
    std::optional<std::streampos> searchInternal(std::streampos nodeOffset, int userId) const;
    void insertInternal(int userId, std::streampos userOffset); // Algoritmo de split

public:
    DiskBTree(const std::string& idxFile, const std::string& datFile);
    ~DiskBTree() = default;

    // Operaciones CRUD expuestas al API
    bool insertUser(const User& user);
    std::optional<User> getUser(int userId) const;
    bool updateUser(const User& user);
    bool deleteUser(int userId);
    
    // Lógica de negocio
    bool verifyQuotaAndAdd(int userId, size_t videoSizeBytes);
};