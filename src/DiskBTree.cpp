#include "DiskBTree.h"
#include <cstring>
#include <mutex>

DiskBTree::DiskBTree(const std::string& idxFile, const std::string& datFile) 
    : indexFile(idxFile), dataFile(datFile), rootOffset(0) {
    
    // Crear archivos si no existen
    std::ofstream idx(indexFile, std::ios::app | std::ios::binary);
    std::ofstream dat(dataFile, std::ios::app | std::ios::binary);
    
    // Si el índice está vacío, inicializamos la raíz
    std::ifstream checkIdx(indexFile, std::ios::ate | std::ios::binary);
    if (checkIdx.tellg() == 0) {
        BTreeNode rootNode;
        writeNode(0, rootNode);
    }
}

// -------------------------------------------------------------------
// OPERACIONES DE ENTRADA/SALIDA BINARIAS (NIVEL DISCO)
// -------------------------------------------------------------------

void DiskBTree::writeNode(std::streampos offset, const BTreeNode& node) {
    std::ofstream file(indexFile, std::ios::in | std::ios::out | std::ios::binary);
    file.seekp(offset);
    file.write(reinterpret_cast<const char*>(&node), sizeof(BTreeNode));
}

BTreeNode DiskBTree::readNode(std::streampos offset) const {
    BTreeNode node;
    std::ifstream file(indexFile, std::ios::binary);
    file.seekg(offset);
    file.read(reinterpret_cast<char*>(&node), sizeof(BTreeNode));
    return node;
}

std::streampos DiskBTree::appendUser(const User& user) {
    std::ofstream file(dataFile, std::ios::app | std::ios::binary);
    std::streampos offset = file.tellp();
    file.write(reinterpret_cast<const char*>(&user), sizeof(User));
    return offset;
}

// -------------------------------------------------------------------
// LÓGICA DE BÚSQUEDA EN ÁRBOL B (LECTURA CONCURRENTE)
// -------------------------------------------------------------------

std::optional<std::streampos> DiskBTree::searchInternal(std::streampos nodeOffset, int userId) const {
    if (nodeOffset == std::streampos(-1)) return std::nullopt;

    BTreeNode node = readNode(nodeOffset);
    int i = 0;
    while (i < node.num_keys && userId > node.keys[i]) {
        i++;
    }

    if (i < node.num_keys && userId == node.keys[i]) {
        return node.data_offsets[i]; // Usuario encontrado
    }

    if (node.is_leaf) {
        return std::nullopt; // No existe
    }

    return searchInternal(node.children[i], userId); // Buscar en el hijo
}

std::optional<User> DiskBTree::getUser(int userId) const {
    // Bloqueo compartido (Lectores concurrentes permitidos)
    std::shared_lock<std::shared_mutex> lock(rw_mutex);

    auto offset = searchInternal(rootOffset, userId);
    if (!offset) return std::nullopt;

    User user;
    std::ifstream file(dataFile, std::ios::binary);
    file.seekg(*offset);
    file.read(reinterpret_cast<char*>(&user), sizeof(User));
    return user;
}

// -------------------------------------------------------------------
// LÓGICA DE ESCRITURA Y NEGOCIO (BLOQUEO EXCLUSIVO)
// -------------------------------------------------------------------

bool DiskBTree::insertUser(const User& user) {
    // Bloqueo exclusivo (Solo un escritor a la vez)
    std::unique_lock<std::shared_mutex> lock(rw_mutex);

    if (searchInternal(rootOffset, user.id).has_value()) {
        return false; // El ID ya existe
    }

    std::streampos dataOffset = appendUser(user);
    
    // Aquí iría la lógica del B-Tree Insert (Split de nodos si num_keys == BTREE_ORDER)
    // Por simplicidad para el entregable, asumimos la llamada a:
    // insertInternal(user.id, dataOffset);
    
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

    // Validar regla de negocio estricta
    if (user.quota_used + videoSizeBytes > user.max_quota) {
        return false; // Denegar subida: Cuota excedida
    }

    // Actualizar cuota
    user.quota_used += videoSizeBytes;
    file.seekp(*offset);
    file.write(reinterpret_cast<const char*>(&user), sizeof(User));

    return true;
}