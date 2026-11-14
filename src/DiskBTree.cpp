#include "DiskBTree.h"
#include <fstream>
#include <cstring>
#include <mutex>

DiskBTree::DiskBTree(const std::string& idxFile, const std::string& datFile)
    : indexFile(idxFile), dataFile(datFile), rootOffset(-1) {

    // Asegurar que ambos archivos existan
    {
        std::ofstream idx(indexFile, std::ios::app | std::ios::binary);
        std::ofstream dat(dataFile, std::ios::app | std::ios::binary);
    }

    std::ifstream checkIdx(indexFile, std::ios::ate | std::ios::binary);
    if (checkIdx.tellg() == 0) {
        checkIdx.close();
        // Inicializar cabecera y nodo raíz vacío
        rootOffset = sizeof(IndexHeader);
        saveHeader();
        BTreeNode rootNode;
        writeNode(rootOffset, rootNode);
    } else {
        checkIdx.seekg(0);
        IndexHeader header{};
        checkIdx.read(reinterpret_cast<char*>(&header), sizeof(IndexHeader));
        rootOffset = header.root_offset;
    }
}

// -------------------------------------------------------------------
// OPERACIONES DE ENTRADA/SALIDA BINARIAS (NIVEL DISCO)
// -------------------------------------------------------------------

void DiskBTree::saveHeader() {
    std::fstream file(indexFile, std::ios::in | std::ios::out | std::ios::binary);
    IndexHeader header{rootOffset};
    file.seekp(0);
    file.write(reinterpret_cast<const char*>(&header), sizeof(IndexHeader));
    file.flush();
}

void DiskBTree::writeNode(int64_t offset, const BTreeNode& node) {
    std::fstream file(indexFile, std::ios::in | std::ios::out | std::ios::binary);
    file.seekp(offset);
    file.write(reinterpret_cast<const char*>(&node), sizeof(BTreeNode));
    file.flush();
}

BTreeNode DiskBTree::readNode(int64_t offset) const {
    BTreeNode node;
    std::ifstream file(indexFile, std::ios::binary);
    file.seekg(offset);
    file.read(reinterpret_cast<char*>(&node), sizeof(BTreeNode));
    return node;
}

int64_t DiskBTree::allocateNode() {
    std::fstream file(indexFile, std::ios::in | std::ios::out | std::ios::binary);
    file.seekp(0, std::ios::end);
    int64_t offset = static_cast<int64_t>(file.tellp());
    BTreeNode blank;
    file.write(reinterpret_cast<const char*>(&blank), sizeof(BTreeNode));
    file.flush();
    return offset;
}

int64_t DiskBTree::appendUser(const User& user) {
    std::fstream file(dataFile, std::ios::in | std::ios::out | std::ios::binary);
    file.seekp(0, std::ios::end);
    int64_t offset = static_cast<int64_t>(file.tellp());
    file.write(reinterpret_cast<const char*>(&user), sizeof(User));
    file.flush();
    return offset;
}

// -------------------------------------------------------------------
// BÚSQUEDA E INSERCIÓN EN ÁRBOL B
// -------------------------------------------------------------------

std::optional<int64_t> DiskBTree::searchInternal(int64_t nodeOffset, int userId) const {
    if (nodeOffset == -1) return std::nullopt;

    BTreeNode node = readNode(nodeOffset);
    int i = 0;
    while (i < node.num_keys && userId > node.keys[i]) {
        i++;
    }

    if (i < node.num_keys && userId == node.keys[i]) {
        return node.data_offsets[i];
    }

    if (node.is_leaf) {
        return std::nullopt;
    }

    return searchInternal(node.children[i], userId);
}

void DiskBTree::splitChild(int64_t parentOffset, int index, int64_t childOffset) {
    BTreeNode parent = readNode(parentOffset);
    BTreeNode y = readNode(childOffset);
    int64_t zOffset = allocateNode();
    BTreeNode z;

    z.is_leaf = y.is_leaf;
    z.num_keys = BTREE_T - 1;

    for (int j = 0; j < BTREE_T - 1; ++j) {
        z.keys[j] = y.keys[j + BTREE_T];
        z.data_offsets[j] = y.data_offsets[j + BTREE_T];
    }

    if (!y.is_leaf) {
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
        parent.keys[j + 1] = parent.keys[j];
        parent.data_offsets[j + 1] = parent.data_offsets[j];
    }

    parent.keys[index] = y.keys[BTREE_T - 1];
    parent.data_offsets[index] = y.data_offsets[BTREE_T - 1];
    parent.num_keys++;

    writeNode(childOffset, y);
    writeNode(zOffset, z);
    writeNode(parentOffset, parent);
}

