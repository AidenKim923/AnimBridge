#pragma once
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace animbridge {
inline constexpr unsigned PortableNamePolicyVersion = 2;
// The build pins utf8proc 2.10.0 / Unicode 16.0.0. Path spelling is never
// normalized with this policy; it only governs portable output collisions.
bool validUtf8(std::string_view value) noexcept;
void requireUtf8(std::string_view value);
std::string portableNameKey(std::string_view value);
std::string fileIdentity(const std::filesystem::path& path);
std::filesystem::path verifiedCanonicalPath(const std::filesystem::path& path);
bool sameFile(const std::filesystem::path& left, const std::filesystem::path& right);
bool pathContains(const std::filesystem::path& root, const std::filesystem::path& path);
std::string projectNamespaceFor(const std::filesystem::path& path);
std::string sourceIdentityDigest(const std::filesystem::path& source,
    std::string_view projectNamespace, std::string_view virtualPath, std::string_view clipIdentity);
std::string legacyClipId(const std::filesystem::path& source, const std::string& name);
std::string exportIdForDigest(const std::string& digest);
}
