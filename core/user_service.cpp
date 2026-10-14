#include "user_service.h"
#include <algorithm>
#include <cstring>

namespace bb {

UserServiceManager::UserServiceManager() {
    reset();
}

void UserServiceManager::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    initialized_ = false;
    initial_user_id_ = SCE_USER_SERVICE_INITIAL_USER_ID;
    users_.clear();
}

std::int32_t UserServiceManager::sceUserServiceInitialize(const SceUserServiceInitializeParams* /*params*/) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (initialized_) {
        return SCE_USER_SERVICE_ERROR_ALREADY_INITIALIZED;
    }
    initialized_ = true;
    initial_user_id_ = SCE_USER_SERVICE_INITIAL_USER_ID;
    users_.clear();

    UserProfile initial_user;
    initial_user.user_id = initial_user_id_;
    initial_user.name = "Hunter";
    initial_user.logged_in = true;
    users_.push_back(initial_user);

    return 0; // SCE_OK
}

std::int32_t UserServiceManager::sceUserServiceInitialize(GuestMemory& mem, std::uint64_t params_addr) {
    if (params_addr != 0) {
        SceUserServiceInitializeParams params{};
        if (mem.load(params_addr, &params, sizeof(params)) != MemoryError::ok) {
            return SCE_USER_SERVICE_ERROR_INVALID_ARG;
        }
        return sceUserServiceInitialize(&params);
    }
    return sceUserServiceInitialize(nullptr);
}

std::int32_t UserServiceManager::sceUserServiceTerminate() {
    std::lock_guard<std::mutex> lock(mutex_);
    initialized_ = false;
    users_.clear();
    return 0; // SCE_OK
}

bool UserServiceManager::is_initialized() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return initialized_;
}

std::int32_t UserServiceManager::sceUserServiceGetInitialUser(SceUserServiceUserId* user_id) {
    if (!user_id) {
        return SCE_USER_SERVICE_ERROR_INVALID_ARG;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_) {
        return SCE_USER_SERVICE_ERROR_NOT_INITIALIZED;
    }
    *user_id = initial_user_id_;
    return 0; // SCE_OK
}

std::int32_t UserServiceManager::sceUserServiceGetInitialUser(GuestMemory& mem, std::uint64_t user_id_addr) {
    if (user_id_addr == 0) {
        return SCE_USER_SERVICE_ERROR_INVALID_ARG;
    }
    SceUserServiceUserId uid = SCE_USER_SERVICE_USER_ID_INVALID;
    std::int32_t ret = sceUserServiceGetInitialUser(&uid);
    if (ret != 0) {
        return ret;
    }
    if (mem.store(user_id_addr, &uid, sizeof(uid)) != MemoryError::ok) {
        return SCE_USER_SERVICE_ERROR_INVALID_ARG;
    }
    return 0; // SCE_OK
}

std::int32_t UserServiceManager::sceUserServiceGetLoginIdList(SceUserServiceLoginUserIdList* list) {
    if (!list) {
        return SCE_USER_SERVICE_ERROR_INVALID_ARG;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_) {
        return SCE_USER_SERVICE_ERROR_NOT_INITIALIZED;
    }

    for (std::size_t i = 0; i < SCE_USER_SERVICE_MAX_USERS; ++i) {
        list->user_id[i] = SCE_USER_SERVICE_USER_ID_INVALID;
    }

    std::size_t out_index = 0;
    for (const auto& u : users_) {
        if (u.logged_in && out_index < SCE_USER_SERVICE_MAX_USERS) {
            list->user_id[out_index++] = u.user_id;
        }
    }
    return 0; // SCE_OK
}

std::int32_t UserServiceManager::sceUserServiceGetLoginIdList(GuestMemory& mem, std::uint64_t list_addr) {
    if (list_addr == 0) {
        return SCE_USER_SERVICE_ERROR_INVALID_ARG;
    }
    SceUserServiceLoginUserIdList list{};
    std::int32_t ret = sceUserServiceGetLoginIdList(&list);
    if (ret != 0) {
        return ret;
    }
    if (mem.store(list_addr, &list, sizeof(list)) != MemoryError::ok) {
        return SCE_USER_SERVICE_ERROR_INVALID_ARG;
    }
    return 0; // SCE_OK
}

