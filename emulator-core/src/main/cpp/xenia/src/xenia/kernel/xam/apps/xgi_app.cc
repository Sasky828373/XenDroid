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
      XELOGI("[BO2SESSION] Create");

      if (!buffer) return X_ERROR_INVALID_PARAMETER;

      auto read32 = [&](uint32_t o) {
        return static_cast<uint32_t>(
            *reinterpret_cast<xe::be<uint32_t>*>(buffer + o));
      };

      uint32_t obj = read32(0x00);
      uint32_t flags = read32(0x04);
      uint32_t pub = read32(0x08);
      uint32_t priv = read32(0x0C);
      uint32_t user = read32(0x10);

      auto host_obj =
          memory_->TranslateVirtual<uint8_t*>(obj);

      auto session =
          XObject::GetNativeObject<XSession>(
              kernel_state(), host_obj);

      if (!session) {
        XELOGE("[BO2SESSION] Create invalid obj {:08X}", obj);
        return X_STATUS_INVALID_HANDLE;
      }

      if (IsXboxLiveSession(
              static_cast<SessionFlags>(flags))) {
        XELOGW("[BO2SESSION] Xbox Live session rejected");
        return X_ERROR_FUNCTION_FAILED;
      }

      session->CreateLocal(user, flags, pub, priv);

      XELOGI(
          "[BO2SESSION] Create OK user={} pub={} priv={} flags={:08X}",
          user, pub, priv, flags);

      return X_ERROR_SUCCESS;
    }

    case 0x000B0011: {
      XELOGI("[BO2SESSION] Delete");

      uint32_t obj =
          static_cast<uint32_t>(
              *reinterpret_cast<xe::be<uint32_t>*>(buffer));

      auto host_obj =
          memory_->TranslateVirtual<uint8_t*>(obj);

      auto session =
          XObject::GetNativeObject<XSession>(
              kernel_state(), host_obj);

      if (!session)
        return X_STATUS_INVALID_HANDLE;

      session->DeleteLocal();

      return X_ERROR_SUCCESS;
    }

    case 0x000B0012: {
      XELOGI("[BO2SESSION] JoinLocal");

      auto read32 = [&](uint32_t o) {
        return static_cast<uint32_t>(
            *reinterpret_cast<xe::be<uint32_t>*>(buffer + o));
      };

      uint32_t obj = read32(0x00);
      uint32_t count = read32(0x04);

      // XGI_SESSION_MANAGE layout:
      // +00 obj
      // +04 user count
      // +08 user indices array
      // +0C private slot flags array
      // +10 XUID array (zero for JoinLocal)
      uint32_t indices_ptr = read32(0x08);
      uint32_t xuid_ptr = read32(0x10);

      auto host_obj =
          memory_->TranslateVirtual<uint8_t*>(obj);

      auto session =
          XObject::GetNativeObject<XSession>(
              kernel_state(), host_obj);

      if (!session) {
        XELOGE("[BO2SESSION] Join invalid obj");
        return X_STATUS_INVALID_HANDLE;
      }

      if (!count) {
        XELOGW("[BO2SESSION] Join count=0");
        return X_ERROR_SUCCESS;
      }

      if (xuid_ptr) {
        XELOGW(
            "[BO2SESSION] JoinLocal received XUID array {:08X}",
            xuid_ptr);
      }

      uint32_t user = 0;

      if (indices_ptr) {
        auto indices =
            memory_->TranslateVirtual<
                xe::be<uint32_t>*>(indices_ptr);

        if (indices)
          user = static_cast<uint32_t>(indices[0]);
      }

      if (user >= XUserMaxUserCount ||
          !kernel_state()
               ->xam_state()
               ->IsUserSignedIn(user)) {
        XELOGE(
            "[BO2SESSION] user {} not signed in",
            user);

        return X_ONLINE_E_SESSION_NOT_LOGGED_ON;
      }

      auto profile =
          kernel_state()
              ->xam_state()
              ->GetUserProfile(user);

      if (!profile)
        return X_ERROR_NO_SUCH_USER;

      uint64_t xuid = profile->xuid();

      session->JoinLocal(user, xuid);

      XELOGI(
          "[BO2SESSION] Join OK user={} xuid={:016X} members={}",
          user, xuid, session->member_count());

      return X_ERROR_SUCCESS;
    }

