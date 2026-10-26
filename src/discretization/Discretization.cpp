#include "discretization/Discretization.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace cad::discretization {

const char* unitSymbol(LengthUnit unit)
{
    return unit == LengthUnit::Meter ? "m" : "mm";
}

const char* unitName(LengthUnit unit)
{
    return unit == LengthUnit::Meter ? "metros" : "milímetros";
}

const char* criterionLabel(InclusionCriterion criterion)
{
    switch (criterion)
    {
    case InclusionCriterion::FullyInside: return "Cubo totalmente contido no sólido";
    case InclusionCriterion::CenterInside: return "Centro do cubo localizado no material";
    case InclusionCriterion::PositiveIntersection: return "Cubo com interseção volumétrica positiva com o sólido";
    }
    return "";
}

const char* criterionKey(InclusionCriterion criterion)
{
    switch (criterion)
    {
    case InclusionCriterion::FullyInside: return "totalmente_contido";
    case InclusionCriterion::CenterInside: return "centro_no_material";
    case InclusionCriterion::PositiveIntersection: return "intersecao_positiva";
    }
    return "";
}

VoxelGrid VoxelGrid::make(const gp_XYZ& minCorner, double lco)
{
    VoxelGrid g;
    g.lco = lco;
    for (int a = 0; a < 3; ++a)
    {
        g.originNm[a] = std::llround(minCorner.Coord(a + 1) * kNanometersPerMm);
        g.origin.SetCoord(a + 1, double(g.originNm[a]) / kNanometersPerMm);
    }
    // lco exato em nm: o double digitado (ex.: 0,1) corresponde ao inteiro de nm mais próximo.
    const double lcoNm = lco * kNanometersPerMm;
    g.lcoNm = std::llround(lcoNm);
    g.exact = g.lcoNm > 0 && double(g.lcoNm) / kNanometersPerMm == lco;
    return g;
}

bool Discretization::cellAccepted(int i, int j, int k) const
{
    if (i < 0 || j < 0 || k < 0 || i >= m_grid.nx || j >= m_grid.ny || k >= m_grid.nz)
        return false;
    return m_accepted[std::size_t(m_grid.cellIndex(i, j, k))];
}

std::int64_t Discretization::vertexId(int i, int j, int k) const
{
    if (i < 0 || j < 0 || k < 0 || i > m_grid.nx || j > m_grid.ny || k > m_grid.nz)
        return 0;
    return m_latticeId[std::size_t(m_grid.latticeIndex(i, j, k))];
}

std::int64_t Discretization::cubeId(int i, int j, int k) const
{
    if (!cellAccepted(i, j, k))
        return 0;
    const std::int64_t cell = m_grid.cellIndex(i, j, k);
    const auto it = std::lower_bound(m_cubes.begin(), m_cubes.end(), cell);
    return it != m_cubes.end() && *it == cell ? std::int64_t(it - m_cubes.begin()) + 1 : 0;
}

CubeInfo Discretization::cube(std::int64_t index) const
{
    CubeInfo c;
    if (index < 0 || index >= cubeCount())
        return c;
    c.id = index + 1;
    m_grid.cellIjk(m_cubes[std::size_t(index)], c.i, c.j, c.k);
    c.center = m_grid.center(c.i, c.j, c.k);
    c.lco = m_grid.lco;
    static constexpr int kOffsets[8][3] = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0},
                                           {0, 0, 1}, {1, 0, 1}, {1, 1, 1}, {0, 1, 1}};
    for (int v = 0; v < 8; ++v)
        c.vertexIds[std::size_t(v)] = vertexId(c.i + kOffsets[v][0], c.j + kOffsets[v][1], c.k + kOffsets[v][2]);
    c.centerId = vertexNodeCount() + c.id;
    return c;
}

NodeInfo Discretization::node(std::int64_t id, LengthUnit unit) const
{
    NodeInfo n;
    if (id < 1 || id > totalNodeCount())
        return n;
    n.id = id;
    if (id <= vertexNodeCount())
    {
        m_grid.latticeIjk(m_vertexPoints[std::size_t(id - 1)], n.i, n.j, n.k);
        n.position = m_grid.vertex(n.i, n.j, n.k, unit);
        return n;
    }
    n.center = true;
    m_grid.cellIjk(m_cubes[std::size_t(id - vertexNodeCount() - 1)], n.i, n.j, n.k);
    n.position = m_grid.center(n.i, n.j, n.k, unit);
    return n;
}

std::size_t Discretization::memoryBytes() const
{
    return m_cubes.capacity() * sizeof(std::int64_t) + m_accepted.capacity() / 8
           + m_latticeId.capacity() * sizeof(std::uint32_t) + m_vertexPoints.capacity() * sizeof(std::int64_t);
}

bool Discretization::build(const VoxelGrid& grid, const Parameters& parameters, const std::vector<std::uint8_t>& accepted,
                           const std::function<bool()>& keepGoing)
{
    m_grid = grid;
    m_parameters = parameters;
    m_cubes.clear();
    m_vertexPoints.clear();
    const std::int64_t cells = grid.cellCount();
    const std::int64_t points = grid.latticeCount();
    if (std::int64_t(accepted.size()) != cells || points >= std::int64_t(std::numeric_limits<std::uint32_t>::max()))
        return false;
    auto cancelled = [&](std::int64_t n) { return (n & 0xFFFF) == 0 && keepGoing && !keepGoing(); };

    // 1. Cubos aceitos em ordem crescente do índice linear (Z → Y → X).
    m_accepted.assign(std::size_t(cells), false);
    std::int64_t count = 0;
    for (std::int64_t c = 0; c < cells; ++c)
        count += accepted[std::size_t(c)] ? 1 : 0;
    m_cubes.reserve(std::size_t(count));
    for (std::int64_t c = 0; c < cells; ++c)
    {
        if (cancelled(c))
            return false;
        if (accepted[std::size_t(c)])
        {
            m_cubes.push_back(c);
            m_accepted[std::size_t(c)] = true;
        }
    }

    // 2. Pontos da rede usados pelos cubos aceitos (deduplicação pelo índice inteiro).
    m_latticeId.assign(std::size_t(points), 0u);
    for (std::size_t n = 0; n < m_cubes.size(); ++n)
    {
        if (cancelled(std::int64_t(n)))
            return false;
        int i, j, k;
        grid.cellIjk(m_cubes[n], i, j, k);
        for (int dk = 0; dk <= 1; ++dk)
            for (int dj = 0; dj <= 1; ++dj)
            {
                const std::int64_t base = grid.latticeIndex(i, j + dj, k + dk);
                m_latticeId[std::size_t(base)] = 1u;
                m_latticeId[std::size_t(base + 1)] = 1u;
            }
    }

    // 3. IDs dos vértices: varredura da rede em ordem crescente (Z → Y → X), sem lacunas.
    std::uint32_t next = 0;
    for (std::int64_t p = 0; p < points; ++p)
    {
        if (cancelled(p))
            return false;
        if (m_latticeId[std::size_t(p)] != 0u)
        {
            m_latticeId[std::size_t(p)] = ++next;
            m_vertexPoints.push_back(p);
        }
    }
    m_vertexPoints.shrink_to_fit();
    return true;
}

} // namespace cad::discretization
