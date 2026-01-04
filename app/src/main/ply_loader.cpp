#include "ply_loader.h"

#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <cstring>

namespace {

enum class PlyFormat {
    Ascii,
    BinaryLittleEndian,
};

struct Property {
    std::string name;
    std::string type;
    size_t offset = 0;
    size_t size = 0;
};

static size_t typeSize(const std::string& t) {
    if (t == "char" || t == "int8") return 1;
    if (t == "uchar" || t == "uint8") return 1;
    if (t == "short" || t == "int16") return 2;
    if (t == "ushort" || t == "uint16") return 2;
    if (t == "int" || t == "int32") return 4;
    if (t == "uint" || t == "uint32") return 4;
    if (t == "float" || t == "float32") return 4;
    if (t == "double" || t == "float64") return 8;
    return 0;
}

template <typename T>
static bool readExact(std::istream& in, T& out) {
    return (bool)in.read(reinterpret_cast<char*>(&out), sizeof(T));
}

static bool readScalar(std::istream& in, const std::string& type, float& outFloat, uint8_t& outU8) {
    outFloat = 0.f;
    outU8 = 0;

    if (type == "float" || type == "float32") {
        float v = 0;
        if (!readExact(in, v)) return false;
        outFloat = v;
        return true;
    }
    if (type == "double" || type == "float64") {
        double v = 0;
        if (!readExact(in, v)) return false;
        outFloat = (float)v;
        return true;
    }
    if (type == "uchar" || type == "uint8") {
        uint8_t v = 0;
        if (!readExact(in, v)) return false;
        outU8 = v;
        outFloat = (float)v;
        return true;
    }
    if (type == "char" || type == "int8") {
        int8_t v = 0;
        if (!readExact(in, v)) return false;
        outU8 = (uint8_t)v;
        outFloat = (float)v;
        return true;
    }
    if (type == "ushort" || type == "uint16") {
        uint16_t v = 0;
        if (!readExact(in, v)) return false;
        outU8 = (uint8_t)(v & 0xFF);
        outFloat = (float)v;
        return true;
    }
    if (type == "short" || type == "int16") {
        int16_t v = 0;
        if (!readExact(in, v)) return false;
        outU8 = (uint8_t)(v & 0xFF);
        outFloat = (float)v;
        return true;
    }
    if (type == "uint" || type == "uint32") {
        uint32_t v = 0;
        if (!readExact(in, v)) return false;
        outU8 = (uint8_t)(v & 0xFF);
        outFloat = (float)v;
        return true;
    }
    if (type == "int" || type == "int32") {
        int32_t v = 0;
        if (!readExact(in, v)) return false;
        outU8 = (uint8_t)(v & 0xFF);
        outFloat = (float)v;
        return true;
    }

    return false;
}

} // namespace