case 0x000B0013: {
      XELOGI("[BO2SESSION] LeaveLocal");

      uint32_t obj =
          static_cast<uint32_t>(
              *reinterpret_cast<xe::be<uint32_t>*>(buffer));

      auto session =
          XObject::GetNativeObject<XSession>(
              kernel_state(),
              memory_->TranslateVirtual<uint8_t*>(obj));

      if (!session)
        return X_STATUS_INVALID_HANDLE;

      session->LeaveLocal();

      return X_ERROR_SUCCESS;
    }

    case 0x000B0014: {
      XELOGI("[BO2SESSION] Start");

      uint32_t obj =
          static_cast<uint32_t>(
              *reinterpret_cast<xe::be<uint32_t>*>(buffer));

      auto session =
          XObject::GetNativeObject<XSession>(
              kernel_state(),
              memory_->TranslateVirtual<uint8_t*>(obj));

      if (!session)
        return X_STATUS_INVALID_HANDLE;

      XELOGI(
          "[BO2SESSION] Start members={}",
          session->member_count());

      session->StartLocal();

      return X_ERROR_SUCCESS;
    }

    case 0x000B0015: {
      XELOGI("[BO2SESSION] End");

      uint32_t obj =
          static_cast<uint32_t>(
              *reinterpret_cast<xe::be<uint32_t>*>(buffer));

      auto session =
          XObject::GetNativeObject<XSession>(
              kernel_state(),
              memory_->TranslateVirtual<uint8_t*>(obj));

      if (!session)
        return X_STATUS_INVALID_HANDLE;

      session->EndLocal();

      return X_ERROR_SUCCESS;
    }

    case 0x000B001D: {
      XELOGI("[BO2SESSION] GetDetails");

      auto read32 = [&](uint32_t o) {
        return static_cast<uint32_t>(
            *reinterpret_cast<xe::be<uint32_t>*>(buffer + o));
      };

      uint32_t obj = read32(0x00);
      uint32_t size_guest = read32(0x04);
      uint32_t details_guest = read32(0x08);

      auto size_ptr =
          memory_->TranslateVirtual<
              xe::be<uint32_t>*>(size_guest);

      if (!size_ptr)
        return X_ERROR_INVALID_PARAMETER;

      auto session =
          XObject::GetNativeObject<XSession>(
              kernel_state(),
              memory_->TranslateVirtual<uint8_t*>(obj));

      if (!session) {
        XELOGE("[BO2SESSION] GetDetails invalid session");
        return X_STATUS_INVALID_HANDLE;
      }

      uint32_t players =
          session->member_count();

      uint32_t size =
          static_cast<uint32_t>(*size_ptr);

      uint32_t required =
          0x80 + players * 0x10;

      if (size < required) {
        *size_ptr = required;

        XELOGI(
            "[BO2SESSION] resize {:X}->{:X}",
            size, required);

        return X_ERROR_INSUFFICIENT_BUFFER;
      }

      auto d =
          memory_->TranslateVirtual<uint8_t*>(
              details_guest);

      if (!d)
        return X_ERROR_INVALID_PARAMETER;

      std::memset(d, 0, size);

      uint32_t pub =
          session->public_slots()
              ? session->public_slots()
              : 4;

      uint32_t available =
          pub > players
              ? pub - players
              : 0;

      // XSESSION_LOCAL_DETAILS
      xe::store_and_swap<uint32_t>(
          d + 0x00, session->user_index());

      xe::store_and_swap<uint32_t>(
          d + 0x0C, session->flags());

      xe::store_and_swap<uint32_t>(
          d + 0x10, pub);

      xe::store_and_swap<uint32_t>(
          d + 0x14, session->private_slots());

      xe::store_and_swap<uint32_t>(
          d + 0x18, available);

      xe::store_and_swap<uint32_t>(
          d + 0x1C, session->private_slots());

      xe::store_and_swap<uint32_t>(
          d + 0x20, players);

      xe::store_and_swap<uint32_t>(
          d + 0x24, players);

      xe::store_and_swap<uint32_t>(
          d + 0x28,
          session->started() ? 1u : 0u);

      if (players) {
        auto member = d + 0x80;

        xe::store_and_swap<uint64_t>(
            member + 0x00,
            session->member_xuid());

        xe::store_and_swap<uint32_t>(
            member + 0x08,
            session->user_index());

        xe::store_and_swap<uint32_t>(
            member + 0x0C, 0);

        uint32_t member_guest =
            memory_->HostToGuestVirtual(member);

        xe::store_and_swap<uint32_t>(
            d + 0x7C, member_guest);

        XELOGI(
            "[BO2SESSION] details members={} xuid={:016X} ptr={:08X}",
            players,
            session->member_xuid(),
            member_guest);
      } else {
        xe::store_and_swap<uint32_t>(
            d + 0x7C, 0);

        XELOGI(
            "[BO2SESSION] details members=0");
      }

      return X_ERROR_SUCCESS;
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
