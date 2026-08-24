#import "HFHostHandleTable.h"

#import <Foundation/Foundation.h>

#include <cstdint>
#include <limits>
#include <mutex>
#include <vector>

struct HFHostHandleLease {
    __strong id object;
    void *value;
};

namespace {

bool isObjectKind(HFHandleKind kind) {
    return kind == HFHandleKindObject || kind == HFHandleKindClass ||
           kind == HFHandleKindBlock;
}

bool validKind(HFHandleKind kind) {
    return isObjectKind(kind) || kind == HFHandleKindSelector ||
           kind == HFHandleKindNativeSymbol;
}

bool validOwnership(HFHostHandleOwnership ownership) {
    return ownership == HFHostHandleOwnershipBorrowed ||
           ownership == HFHostHandleOwnershipStrong ||
           ownership == HFHostHandleOwnershipWeak;
}

struct Slot {
    std::uint32_t generation = 0;
    HFHandleKind kind = HFHandleKindInvalid;
    HFHandleFlags flags = HFHandleFlagNone;
    bool active = false;
    void *rawValue = nullptr;
    __strong id strongObject = nil;
    __weak id weakObject = nil;
};

class HandleTable {
public:
    static HandleTable &shared() {
        static HandleTable table;
        return table;
    }

    HFStatus registerValue(void *value, HFHandleKind kind,
                           HFHostHandleOwnership ownership,
                           HFHandle &handle) {
        handle = HFInvalidHandle();
        if (value == nullptr || !validKind(kind) || !validOwnership(ownership))
            return HFStatusInvalidArguments;
        if (ownership == HFHostHandleOwnershipWeak && !isObjectKind(kind))
            return HFStatusInvalidArguments;

        std::lock_guard lock(mutex_);
        std::size_t index = 0;
        if (!freeSlots_.empty()) {
            index = freeSlots_.back();
            freeSlots_.pop_back();
        } else {
            if (slots_.size() >= std::numeric_limits<std::uint32_t>::max())
                return HFStatusHandleTableFull;
            index = slots_.size();
            slots_.emplace_back();
        }

        Slot &slot = slots_[index];
        ++slot.generation;
        if (slot.generation == 0)
            ++slot.generation;
        slot.kind = kind;
        slot.flags = static_cast<HFHandleFlags>(ownership);
        slot.active = true;
        slot.rawValue = nullptr;
        slot.strongObject = nil;
        slot.weakObject = nil;

        if (isObjectKind(kind)) {
            id object = (__bridge id)value;
            if (ownership == HFHostHandleOwnershipStrong)
                slot.strongObject = object;
            else if (ownership == HFHostHandleOwnershipWeak)
                slot.weakObject = object;
            else
                slot.rawValue = value;
        } else {
            slot.rawValue = value;
        }

        handle.token = static_cast<std::uint64_t>(index) + 1;
        handle.generation = slot.generation;
        handle.kind = slot.kind;
        handle.flags = slot.flags;
        ++liveCount_;
        return HFStatusApplied;
    }

    HFStatus validate(HFHandle handle) {
        std::lock_guard lock(mutex_);
        const Slot *slot = findLocked(handle);
        if (slot == nullptr)
            return HFStatusStaleHandle;
        if (slot->flags == HFHandleFlagWeak && slot->weakObject == nil)
            return HFStatusStaleHandle;
        return HFStatusApplied;
    }

    HFStatus resolve(HFHandle handle, HFHostHandleLease *&lease,
                     void *&value) {
        lease = nullptr;
        value = nullptr;
        std::lock_guard lock(mutex_);
        const Slot *slot = findLocked(handle);
        if (slot == nullptr)
            return HFStatusStaleHandle;

        id object = nil;
        if (isObjectKind(slot->kind)) {
            if (slot->flags == HFHandleFlagRetained)
                object = slot->strongObject;
            else if (slot->flags == HFHandleFlagWeak)
                object = slot->weakObject;
            else if (slot->rawValue != nullptr)
                object = (__bridge id)slot->rawValue;
            if (object == nil)
                return HFStatusStaleHandle;
            value = (__bridge void *)object;
        } else {
            value = slot->rawValue;
            if (value == nullptr)
                return HFStatusStaleHandle;
        }

        lease = new HFHostHandleLease{object, value};
        return HFStatusApplied;
    }

