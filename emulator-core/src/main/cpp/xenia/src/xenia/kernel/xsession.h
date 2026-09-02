/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XSESSION_H_
#define XENIA_KERNEL_XSESSION_H_

#include "xenia/base/byte_order.h"
#include "xenia/kernel/xobject.h"

namespace xe {
namespace kernel {

enum SessionFlags {
  HOST = 0x01,
  PRESENCE = 0x02,
  STATS = 0x04,
  MATCHMAKING = 0x08,
  ARBITRATION = 0x10,
  PEER_NETWORK = 0x20,
  SOCIAL_MATCHMAKING_ALLOWED = 0x80,
  INVITES_DISABLED = 0x0100,
  JOIN_VIA_PRESENCE_DISABLED = 0x0200,
  JOIN_IN_PROGRESS_DISABLED = 0x0400,
  JOIN_VIA_PRESENCE_FRIENDS_ONLY = 0x0800,
  UNKNOWN = 0x1000,  // 4156091D and 5841128F sets this flag?

  SINGLEPLAYER_WITH_STATS = PRESENCE | STATS | INVITES_DISABLED |
                            JOIN_VIA_PRESENCE_DISABLED |
                            JOIN_IN_PROGRESS_DISABLED,

  LIVE_MULTIPLAYER_STANDARD = PRESENCE | STATS | MATCHMAKING | PEER_NETWORK,
  LIVE_MULTIPLAYER_RANKED = LIVE_MULTIPLAYER_STANDARD | ARBITRATION,
  SYSTEMLINK = PEER_NETWORK,
  GROUP_LOBBY = PRESENCE | PEER_NETWORK,
  GROUP_GAME = STATS | MATCHMAKING | PEER_NETWORK,

  // HELPERS
  SYSTEMLINK_FEATURES = HOST | SYSTEMLINK,
  LIVE_FEATURES = PRESENCE | STATS | MATCHMAKING | ARBITRATION
};

inline bool IsOfflineSession(const SessionFlags flags) { return !flags; }

inline bool IsXboxLiveSession(const SessionFlags flags) {
  return !IsOfflineSession(flags) && flags & SessionFlags::LIVE_FEATURES;
}


// Minimal real XSession object used by local/offline sessions.
//
// XObject::SetNativePointer currently stores its internal object signature
// in the first 0x10 bytes of the guest object, therefore allocate 0x10
// instead of the 4-byte X_KSESSION used by newer desktop Xenia builds.
// The first dword still contains the session handle expected by XAM.
struct X_KSESSION_MINI {
  xe::be<uint32_t> handle;
  uint8_t reserved[0x0C];
};
static_assert_size(X_KSESSION_MINI, 0x10);

class XSession : public XObject {
 public:
  static const XObject::Type kObjectType = XObject::Type::Session;

  explicit XSession(KernelState* kernel_state)
      : XObject(kernel_state, kObjectType) {}

  X_STATUS Initialize() {
    auto* guest = CreateNative<X_KSESSION_MINI>();
    if (!guest) {
      return X_STATUS_NO_MEMORY;
    }
    guest->handle = handle();
    return X_STATUS_SUCCESS;
  }

  void CreateLocal(uint32_t user_index, uint32_t flags,
                   uint32_t public_slots, uint32_t private_slots) {
    created_ = true;
    started_ = false;
    user_index_ = user_index;
    flags_ = flags;
    public_slots_ = public_slots ? public_slots : 4;
    private_slots_ = private_slots;
    member_count_ = 0;
    member_xuid_ = 0;
  }

  void DeleteLocal() {
    created_ = false;
    started_ = false;
    member_count_ = 0;
    member_xuid_ = 0;
  }

  void JoinLocal(uint32_t user_index, uint64_t xuid) {
    user_index_ = user_index;
    member_xuid_ = xuid;
    member_count_ = 1;
  }

  void LeaveLocal() {
    member_count_ = 0;
    member_xuid_ = 0;
  }

  void StartLocal() { started_ = true; }
  void EndLocal() { started_ = false; }

  bool created() const { return created_; }
  bool started() const { return started_; }

  uint32_t user_index() const { return user_index_; }
  uint32_t flags() const { return flags_; }
  uint32_t public_slots() const { return public_slots_; }
  uint32_t private_slots() const { return private_slots_; }
  uint32_t member_count() const { return member_count_; }
  uint64_t member_xuid() const { return member_xuid_; }

 private:
  bool created_ = false;
  bool started_ = false;

  uint32_t user_index_ = 0;
  uint32_t flags_ = 0;
  uint32_t public_slots_ = 4;
  uint32_t private_slots_ = 0;

  uint32_t member_count_ = 0;
  uint64_t member_xuid_ = 0;
};

}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XSESSION_H_
