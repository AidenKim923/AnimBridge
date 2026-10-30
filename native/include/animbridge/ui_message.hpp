#pragma once
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <utility>

namespace animbridge {
class UiError final : public std::runtime_error {
public:
    std::string messageKey;
    nlohmann::json messageParams;
    UiError(std::string key, std::string fallback, nlohmann::json params = nlohmann::json::object())
        : std::runtime_error(std::move(fallback)), messageKey(std::move(key)), messageParams(std::move(params)) {}
};

inline nlohmann::json messageData(const std::exception& error, const std::string& field = "message") {
    nlohmann::json result = {{field, error.what()}};
    if (const auto* localized = dynamic_cast<const UiError*>(&error)) {
        result[field + "Key"] = localized->messageKey;
        result[field + "Params"] = localized->messageParams;
    }
    return result;
}
}
