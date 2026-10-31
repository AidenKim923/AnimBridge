#include "animbridge/fbx_rig.hpp"
#include <cstdint>
#include <cstring>
#include <fstream>
#include <regex>
#include <string>
#include <vector>

namespace animbridge {
namespace {
void count(FbxRig& rig, const std::string& node, const std::string& type) {
    if (node == "Model") {
        if (type == "LimbNode" || type == "Limb" || type == "Root") ++rig.boneModels;
        else if (type == "Null") ++rig.nullModels;
        else if (type == "Mesh") ++rig.meshes;
    } else if (node == "Geometry" && type == "Mesh") ++rig.meshes;
    else if (node == "Deformer" && type == "Skin") ++rig.skins;
}

// Binary FBX: walk node records, reading only names and string properties.
// Array properties are skipped by their stored length, never decompressed.
class BinaryReader {
public:
    BinaryReader(std::ifstream& file, std::uint64_t size, bool wide) : file_(file), size_(size), wide_(wide) {}

    struct Record { std::uint64_t end = 0, properties = 0, propertyBytes = 0, body = 0; std::string name; };

    // Reads the record header at `offset`. Returns false at a null record or on bad data.
    bool record(std::uint64_t offset, Record& out) {
        if (!seek(offset)) return false;
        out.end = number(); out.properties = number(); out.propertyBytes = number();
        unsigned char length = 0;
        if (!read(&length, 1)) return false;
        out.name.assign(length, '\0');
        if (length && !read(out.name.data(), length)) return false;
        out.body = static_cast<std::uint64_t>(file_.tellg());
        if (out.end == 0) return false;
        return ok_ && out.end > offset && out.end <= size_ && out.body + out.propertyBytes <= out.end;
    }

    // String properties of a record, in order; other property types are skipped.
    std::vector<std::string> strings(const Record& record) {
        std::vector<std::string> values;
        if (!seek(record.body)) return values;
        for (std::uint64_t i = 0; i < record.properties && ok_; ++i) {
            char type = 0;
            if (!read(&type, 1)) break;
            switch (type) {
            case 'C': skip(1); break;
            case 'Y': skip(2); break;
            case 'I': case 'F': skip(4); break;
            case 'L': case 'D': skip(8); break;
            case 'S': case 'R': {
                std::uint32_t length = 0;
                if (!read(&length, 4)) break;
                if (type == 'R') { skip(length); break; }
                if (record.body + record.propertyBytes < static_cast<std::uint64_t>(file_.tellg()) + length) { ok_ = false; break; }
                std::string text(length, '\0');
                if (length && !read(text.data(), length)) break;
                values.push_back(text);
                break;
            }
            case 'f': case 'd': case 'l': case 'i': case 'b': {
                std::uint32_t header[3] = {};
                if (!read(header, sizeof(header))) break;
                skip(header[2]);
                break;
            }
            default: ok_ = false;
            }
        }
        return values;
    }

    bool good() const { return ok_; }

private:
    std::uint64_t number() {
        if (wide_) { std::uint64_t value = 0; read(&value, 8); return value; }
        std::uint32_t value = 0; read(&value, 4); return value;
    }
    bool read(void* target, std::size_t bytes) {
        if (!ok_) return false;
        file_.read(static_cast<char*>(target), static_cast<std::streamsize>(bytes));
        ok_ = static_cast<std::size_t>(file_.gcount()) == bytes;
        return ok_;
    }
    void skip(std::uint64_t bytes) {
        auto position = static_cast<std::uint64_t>(file_.tellg());
        if (position + bytes > size_) { ok_ = false; return; }
        seek(position + bytes);
    }
    bool seek(std::uint64_t offset) {
        if (!ok_ || offset > size_) return ok_ = false;
        file_.clear(); file_.seekg(static_cast<std::streamoff>(offset));
        return ok_ = static_cast<bool>(file_);
    }
    std::ifstream& file_;
    std::uint64_t size_;
    bool wide_;
    bool ok_ = true;
};

FbxRig inspectBinary(std::ifstream& file, std::uint64_t size) {
    FbxRig rig;
    char header[27] = {};
    file.seekg(0); file.read(header, sizeof(header));
    if (file.gcount() != sizeof(header)) return rig;
    std::uint32_t version = 0; std::memcpy(&version, header + 23, 4);
    BinaryReader reader(file, size, version >= 7500);
    BinaryReader::Record top;
    std::uint64_t offset = sizeof(header);
    while (reader.record(offset, top)) {
        if (top.name == "Objects") {
            BinaryReader::Record child;
            std::uint64_t at = top.body + top.propertyBytes;
            while (at < top.end && reader.record(at, child)) {
                auto values = reader.strings(child);
                if (!reader.good()) return {};
                // FBX 7 objects carry (id, "Name\0\1Class", "Type"); FBX 6 drops the id.
                if (!values.empty()) count(rig, child.name, values.back());
                at = child.end;
            }
            // Children end with a null record; anything else means damaged data.
            rig.parsed = reader.good();
            return rig.parsed ? rig : FbxRig{};
        }
        offset = top.end;
    }
    return rig;
}

// ASCII FBX: object lines look like `Model: 123, "Model::Hips", "LimbNode" {`.
FbxRig inspectAscii(std::ifstream& file) {
    FbxRig rig;
    static const std::regex object(R"re(^\s*(Model|Geometry|Deformer):.*"[^"]*"\s*,\s*"([^"]*)"\s*\{)re");
    file.clear(); file.seekg(0);
    std::string line;
    bool objects = false;
    while (std::getline(file, line)) {
        if (!objects) { objects = line.rfind("Objects:", 0) == 0; continue; }
        if (line.rfind("Connections:", 0) == 0 || line.rfind("Takes:", 0) == 0) break;
        std::smatch match;
        if (std::regex_search(line, match, object)) count(rig, match[1].str(), match[2].str());
    }
    rig.parsed = objects;
    return rig;
}
}

FbxRig inspectFbxRig(const std::filesystem::path& path) {
    std::error_code error;
    auto size = std::filesystem::file_size(path, error);
    if (error) return {};
    std::ifstream file(path, std::ios::binary);
    if (!file) return {};
    char magic[20] = {};
    file.read(magic, sizeof(magic));
    if (file.gcount() == sizeof(magic) && std::memcmp(magic, "Kaydara FBX Binary  ", 20) == 0) return inspectBinary(file, size);
    std::string start(magic, static_cast<std::size_t>(file.gcount()));
    if (start.rfind("; FBX", 0) == 0) return inspectAscii(file);
    return {};
}

bool lacksSkeleton(const FbxRig& rig) {
    return rig.parsed && rig.boneModels == 0 && rig.skins == 0;
}

std::string describeMissingSkeleton(const FbxRig& rig) {
    auto text = std::string("This FBX has no skeleton that Unreal or Blender can read: it has no LimbNode bones and no skinned mesh");
    if (rig.nullModels) text += ", and its " + std::to_string(rig.nullModels) + " joints are plain Null objects (an animation-only export, for example from 3ds Max)";
    return text;
}
}
