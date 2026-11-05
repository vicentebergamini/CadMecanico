#include "discretization/LatticeTopology.h"

#include "discretization/ApdlExporter.h"

#include <cmath>
#include <cstdio>

namespace cad::discretization {

namespace {

// Número de células aceitas no bloco [i0..i1] × [j0..j1] × [k0..k1] (fora da grade = inativa, como a
// função cel() do MATLAB, l. 21).
int activeCells(const Discretization& d, int i0, int i1, int j0, int j1, int k0, int k1)
{
    int n = 0;
    for (int k = k0; k <= k1; ++k)
        for (int j = j0; j <= j1; ++j)
            for (int i = i0; i <= i1; ++i)
                n += d.cellAccepted(i, j, k) ? 1 : 0;
    return n;
}

} // namespace

LatticeTopology classifyLattice(const Discretization& d)
{
    LatticeTopology t;
    if (d.empty())
    {
        t.message = "Discretização vazia.";
        return t;
    }
    const VoxelGrid& g = d.grid();

    // cVert (malha_por_mascara.m, l. 31-32).
    const auto& points = d.vertexPoints();
    t.vertexCubeCount.resize(points.size());
    for (std::size_t v = 0; v < points.size(); ++v)
    {
        int i = 0, j = 0, k = 0;
        g.latticeIjk(points[v], i, j, k);
        const int c = activeCells(d, i - 1, i, j - 1, j, k - 1, k);
        if (c < 1 || c > 8)
        {
            t.message = "Vértice " + std::to_string(v + 1) + " sem cubo ativo adjacente (cVert = "
                        + std::to_string(c) + "): inconsistente com a regra do MATLAB (cVert > 0).";
            return t;
        }
        t.vertexCubeCount[v] = std::uint8_t(c);
    }

    // tp das ligações, na ordem do exportador.
    t.linkTp.reserve(std::size_t(ApdlLinkSequence::count(d).total()));
    ApdlLinkSequence seq(d);
    LinkElement e;
    while (seq.next(e))
    {
        int tp = 0;
        switch (e.group)
        {
        case LinkGroup::VertexEdge:
        {
            // n1 é o vértice de menor ID (o vizinho n2 está na direção positiva do eixo).
            int i = 0, j = 0, k = 0;
            g.latticeIjk(points[std::size_t(e.n1 - 1)], i, j, k);
            if (e.axis == 0)
                tp = activeCells(d, i, i, j - 1, j, k - 1, k); // l. 95
            else if (e.axis == 1)
                tp = activeCells(d, i - 1, i, j, j, k - 1, k); // l. 113
            else
                tp = activeCells(d, i - 1, i, j - 1, j, k, k); // l. 130
            if (tp < 1 || tp > 4)
            {
                t.message = "Aresta " + std::to_string(e.id) + " com n4 = " + std::to_string(tp)
                            + ": inconsistente com a regra do MATLAB (aresta existe somente com n4 > 0).";
                return t;
            }
            ++t.edgeLinks;
            break;
        }
        case LinkGroup::CenterCenter:
            tp = 4; // l. 154, 161, 168
            ++t.centerCenterLinks;
            break;
        case LinkGroup::CenterVertex:
            tp = 0; // l. 180
            ++t.diagonalLinks;
            break;
        }
        if (e.id != std::int64_t(t.linkTp.size()) + 1)
        {
            t.message = "Sequência de ligações com IDs não contínuos.";
            return t;
        }
        t.linkTp.push_back(std::int8_t(tp));
        ++t.countByTp[std::size_t(tp)];
    }
    t.ok = true;
    return t;
}

NodalMasses computeNodalMasses(const Discretization& d, const LatticeTopology& t, double density)
{
    NodalMasses m;
    if (!t.ok || std::int64_t(t.vertexCubeCount.size()) != d.vertexNodeCount())
    {
        m.message = "Classificação da rede ausente ou incompatível com a discretização.";
        return m;
    }
    if (!(std::isfinite(density) && density > 0.0))
    {
        m.message = "A densidade ROH deve ser finita e positiva.";
        return m;
    }
    m.lcoMeters = d.grid().lco / 1000.0; // mm → m (mesma conversão de dem::units::mmToM)
    m.density = density;
    m.cubeMass = std::pow(m.lcoMeters, 3) * density / 2.0; // mss = LCO^3 * ROH / 2 (l. 195)
    if (!(std::isfinite(m.cubeMass) && m.cubeMass > 0.0))
    {
        m.message = "A massa de referência LCO³·ROH/2 não é representável numericamente.";
        return m;
    }
    const std::int64_t nv = d.vertexNodeCount();
    m.mass.resize(std::size_t(d.totalNodeCount()));
    for (std::int64_t n = 0; n < nv; ++n)
    {
        m.mass[std::size_t(n)] = m.cubeMass * t.vertexCubeCount[std::size_t(n)] / 8.0; // l. 207
        m.total += m.mass[std::size_t(n)];
    }
    for (std::int64_t n = nv; n < d.totalNodeCount(); ++n)
    {
        m.mass[std::size_t(n)] = m.cubeMass; // l. 223
        m.total += m.mass[std::size_t(n)];
    }
    m.ok = true;
    return m;
}

std::vector<MassCommand> massCommands(const NodalMasses& m, std::int64_t bondCount)
{
    std::vector<MassCommand> out;
    out.reserve(m.mass.size());
    for (std::size_t i = 0; i < m.mass.size(); ++i)
    {
        const std::int64_t n = std::int64_t(i) + 1;
        out.push_back({n, bondCount + n, bondCount + n, m.mass[i]});
    }
    return out;
}

std::string matlabG(double value)
{
    char buf[64];
    std::snprintf(buf, sizeof buf, "%g", value);
    return buf;
}

std::string matlabMassBlock(const NodalMasses& m, std::int64_t bondCount)
{
    std::string s;
    s += "\n";
    s += "!************************NODAL DENSITY***********\n";
    s += "! Define o Elemento de massa MASS166\n";
    s += "ET,2,166\n";
    for (const MassCommand& c : massCommands(m, bondCount))
    {
        s += "R," + std::to_string(c.realId) + "," + matlabG(c.mass) + "\n";
        s += "TYPE,2\n";
        s += "REAL," + std::to_string(c.realId) + "\n";
        s += "E," + std::to_string(c.nodeId) + "\n";
    }
    return s;
}

} // namespace cad::discretization
