#pragma once
#include "User.h"
#include <string>
#include <shared_mutex>
#include <optional>

class DiskBTree {
private:
    std::string indexFile;
    std::string dataFile;
    int64_t rootOffset;
    mutable std::shared_mutex rw_mutex;

    // Operaciones de E/S binaria en disco
    void saveHeader();
    void writeNode(int64_t offset, const BTreeNode& node);
    BTreeNode readNode(int64_t offset) const;
    int64_t allocateNode();
    int64_t appendUser(const User& user);

    // Búsqueda e Inserción en Árbol B
    std::optional<int64_t> searchInternal(int64_t nodeOffset, int userId) const;
    void splitChild(int64_t parentOffset, int index, int64_t childOffset);
    void insertNonFull(int64_t nodeOffset, int userId, int64_t dataOffset);
    void insertInternal(int userId, int64_t dataOffset);

    // Eliminación y Rebalanceo en Árbol B
    int findKey(const BTreeNode& node, int userId) const;
    std::pair<int, int64_t> getPredecessor(int64_t nodeOffset, int idx) const;
    std::pair<int, int64_t> getSuccessor(int64_t nodeOffset, int idx) const;
    void borrowFromPrev(int64_t nodeOffset, int idx);
    void borrowFromNext(int64_t nodeOffset, int idx);
    void merge(int64_t nodeOffset, int idx);
    void fill(int64_t nodeOffset, int idx);
    void removeInternal(int64_t nodeOffset, int userId);

public:
    DiskBTree(const std::string& idxFile, const std::string& datFile);
    ~DiskBTree() = default;

    // CRUD completo
    bool insertUser(const User& user);
    std::optional<User> getUser(int userId) const;
    bool updateUser(int userId, const std::string& newUsername, const std::string& newPassword);
    bool deleteUser(int userId);

    // Lógica de negocio de cuota
    bool verifyQuotaAndAdd(int userId, size_t videoSizeBytes);
};