#include "animbridge/path_identity.hpp"
#include "animbridge/core.hpp"
#include <utf8proc.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <memory>
#include <sstream>
#include <stdexcept>
#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace animbridge {
bool validUtf8(std::string_view value) noexcept {
    auto p = reinterpret_cast<const utf8proc_uint8_t*>(value.data());
    std::size_t index = 0;
    while (index < value.size()) {
        utf8proc_int32_t cp = 0;
        auto count = utf8proc_iterate(p + index, static_cast<utf8proc_ssize_t>(value.size() - index), &cp);
        if (count <= 0 || cp == 0) return false;
        index += static_cast<std::size_t>(count);
    }
    return true;
}
void requireUtf8(std::string_view value) {
    if (!validUtf8(value)) throw std::runtime_error("INVALID_ENCODING: invalid UTF-8 or embedded NUL (entry omitted)");
}
std::string portableNameKey(std::string_view value) {
    requireUtf8(value);
    auto map = [](std::string_view input, utf8proc_option_t options) {
        utf8proc_uint8_t* output = nullptr;
        auto count = utf8proc_map(reinterpret_cast<const utf8proc_uint8_t*>(input.data()),
            static_cast<utf8proc_ssize_t>(input.size()), &output, options);
        std::unique_ptr<utf8proc_uint8_t, decltype(&std::free)> owner(output, &std::free);
        if (count < 0) throw std::runtime_error("INVALID_ENCODING: Unicode normalization failed");
        return std::string(reinterpret_cast<const char*>(output), static_cast<std::size_t>(count));
    };
    auto composed = map(value, static_cast<utf8proc_option_t>(UTF8PROC_STABLE | UTF8PROC_COMPOSE));
    return map(composed, static_cast<utf8proc_option_t>(UTF8PROC_STABLE | UTF8PROC_CASEFOLD | UTF8PROC_COMPOSE));
}
namespace {
#ifdef _WIN32
struct NativeFile {
    HANDLE value = INVALID_HANDLE_VALUE;
    explicit NativeFile(const fs::path& path) {
        value = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (value == INVALID_HANDLE_VALUE) throw std::runtime_error("FILE_IDENTITY_FAILED: cannot open input identity");
        BY_HANDLE_FILE_INFORMATION info{};
        if (!GetFileInformationByHandle(value, &info) || (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
            CloseHandle(value); value = INVALID_HANDLE_VALUE; throw std::runtime_error("FILE_IDENTITY_FAILED: linked input is not permitted");
        }
    }
    ~NativeFile() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
#else
struct NativeFile {
    int value = -1;
    explicit NativeFile(const fs::path& path) {
        value = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
        if (value < 0) throw std::runtime_error("FILE_IDENTITY_FAILED: cannot open input identity");
    }
    ~NativeFile() { if (value >= 0) close(value); }
};
#endif
std::string pathText(const fs::path& path) { auto bytes = path.u8string(); std::string value(bytes.begin(), bytes.end()); requireUtf8(value); return value; }
void appendField(std::string& output, std::string_view value) {
    requireUtf8(value);
    auto length = static_cast<std::uint64_t>(value.size());
    for (int shift = 56; shift >= 0; shift -= 8) output.push_back(static_cast<char>((length >> shift) & 255));
    output.append(value);
}
std::string identityOf(const NativeFile& file) {
    std::ostringstream out; out << std::hex;
#ifdef _WIN32
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(file.value, &info)) throw std::runtime_error("FILE_IDENTITY_FAILED: cannot query input identity");
    out << info.dwVolumeSerialNumber << ':' << info.nFileIndexHigh << ':' << info.nFileIndexLow;
#else
    struct stat info{};
    if (fstat(file.value, &info)) throw std::runtime_error("FILE_IDENTITY_FAILED: cannot query input identity");
    out << static_cast<std::uint64_t>(info.st_dev) << ':' << static_cast<std::uint64_t>(info.st_ino);
#endif
    return out.str();
}
}
std::string fileIdentity(const fs::path& path) {
    pathText(path); NativeFile file(path); return identityOf(file);
}
fs::path verifiedCanonicalPath(const fs::path& path) {
    pathText(path);
    NativeFile file(path);
    fs::path result;
#ifdef _WIN32
    std::wstring text(32768, L'\0');
    auto count = GetFinalPathNameByHandleW(file.value, text.data(), static_cast<DWORD>(text.size()), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (!count || count >= text.size()) throw std::runtime_error("FILE_IDENTITY_FAILED: cannot obtain canonical input path");
    text.resize(count);
    if (text.starts_with(L"\\\\?\\UNC\\")) text = L"\\\\" + text.substr(8);
    else if (text.starts_with(L"\\\\?\\")) text.erase(0, 4);
    result = text;
#elif defined(__APPLE__)
    std::array<char, 4096> text{};
    if (fcntl(file.value, F_GETPATH, text.data()) < 0) throw std::runtime_error("FILE_IDENTITY_FAILED: cannot obtain canonical input path");
    result = text.data();
#else
    std::array<char, 65536> text{};
    auto descriptor = "/proc/self/fd/" + std::to_string(file.value);
    auto count = readlink(descriptor.c_str(), text.data(), text.size());
    if (count <= 0 || static_cast<std::size_t>(count) >= text.size()) throw std::runtime_error("FILE_IDENTITY_FAILED: cannot obtain canonical input path");
    result = std::string(text.data(), static_cast<std::size_t>(count));
#endif
    pathText(result);
    // Re-open after obtaining spelling to reject replacement during resolution.
    auto openedIdentity = identityOf(file);
    if (fileIdentity(result) != openedIdentity || fileIdentity(path) != openedIdentity) throw std::runtime_error("FILE_IDENTITY_FAILED: input changed during canonical resolution");
    return result;
}
bool sameFile(const fs::path& left, const fs::path& right) {
    std::error_code error;
    if (!fs::exists(left, error) || error || !fs::exists(right, error) || error) return false;
    return fileIdentity(left) == fileIdentity(right);
}
bool pathContains(const fs::path& root, const fs::path& path) {
    auto a = fs::weakly_canonical(root), b = fs::weakly_canonical(path);
    pathText(a); pathText(b);
    auto x = a.begin(), y = b.begin(); fs::path left, right;
    for (; x != a.end(); ++x, ++y) {
        if (y == b.end()) return false;
        left /= *x; right /= *y;
        if (*x != *y && !sameFile(left, right)) return false;
    }
    return true;
}
std::string projectNamespaceFor(const fs::path& path) {
    auto canonical = verifiedCanonicalPath(path);
    auto current = fs::is_directory(canonical) ? canonical : canonical.parent_path();
    for (auto parent = current; !parent.empty();) {
        if (fs::is_directory(parent / "Assets") && fs::is_directory(parent / "ProjectSettings")) return pathText(parent);
        auto next = parent.parent_path(); if (next == parent) break; parent = next;
    }
    return pathText(current);
}
std::string sourceIdentityDigest(const fs::path& source, std::string_view projectNamespace,
    std::string_view virtualPath, std::string_view clipIdentity) {
    std::string value;
    appendField(value, "animbridge-source-identity-v2"); appendField(value, pathText(verifiedCanonicalPath(source)));
    appendField(value, projectNamespace); appendField(value, virtualPath); appendField(value, clipIdentity);
    return textSha256(value);
}
std::string legacyClipId(const fs::path& source, const std::string& name) {
    auto value = pathText(fs::weakly_canonical(source));
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    value += '#' + name;
    std::uint64_t hash = 14695981039346656037ull;
    for (unsigned char c : value) { hash ^= c; hash *= 1099511628211ull; }
    std::ostringstream output; output << std::hex << hash; return output.str();
}
std::string exportIdForDigest(const std::string& digest) {
    if (digest.size() != 64 || digest.find_first_not_of("0123456789abcdef") != std::string::npos) throw std::runtime_error("EXPORT_ID_COLLISION: invalid identity digest");
    return "v2_" + digest.substr(0, 32);
}
}
