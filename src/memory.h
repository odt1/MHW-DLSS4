#pragma once

#include <windows.h>

#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace mhwdlss::mem {

struct Section {
    uint8_t* begin = nullptr;
    uint8_t* end = nullptr;
    bool Contains(const void* p) const { return p >= begin && p < end; }
    size_t size() const { return static_cast<size_t>(end - begin); }
};

// Section of a loaded image by name (".text", ".rdata", ...).
std::optional<Section> FindSection(HMODULE module, std::string_view name);

// IDA-style pattern, "48 8B 05 ?? ?? ?? ??". Returns every match inside the range.
std::vector<uint8_t*> FindPattern(const Section& range, std::string_view pattern);

// Target of a RIP-relative operand whose 32-bit displacement sits at `disp` and whose
// instruction ends at `insnEnd`.
inline uint8_t* RipTarget(const uint8_t* disp, const uint8_t* insnEnd) {
    int32_t d;
    std::memcpy(&d, disp, sizeof(d));
    return const_cast<uint8_t*>(insnEnd) + d;
}

// Writes a pointer-sized value into memory that may be read-only. Returns the old value.
void* WritePointer(void** slot, void* value);

// Points a function pointer slot (vtable entry, import) at `hook`, chaining to what it held: that
// value is stored in `original` before the slot changes, so a call through the slot right after
// finds it. The swap only happens if the slot still holds that value; when another plugin changes
// the slot meanwhile, it is read again. False when the slot is empty, already hooked or never
// held still. Same scheme as mem::HookPointer in the user's other MHW plugins.
bool HookSlot(void** slot, void* hook, void** original);

// Reads memory without faulting; false if the range is not readable.
bool SafeRead(const void* src, void* dst, size_t size);

template <typename T>
std::optional<T> SafeRead(const void* src) {
    T value;
    if (!SafeRead(src, &value, sizeof(T))) return std::nullopt;
    return value;
}

}  // namespace mhwdlss::mem
