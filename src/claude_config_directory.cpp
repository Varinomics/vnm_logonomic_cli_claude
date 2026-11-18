#include "claude_native_context.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>

#ifdef _WIN32
#include <windows.h>
#elif defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#else
#include <uninorm.h>
#endif

namespace claude::native_context {

std::filesystem::path nfc_config_directory(const std::filesystem::path& path)
{
    const auto& text = path.native();
    if (std::all_of(text.begin(), text.end(), [](auto ch) { return (unsigned int)ch < 128; })) return path;
#ifdef _WIN32
    const int length = (int)text.size();
    int size = NormalizeString(NormalizationC, text.data(), length, nullptr, 0);
    while (size > 0) {
        std::wstring normalized(size, L'\0');
        const int count = NormalizeString(NormalizationC, text.data(), length, normalized.data(), size);
        if (count > 0) {
            normalized.resize(count);
            return std::filesystem::path(normalized);
        }
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || -count <= size) break;
        size = -count;
    }
#elif defined(__APPLE__)
    const auto input = CFStringCreateWithBytes(kCFAllocatorDefault, (const UInt8*)text.data(),
        (CFIndex)text.size(), kCFStringEncodingUTF8, false);
    if (input) {
        std::unique_ptr<std::remove_pointer_t<CFMutableStringRef>, decltype(&CFRelease)> normalized(
            CFStringCreateMutableCopy(kCFAllocatorDefault, 0, input), &CFRelease);
        CFRelease(input);
        if (normalized) {
            CFStringNormalize(normalized.get(), kCFStringNormalizationFormC);
            const auto length = CFStringGetLength(normalized.get());
            const auto size = CFStringGetMaximumSizeForEncoding(length, kCFStringEncodingUTF8);
            std::string bytes((std::size_t)size, '\0');
            CFIndex used = 0;
            const auto converted = CFStringGetBytes(normalized.get(), CFRangeMake(0, length),
                kCFStringEncodingUTF8, 0, false, (UInt8*)bytes.data(), size, &used);
            if (converted == length) {
                bytes.resize((std::size_t)used);
                return std::filesystem::path(bytes);
            }
        }
    }
#else
    std::size_t size = 0;
    std::unique_ptr<std::uint8_t, decltype(&std::free)> normalized(
        u8_normalize(UNINORM_NFC, (const std::uint8_t*)text.data(), text.size(), nullptr, &size), &std::free);
    if (normalized) return std::filesystem::path(std::string((const char*)normalized.get(), size));
#endif
    throw std::runtime_error("profile_normalization_failed");
}

} // namespace claude::native_context