void DiskBTree::insertNonFull(int64_t nodeOffset, int userId, int64_t dataOffset) {
    BTreeNode node = readNode(nodeOffset);
    int i = node.num_keys - 1;

    if (node.is_leaf) {
        while (i >= 0 && userId < node.keys[i]) {
            node.keys[i + 1] = node.keys[i];
            node.data_offsets[i + 1] = node.data_offsets[i];
            i--;
        }
        node.keys[i + 1] = userId;
        node.data_offsets[i + 1] = dataOffset;
        node.num_keys++;
        writeNode(nodeOffset, node);
    } else {
        while (i >= 0 && userId < node.keys[i]) {
            i--;
        }
        i++;
        BTreeNode child = readNode(node.children[i]);
        if (child.num_keys == BTREE_ORDER - 1) {
            splitChild(nodeOffset, i, node.children[i]);
            node = readNode(nodeOffset);
            if (userId > node.keys[i]) {
                i++;
            }
        }
        insertNonFull(node.children[i], userId, dataOffset);
    }
}

void DiskBTree::insertInternal(int userId, int64_t dataOffset) {
    BTreeNode root = readNode(rootOffset);
    if (root.num_keys == BTREE_ORDER - 1) {
        int64_t newRootOffset = allocateNode();
        BTreeNode newRoot;
        newRoot.is_leaf = false;
        newRoot.num_keys = 0;
        newRoot.children[0] = rootOffset;

        int64_t oldRootOffset = rootOffset;
        rootOffset = newRootOffset;
        writeNode(newRootOffset, newRoot);
        saveHeader();

        splitChild(newRootOffset, 0, oldRootOffset);
        insertNonFull(newRootOffset, userId, dataOffset);
    } else {
        insertNonFull(rootOffset, userId, dataOffset);
    }
}

// -------------------------------------------------------------------
// ELIMINACIÓN Y REBALANCEO EN ÁRBOL B
// -------------------------------------------------------------------

int DiskBTree::findKey(const BTreeNode& node, int userId) const {
    int idx = 0;
    while (idx < node.num_keys && node.keys[idx] < userId) {
        ++idx;
    }
    return idx;
}

std::pair<int, int64_t> DiskBTree::getPredecessor(int64_t nodeOffset, int idx) const {
    BTreeNode curr = readNode(readNode(nodeOffset).children[idx]);
    while (!curr.is_leaf) {
        curr = readNode(curr.children[curr.num_keys]);
    }
    return {curr.keys[curr.num_keys - 1], curr.data_offsets[curr.num_keys - 1]};
}

std::pair<int, int64_t> DiskBTree::getSuccessor(int64_t nodeOffset, int idx) const {
    BTreeNode curr = readNode(readNode(nodeOffset).children[idx + 1]);
    while (!curr.is_leaf) {
        curr = readNode(curr.children[0]);
    }
    return {curr.keys[0], curr.data_offsets[0]};
}

