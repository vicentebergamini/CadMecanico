// Leitor STL independente (sem OCCT) para validar os arquivos exportados: triângulos, caixa
// envolvente, volume pelo teorema da divergência e triângulos degenerados.
#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

namespace cad::test::stl {

struct Vec3
{
    double x = 0, y = 0, z = 0;
};

struct Mesh
{
    bool ok = false;
    bool binary = false;
    std::string error;
    std::vector<std::array<Vec3, 3>> triangles;
    std::vector<Vec3> normals;

    Vec3 min{std::numeric_limits<double>::max(), std::numeric_limits<double>::max(), std::numeric_limits<double>::max()};
    Vec3 max{-std::numeric_limits<double>::max(), -std::numeric_limits<double>::max(), -std::numeric_limits<double>::max()};

    // Volume com sinal (malha fechada orientada para fora => positivo).
    double volume() const
    {
        double v = 0.0;
        for (const auto& t : triangles)
        {
            const Vec3 &a = t[0], &b = t[1], &c = t[2];
            v += a.x * (b.y * c.z - b.z * c.y) - a.y * (b.x * c.z - b.z * c.x) + a.z * (b.x * c.y - b.y * c.x);
        }
        return v / 6.0;
    }
    double area(const std::array<Vec3, 3>& t) const
    {
        const Vec3 u{t[1].x - t[0].x, t[1].y - t[0].y, t[1].z - t[0].z};
        const Vec3 w{t[2].x - t[0].x, t[2].y - t[0].y, t[2].z - t[0].z};
        const Vec3 n{u.y * w.z - u.z * w.y, u.z * w.x - u.x * w.z, u.x * w.y - u.y * w.x};
        return 0.5 * std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
    }
    int degenerate(double minArea = 1e-12) const
    {
        int n = 0;
        for (const auto& t : triangles)
            n += area(t) <= minArea;
        return n;
    }
    bool finite() const
    {
        for (const auto& t : triangles)
            for (const Vec3& p : t)
                if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z))
                    return false;
        return true;
    }
    void add(const std::array<Vec3, 3>& t)
    {
        triangles.push_back(t);
        for (const Vec3& p : t)
        {
            min = {std::min(min.x, p.x), std::min(min.y, p.y), std::min(min.z, p.z)};
            max = {std::max(max.x, p.x), std::max(max.y, p.y), std::max(max.z, p.z)};
        }
    }
};

inline Mesh read(const std::string& path)
{
    Mesh m;
    std::ifstream in(path, std::ios::binary);
    if (!in)
    {
        m.error = "arquivo não abre";
        return m;
    }
    const std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    // Binário: 80 bytes de cabeçalho + uint32 + 50 bytes por triângulo.
    if (data.size() >= 84)
    {
        std::uint32_t count = 0;
        std::memcpy(&count, data.data() + 80, 4);
        if (data.size() == 84ull + 50ull * count)
        {
            m.binary = true;
            for (std::uint32_t i = 0; i < count; ++i)
            {
                const char* p = data.data() + 84 + 50ull * i;
                float f[12];
                std::memcpy(f, p, sizeof(f));
                m.normals.push_back({f[0], f[1], f[2]});
                m.add({Vec3{f[3], f[4], f[5]}, Vec3{f[6], f[7], f[8]}, Vec3{f[9], f[10], f[11]}});
            }
            m.ok = count > 0;
            if (!m.ok)
                m.error = "binário sem triângulos";
            return m;
        }
    }
    // ASCII.
    if (data.compare(0, 5, "solid") != 0)
    {
        m.error = "formato desconhecido";
        return m;
    }
    std::istringstream s(data);
    std::string word;
    std::array<Vec3, 3> tri;
    int vertex = 0;
    bool ended = false;
    while (s >> word)
    {
        if (word == "normal")
        {
            Vec3 n;
            s >> n.x >> n.y >> n.z;
            m.normals.push_back(n);
        }
        else if (word == "vertex")
        {
            if (vertex > 2)
            {
                m.error = "faceta com mais de 3 vértices";
                return m;
            }
            s >> tri[vertex].x >> tri[vertex].y >> tri[vertex].z;
            ++vertex;
        }
        else if (word == "endfacet")
        {
            if (vertex != 3)
            {
                m.error = "faceta incompleta";
                return m;
            }
            m.add(tri);
            vertex = 0;
        }
        else if (word == "endsolid")
            ended = true;
    }
    m.ok = ended && !m.triangles.empty();
    if (!m.ok)
        m.error = ended ? "ASCII sem triângulos" : "ASCII sem endsolid";
    return m;
}

} // namespace cad::test::stl
