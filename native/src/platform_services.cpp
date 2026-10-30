#include "animbridge/platform_services.hpp"
#include <algorithm>
#include <array>
#include <stdexcept>
#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#elif defined(__APPLE__)
#include <cstdlib>
#else
#include <sys/random.h>
#include <cerrno>
#endif

namespace animbridge {
std::string nativeCapability() {
    std::array<unsigned char, 32> bytes{};
#ifdef _WIN32
    if (BCryptGenRandom(nullptr, bytes.data(), static_cast<ULONG>(bytes.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) throw std::runtime_error("Native capability entropy is unavailable");
#elif defined(__APPLE__)
    arc4random_buf(bytes.data(), bytes.size());
#else
    std::size_t offset = 0;
    while (offset != bytes.size()) { auto count = getrandom(bytes.data() + offset, bytes.size() - offset, 0); if (count < 0 && errno == EINTR) continue; if (count <= 0) throw std::runtime_error("Native capability entropy is unavailable"); offset += static_cast<std::size_t>(count); }
#endif
    std::string result; constexpr char hex[] = "0123456789abcdef"; for (auto value : bytes) { result += hex[value >> 4]; result += hex[value & 15]; } return result;
}
void WebViewSession::invalidate() { std::lock_guard lock(mutex_); committed_ = false; epoch_.clear(); capability_.clear(); seenIds_.clear(); }
WebViewSession::Json WebViewSession::commit(const std::string& origin, const std::string& platform, bool customFrame) {
    std::lock_guard lock(mutex_);
    epoch_ = nativeCapability(); capability_ = nativeCapability(); origin_ = origin; seenIds_.clear(); committed_ = true;
    return {{"event", "bootstrap"}, {"protocolVersion", 1}, {"epoch", epoch_}, {"capability", capability_},
        {"data", {{"platform", platform}, {"customFrame", customFrame}, {"protocolVersion", 1}, {"testHooks",
#if defined(ANIMBRIDGE_ENABLE_UI_TEST_HOOKS) && ANIMBRIDGE_ENABLE_UI_TEST_HOOKS
        true
#else
        false
#endif
        }}}};
}
bool WebViewSession::accept(const Json& request, const std::string& engineOrigin, bool mainFrame, bool frameCreationBlocked) {
    std::lock_guard lock(mutex_);
    try {
    if (!committed_ || !mainFrame || !frameCreationBlocked || engineOrigin != origin_ || !request.is_object()) return false;
    if (request.value("protocolVersion", 0) != 1 || request.value("epoch", "") != epoch_ || request.value("capability", "") != capability_) return false;
    if (!request.contains("id") || !request["id"].is_string() || request["id"].get_ref<const std::string&>().size() > 160 || request["id"].get_ref<const std::string&>().empty()) return false;
    if (!request.contains("action") || !request["action"].is_string() || !request.value("payload", Json::object()).is_object() || request.dump().size() > 1024 * 1024) return false;
    static const std::set<std::string> actions = {"getState", "getAppInfo", "openExternalLink", "checkForUpdate", "chooseFiles", "chooseFolder", "scan", "drop", "select", "plan", "start", "cancel", "retryFailed", "inspect", "preview", "validateSkeleton", "connectUnity", "saveSettings", "openPath", "copyText", "saveReport", "clearCache", "convertUnrealAsset", "window"};
    if (!actions.contains(request["action"].get<std::string>()) || seenIds_.size() >= 100000) return false;
    return seenIds_.insert(request["id"].get<std::string>()).second;
    } catch (...) { return false; }
}
WebViewSession::Json WebViewSession::envelope(const Json& value) const {
    std::lock_guard lock(mutex_); if (!committed_) return Json();
    auto result = value; result["protocolVersion"] = 1; result["epoch"] = epoch_; return result;
}
bool WebViewSession::ready() const { std::lock_guard lock(mutex_); return committed_; }
}