void DiskBTree::borrowFromPrev(int64_t nodeOffset, int idx) {
    BTreeNode node = readNode(nodeOffset);
    int64_t childOffset = node.children[idx];
    int64_t siblingOffset = node.children[idx - 1];

    BTreeNode child = readNode(childOffset);
    BTreeNode sibling = readNode(siblingOffset);

    for (int i = child.num_keys - 1; i >= 0; --i) {
        child.keys[i + 1] = child.keys[i];
        child.data_offsets[i + 1] = child.data_offsets[i];
    }

    if (!child.is_leaf) {
        for (int i = child.num_keys; i >= 0; --i) {
            child.children[i + 1] = child.children[i];
        }
    }

    child.keys[0] = node.keys[idx - 1];
    child.data_offsets[0] = node.data_offsets[idx - 1];

    if (!child.is_leaf) {
        child.children[0] = sibling.children[sibling.num_keys];
    }

    node.keys[idx - 1] = sibling.keys[sibling.num_keys - 1];
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

    child.keys[child.num_keys] = node.keys[idx];
    child.data_offsets[child.num_keys] = node.data_offsets[idx];

    if (!child.is_leaf) {
        child.children[child.num_keys + 1] = sibling.children[0];
    }

    node.keys[idx] = sibling.keys[0];
    node.data_offsets[idx] = sibling.data_offsets[0];

    for (int i = 1; i < sibling.num_keys; ++i) {
        sibling.keys[i - 1] = sibling.keys[i];
        sibling.data_offsets[i - 1] = sibling.data_offsets[i];
    }

    if (!sibling.is_leaf) {
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

    child.keys[BTREE_T - 1] = node.keys[idx];
    child.data_offsets[BTREE_T - 1] = node.data_offsets[idx];

    for (int i = 0; i < sibling.num_keys; ++i) {
        child.keys[i + BTREE_T] = sibling.keys[i];
        child.data_offsets[i + BTREE_T] = sibling.data_offsets[i];
    }

    if (!child.is_leaf) {
        for (int i = 0; i <= sibling.num_keys; ++i) {
            child.children[i + BTREE_T] = sibling.children[i];
        }
    }

    for (int i = idx + 1; i < node.num_keys; ++i) {
        node.keys[i - 1] = node.keys[i];
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

void DiskBTree::removeInternal(int64_t nodeOffset, int userId) {
    BTreeNode node = readNode(nodeOffset);
    int idx = findKey(node, userId);

    if (idx < node.num_keys && node.keys[idx] == userId) {
        if (node.is_leaf) {
            for (int i = idx + 1; i < node.num_keys; ++i) {
                node.keys[i - 1] = node.keys[i];
                node.data_offsets[i - 1] = node.data_offsets[i];
            }
            node.num_keys--;
            writeNode(nodeOffset, node);
        } else {
            int k = node.keys[idx];
            if (readNode(node.children[idx]).num_keys >= BTREE_T) {
                auto [predKey, predOffset] = getPredecessor(nodeOffset, idx);
                node.keys[idx] = predKey;
                node.data_offsets[idx] = predOffset;
                writeNode(nodeOffset, node);
                removeInternal(node.children[idx], predKey);
            } else if (readNode(node.children[idx + 1]).num_keys >= BTREE_T) {
                auto [succKey, succOffset] = getSuccessor(nodeOffset, idx);
                node.keys[idx] = succKey;
                node.data_offsets[idx] = succOffset;
                writeNode(nodeOffset, node);
                removeInternal(node.children[idx + 1], succKey);
            } else {
                merge(nodeOffset, idx);
                node = readNode(nodeOffset);
                removeInternal(node.children[idx], k);
            }
        }
    } else {
        if (node.is_leaf) return;

        bool flag = (idx == node.num_keys);
        if (readNode(node.children[idx]).num_keys < BTREE_T) {
            fill(nodeOffset, idx);
            node = readNode(nodeOffset);
        }

        if (flag && idx > node.num_keys) {
            removeInternal(node.children[idx - 1], userId);
        } else {
            removeInternal(node.children[idx], userId);
        }
    }
}

// -------------------------------------------------------------------
// MÉTODOS PÚBLICOS (CRUD Y LÓGICA DE NEGOCIO CON CONCURRENCIA)
// -------------------------------------------------------------------

bool DiskBTree::insertUser(const User& user) {
    std::unique_lock<std::shared_mutex> lock(rw_mutex);

    if (searchInternal(rootOffset, user.id).has_value()) {
        return false; // El ID ya existe
    }

    int64_t dataOffset = appendUser(user);
    insertInternal(user.id, dataOffset); // <-- AHORA SÍ SE INDEXA EN EL ÁRBOL B
    return true;
}

std::optional<User> DiskBTree::getUser(int userId) const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex);

    auto offset = searchInternal(rootOffset, userId);
    if (!offset) return std::nullopt;

    User user;
    std::ifstream file(dataFile, std::ios::binary);
    file.seekg(*offset);
    file.read(reinterpret_cast<char*>(&user), sizeof(User));
    return user;
}

bool DiskBTree::updateUser(int userId, const std::string& newUsername, const std::string& newPassword) {
    std::unique_lock<std::shared_mutex> lock(rw_mutex);

    auto offset = searchInternal(rootOffset, userId);
    if (!offset) return false;

    User user;
    std::fstream file(dataFile, std::ios::in | std::ios::out | std::ios::binary);
    file.seekg(*offset);
    file.read(reinterpret_cast<char*>(&user), sizeof(User));

    // La cuota (max_quota) es inmodificable; solo actualizamos credenciales
    if (!newUsername.empty()) {
        std::memset(user.username, 0, sizeof(user.username));
        std::strncpy(user.username, newUsername.c_str(), sizeof(user.username) - 1);
    }
    if (!newPassword.empty()) {
        std::memset(user.password, 0, sizeof(user.password));
        std::strncpy(user.password, newPassword.c_str(), sizeof(user.password) - 1);
    }

    file.seekp(*offset);
    file.write(reinterpret_cast<const char*>(&user), sizeof(User));
    file.flush();
    return true;
}

bool DiskBTree::deleteUser(int userId) {
    std::unique_lock<std::shared_mutex> lock(rw_mutex);

    if (!searchInternal(rootOffset, userId).has_value()) {
        return false;
    }

    removeInternal(rootOffset, userId);

    // Si la raíz quedó vacía y tiene hijos, reducir altura del árbol
    BTreeNode root = readNode(rootOffset);
    if (root.num_keys == 0 && !root.is_leaf) {
        rootOffset = root.children[0];
        saveHeader();
    }

    return true;
}

bool DiskBTree::verifyQuotaAndAdd(int userId, size_t videoSizeBytes) {
    std::unique_lock<std::shared_mutex> lock(rw_mutex);

    auto offset = searchInternal(rootOffset, userId);
    if (!offset) return false;

    User user;
    std::fstream file(dataFile, std::ios::in | std::ios::out | std::ios::binary);
    file.seekg(*offset);
    file.read(reinterpret_cast<char*>(&user), sizeof(User));

    // Regla de negocio: cuota inmodificable
    if (user.quota_used + videoSizeBytes > user.max_quota) {
        return false;
    }

    user.quota_used += videoSizeBytes;
    file.seekp(*offset);
    file.write(reinterpret_cast<const char*>(&user), sizeof(User));
    file.flush();
    return true;
}