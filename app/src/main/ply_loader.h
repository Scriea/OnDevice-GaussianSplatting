#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct PlyPoint {
    float x = 0.f;
    float y = 0.f;
    float z = 0.f;
    uint8_t r = 255;
    uint8_t g = 255;
    uint8_t b = 255;
};

// Minimal PLY loader:
// - Supports ASCII and binary_little_endian
// - Reads only vertex element
// - Reads x,y,z and optional uchar r,g,b (common PLY export)
bool loadPlyVertices(const std::string& path, std::vector<PlyPoint>& out);
