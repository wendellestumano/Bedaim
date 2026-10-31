#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <elf.h>
#include <link.h>
#include <limits>
#include <string_view>

namespace bedrocktools::core::memory {

inline constexpr std::array<std::uint8_t, 20> kMinecraft260BuildId{
    0xEB, 0x29, 0x1B, 0x88, 0x3D, 0xFD, 0xFA, 0xFF, 0x70, 0xC4,
    0x99, 0x2C, 0x9E, 0xAB, 0xD6, 0x18, 0x85, 0xAE, 0x3E, 0x15
};

inline bool matchesMinecraft260BuildId(const std::uint8_t* bytes, std::size_t size) noexcept {
    return bytes && size == kMinecraft260BuildId.size() &&
        std::memcmp(bytes, kMinecraft260BuildId.data(), kMinecraft260BuildId.size()) == 0;
}

namespace detail {
struct MinecraftBuildSearch {
    bool librarySeen = false;
    bool buildIdSeen = false;
    bool matched = false;
};

inline constexpr bool checkedAdd(std::uintptr_t left, std::uintptr_t right,
                                 std::uintptr_t& result) noexcept {
    if (right > std::numeric_limits<std::uintptr_t>::max() - left) return false;
    result = left + right;
    return true;
}

inline bool alignNoteSize(std::uint32_t value, std::size_t& aligned) noexcept {
    const auto raw = static_cast<std::size_t>(value);
    if (raw > std::numeric_limits<std::size_t>::max() - 3u) return false;
    aligned = (raw + 3u) & ~std::size_t(3u);
    return true;
}

inline int inspectMinecraftImage(struct dl_phdr_info* info, std::size_t, void* opaque) {
    if (!info || !info->dlpi_name) return 0;
    const std::string_view imageName(info->dlpi_name);
    const auto slash = imageName.find_last_of('/');
    const auto basename = slash == std::string_view::npos
        ? imageName : imageName.substr(slash + 1);
    if (basename != "libminecraftpe.so") return 0;

    auto& state = *static_cast<MinecraftBuildSearch*>(opaque);
    state.librarySeen = true;

    for (ElfW(Half) i = 0; i < info->dlpi_phnum; ++i) {
        const auto& noteSegment = info->dlpi_phdr[i];
        if (noteSegment.p_type != PT_NOTE || noteSegment.p_memsz < sizeof(ElfW(Nhdr)))
            continue;

        const auto noteBegin = static_cast<std::uintptr_t>(noteSegment.p_vaddr);
        const auto noteSize = static_cast<std::uintptr_t>(noteSegment.p_memsz);
        std::uintptr_t noteEnd = 0;
        if (!checkedAdd(noteBegin, noteSize, noteEnd)) continue;

        // PT_NOTE is read only when its complete range is covered by a readable PT_LOAD.
        bool mapped = false;
        for (ElfW(Half) j = 0; j < info->dlpi_phnum; ++j) {
            const auto& loadSegment = info->dlpi_phdr[j];
            if (loadSegment.p_type != PT_LOAD || (loadSegment.p_flags & PF_R) == 0) continue;
            const auto loadBegin = static_cast<std::uintptr_t>(loadSegment.p_vaddr);
            const auto loadSize = static_cast<std::uintptr_t>(loadSegment.p_memsz);
            std::uintptr_t loadEnd = 0;
            if (!checkedAdd(loadBegin, loadSize, loadEnd)) continue;
            if (noteBegin >= loadBegin && noteEnd <= loadEnd) {
                mapped = true;
                break;
            }
        }
        if (!mapped) continue;

        std::uintptr_t address = 0;
        if (!checkedAdd(static_cast<std::uintptr_t>(info->dlpi_addr), noteBegin, address))
            continue;
        const auto* cursor = reinterpret_cast<const std::uint8_t*>(address);
        std::size_t remaining = static_cast<std::size_t>(noteSize);

        while (remaining >= sizeof(ElfW(Nhdr))) {
            ElfW(Nhdr) note{};
            std::memcpy(&note, cursor, sizeof(note));
            cursor += sizeof(note);
            remaining -= sizeof(note);

            std::size_t nameBytes = 0;
            std::size_t descBytes = 0;
            if (!alignNoteSize(note.n_namesz, nameBytes) ||
                !alignNoteSize(note.n_descsz, descBytes)) break;
            if (nameBytes > remaining) break;
            const auto* name = cursor;
            cursor += nameBytes;
            remaining -= nameBytes;

            if (descBytes > remaining) break;
            const auto* description = cursor;
            cursor += descBytes;
            remaining -= descBytes;

            if (note.n_type != NT_GNU_BUILD_ID || note.n_namesz != 4u ||
                std::memcmp(name, "GNU\0", 4u) != 0) {
                continue;
            }

            state.buildIdSeen = true;
            if (matchesMinecraft260BuildId(description, static_cast<std::size_t>(note.n_descsz))) {
                state.matched = true;
                return 1;
            }
        }
    }
    return 0;
}
} // namespace detail

inline bool isExactMinecraft260Build() noexcept {
    static std::atomic<int> cached{-1};
    const int value = cached.load(std::memory_order_acquire);
    if (value >= 0) return value == 1;

    detail::MinecraftBuildSearch search{};
    dl_iterate_phdr(&detail::inspectMinecraftImage, &search);

    // Do not cache "not loaded yet"; cache only after a GNU build ID was observed.
    if (search.buildIdSeen)
        cached.store(search.matched ? 1 : 0, std::memory_order_release);
    return search.matched;
}

} // namespace bedrocktools::core::memory
