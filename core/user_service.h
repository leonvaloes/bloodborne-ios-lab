#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>
#include "guest_memory.h"

namespace bb {

using SceUserServiceUserId = std::int32_t;

// Identificadores de usuário padrão PS4 / Orbis
constexpr SceUserServiceUserId SCE_USER_SERVICE_USER_ID_INVALID = -1;
constexpr SceUserServiceUserId SCE_USER_SERVICE_INITIAL_USER_ID = 0x10000000;
constexpr std::size_t SCE_USER_SERVICE_MAX_USERS = 4;
constexpr std::size_t SCE_USER_SERVICE_MAX_USER_NAME_LENGTH = 16;

// Códigos de erro SCE UserService
constexpr std::int32_t SCE_USER_SERVICE_ERROR_NOT_INITIALIZED     = static_cast<std::int32_t>(0x80960001);
constexpr std::int32_t SCE_USER_SERVICE_ERROR_ALREADY_INITIALIZED = static_cast<std::int32_t>(0x80960002);
constexpr std::int32_t SCE_USER_SERVICE_ERROR_INVALID_ARG         = static_cast<std::int32_t>(0x80960003);
constexpr std::int32_t SCE_USER_SERVICE_ERROR_NO_USER             = static_cast<std::int32_t>(0x80960004);
constexpr std::int32_t SCE_USER_SERVICE_ERROR_BUFFER_TOO_SMALL    = static_cast<std::int32_t>(0x80960005);

#pragma pack(push, 1)
struct SceUserServiceInitializeParams {
    std::uint32_t priority{0};
};

struct SceUserServiceLoginUserIdList {
    SceUserServiceUserId user_id[SCE_USER_SERVICE_MAX_USERS]{
        SCE_USER_SERVICE_USER_ID_INVALID,
        SCE_USER_SERVICE_USER_ID_INVALID,
        SCE_USER_SERVICE_USER_ID_INVALID,
        SCE_USER_SERVICE_USER_ID_INVALID
    };
};
#pragma pack(pop)

struct UserProfile {
    SceUserServiceUserId user_id{SCE_USER_SERVICE_USER_ID_INVALID};
    std::string name;
    bool logged_in{false};
};

class UserServiceManager {
public:
    UserServiceManager();
    ~UserServiceManager() = default;

    std::int32_t sceUserServiceInitialize(const SceUserServiceInitializeParams* params = nullptr);
    std::int32_t sceUserServiceInitialize(GuestMemory& mem, std::uint64_t params_addr);

    std::int32_t sceUserServiceTerminate();
    bool is_initialized() const;

    std::int32_t sceUserServiceGetInitialUser(SceUserServiceUserId* user_id);
    std::int32_t sceUserServiceGetInitialUser(GuestMemory& mem, std::uint64_t user_id_addr);

    std::int32_t sceUserServiceGetLoginIdList(SceUserServiceLoginUserIdList* list);
    std::int32_t sceUserServiceGetLoginIdList(GuestMemory& mem, std::uint64_t list_addr);

    std::int32_t sceUserServiceGetUserName(SceUserServiceUserId user_id, char* buffer, std::size_t size);
    std::int32_t sceUserServiceGetUserName(GuestMemory& mem, SceUserServiceUserId user_id, std::uint64_t buffer_addr, std::size_t size);

    // Métodos utilitários de gestão de sessões
    bool register_user(SceUserServiceUserId user_id, const std::string& name);
    bool login_user(SceUserServiceUserId user_id, const std::string& name);
    bool logout_user(SceUserServiceUserId user_id);
    bool is_user_logged_in(SceUserServiceUserId user_id) const;
    std::string get_user_name(SceUserServiceUserId user_id) const;
    void reset();

private:
    mutable std::mutex mutex_;
    bool initialized_{false};
    SceUserServiceUserId initial_user_id_{SCE_USER_SERVICE_INITIAL_USER_ID};
    std::vector<UserProfile> users_;
};

// Instância padrão global para chamadas HLE estáticas
UserServiceManager& default_user_service();

// Funções C-style / wrappers globais HLE
std::int32_t sceUserServiceInitialize(const SceUserServiceInitializeParams* params = nullptr);
std::int32_t sceUserServiceTerminate();
std::int32_t sceUserServiceGetInitialUser(SceUserServiceUserId* user_id);
std::int32_t sceUserServiceGetLoginIdList(SceUserServiceLoginUserIdList* list);
std::int32_t sceUserServiceGetUserName(SceUserServiceUserId user_id, char* buffer, std::size_t size);

} // namespace bb
