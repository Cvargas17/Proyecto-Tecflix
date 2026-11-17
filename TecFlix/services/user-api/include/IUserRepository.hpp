#pragma once

#include "UserRecord.hpp"
#include <cstdint>
#include <optional>
#include <string>

namespace tecflix::users {

class IUserRepository {
public:
    virtual ~IUserRepository() = default;

    virtual bool createUser(const UserRecord& user) = 0;
    virtual std::optional<UserRecord> findByUsername(const std::string& username) const = 0;
    virtual bool updateCredentials(const std::string& username,
                                   const std::string& newSalt,
                                   const std::string& newPasswordHash) = 0;
    virtual bool deleteUser(const std::string& username) = 0;

    // Gestión atómica de cuota inmutable
    virtual bool reserveQuota(const std::string& username, uint64_t bytesToAdd) = 0;
    virtual bool releaseQuota(const std::string& username, uint64_t bytesToRelease) = 0;
};

} // namespace tecflix::users