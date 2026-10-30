// psport co-op: shadPS4's user list, reduced to the one user the runtime logs in (id 1).
// The shadNet account comes from the launcher through PS_SHADNET_NPID; its password and token
// from the runtime (PsNetHost, src/net/runtime_secrets.c), never from the environment.
#pragma once
#include <array>
#include <cstdlib>
#include <string>
#include "common/types.h"
#include "../../../../src/platform/ps_host.h"

struct ShadNetSecrets {
    const char* sz_password = nullptr;
    const char* sz_token = nullptr;
};
// Set by psport_net_init, before any NP library can construct the UserManager.
inline ShadNetSecrets g_shadnet_secrets;

// Over the whole capacity: a shorter value assigned since may have left the old bytes behind it.
inline void WipeSecret(std::string& secret) {
    secret.resize(secret.capacity());
    ps_host_wipe(secret.data(), secret.size());
    secret.clear();
}

struct User {
    s32 user_id = -1;
    std::string user_name = "";
    u32 user_color = 1;
    int player_index = 0; // 1-4
    bool logged_in = false;
    std::string shadnet_npid = "";
    std::string shadnet_password = "";
    std::string shadnet_token = "";
    std::string shadnet_email = "";
    bool shadnet_enabled = false;
    std::string np_country = "us";
    std::string np_language = "en";
    u8 np_age = 30;
    std::string np_date_of_birth = "1994-01-01";
};

using LoggedInUsers = std::array<User*, 4>;

class UserManager {
public:
    static constexpr s32 RuntimeUserId = 1; // runtime_services.c USER_ID

    UserManager() {
        const auto text = [](const char* name, const char* fallback) {
            const char* value = std::getenv(name);
            return std::string{value && value[0] ? value : fallback};
        };
        user.user_id = RuntimeUserId;
        user.user_name = text("PS_USER_NAME", "Player");
        user.player_index = 1;
        user.logged_in = true;
        user.shadnet_npid = text("PS_SHADNET_NPID", "");
        user.shadnet_password = g_shadnet_secrets.sz_password ? g_shadnet_secrets.sz_password : "";
        user.shadnet_token = g_shadnet_secrets.sz_token ? g_shadnet_secrets.sz_token : "";
        user.shadnet_enabled = !user.shadnet_npid.empty();
        user.np_country = text("PS_NP_COUNTRY", "us");
        user.np_language = text("PS_NP_LANGUAGE", "en");
    }

    ~UserManager() {
        WipeSecret(user.shadnet_password);
        WipeSecret(user.shadnet_token);
    }
    UserManager(const UserManager&) = delete;
    UserManager& operator=(const UserManager&) = delete;

    User* GetUserByID(s32 user_id) { return user_id == RuntimeUserId ? &user : nullptr; }
    LoggedInUsers GetLoggedInUsers() const {
        return {const_cast<User*>(&user), nullptr, nullptr, nullptr};
    }

private:
    User user;
};