bool loadPlyVertices(const std::string& path, std::vector<PlyPoint>& out) {
    out.clear();

    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) return false;

    std::string line;
    if (!std::getline(in, line)) return false;
    if (line.rfind("ply", 0) != 0) return false;

    PlyFormat format = PlyFormat::Ascii;
    bool inVertexElement = false;
    uint32_t vertexCount = 0;

    std::vector<Property> props;
    size_t stride = 0;

    while (std::getline(in, line)) {
        if (line == "end_header") break;

        std::istringstream ss(line);
        std::string tok;
        ss >> tok;

        if (tok == "format") {
            std::string fmt;
            ss >> fmt;
            if (fmt == "ascii") format = PlyFormat::Ascii;
            else if (fmt == "binary_little_endian") format = PlyFormat::BinaryLittleEndian;
            else return false; // unsupported
        } else if (tok == "element") {
            std::string name;
            uint32_t count = 0;
            ss >> name >> count;
            inVertexElement = (name == "vertex");
            if (inVertexElement) {
                vertexCount = count;
                props.clear();
                stride = 0;
            }
        } else if (tok == "property") {
            if (!inVertexElement) continue;

            std::string type;
            ss >> type;

            // Ignore list properties
            if (type == "list") {
                // property list <countType> <itemType> <name>
                // We skip lists entirely (faces, etc.)
                continue;
            }

            std::string name;
            ss >> name;

            size_t sz = typeSize(type);
            if (sz == 0) return false;

            Property p;
            p.name = name;
            p.type = type;
            p.offset = stride;
            p.size = sz;
            props.push_back(p);
            stride += sz;
        }
    }

    if (vertexCount == 0) return false;

    // Find x/y/z and optional r/g/b
    const Property* px = nullptr;
    const Property* py = nullptr;
    const Property* pz = nullptr;
    const Property* pr = nullptr;
    const Property* pg = nullptr;
    const Property* pb = nullptr;

    for (const auto& p : props) {
        if (p.name == "x") px = &p;
        else if (p.name == "y") py = &p;
        else if (p.name == "z") pz = &p;
        else if (p.name == "red" || p.name == "r") pr = &p;
        else if (p.name == "green" || p.name == "g") pg = &p;
        else if (p.name == "blue" || p.name == "b") pb = &p;
    }

    if (!px || !py || !pz) return false;

    out.resize(vertexCount);

    if (format == PlyFormat::Ascii) {
        // After header, stream is already at first vertex line
        for (uint32_t i = 0; i < vertexCount; i++) {
            if (!std::getline(in, line)) return false;
            std::istringstream vs(line);

            // Read all properties in order
            float x=0,y=0,z=0;
            uint8_t r=255,g=255,b=255;

            for (const auto& p : props) {
                if (p.type == "float" || p.type == "float32" || p.type == "double" || p.type == "float64") {
                    double v=0;
                    vs >> v;
                    if (p.name == "x") x = (float)v;
                    else if (p.name == "y") y = (float)v;
                    else if (p.name == "z") z = (float)v;
                } else {
                    int v=0;
                    vs >> v;
                    if (p.name == "red" || p.name == "r") r = (uint8_t)v;
                    else if (p.name == "green" || p.name == "g") g = (uint8_t)v;
                    else if (p.name == "blue" || p.name == "b") b = (uint8_t)v;
                }
            }

            out[i] = PlyPoint{ x, y, z, r, g, b };
        }
        return true;
    }

    // Binary little endian
    std::vector<uint8_t> row(stride);

    for (uint32_t i = 0; i < vertexCount; i++) {
        if (!in.read(reinterpret_cast<char*>(row.data()), (std::streamsize)stride)) return false;

        auto readF = [&](const Property* p, float def) -> float {
            if (!p) return def;
            std::istringstream dummy;
            float outF=0.f; uint8_t outU8=0;
            // Use a memstream approach: just copy bytes into scalar variables
            // We rely on little-endian host (Android ARM64 is LE).
            if (p->type == "float" || p->type == "float32") {
                float v;
                std::memcpy(&v, row.data() + p->offset, sizeof(float));
                return v;
            }
            if (p->type == "double" || p->type == "float64") {
                double v;
                std::memcpy(&v, row.data() + p->offset, sizeof(double));
                return (float)v;
            }
            if (p->type == "int" || p->type == "int32") {
                int32_t v; std::memcpy(&v, row.data() + p->offset, sizeof(int32_t));
                return (float)v;
            }
            if (p->type == "uint" || p->type == "uint32") {
                uint32_t v; std::memcpy(&v, row.data() + p->offset, sizeof(uint32_t));
                return (float)v;
            }
            if (p->type == "short" || p->type == "int16") {
                int16_t v; std::memcpy(&v, row.data() + p->offset, sizeof(int16_t));
                return (float)v;
            }
            if (p->type == "ushort" || p->type == "uint16") {
                uint16_t v; std::memcpy(&v, row.data() + p->offset, sizeof(uint16_t));
                return (float)v;
            }
            if (p->type == "char" || p->type == "int8") {
                int8_t v; std::memcpy(&v, row.data() + p->offset, sizeof(int8_t));
                return (float)v;
            }
            if (p->type == "uchar" || p->type == "uint8") {
                uint8_t v; std::memcpy(&v, row.data() + p->offset, sizeof(uint8_t));
                return (float)v;
            }
            return def;
        };

        float x = readF(px, 0.f);
        float y = readF(py, 0.f);
        float z = readF(pz, 0.f);

        uint8_t r = 255, g = 255, b = 255;
        if (pr) r = (uint8_t)readF(pr, 255.f);
        if (pg) g = (uint8_t)readF(pg, 255.f);
        if (pb) b = (uint8_t)readF(pb, 255.f);

        out[i] = PlyPoint{ x, y, z, r, g, b };
    }

    return true;
}
