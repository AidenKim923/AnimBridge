#include "animbridge/ui_resources.hpp"
#include "animbridge/path_grants.hpp"
#include <fstream>
#include <set>

namespace animbridge {
std::optional<UiResource> readUiResource(const fs::path& root, const std::string& url, const std::string& origin) {
    if (!url.starts_with(origin) || url.size() > 4096) return {};
    auto encoded = url.substr(origin.size()); if (encoded.empty()) encoded = "index.html";
    // No query/fragment/path normalization aliases in the static manifest.
    std::string name;
    for (std::size_t i = 0; i < encoded.size(); ++i) {
        auto c = encoded[i];
        if (c == '%') {
            if (i + 2 >= encoded.size()) return {};
            auto hex = [](char x) -> int { if (x >= '0' && x <= '9') return x-'0'; if (x >= 'a' && x <= 'f') return x-'a'+10; if (x >= 'A' && x <= 'F') return x-'A'+10; return -1; };
            auto a = hex(encoded[++i]), b = hex(encoded[++i]); if (a < 0 || b < 0) return {}; c = static_cast<char>(a * 16 + b);
        }
        if (c <= 0 || c == '\\' || c == ':' || c == '?' || c == '#') return {}; name += c;
    }
    static const std::set<std::string> manifest = {"index.html", "app.js", "host-transport.js", "styles.css", "icons.js", "viewer.js", "preview-queue.js", "i18n.js", "locales/en.js", "locales/ko.js", "locales/es.js", "locales/ja.js", "locales/zh-CN.js", "locales/zh-TW.js", "locales/pl.js", "locales/hi.js", "locales/ru.js", "locales/ar.js", "locales/pt-BR.js", "locales/pt-PT.js", "locales/fr.js", "locales/th.js", "locales/tr.js", "locales/native-en.js", "locales/native-ko.js", "app-icon.png"};
    if (!manifest.contains(name)) return {};
    auto path = root / fromUtf8(name); std::error_code error;
    if (!pathWithin(path, root) || !fs::is_regular_file(path, error)) return {};
    auto cursor = root; for (const auto& component : fromUtf8(name)) { cursor /= component; if (fs::is_symlink(fs::symlink_status(cursor, error)) || error) return {}; }
    std::ifstream input(path, std::ios::binary); if (!input) return {};
    std::string bytes{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()}; if (bytes.size() > 8 * 1024 * 1024) return {};
    return UiResource{name.ends_with(".png") ? "image/png" : name.ends_with(".js") ? "text/javascript; charset=utf-8" : name.ends_with(".css") ? "text/css; charset=utf-8" : "text/html; charset=utf-8", std::move(bytes)};
}
}
