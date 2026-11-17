#include "DiskBTree.hpp"
#include "TelemetryManager.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>

namespace fs = std::filesystem;

namespace tecflix::users {

int DiskBTree::compareKeys(const char* a, const char* b) {
    for (size_t i = 0; i < USERNAME_MAX_LEN; ++i) {
        unsigned char ca = static_cast<unsigned char>(std::tolower(static_cast<unsigned char>(a[i])));
        unsigned char cb = static_cast<unsigned char>(std::tolower(static_cast<unsigned char>(b[i])));
        if (ca < cb) return -1;
        if (ca > cb) return 1;
        if (ca == 0 && cb == 0) return 0;
    }
    return 0;
}

void DiskBTree::copyKey(char dest[USERNAME_MAX_LEN], const std::string& src) {
    std::memset(dest, 0, USERNAME_MAX_LEN);
    std::strncpy(dest, src.c_str(), USERNAME_MAX_LEN - 1);
}

DiskBTree::DiskBTree(const std::string& idxFile, const std::string& datFile)
    : indexFile_(idxFile), dataFile_(datFile), rootOffset_(-1) {
    std::error_code ec;
    fs::path idxPath(indexFile_);
    fs::path datPath(dataFile_);
    if (idxPath.has_parent_path()) fs::create_directories(idxPath.parent_path(), ec);
    if (datPath.has_parent_path()) fs::create_directories(datPath.parent_path(), ec);

    {
        std::ofstream idx(indexFile_, std::ios::app | std::ios::binary);
        std::ofstream dat(dataFile_, std::ios::app | std::ios::binary);
    }

    std::ifstream checkIdx(indexFile_, std::ios::ate | std::ios::binary);
    if (!checkIdx || checkIdx.tellg() < static_cast<std::streamoff>(sizeof(IndexHeader))) {
        checkIdx.close();
        rootOffset_ = sizeof(IndexHeader);
        saveHeader();
        BTreeNode rootNode;
        writeNode(rootOffset_, rootNode);
    } else {
        checkIdx.seekg(0);
        IndexHeader header{};
        checkIdx.read(reinterpret_cast<char*>(&header), sizeof(IndexHeader));
        rootOffset_ = header.root_offset;
    }
}

void DiskBTree::saveHeader() {
    std::fstream file(indexFile_, std::ios::in | std::ios::out | std::ios::binary);
    if (!file) return;
    IndexHeader header{rootOffset_};
    file.seekp(0);
    file.write(reinterpret_cast<const char*>(&header), sizeof(IndexHeader));
    file.flush();
}

void DiskBTree::writeNode(int64_t offset, const BTreeNode& node) {
    std::fstream file(indexFile_, std::ios::in | std::ios::out | std::ios::binary);
    if (!file) return;
    file.seekp(offset);
    file.write(reinterpret_cast<const char*>(&node), sizeof(BTreeNode));
    file.flush();
}

BTreeNode DiskBTree::readNode(int64_t offset) const {
    BTreeNode node;
    std::ifstream file(indexFile_, std::ios::binary);
    if (!file) return node;
    file.seekg(offset);
    file.read(reinterpret_cast<char*>(&node), sizeof(BTreeNode));
    return node;
}

int64_t DiskBTree::allocateNode() {
    std::fstream file(indexFile_, std::ios::in | std::ios::out | std::ios::binary);
    file.seekp(0, std::ios::end);
    int64_t offset = static_cast<int64_t>(file.tellp());
    BTreeNode blank;
    file.write(reinterpret_cast<const char*>(&blank), sizeof(BTreeNode));
    file.flush();
    return offset;
}

int64_t DiskBTree::appendUserRecord(const UserRecord& user) {
    std::fstream file(dataFile_, std::ios::in | std::ios::out | std::ios::binary);
    file.seekp(0, std::ios::end);
    int64_t offset = static_cast<int64_t>(file.tellp());
    file.write(reinterpret_cast<const char*>(&user), sizeof(UserRecord));
    file.flush();
    return offset;
}

std::optional<int64_t> DiskBTree::searchInternal(int64_t nodeOffset, const char* key) const {
    if (nodeOffset < 0) return std::nullopt;

    BTreeNode node = readNode(nodeOffset);
    int i = 0;
    while (i < node.num_keys && compareKeys(key, node.keys[i]) > 0) {
        ++i;
    }

    if (i < node.num_keys && compareKeys(key, node.keys[i]) == 0) {
        return node.data_offsets[i];
    }

    if (node.is_leaf != 0) {
        return std::nullopt;
    }

    return searchInternal(node.children[i], key);
}

void DiskBTree::splitChild(int64_t parentOffset, int index, int64_t childOffset) {
    BTreeNode parent = readNode(parentOffset);
    BTreeNode y = readNode(childOffset);
    int64_t zOffset = allocateNode();
    BTreeNode z;

    z.is_leaf = y.is_leaf;
    z.num_keys = BTREE_T - 1;

    for (int j = 0; j < BTREE_T - 1; ++j) {
        std::memcpy(z.keys[j], y.keys[j + BTREE_T], USERNAME_MAX_LEN);
        z.data_offsets[j] = y.data_offsets[j + BTREE_T];
    }

    if (y.is_leaf == 0) {
        for (int j = 0; j < BTREE_T; ++j) {
            z.children[j] = y.children[j + BTREE_T];
        }
    }

    y.num_keys = BTREE_T - 1;

    for (int j = parent.num_keys; j >= index + 1; --j) {
        parent.children[j + 1] = parent.children[j];
    }
    parent.children[index + 1] = zOffset;

    for (int j = parent.num_keys - 1; j >= index; --j) {
        std::memcpy(parent.keys[j + 1], parent.keys[j], USERNAME_MAX_LEN);
        parent.data_offsets[j + 1] = parent.data_offsets[j];
    }

    std::memcpy(parent.keys[index], y.keys[BTREE_T - 1], USERNAME_MAX_LEN);
    parent.data_offsets[index] = y.data_offsets[BTREE_T - 1];
    parent.num_keys++;

    writeNode(childOffset, y);
    writeNode(zOffset, z);
    writeNode(parentOffset, parent);
}

void DiskBTree::insertNonFull(int64_t nodeOffset, const char* key, int64_t dataOffset) {
    BTreeNode node = readNode(nodeOffset);
    int i = node.num_keys - 1;

    if (node.is_leaf != 0) {
        while (i >= 0 && compareKeys(key, node.keys[i]) < 0) {
            std::memcpy(node.keys[i + 1], node.keys[i], USERNAME_MAX_LEN);
            node.data_offsets[i + 1] = node.data_offsets[i];
            --i;
        }
        std::memcpy(node.keys[i + 1], key, USERNAME_MAX_LEN);
        node.data_offsets[i + 1] = dataOffset;
        node.num_keys++;
        writeNode(nodeOffset, node);
    } else {
        while (i >= 0 && compareKeys(key, node.keys[i]) < 0) {
            --i;
        }
        ++i;
        BTreeNode child = readNode(node.children[i]);
        if (child.num_keys == BTREE_ORDER - 1) {
            splitChild(nodeOffset, i, node.children[i]);
            node = readNode(nodeOffset);
            if (compareKeys(key, node.keys[i]) > 0) {
                ++i;
            }
        }
        insertNonFull(node.children[i], key, dataOffset);
    }
}

void DiskBTree::insertInternal(const char* key, int64_t dataOffset) {
    BTreeNode root = readNode(rootOffset_);
    if (root.num_keys == BTREE_ORDER - 1) {
        int64_t newRootOffset = allocateNode();
        BTreeNode newRoot;
        newRoot.is_leaf = 0;
        newRoot.num_keys = 0;
        newRoot.children[0] = rootOffset_;

        int64_t oldRootOffset = rootOffset_;
        rootOffset_ = newRootOffset;
        writeNode(newRootOffset, newRoot);
        saveHeader();

        splitChild(newRootOffset, 0, oldRootOffset);
        insertNonFull(newRootOffset, key, dataOffset);
    } else {
        insertNonFull(rootOffset_, key, dataOffset);
    }
}

int DiskBTree::findKey(const BTreeNode& node, const char* key) const {
    int idx = 0;
    while (idx < node.num_keys && compareKeys(node.keys[idx], key) < 0) {
        ++idx;
    }
    return idx;
}

std::pair<std::string, int64_t> DiskBTree::getPredecessor(int64_t nodeOffset, int idx) const {
    BTreeNode curr = readNode(readNode(nodeOffset).children[idx]);
    while (curr.is_leaf == 0) {
        curr = readNode(curr.children[curr.num_keys]);
    }
    return {std::string(curr.keys[curr.num_keys - 1]), curr.data_offsets[curr.num_keys - 1]};
}

std::pair<std::string, int64_t> DiskBTree::getSuccessor(int64_t nodeOffset, int idx) const {
    BTreeNode curr = readNode(readNode(nodeOffset).children[idx + 1]);
    while (curr.is_leaf == 0) {
        curr = readNode(curr.children[0]);
    }
    return {std::string(curr.keys[0]), curr.data_offsets[0]};
}

void DiskBTree::borrowFromPrev(int64_t nodeOffset, int idx) {
    BTreeNode node = readNode(nodeOffset);
    int64_t childOffset = node.children[idx];
    int64_t siblingOffset = node.children[idx - 1];

    BTreeNode child = readNode(childOffset);
    BTreeNode sibling = readNode(siblingOffset);

    for (int i = child.num_keys - 1; i >= 0; --i) {
        std::memcpy(child.keys[i + 1], child.keys[i], USERNAME_MAX_LEN);
        child.data_offsets[i + 1] = child.data_offsets[i];
    }

    if (child.is_leaf == 0) {
        for (int i = child.num_keys; i >= 0; --i) {
            child.children[i + 1] = child.children[i];
        }
    }

    std::memcpy(child.keys[0], node.keys[idx - 1], USERNAME_MAX_LEN);
    child.data_offsets[0] = node.data_offsets[idx - 1];

    if (child.is_leaf == 0) {
        child.children[0] = sibling.children[sibling.num_keys];
    }

    std::memcpy(node.keys[idx - 1], sibling.keys[sibling.num_keys - 1], USERNAME_MAX_LEN);
    node.data_offsets[idx - 1] = sibling.data_offsets[sibling.num_keys - 1];

    child.num_keys += 1;
    sibling.num_keys -= 1;

    writeNode(childOffset, child);
    writeNode(siblingOffset, sibling);
    writeNode(nodeOffset, node);
}

void DiskBTree::borrowFromNext(int64_t nodeOffset, int idx) {
    BTreeNode node = readNode(nodeOffset);
    int64_t childOffset = node.children[idx];
    int64_t siblingOffset = node.children[idx + 1];

    BTreeNode child = readNode(childOffset);
    BTreeNode sibling = readNode(siblingOffset);

    std::memcpy(child.keys[child.num_keys], node.keys[idx], USERNAME_MAX_LEN);
    child.data_offsets[child.num_keys] = node.data_offsets[idx];

    if (child.is_leaf == 0) {
        child.children[child.num_keys + 1] = sibling.children[0];
    }

    std::memcpy(node.keys[idx], sibling.keys[0], USERNAME_MAX_LEN);
    node.data_offsets[idx] = sibling.data_offsets[0];

    for (int i = 1; i < sibling.num_keys; ++i) {
        std::memcpy(sibling.keys[i - 1], sibling.keys[i], USERNAME_MAX_LEN);
        sibling.data_offsets[i - 1] = sibling.data_offsets[i];
    }

    if (sibling.is_leaf == 0) {
        for (int i = 1; i <= sibling.num_keys; ++i) {
            sibling.children[i - 1] = sibling.children[i];
        }
    }

    child.num_keys += 1;
    sibling.num_keys -= 1;

    writeNode(childOffset, child);
    writeNode(siblingOffset, sibling);
    writeNode(nodeOffset, node);
}

void DiskBTree::merge(int64_t nodeOffset, int idx) {
    BTreeNode node = readNode(nodeOffset);
    int64_t childOffset = node.children[idx];
    int64_t siblingOffset = node.children[idx + 1];

    BTreeNode child = readNode(childOffset);
    BTreeNode sibling = readNode(siblingOffset);

    std::memcpy(child.keys[BTREE_T - 1], node.keys[idx], USERNAME_MAX_LEN);
    child.data_offsets[BTREE_T - 1] = node.data_offsets[idx];

    for (int i = 0; i < sibling.num_keys; ++i) {
        std::memcpy(child.keys[i + BTREE_T], sibling.keys[i], USERNAME_MAX_LEN);
        child.data_offsets[i + BTREE_T] = sibling.data_offsets[i];
    }

    if (child.is_leaf == 0) {
        for (int i = 0; i <= sibling.num_keys; ++i) {
            child.children[i + BTREE_T] = sibling.children[i];
        }
    }

    for (int i = idx + 1; i < node.num_keys; ++i) {
        std::memcpy(node.keys[i - 1], node.keys[i], USERNAME_MAX_LEN);
        node.data_offsets[i - 1] = node.data_offsets[i];
    }

    for (int i = idx + 2; i <= node.num_keys; ++i) {
        node.children[i - 1] = node.children[i];
    }

    child.num_keys += sibling.num_keys + 1;
    node.num_keys--;

    writeNode(childOffset, child);
    writeNode(nodeOffset, node);
}

void DiskBTree::fill(int64_t nodeOffset, int idx) {
    BTreeNode node = readNode(nodeOffset);
    if (idx != 0 && readNode(node.children[idx - 1]).num_keys >= BTREE_T) {
        borrowFromPrev(nodeOffset, idx);
    } else if (idx != node.num_keys && readNode(node.children[idx + 1]).num_keys >= BTREE_T) {
        borrowFromNext(nodeOffset, idx);
    } else {
        if (idx != node.num_keys) {
            merge(nodeOffset, idx);
        } else {
            merge(nodeOffset, idx - 1);
        }
    }
}

void DiskBTree::removeInternal(int64_t nodeOffset, const char* key) {
    BTreeNode node = readNode(nodeOffset);
    int idx = findKey(node, key);

    if (idx < node.num_keys && compareKeys(node.keys[idx], key) == 0) {
        if (node.is_leaf != 0) {
            for (int i = idx + 1; i < node.num_keys; ++i) {
                std::memcpy(node.keys[i - 1], node.keys[i], USERNAME_MAX_LEN);
                node.data_offsets[i - 1] = node.data_offsets[i];
            }
            node.num_keys--;
            writeNode(nodeOffset, node);
        } else {
            char kCopy[USERNAME_MAX_LEN];
            std::memcpy(kCopy, node.keys[idx], USERNAME_MAX_LEN);
            if (readNode(node.children[idx]).num_keys >= BTREE_T) {
                auto [predKey, predOffset] = getPredecessor(nodeOffset, idx);
                copyKey(node.keys[idx], predKey);
                node.data_offsets[idx] = predOffset;
                writeNode(nodeOffset, node);
                removeInternal(node.children[idx], node.keys[idx]);
            } else if (readNode(node.children[idx + 1]).num_keys >= BTREE_T) {
                auto [succKey, succOffset] = getSuccessor(nodeOffset, idx);
                copyKey(node.keys[idx], succKey);
                node.data_offsets[idx] = succOffset;
                writeNode(nodeOffset, node);
                removeInternal(node.children[idx + 1], node.keys[idx]);
            } else {
                merge(nodeOffset, idx);
                node = readNode(nodeOffset);
                removeInternal(node.children[idx], kCopy);
            }
        }
    } else {
        if (node.is_leaf != 0) return;

        bool flag = (idx == node.num_keys);
        if (readNode(node.children[idx]).num_keys < BTREE_T) {
            fill(nodeOffset, idx);
            node = readNode(nodeOffset);
        }

        if (flag && idx > node.num_keys) {
            removeInternal(node.children[idx - 1], key);
        } else {
            removeInternal(node.children[idx], key);
        }
    }
}

bool DiskBTree::insertUser(const UserRecord& user) {
    tecflix::observability::ScopedSpan span("btree.lookup", "", {{"btree.op", "insert"}, {"user", user.username}});
    std::unique_lock<std::shared_mutex> lock(rwMutex_);

    char key[USERNAME_MAX_LEN];
    copyKey(key, user.username);

    if (searchInternal(rootOffset_, key).has_value()) {
        span.setStatusCode(409);
        return false;
    }

    int64_t dataOffset = appendUserRecord(user);
    insertInternal(key, dataOffset);
    span.setStatusCode(201);
    return true;
}

std::optional<UserRecord> DiskBTree::getUser(const std::string& username) const {
    tecflix::observability::ScopedSpan span("btree.lookup", "", {{"btree.op", "get"}, {"user", username}});
    std::shared_lock<std::shared_mutex> lock(rwMutex_);

    char key[USERNAME_MAX_LEN];
    copyKey(key, username);

    auto offset = searchInternal(rootOffset_, key);
    if (!offset) {
        span.setStatusCode(404);
        return std::nullopt;
    }

    UserRecord user;
    std::ifstream file(dataFile_, std::ios::binary);
    file.seekg(*offset);
    file.read(reinterpret_cast<char*>(&user), sizeof(UserRecord));
    span.setStatusCode(200);
    return user;
}

bool DiskBTree::updateCredentials(const std::string& username,
                                  const std::string& newSalt,
                                  const std::string& newPasswordHash) {
    tecflix::observability::ScopedSpan span("btree.lookup", "", {{"btree.op", "update"}, {"user", username}});
    std::unique_lock<std::shared_mutex> lock(rwMutex_);

    char key[USERNAME_MAX_LEN];
    copyKey(key, username);

    auto offset = searchInternal(rootOffset_, key);
    if (!offset) {
        span.setStatusCode(404);
        return false;
    }

    UserRecord user;
    std::fstream file(dataFile_, std::ios::in | std::ios::out | std::ios::binary);
    file.seekg(*offset);
    file.read(reinterpret_cast<char*>(&user), sizeof(UserRecord));

    // La clave username y la cuota quota_bytes son estrictamente inmutables
    std::memset(user.salt, 0, sizeof(user.salt));
    std::strncpy(user.salt, newSalt.c_str(), sizeof(user.salt) - 1);

    std::memset(user.password_hash, 0, sizeof(user.password_hash));
    std::strncpy(user.password_hash, newPasswordHash.c_str(), sizeof(user.password_hash) - 1);

    file.seekp(*offset);
    file.write(reinterpret_cast<const char*>(&user), sizeof(UserRecord));
    file.flush();
    span.setStatusCode(200);
    return true;
}

bool DiskBTree::deleteUser(const std::string& username) {
    tecflix::observability::ScopedSpan span("btree.lookup", "", {{"btree.op", "delete"}, {"user", username}});
    std::unique_lock<std::shared_mutex> lock(rwMutex_);

    char key[USERNAME_MAX_LEN];
    copyKey(key, username);

    if (!searchInternal(rootOffset_, key).has_value()) {
        span.setStatusCode(404);
        return false;
    }

    removeInternal(rootOffset_, key);

    BTreeNode root = readNode(rootOffset_);
    if (root.num_keys == 0 && root.is_leaf == 0) {
        rootOffset_ = root.children[0];
        saveHeader();
    }

    span.setStatusCode(200);
    return true;
}

bool DiskBTree::reserveQuota(const std::string& username, uint64_t bytesToAdd) {
    tecflix::observability::ScopedSpan span("btree.lookup", "", {{"btree.op", "reserveQuota"}, {"user", username}});
    std::unique_lock<std::shared_mutex> lock(rwMutex_);

    char key[USERNAME_MAX_LEN];
    copyKey(key, username);

    auto offset = searchInternal(rootOffset_, key);
    if (!offset) {
        span.setStatusCode(404);
        return false;
    }

    UserRecord user;
    std::fstream file(dataFile_, std::ios::in | std::ios::out | std::ios::binary);
    file.seekg(*offset);
    file.read(reinterpret_cast<char*>(&user), sizeof(UserRecord));

    if (bytesToAdd > user.quota_bytes || user.used_bytes + bytesToAdd > user.quota_bytes) {
        span.setStatusCode(413);
        return false;
    }

    user.used_bytes += bytesToAdd;
    file.seekp(*offset);
    file.write(reinterpret_cast<const char*>(&user), sizeof(UserRecord));
    file.flush();
    span.setStatusCode(200);
    return true;
}

bool DiskBTree::releaseQuota(const std::string& username, uint64_t bytesToRelease) {
    std::unique_lock<std::shared_mutex> lock(rwMutex_);

    char key[USERNAME_MAX_LEN];
    copyKey(key, username);

    auto offset = searchInternal(rootOffset_, key);
    if (!offset) return false;

    UserRecord user;
    std::fstream file(dataFile_, std::ios::in | std::ios::out | std::ios::binary);
    file.seekg(*offset);
    file.read(reinterpret_cast<char*>(&user), sizeof(UserRecord));

    user.used_bytes = (bytesToRelease >= user.used_bytes) ? 0 : (user.used_bytes - bytesToRelease);
    file.seekp(*offset);
    file.write(reinterpret_cast<const char*>(&user), sizeof(UserRecord));
    file.flush();
    return true;
}

} // namespace tecflix::users