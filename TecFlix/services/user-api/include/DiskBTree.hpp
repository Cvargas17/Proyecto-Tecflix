#pragma once

#include "UserRecord.hpp"
#include <optional>
#include <shared_mutex>
#include <string>
#include <utility>

namespace tecflix::users {

class DiskBTree {
private:
    std::string indexFile_;
    std::string dataFile_;
    int64_t rootOffset_;
    mutable std::shared_mutex rwMutex_;

    static int compareKeys(const char* a, const char* b);
    static void copyKey(char dest[USERNAME_MAX_LEN], const std::string& src);

    // E/S binaria en disco y sincronización multi-nodo
    void refreshHeader() const;
    void saveHeader();
    void writeNode(int64_t offset, const BTreeNode& node);
    BTreeNode readNode(int64_t offset) const;
    int64_t allocateNode();
    int64_t appendUserRecord(const UserRecord& user);

    // Búsqueda e inserción O(log_t n)
    std::optional<int64_t> searchInternal(int64_t nodeOffset, const char* key) const;
    void splitChild(int64_t parentOffset, int index, int64_t childOffset);
    void insertNonFull(int64_t nodeOffset, const char* key, int64_t dataOffset);
    void insertInternal(const char* key, int64_t dataOffset);

    // Eliminación y rebalanceo O(log_t n)
    int findKey(const BTreeNode& node, const char* key) const;
    std::pair<std::string, int64_t> getPredecessor(int64_t nodeOffset, int idx) const;
    std::pair<std::string, int64_t> getSuccessor(int64_t nodeOffset, int idx) const;
    void borrowFromPrev(int64_t nodeOffset, int idx);
    void borrowFromNext(int64_t nodeOffset, int idx);
    void merge(int64_t nodeOffset, int idx);
    void fill(int64_t nodeOffset, int idx);
    void removeInternal(int64_t nodeOffset, const char* key);

public:
    DiskBTree(const std::string& idxFile, const std::string& datFile);
    ~DiskBTree() = default;

    bool insertUser(const UserRecord& user);
    std::optional<UserRecord> getUser(const std::string& username) const;
    bool updateCredentials(const std::string& username,
                           const std::string& newSalt,
                           const std::string& newPasswordHash);
    bool deleteUser(const std::string& username);

    bool reserveQuota(const std::string& username, uint64_t bytesToAdd);
    bool releaseQuota(const std::string& username, uint64_t bytesToRelease);
};

} // namespace tecflix::users