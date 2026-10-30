#pragma once
#include "core.hpp"
#include <optional>

namespace animbridge {
struct UiResource { std::string mime, bytes; };
inline constexpr const char* UiContentSecurityPolicy = "default-src 'self'; script-src 'self'; style-src 'self' 'unsafe-inline'; img-src 'self' data: blob:; connect-src 'none'; object-src 'none'; base-uri 'none'; frame-src 'none'; child-src 'none'; form-action 'none'";
// Fixed bundle manifest. This is intentionally not a general filesystem server.
std::optional<UiResource> readUiResource(const fs::path& root, const std::string& url, const std::string& origin);
}
