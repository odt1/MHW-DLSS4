#include "memory.h"

#include <charconv>

namespace mhwdlss::mem {

std::optional<Section> FindSection(HMODULE module, std::string_view name) {
    auto* base = reinterpret_cast<uint8_t*>(module);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    auto* sec = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        char secName[9] = {};
        std::memcpy(secName, sec->Name, 8);
        if (name == secName) {
            Section s;
            s.begin = base + sec->VirtualAddress;
            s.end = s.begin + sec->Misc.VirtualSize;
            return s;
        }
    }
    return std::nullopt;
}

std::vector<uint8_t*> FindPattern(const Section& range, std::string_view pattern) {
    std::vector<int> bytes;  // -1 = wildcard
    for (size_t i = 0; i < pattern.size();) {
        if (pattern[i] == ' ') {
            ++i;
            continue;
        }
        if (pattern[i] == '?') {
            bytes.push_back(-1);
            while (i < pattern.size() && pattern[i] == '?') ++i;
            continue;
        }
        int value = 0;
        std::from_chars(pattern.data() + i, pattern.data() + i + 2, value, 16);
        bytes.push_back(value);
        i += 2;
    }

    std::vector<uint8_t*> results;
    if (bytes.empty() || range.size() < bytes.size()) return results;

    // Anchor on the first non-wildcard byte so memchr does most of the work.
    size_t anchor = 0;
    while (anchor < bytes.size() && bytes[anchor] < 0) ++anchor;
    if (anchor == bytes.size()) return results;

    uint8_t* last = range.end - bytes.size();
    uint8_t* p = range.begin;
    while (p <= last) {
        auto* hit = static_cast<uint8_t*>(
            std::memchr(p + anchor, bytes[anchor], static_cast<size_t>(last - p) + 1));
        if (!hit) break;
        uint8_t* start = hit - anchor;
        bool ok = true;
        for (size_t j = 0; j < bytes.size(); ++j) {
            if (bytes[j] >= 0 && start[j] != static_cast<uint8_t>(bytes[j])) {
                ok = false;
                break;
            }
        }
        if (ok) results.push_back(start);
        p = start + 1;
    }
    return results;
}

void* WritePointer(void** slot, void* value) {
    DWORD oldProtect;
    VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &oldProtect);
    void* old = InterlockedExchangePointer(slot, value);
    DWORD dummy;
    VirtualProtect(slot, sizeof(void*), oldProtect, &dummy);
    return old;
}

bool HookSlot(void** slot, void* hook, void** original) {
    for (int attempt = 0; attempt < 16; ++attempt) {
        auto current = SafeRead<void*>(slot);
        if (!current || !*current || *current == hook) return false;
        *original = *current;
        DWORD oldProtect;
        if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &oldProtect)) return false;
        void* seen = InterlockedCompareExchangePointer(slot, hook, *current);
        DWORD dummy;
        VirtualProtect(slot, sizeof(void*), oldProtect, &dummy);
        if (seen == *current) return true;
    }
    return false;
}

bool SafeRead(const void* src, void* dst, size_t size) {
    __try {
        std::memcpy(dst, src, size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

}  // namespace mhwdlss::mem
