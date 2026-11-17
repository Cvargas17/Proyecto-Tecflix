#include "BTreeUserRepository.hpp"

namespace tecflix::users {

BTreeUserRepository::BTreeUserRepository(const std::string& indexPath, const std::string& dataPath)
    : tree_(std::make_unique<DiskBTree>(indexPath, dataPath)) {}

bool BTreeUserRepository::createUser(const UserRecord& user) {
    return tree_->insertUser(user);
}

std::optional<UserRecord> BTreeUserRepository::findByUsername(const std::string& username) const {
    return tree_->getUser(username);
}

bool BTreeUserRepository::updateCredentials(const std::string& username,
                                            const std::string& newSalt,
                                            const std::string& newPasswordHash) {
    return tree_->updateCredentials(username, newSalt, newPasswordHash);
}

bool BTreeUserRepository::deleteUser(const std::string& username) {
    return tree_->deleteUser(username);
}

bool BTreeUserRepository::reserveQuota(const std::string& username, uint64_t bytesToAdd) {
    return tree_->reserveQuota(username, bytesToAdd);
}

bool BTreeUserRepository::releaseQuota(const std::string& username, uint64_t bytesToRelease) {
    return tree_->releaseQuota(username, bytesToRelease);
}

} // namespace tecflix::users