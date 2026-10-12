#pragma once
#include "midix/abi/protocols/usb_controller.h"
#include "midix/abi/status.h"

namespace mochi::usb {

enum class CacheMode : mx_u8_t { Coherent = 0, NonCacheable = 1, NonCoherent = 2 };
enum class RoleState : mx_u8_t { Detached, Quiescing, Preparing, Active, Fault };

struct PlatformResources {
    mx_handle_t mmio;
    mx_handle_t irq;
    mx_handle_t dma_arena;
    mx_u64_t mmio_bytes;
    mx_u64_t dma_address_limit;
    mx_u64_t generation;
    mx_u32_t features;
    mx_u32_t vbus_budget_ma;
    CacheMode cache_mode;
    bool dma_isolated;
};

// Hardware completion gates each transition. request() alone never enables a
// new role or releases DMA memory. An active port owns exactly one role.
class RoleLifecycle {
public:
    mx_status_t request(MxUsbRole next, mx_u32_t features) {
        if (next > MxUsbRole::Device) return MX_ERR_INVALID;
        if ((next == MxUsbRole::Host && !(features & MX_USB_FEATURE_HOST)) ||
            (next == MxUsbRole::Device && !(features & MX_USB_FEATURE_DEVICE))) return MX_ERR_NOT_IMPLEMENTED;
        if (state_ == RoleState::Quiescing || state_ == RoleState::Preparing || state_ == RoleState::Fault)
            return MX_ERR_BUSY;
        if (next == role_) return MX_OK;
        desired_ = next;
        state_ = role_ == MxUsbRole::Detached ? RoleState::Preparing : RoleState::Quiescing;
        return MX_OK;
    }
    mx_status_t quiesced() {
        if (state_ != RoleState::Quiescing) return MX_ERR_INVALID;
        role_ = MxUsbRole::Detached;
        state_ = RoleState::Preparing;
        return MX_OK;
    }
    mx_status_t prepared() {
        if (state_ != RoleState::Preparing) return MX_ERR_INVALID;
        if (generation_ == ~mx_u64_t{0}) { fault(); return MX_ERR_OVERFLOW; }
        ++generation_;
        role_ = desired_;
        state_ = role_ == MxUsbRole::Detached ? RoleState::Detached : RoleState::Active;
        return MX_OK;
    }
    void fault() { state_ = RoleState::Fault; }
    // Recovery may proceed only after a supervisor proves the old engine stopped.
    mx_status_t recover(bool hardware_stopped) {
        if (state_ != RoleState::Fault || !hardware_stopped) return MX_ERR_BUSY;
        role_ = desired_ = MxUsbRole::Detached;
        state_ = RoleState::Detached;
        return MX_OK;
    }
    RoleState state() const { return state_; }
    MxUsbRole role() const { return role_; }
    mx_u64_t generation() const { return generation_; }
private:
    MxUsbRole role_ = MxUsbRole::Detached;
    MxUsbRole desired_ = MxUsbRole::Detached;
    RoleState state_ = RoleState::Detached;
    mx_u64_t generation_ = 1;
};

} // namespace mochi::usb
