#pragma once

#include "DiskBTree.hpp"
#include "IUserRepository.hpp"
#include <memory>
#include <string>

namespace tecflix::users {

class BTreeUserRepository : public IUserRepository {
private:
    std::unique_ptr<DiskBTree> tree_;

public:
    BTreeUserRepository(const std::string& indexPath, const std::string& dataPath);
    ~BTreeUserRepository() override = default;

    bool createUser(const UserRecord& user) override;
    std::optional<UserRecord> findByUsername(const std::string& username) const override;
    bool updateCredentials(const std::string& username,
                           const std::string& newSalt,
                           const std::string& newPasswordHash) override;
    bool deleteUser(const std::string& username) override;

    bool reserveQuota(const std::string& username, uint64_t bytesToAdd) override;
    bool releaseQuota(const std::string& username, uint64_t bytesToRelease) override;
};

} // namespace tecflix::users