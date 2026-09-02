#include <cstring>
/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2021 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xam/apps/xgi_app.h"
#include "xenia/kernel/xsession.h"

#include "xenia/base/logging.h"

namespace xe {
namespace kernel {
namespace xam {
namespace apps {

// Minimal local session state for BO2 Zombies.
static bool bo2_local_session_active = false;
static uint32_t bo2_local_session_flags = 0;
static uint32_t bo2_local_public_slots = 4;
static uint32_t bo2_local_private_slots = 0;
static uint32_t bo2_local_user_count = 0;
static uint32_t bo2_local_user_index = 0;

/*
 * Most of the structs below were found in the Source SDK, provided as stubs.
 * Specifically, they can be found in the Source 2007 SDK and the Alien Swarm
 * Source SDK. Both are available on Steam for free. A GitHub mirror of the
 * Alien Swarm SDK can be found here:
 * https://github.com/NicolasDe/AlienSwarm/blob/master/src/common/xbox/xboxstubs.h
 */

struct X_USER_ACHIEVEMENT {
  xe::be<uint32_t> user_index;
  xe::be<uint32_t> achievement_id;
};
static_assert_size(X_USER_ACHIEVEMENT, 0x8);

struct XGI_WRITEACHIEVEMENT {
  xe::be<uint32_t> num_achievements;
  xe::be<uint32_t> achievements_ptr;  // X_USER_ACHIEVEMENT*
};
static_assert_size(XGI_WRITEACHIEVEMENT, 0x8);

struct X_USER_AVATAR_ASSET {
  xe::be<uint32_t> user_index;
  xe::be<uint32_t> award_id;
};
static_assert_size(X_USER_AVATAR_ASSET, 0x8);

struct XGI_AWARD_AVATAR_ASSETS {
  xe::be<uint32_t> num_assets;
  xe::be<uint32_t> assets_ptr;  // X_USER_AVATAR_ASSET*
};
static_assert_size(XGI_AWARD_AVATAR_ASSETS, 0x8);

struct XGI_XUSER_GET_PROPERTY {
  xe::be<uint32_t> user_index;
  xe::be<uint32_t> unused;
  xe::be<uint64_t> xuid;  // If xuid is 0 then user_index is used.
  xe::be<uint32_t>
      property_size_ptr;  // Normally filled with sizeof(XUSER_PROPERTY), with
                          // exception of binary and wstring type.
  xe::be<uint32_t> context_address;
  xe::be<uint32_t> property_address;
};
static_assert_size(XGI_XUSER_GET_PROPERTY, 0x20);

struct XGI_XUSER_SET_CONTEXT {
  xe::be<uint32_t> user_index;
  xe::be<uint32_t> unused;
  xe::be<uint64_t> xuid;
  XUSER_CONTEXT context;
};
static_assert_size(XGI_XUSER_SET_CONTEXT, 0x18);

struct XGI_XUSER_SET_PROPERTY {
  xe::be<uint32_t> user_index;
  xe::be<uint32_t> unused;
  xe::be<uint64_t> xuid;
  xe::be<uint32_t> property_id;
  xe::be<uint32_t> data_size;
  xe::be<uint32_t> data_address;
};
static_assert_size(XGI_XUSER_SET_PROPERTY, 0x20);

struct XUSER_STATS_VIEW {
  xe::be<uint32_t> ViewId;
  xe::be<uint32_t> TotalViewRows;
  xe::be<uint32_t> NumRows;
  xe::be<uint32_t> pRows;
};

struct XUSER_STATS_COLUMN {
  xe::be<uint16_t> ColumnId;
  X_USER_DATA Value;
};

struct XUSER_STATS_RESET {
  xe::be<uint32_t> user_index;
  xe::be<uint32_t> view_id;
};

struct XUSER_ANID {
  xe::be<uint32_t> user_index;
  xe::be<uint32_t> cchAnIdBuffer;
  xe::be<uint32_t> pszAnIdBuffer;
  xe::be<uint32_t> value_const;  // 1
};

XgiApp::XgiApp(KernelState* kernel_state) : App(kernel_state, 0xFB) {}

// http://mb.mirage.org/bugzilla/xliveless/main.c

X_HRESULT XgiApp::DispatchMessageSync(uint32_t message, uint32_t buffer_ptr,
                                      uint32_t buffer_length) {
  // NOTE: buffer_length may be zero or valid.
  auto buffer = memory_->TranslateVirtual(buffer_ptr);
  switch (message) {
    case 0x000B0006: {
      assert_true(!buffer_length ||
                  buffer_length == sizeof(XGI_XUSER_SET_CONTEXT));
      const XGI_XUSER_SET_CONTEXT* xgi_context =
          reinterpret_cast<const XGI_XUSER_SET_CONTEXT*>(buffer);

      XELOGD("XGIUserSetContext({:08X}, ID: {:08X}, Value: {:08X})",
             xgi_context->user_index.get(),
             xgi_context->context.context_id.get(),
             xgi_context->context.value.get());

      UserProfile* user = nullptr;
      if (xgi_context->xuid != 0) {
        user = kernel_state_->xam_state()->GetUserProfile(xgi_context->xuid);
      } else {
        user =
            kernel_state_->xam_state()->GetUserProfile(xgi_context->user_index);
      }

      if (user) {
        kernel_state_->xam_state()->user_tracker()->UpdateContext(
            user->xuid(), xgi_context->context.context_id,
            xgi_context->context.value);
      }
      return X_E_SUCCESS;
    }
    case 0x000B0007: {
      assert_true(!buffer_length ||
                  buffer_length == sizeof(XGI_XUSER_SET_PROPERTY));
      const XGI_XUSER_SET_PROPERTY* xgi_property =
          reinterpret_cast<const XGI_XUSER_SET_PROPERTY*>(buffer);

      XELOGD("XGIUserSetPropertyEx({:08X}, {:08X}, {}, {:08X})",
             xgi_property->user_index.get(), xgi_property->property_id.get(),
             xgi_property->data_size.get(), xgi_property->data_address.get());

      UserProfile* user = nullptr;
      if (xgi_property->xuid != 0) {
        user = kernel_state_->xam_state()->GetUserProfile(xgi_property->xuid);
      } else {
        user = kernel_state_->xam_state()->GetUserProfile(
            xgi_property->user_index);
      }

      if (user) {
        Property property(
            xgi_property->property_id,
            Property::get_valid_data_size(xgi_property->property_id,
                                          xgi_property->data_size),
            memory_->TranslateVirtual<uint8_t*>(xgi_property->data_address));

        kernel_state_->xam_state()->user_tracker()->AddProperty(user->xuid(),
                                                                &property);
      }
      return X_E_SUCCESS;
    }
    case 0x000B0008: {
      assert_true(!buffer_length ||
                  buffer_length == sizeof(X_USER_ACHIEVEMENT));

      const XGI_WRITEACHIEVEMENT* write_achievements =
          reinterpret_cast<const XGI_WRITEACHIEVEMENT*>(buffer);

      const X_USER_ACHIEVEMENT* achievements =
          memory_->TranslateVirtual<X_USER_ACHIEVEMENT*>(
              write_achievements->achievements_ptr);

      XELOGD("XGIUserWriteAchievements({:08X}, {:08X})",
             write_achievements->num_achievements.get(),
             write_achievements->achievements_ptr.get());

      for (uint32_t i = 0; i < write_achievements->num_achievements; i++) {
        const X_USER_ACHIEVEMENT& achievement = achievements[i];

        kernel_state_->achievement_manager()->EarnAchievement(
            achievement.user_index, kernel_state_->title_id(),
            achievement.achievement_id);
      }

      return X_E_SUCCESS;
    }
    case 0x000B0010: {
      XELOGD("XSessionCreate({:08X}, {:08X}), implemented in netplay",
             buffer_ptr, buffer_length);
      assert_true(!buffer_length || buffer_length == 28);
      // Sequence:
      // - XamSessionCreateHandle
      // - XamSessionRefObjByHandle
      // - [this]
      // - CloseHandle
      uint32_t session_ptr = xe::load_and_swap<uint32_t>(buffer + 0x0);
      uint32_t flags = xe::load_and_swap<uint32_t>(buffer + 0x4);
      uint32_t num_slots_public = xe::load_and_swap<uint32_t>(buffer + 0x8);
      uint32_t num_slots_private = xe::load_and_swap<uint32_t>(buffer + 0xC);
      uint32_t user_xuid = xe::load_and_swap<uint32_t>(buffer + 0x10);
      uint32_t session_info_ptr = xe::load_and_swap<uint32_t>(buffer + 0x14);
      uint32_t nonce_ptr = xe::load_and_swap<uint32_t>(buffer + 0x18);

      XELOGD(
          "XGISessionCreateImpl({:08X}, {:08X}, {}, {}, {:08X}, {:08X}, "
          "{:08X})",
          session_ptr, flags, num_slots_public, num_slots_private, user_xuid,
          session_info_ptr, nonce_ptr);

      // 584107FB expects offline session creation using flags 0 to succeed
      // while offline.
      // 58410889 expects stats session creation failure while offline.
      //
      // Allow offline session creation, but do not allow Xbox Live featured
      // session creation.

      if (IsXboxLiveSession(static_cast<SessionFlags>(flags))) {
        return 0x80155209;  // X_ONLINE_E_SESSION_NOT_LOGGED_ON
      }

      bo2_local_session_active = true;
      bo2_local_session_flags = flags;
      bo2_local_public_slots = num_slots_public ? num_slots_public : 4;
      bo2_local_private_slots = num_slots_private;

      // BO2 already has profile 0 visible in the local frontend.
      // Treat it as the host member immediately.
      bo2_local_user_count = 1;
      bo2_local_user_index = 0;

      XELOGD("BO2 local session created: flags={:08X}, players={}",
             bo2_local_session_flags, bo2_local_user_count);

      return X_E_SUCCESS;
    }
    case 0x000B0011: {
      XELOGD("XGISessionDelete({:08X}, {:08X}), implemented in netplay",
             buffer_ptr, buffer_length);
      bo2_local_session_active = false;
      bo2_local_user_count = 0;
      return X_STATUS_SUCCESS;
    }
    case 0x000B0012: {
      assert_true(buffer_length == 0x14);
      uint32_t session_ptr = xe::load_and_swap<uint32_t>(buffer + 0x0);
      uint32_t user_count = xe::load_and_swap<uint32_t>(buffer + 0x4);
      uint32_t unk_0 = xe::load_and_swap<uint32_t>(buffer + 0x8);
      uint32_t user_index_array = xe::load_and_swap<uint32_t>(buffer + 0xC);
      uint32_t private_slots_array = xe::load_and_swap<uint32_t>(buffer + 0x10);

      assert_zero(unk_0);
      XELOGD("XGISessionJoinLocal({:08X}, {}, {}, {:08X}, {:08X})", session_ptr,
             user_count, unk_0, user_index_array, private_slots_array);

      bo2_local_session_active = true;

      if (user_count) {
        bo2_local_user_count = 1;

        if (user_index_array) {
          auto users =
              memory_->TranslateVirtual<xe::be<uint32_t>*>(user_index_array);
          bo2_local_user_index = users[0];
        } else {
          bo2_local_user_index = 0;
        }
      }

      XELOGD("BO2 JoinLocal: players={}, user={}",
             bo2_local_user_count, bo2_local_user_index);

      return X_E_SUCCESS;
    }
case 0x000B0013: {
  XELOGD("XGISessionLeaveLocal({:08X}, {:08X})",
         buffer_ptr, buffer_length);
  bo2_local_user_count = 0;
  return X_E_SUCCESS;
}
    case 0x000B0014: {
      // Gets 584107FB in game.
      // get high score table?
      XELOGD("XSessionStart({:08X}), implemented in netplay", buffer_ptr);
      return X_STATUS_SUCCESS;
    }
    case 0x000B0015: {
      // send high scores?
      XELOGD("XSessionEnd({:08X}, {:08X}), implemented in netplay", buffer_ptr,
             buffer_length);
      return X_STATUS_SUCCESS;
    }
    case 0x000B001D: {
      XELOGD("BO2 XSessionGetDetails");

      if (!buffer || buffer_length != 0x18) {
        return X_E_INVALIDARG;
      }

      uint32_t details_size_ptr =
          xe::load_and_swap<uint32_t>(buffer + 0x4);
      uint32_t details_ptr =
          xe::load_and_swap<uint32_t>(buffer + 0x8);

      if (!details_size_ptr || !details_ptr) {
        return X_E_INVALIDARG;
      }

      auto size_ptr =
          memory_->TranslateVirtual<xe::be<uint32_t>*>(details_size_ptr);

      uint32_t size = *size_ptr;

      uint32_t players =
          bo2_local_session_active ? bo2_local_user_count : 0;

      // XSESSION_LOCAL_DETAILS = 0x80 bytes.
      // Every returned XSESSION_MEMBER needs another 0x10 bytes.
      //
      // Do NOT return success with ActualMemberCount=1 while the caller only
      // supplied 0x80 bytes, because then SessionMembers_ptr cannot contain
      // that player. BO2 sees the lobby player visually, but considers the
      // playable session to contain zero valid members.
      uint32_t required_size = 0x80 + (players * 0x10);

      if (size < required_size) {
        XELOGD(
            "BO2 XSessionGetDetails buffer too small: have={:X}, need={:X}, "
            "players={}",
            size, required_size, players);

        *size_ptr = required_size;
        return X_ERROR_INSUFFICIENT_BUFFER;
      }

      uint8_t* d =
          memory_->TranslateVirtual<uint8_t*>(details_ptr);

      std::memset(d, 0, size);

      uint32_t public_slots =
          bo2_local_public_slots ? bo2_local_public_slots : 4;

      uint32_t available_public =
          public_slots > players ? public_slots - players : 0;

      // XSESSION_LOCAL_DETAILS
      xe::store_and_swap<uint32_t>(d + 0x00, bo2_local_user_index);
      xe::store_and_swap<uint32_t>(d + 0x0C, bo2_local_session_flags);
      xe::store_and_swap<uint32_t>(d + 0x10, public_slots);
      xe::store_and_swap<uint32_t>(d + 0x14, bo2_local_private_slots);
      xe::store_and_swap<uint32_t>(d + 0x18, available_public);
      xe::store_and_swap<uint32_t>(d + 0x1C, bo2_local_private_slots);
      xe::store_and_swap<uint32_t>(d + 0x20, players);

      // We already required enough room for every local member above.
      uint32_t returned_players = players;
      xe::store_and_swap<uint32_t>(d + 0x24, returned_players);

      xe::store_and_swap<uint32_t>(d + 0x28, 0);

      // Only append XSESSION_MEMBER when the caller actually supplied
      // enough room. This avoids the old 0x80 out-of-bounds problem.
      if (players && size >= 0x90) {
        uint8_t* member = d + 0x80;

        xe::store_and_swap<uint64_t>(
            member + 0x00, 0xE03000000BEE0DF5ULL);
        xe::store_and_swap<uint32_t>(
            member + 0x08, bo2_local_user_index);
        xe::store_and_swap<uint32_t>(member + 0x0C, 0);

        xe::store_and_swap<uint32_t>(
            d + 0x7C, details_ptr + 0x80);
      } else {
        xe::store_and_swap<uint32_t>(d + 0x7C, 0);
      }

      XELOGD("BO2 session details: players={}, size={}",
             players, size);

      return X_E_SUCCESS;
    }

    case 0x000B0021: {
      XELOGD("XUserReadStats");

      struct XUserReadStats {
        xe::be<uint32_t> titleId;
        xe::be<uint32_t> xuids_count;
        xe::be<uint32_t> xuids_guest_address;
        xe::be<uint32_t> specs_count;
        xe::be<uint32_t> specs_guest_address;
        xe::be<uint32_t> results_size;
        xe::be<uint32_t> results_guest_address;
      }* data = reinterpret_cast<XUserReadStats*>(buffer);

      return 0x80151802;  // X_ONLINE_E_LOGON_NOT_LOGGED_ON
    }
    case 0x000B0036: {
      // Called after opening xbox live arcade and clicking on xbox live v5759
      // to 5787 and called after clicking xbox live in the game library from
      // v6683 to v6717
      // Does not get sent a buffer
      XELOGD("XInvalidateGamerTileCache, unimplemented");
      return X_E_FAIL;
    }
    case 0x000B003D: {
      // Used in 5451082A, 5553081E
      // XUserGetCachedANID
      XELOGI("XUserGetANID({:08X}, {:08X}), implemented in netplay", buffer_ptr,
             buffer_length);
      return X_E_FAIL;
    }
    case 0x000B0041: {
      assert_true(!buffer_length ||
                  buffer_length == sizeof(XGI_XUSER_GET_PROPERTY));
      const XGI_XUSER_GET_PROPERTY* xgi_property =
          reinterpret_cast<const XGI_XUSER_GET_PROPERTY*>(buffer);

      UserProfile* user = nullptr;
      if (xgi_property->xuid != 0) {
        user = kernel_state_->xam_state()->GetUserProfile(xgi_property->xuid);
      } else {
        user = kernel_state_->xam_state()->GetUserProfile(
            xgi_property->user_index);
      }

      if (!user) {
        XELOGD(
            "XGIUserGetProperty - Invalid user provided: Index: {:08X} XUID: "
            "{:16X}",
            xgi_property->user_index.get(), xgi_property->xuid.get());
        return X_E_NOTFOUND;
      }

      // Process context
      if (xgi_property->context_address) {
        XUSER_CONTEXT* context = memory_->TranslateVirtual<XUSER_CONTEXT*>(
            xgi_property->context_address);

        XELOGD("XGIUserGetProperty - Context requested: {:08X} XUID: {:16X}",
               context->context_id.get(), user->xuid());

        auto context_value =
            kernel_state_->xam_state()->user_tracker()->GetUserContext(
                user->xuid(), context->context_id);

        if (!context_value) {
          return X_E_INVALIDARG;
        }

        context->value = context_value.value();
        return X_E_SUCCESS;
      }

      if (!xgi_property->property_size_ptr || !xgi_property->property_address) {
        return X_E_INVALIDARG;
      }

      // Process property
      XUSER_PROPERTY* property = memory_->TranslateVirtual<XUSER_PROPERTY*>(
          xgi_property->property_address);

      XELOGD("XGIUserGetProperty - Property requested: {:08X} XUID: {:16X}",
             property->property_id.get(), user->xuid());

      return kernel_state_->xam_state()->user_tracker()->GetProperty(
          user->xuid(),
          memory_->TranslateVirtual<uint32_t*>(xgi_property->property_size_ptr),
          property);
    }
    case 0x000B0071: {
      assert_true(!buffer_length ||
                  buffer_length == sizeof(XGI_AWARD_AVATAR_ASSETS));
      const XGI_AWARD_AVATAR_ASSETS* award_avatar_assets =
          reinterpret_cast<const XGI_AWARD_AVATAR_ASSETS*>(buffer);

      XELOGD("XUserAwardAvatarAssets({:08X}, {:08X})",
             award_avatar_assets->num_assets.get(),
             award_avatar_assets->assets_ptr.get());

      const X_USER_AVATAR_ASSET* avatar_assets =
          memory_->TranslateVirtual<X_USER_AVATAR_ASSET*>(
              award_avatar_assets->assets_ptr);

      for (uint32_t i = 0; i < award_avatar_assets->num_assets; i++) {
        const X_USER_AVATAR_ASSET& avatar_asset = avatar_assets[i];

        const auto user =
            kernel_state_->xam_state()->GetUserProfile(avatar_asset.user_index);

        if (user) {
          XELOGI("Player: {} Unlocked Avatar Award Asset ID: {}", user->name(),
                 avatar_asset.award_id.get());
        }
      }

      return X_E_SUCCESS;
    }
  }
  XELOGE(
      "Unimplemented XGI message app={:08X}, msg={:08X}, arg1={:08X}, "
      "arg2={:08X}",
      app_id(), message, buffer_ptr, buffer_length);
  return X_E_FAIL;
}

}  // namespace apps
}  // namespace xam
}  // namespace kernel
}  // namespace xe