std::int32_t UserServiceManager::sceUserServiceGetUserName(SceUserServiceUserId user_id, char* buffer, std::size_t size) {
    if (!buffer || size == 0) {
        return SCE_USER_SERVICE_ERROR_INVALID_ARG;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_) {
        return SCE_USER_SERVICE_ERROR_NOT_INITIALIZED;
    }

    auto it = std::find_if(users_.begin(), users_.end(), [user_id](const UserProfile& u) {
        return u.user_id == user_id && u.logged_in;
    });

    if (it == users_.end()) {
        return SCE_USER_SERVICE_ERROR_NO_USER;
    }

    if (size < it->name.length() + 1) {
        return SCE_USER_SERVICE_ERROR_BUFFER_TOO_SMALL;
    }

    std::memcpy(buffer, it->name.c_str(), it->name.length() + 1);
    return 0; // SCE_OK
}

std::int32_t UserServiceManager::sceUserServiceGetUserName(GuestMemory& mem, SceUserServiceUserId user_id, std::uint64_t buffer_addr, std::size_t size) {
    if (buffer_addr == 0 || size == 0) {
        return SCE_USER_SERVICE_ERROR_INVALID_ARG;
    }
    std::vector<char> temp(size);
    std::int32_t ret = sceUserServiceGetUserName(user_id, temp.data(), size);
    if (ret != 0) {
        return ret;
    }
    std::size_t written_len = std::strlen(temp.data()) + 1;
    if (mem.store(buffer_addr, temp.data(), written_len) != MemoryError::ok) {
        return SCE_USER_SERVICE_ERROR_INVALID_ARG;
    }
    return 0; // SCE_OK
}

bool UserServiceManager::register_user(SceUserServiceUserId user_id, const std::string& name) {
    if (user_id == SCE_USER_SERVICE_USER_ID_INVALID || name.empty()) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = std::find_if(users_.begin(), users_.end(), [user_id](const UserProfile& u) {
        return u.user_id == user_id;
    });
    if (it != users_.end()) {
        it->name = name;
        return true;
    }
    if (users_.size() >= SCE_USER_SERVICE_MAX_USERS) {
        return false;
    }
    UserProfile user;
    user.user_id = user_id;
    user.name = name;
    user.logged_in = false;
    users_.push_back(user);
    return true;
}

bool UserServiceManager::login_user(SceUserServiceUserId user_id, const std::string& name) {
    if (user_id == SCE_USER_SERVICE_USER_ID_INVALID) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = std::find_if(users_.begin(), users_.end(), [user_id](const UserProfile& u) {
        return u.user_id == user_id;
    });
    if (it != users_.end()) {
        if (!name.empty()) {
            it->name = name;
        }
        it->logged_in = true;
        return true;
    }
    if (users_.size() >= SCE_USER_SERVICE_MAX_USERS) {
        return false;
    }
    UserProfile user;
    user.user_id = user_id;
    user.name = name.empty() ? "Hunter" : name;
    user.logged_in = true;
    users_.push_back(user);
    return true;
}

bool UserServiceManager::logout_user(SceUserServiceUserId user_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = std::find_if(users_.begin(), users_.end(), [user_id](const UserProfile& u) {
        return u.user_id == user_id;
    });
    if (it == users_.end() || !it->logged_in) {
        return false;
    }
    it->logged_in = false;
    return true;
}

bool UserServiceManager::is_user_logged_in(SceUserServiceUserId user_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = std::find_if(users_.begin(), users_.end(), [user_id](const UserProfile& u) {
        return u.user_id == user_id && u.logged_in;
    });
    return it != users_.end();
}

std::string UserServiceManager::get_user_name(SceUserServiceUserId user_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = std::find_if(users_.begin(), users_.end(), [user_id](const UserProfile& u) {
        return u.user_id == user_id;
    });
    if (it != users_.end()) {
        return it->name;
    }
    return {};
}

UserServiceManager& default_user_service() {
    static UserServiceManager s_instance;
    return s_instance;
}

std::int32_t sceUserServiceInitialize(const SceUserServiceInitializeParams* params) {
    return default_user_service().sceUserServiceInitialize(params);
}

std::int32_t sceUserServiceTerminate() {
    return default_user_service().sceUserServiceTerminate();
}

std::int32_t sceUserServiceGetInitialUser(SceUserServiceUserId* user_id) {
    return default_user_service().sceUserServiceGetInitialUser(user_id);
}

std::int32_t sceUserServiceGetLoginIdList(SceUserServiceLoginUserIdList* list) {
    return default_user_service().sceUserServiceGetLoginIdList(list);
}

std::int32_t sceUserServiceGetUserName(SceUserServiceUserId user_id, char* buffer, std::size_t size) {
    return default_user_service().sceUserServiceGetUserName(user_id, buffer, size);
}

} // namespace bb
