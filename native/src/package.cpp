#include "animbridge/core.hpp"
#include "animbridge/file_commit.hpp"
#include "animbridge/path_identity.hpp"
#include <zlib.h>
#include <nlohmann/json.hpp>
#include <array>
#include <algorithm>
#include <chrono>
#include <fstream>
#include <map>
#include <random>
#include <set>
#include <stdexcept>
#include <cctype>
#ifdef _WIN32
#include <windows.h>
#endif

namespace animbridge {
namespace {
std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); }); return s;
}
void rejectLink(const fs::path& p) {
    for (auto current = fs::absolute(p); !current.empty();) {
#ifdef _WIN32
        auto attr = GetFileAttributesW(current.c_str());
        if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_REPARSE_POINT))
            throw std::runtime_error("Cache contains a reparse point: " + utf8(current));
#else
        if (fs::is_symlink(fs::symlink_status(current))) throw std::runtime_error("Cache contains a symbolic link");
#endif
        auto parent = current.parent_path(); if (parent == current) break; current = parent;
    }
}
std::string checkedPath(std::string s) {
    requireUtf8(s);
    std::replace(s.begin(), s.end(), '\\', '/');
    if (s.empty() || s.front() == '/' || s.find(':') != std::string::npos || s.find('\0') != std::string::npos)
        throw std::runtime_error("Unsafe package path");
    while (!s.empty() && s.back() == '/') s.pop_back();
    std::string out;
    std::size_t pos = 0;
    while (pos < s.size()) {
        auto end = s.find('/', pos); if (end == std::string::npos) end = s.size();
        auto part = s.substr(pos, end - pos); pos = end + 1;
        if (part == "." && out.empty()) continue;
        if (part.empty() || part == "." || part == ".." || part.back() == '.' || part.back() == ' ')
            throw std::runtime_error("Unsafe package path component: " + part);
        if (part.find_first_of("<>\"|?*") != std::string::npos ||
            std::any_of(part.begin(), part.end(), [](unsigned char c) { return c < 32; }))
            throw std::runtime_error("Invalid package filename");
        auto stem = lower(part.substr(0, part.find('.')));
        if (stem == "con" || stem == "prn" || stem == "aux" || stem == "nul" ||
            (stem.size() == 4 && (stem.starts_with("com") || stem.starts_with("lpt")) && stem[3] >= '0' && stem[3] <= '9'))
            throw std::runtime_error("Reserved package filename");
        if (!out.empty()) out += '/'; out += part;
    }
    if (out.empty()) throw std::runtime_error("Empty package path");
    if (out.size() > 2000) throw std::runtime_error("Package path too long");
    return out;
}
std::uint64_t octal(const char* p, std::size_t n) {
    std::uint64_t value = 0; bool ended = false;
    for (std::size_t i = 0; i < n; ++i) {
        unsigned char c = static_cast<unsigned char>(p[i]);
        if (c == 0 || c == ' ') { if (value) ended = true; continue; }
        if (ended || c < '0' || c > '7' || value > (UINT64_MAX >> 3)) throw std::runtime_error("Invalid tar integer");
        value = (value << 3) + (c - '0');
    }
    return value;
}
std::string field(const char* p, std::size_t n) {
    auto end = std::find(p, p + n, '\0'); return std::string(p, end);
}
class Gzip {
    gzFile file_ = nullptr;
    std::uint64_t read_ = 0, limit_;
    std::chrono::steady_clock::time_point deadline_;
public:
    Gzip(const fs::path& p, std::uint64_t limit, unsigned seconds) : limit_(limit), deadline_(std::chrono::steady_clock::now() + std::chrono::seconds(seconds)) {
#ifdef _WIN32
        file_ = gzopen_w(p.c_str(), "rb");
#else
        file_ = gzopen(p.c_str(), "rb");
#endif
        if (!file_) throw std::runtime_error("Cannot open package");
    }
    ~Gzip() { if (file_) gzclose(file_); }
    void read(char* data, std::size_t size) {
        if (size > limit_ - read_) throw std::runtime_error("Package expansion limit exceeded");
        std::size_t done = 0;
        while (done < size) {
            if (std::chrono::steady_clock::now() >= deadline_) throw std::runtime_error("Package processing time limit exceeded");
            auto count = gzread(file_, data + done, static_cast<unsigned>(std::min<std::size_t>(size - done, 65536)));
            if (count <= 0) throw std::runtime_error("Truncated or corrupt gzip/tar package");
            done += count; read_ += count;
        }
    }
    void finish() {
        std::array<char, 65536> data{};
        for (;;) {
            if (std::chrono::steady_clock::now() >= deadline_) throw std::runtime_error("Package processing time limit exceeded");
            int count = gzread(file_, data.data(), static_cast<unsigned>(data.size()));
            if (count < 0) throw std::runtime_error("Corrupt gzip package");
            if (!count) break;
            if (static_cast<std::uint64_t>(count) > limit_ - read_) throw std::runtime_error("Package expansion limit exceeded");
            read_ += count;
            if (std::any_of(data.begin(), data.begin() + count, [](char c) { return c != 0; }))
                throw std::runtime_error("Unexpected data after tar end marker");
        }
        int error = Z_OK; gzerror(file_, &error);
        if (error != Z_OK && error != Z_STREAM_END) throw std::runtime_error("Corrupt gzip package trailer");
    }
};
struct Asset { fs::path content, meta; std::string path; };
struct Cleanup {
    fs::path root, owner; bool keep = false;
    ~Cleanup() { if (!keep) { std::error_code error; removeOwnedTree(root, owner, error); } }
};
}
fs::path extractUnityPackage(const fs::path& package, const fs::path& cache, const Limits& limits) {
    if (!limits.maxEntries || !limits.maxFileBytes || !limits.maxExpandedBytes || !limits.maxCompressedBytes || !limits.maxSeconds) throw std::runtime_error("Invalid package limits");
    if (fs::file_size(package) > limits.maxCompressedBytes) throw std::runtime_error("Compressed package size limit exceeded");
    rejectLink(cache);
    fs::create_directories(cache);
    rejectLink(cache);
    fs::path root;
    std::random_device random;
    for (int i = 0; i < 20; ++i) {
        root = fs::absolute(cache) / ("package-" + std::to_string(random()) + "-" + std::to_string(random()));
        if (fs::create_directory(root)) break;
        root.clear();
    }
    if (root.empty()) throw std::runtime_error("Cannot allocate package cache");
    Cleanup guard{root, cache};
    const auto raw = root / "raw"; const auto assetsRoot = root / "assets";
    fs::create_directory(raw); fs::create_directory(assetsRoot);
    Gzip input(package, limits.maxExpandedBytes, limits.maxSeconds);
    std::map<std::string, Asset> assets;
    std::set<std::string> seen;
    std::array<char, 512> header{};
    std::array<char, 65536> buffer{};
    std::size_t entries = 0;
    for (;;) {
        input.read(header.data(), header.size());
        if (std::all_of(header.begin(), header.end(), [](char c) { return c == 0; })) {
            input.read(header.data(), header.size());
            if (!std::all_of(header.begin(), header.end(), [](char c) { return c == 0; })) throw std::runtime_error("Invalid tar end marker");
            input.finish(); break;
        }
        if (++entries > limits.maxEntries) throw std::runtime_error("Package entry count limit exceeded");
        auto checksum = octal(header.data() + 148, 8);
        std::uint64_t actual = 0;
        for (std::size_t i = 0; i < header.size(); ++i) actual += (i >= 148 && i < 156) ? 32 : static_cast<unsigned char>(header[i]);
        if (actual != checksum) throw std::runtime_error("Invalid tar checksum");
        auto name = field(header.data(), 100);
        auto prefix = field(header.data() + 345, 155); if (!prefix.empty()) name = prefix + '/' + name;
        if ((name == "." || name == "./") && header[156] == '5' && octal(header.data() + 124, 12) == 0) continue;
        name = checkedPath(name);
        if (!seen.insert(portableNameKey(name)).second) throw std::runtime_error("Duplicate tar entry under portableNamePolicyVersion=2");
        char type = header[156];
        if (type != 0 && type != '0' && type != '5') throw std::runtime_error("Package links and special tar entries are not supported");
        auto size = octal(header.data() + 124, 12);
        if (size > limits.maxFileBytes) throw std::runtime_error("Package file size limit exceeded");
        if (type == '5' && size) throw std::runtime_error("Directory tar entry has content");
        auto slash = name.find('/');
        std::string guid = slash == std::string::npos ? name : name.substr(0, slash);
        std::string leaf = slash == std::string::npos ? "" : name.substr(slash + 1);
        bool recognized = (leaf == "asset" || leaf == "asset.meta" || leaf == "pathname");
        if (recognized && (guid.size() != 32 || !std::all_of(guid.begin(), guid.end(), [](unsigned char c) { return std::isxdigit(c); })))
            throw std::runtime_error("Invalid Unity package GUID");
        if (leaf == "pathname" && size > 4096) throw std::runtime_error("Package pathname too large");
        fs::path stored;
        std::ofstream output;
        std::string path;
        if (recognized && leaf != "pathname") {
            stored = raw / (guid + (leaf == "asset" ? ".asset" : ".meta"));
            output.open(stored, std::ios::binary); if (!output) throw std::runtime_error("Cannot write package cache");
        }
        auto remaining = size;
        while (remaining) {
            auto count = static_cast<std::size_t>(std::min<std::uint64_t>(remaining, buffer.size()));
            input.read(buffer.data(), count);
            if (output.is_open()) { output.write(buffer.data(), static_cast<std::streamsize>(count)); if (!output) throw std::runtime_error("Package cache write failed"); }
            if (recognized && leaf == "pathname") path.append(buffer.data(), count);
            remaining -= count;
        }
        output.close();
        auto padding = (512 - size % 512) % 512; if (padding) input.read(buffer.data(), static_cast<std::size_t>(padding));
        if (recognized) {
            auto& asset = assets[guid];
            if (leaf == "asset") asset.content = stored;
            else if (leaf == "asset.meta") asset.meta = stored;
            else {
                while (!path.empty() && (path.back() == '\r' || path.back() == '\n')) path.pop_back();
                asset.path = checkedPath(path);
                if (!asset.path.starts_with("Assets/") && !asset.path.starts_with("Packages/") && asset.path != "Assets")
                    throw std::runtime_error("Unity package pathname must be under Assets or Packages");
            }
        }
    }
    std::set<std::string> destinations;
    std::map<std::string, std::pair<std::string, bool>> components;
    auto reserveComponents = [&](const std::string& name, bool file) {
        std::string preserved, folded; std::size_t start = 0;
        while (start < name.size()) {
            auto end = name.find('/', start); if (end == std::string::npos) end = name.size();
            auto component = name.substr(start, end - start);
            if (!preserved.empty()) { preserved += '/'; folded += '/'; }
            preserved += component; folded += portableNameKey(component);
            bool isFile = file && end == name.size();
            auto [previous, inserted] = components.emplace(folded, std::pair{preserved, isFile});
            if (!inserted && (previous->second.first != preserved || previous->second.second != isFile)) throw std::runtime_error("Conflicting portable package path components under portableNamePolicyVersion=2");
            start = end + 1;
        }
    };
    nlohmann::json virtualPaths = nlohmann::json::object();
    for (auto& [guid, asset] : assets) {
        if (asset.path.empty()) { if (!asset.content.empty()) throw std::runtime_error("Package asset is missing pathname"); continue; }
        reserveComponents(asset.path, !asset.content.empty());
        if (asset.content.empty()) continue; // Unity folder records only have metadata.
        auto relative = fromUtf8(asset.path);
        if (!destinations.insert(portableNameKey(asset.path)).second) throw std::runtime_error("Duplicate Unity asset pathname under portableNamePolicyVersion=2");
        if (!asset.meta.empty() && !destinations.insert(portableNameKey(asset.path + ".meta")).second) throw std::runtime_error("Conflicting Unity metadata pathname");
        if (!asset.meta.empty()) reserveComponents(asset.path + ".meta", true);
        auto target = assetsRoot / relative;
        fs::create_directories(target.parent_path());
        if (FileCommitter::noReplace(asset.content, target) != PublishResult::Committed) throw std::runtime_error("Filesystem normalization caused a package destination collision");
        if (!asset.meta.empty() && FileCommitter::noReplace(asset.meta, fromUtf8(utf8(target) + ".meta")) != PublishResult::Committed) throw std::runtime_error("Filesystem normalization caused a package metadata collision");
        virtualPaths[portableNameKey(asset.path)] = asset.path;
    }
    FileCommitter::writeAtomic(root / ".animbridge-virtual-paths.json", nlohmann::json{{"program", "AnimBridge"}, {"portableNamePolicyVersion", PortableNamePolicyVersion}, {"paths", virtualPaths}}.dump(2) + '\n');
    fs::remove_all(raw);
    guard.keep = true;
    return assetsRoot;
}
}