    HFStatus release(HFHandle handle) {
        std::lock_guard lock(mutex_);
        Slot *slot = findLocked(handle);
        if (slot == nullptr)
            return HFStatusStaleHandle;
        const std::size_t index = static_cast<std::size_t>(handle.token - 1);
        slot->active = false;
        slot->kind = HFHandleKindInvalid;
        slot->flags = HFHandleFlagNone;
        slot->rawValue = nullptr;
        slot->strongObject = nil;
        slot->weakObject = nil;
        freeSlots_.push_back(index);
        --liveCount_;
        return HFStatusApplied;
    }

    std::uint64_t liveCount() {
        std::lock_guard lock(mutex_);
        return liveCount_;
    }

private:
    const Slot *findLocked(HFHandle handle) const {
        if (handle.token == 0 || handle.generation == 0 ||
            !validKind(handle.kind) ||
            handle.token > static_cast<std::uint64_t>(slots_.size()))
            return nullptr;
        const Slot &slot = slots_[static_cast<std::size_t>(handle.token - 1)];
        if (!slot.active || slot.generation != handle.generation ||
            slot.kind != handle.kind || slot.flags != handle.flags)
            return nullptr;
        return &slot;
    }

    Slot *findLocked(HFHandle handle) {
        return const_cast<Slot *>(
            static_cast<const HandleTable *>(this)->findLocked(handle));
    }

    std::mutex mutex_;
    std::vector<Slot> slots_;
    std::vector<std::size_t> freeSlots_;
    std::uint64_t liveCount_ = 0;
};

} // namespace

HFStatus hf_host_handle_register(void *value, HFHandleKind kind,
                                 HFHostHandleOwnership ownership,
                                 HFHandle *handle) {
    if (handle == nullptr)
        return HFStatusInvalidArguments;
    return HandleTable::shared().registerValue(value, kind, ownership, *handle);
}

HFStatus hf_host_handle_scope_begin(void *value, HFHandleKind kind,
                                    HFHandle *handle) {
    return hf_host_handle_register(value, kind, HFHostHandleOwnershipBorrowed,
                                   handle);
}

HFStatus hf_host_handle_scope_end(HFHandle handle) {
    return hf_host_handle_release(handle);
}

HFStatus hf_host_handle_scope_end_ref(const HFHandle *handle) {
    return handle == nullptr ? HFStatusInvalidArguments
                             : hf_host_handle_release(*handle);
}

HFStatus hf_host_handle_validate(HFHandle handle) {
    return HandleTable::shared().validate(handle);
}

HFStatus hf_host_handle_resolve(HFHandle handle, HFHostHandleLease **lease,
                                void **value) {
    if (lease == nullptr || value == nullptr)
        return HFStatusInvalidArguments;
    return HandleTable::shared().resolve(handle, *lease, *value);
}

void hf_host_handle_lease_release(HFHostHandleLease *lease) {
    delete lease;
}

HFStatus hf_host_handle_retain(HFHandle handle, HFHandle *retainedHandle) {
    if (retainedHandle == nullptr)
        return HFStatusInvalidArguments;
    HFHostHandleLease *lease = nullptr;
    void *value = nullptr;
    const HFStatus status = hf_host_handle_resolve(handle, &lease, &value);
    if (status != HFStatusApplied)
        return status;
    const HFStatus registerStatus = hf_host_handle_register(
        value, handle.kind, HFHostHandleOwnershipStrong, retainedHandle);
    hf_host_handle_lease_release(lease);
    return registerStatus;
}

HFStatus hf_host_handle_release(HFHandle handle) {
    return HandleTable::shared().release(handle);
}

uint64_t hf_host_handle_live_count(void) {
    return HandleTable::shared().liveCount();
}
