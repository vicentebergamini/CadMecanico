// E5.7.4b.3 sem interface — transcrição de malha_por_mascara.m: classificação tp das barras, cVert
// e massas nodais (MASS166), sobre a discretização existente:
//  - comparação com o exemplo MATLAB DEM_DYNA.txt (caixa 4 × 4 × 2, LCO = 1 mm, ROH = 1240):
//    tp das 490 barras (extraído do arquivo pela lei atribuída a cada barra em EMODIF e pela razão
//    FP/EEP de cada lei), as 107 massas R (formato %g) e o texto do bloco de massas;
//  - verificação independente do tp e do cVert (enumeração das 12 arestas e 8 vértices de cada
//    cubo ativo) em caixas, seção L, viga I e retangular vazada (máscaras de mascara_secao.m),
//    cubo isolado, cubos que se tocam só por aresta ou por vértice e máscara pseudoaleatória;
//  - tp = 3 em cantos reentrantes (apenas classificado), invariantes Σtp(arestas) = 12·Nc,
//    Σ cVert = 8·Nc, diagonais = 8·Nc e conservação da massa ROH·LCO³·Nc;
//  - estruturas de diagnóstico das leis (dem/LawDiagnostics.h).
#include "TestCheck.h"

#include "dem/LawDiagnostics.h"
#include "discretization/ApdlExporter.h"
#include "discretization/LatticeTopology.h"

#include <cmath>
#include <cstdio>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

using namespace cad::discretization;

namespace {

bool approx(double value, double expected, double relative)
{
    return std::fabs(value - expected) <= relative * std::fabs(expected);
}

struct Mask
{
    int nx = 0, ny = 0, nz = 0;
    std::vector<std::uint8_t> on; // ordem do índice linear da grade (X mais rápido)
    bool at(int i, int j, int k) const
    {
        return i >= 0 && j >= 0 && k >= 0 && i < nx && j < ny && k < nz
               && on[std::size_t((std::int64_t(k) * ny + j) * nx + i)] != 0;
    }
    int count() const
    {
        int n = 0;
        for (auto v : on)
            n += v ? 1 : 0;
        return n;
    }
};

Mask makeMask(int nx, int ny, int nz, const std::function<bool(int, int, int)>& active)
{
    Mask m{nx, ny, nz, std::vector<std::uint8_t>(std::size_t(nx) * ny * nz, 0)};
    for (int k = 0; k < nz; ++k)
        for (int j = 0; j < ny; ++j)
            for (int i = 0; i < nx; ++i)
                m.on[std::size_t((std::int64_t(k) * ny + j) * nx + i)] = active(i, j, k) ? 1 : 0;
    return m;
}

Discretization build(const Mask& m, double lcoMm)
{
    VoxelGrid g = VoxelGrid::make(gp_XYZ(0.0, 0.0, 0.0), lcoMm);
    g.nx = m.nx;
    g.ny = m.ny;
    g.nz = m.nz;
    Parameters p;
    p.lco = lcoMm;
    p.criterion = InclusionCriterion::CenterInside;
    Discretization d;
    const bool ok = d.build(g, p, m.on);
    CAD_CHECK(ok);
    return d;
}

// Seções de mascara_secao.m (plano 'Z': U = X, V = Y), centros das células em metros.
Mask vigaI(int nx, int ny, int nz, double lco, double tf, double tw)
{
    const double wu = nx * lco, wv = ny * lco, cu = wu / 2, cv = wv / 2;
    return makeMask(nx, ny, nz, [&](int i, int j, int) {
        const double u = (i + 1 - 0.5) * lco, v = (j + 1 - 0.5) * lco;
        const bool mesa = std::fabs(v - cv) >= wv / 2 - tf;
        const bool alma = std::fabs(u - cu) <= tw / 2;
        return mesa || alma;
    });
}

Mask retangularVazado(int nx, int ny, int nz, double lco, double e)
{
    const double wu = nx * lco, wv = ny * lco, cu = wu / 2, cv = wv / 2;
    return makeMask(nx, ny, nz, [&](int i, int j, int) {
        const double u = (i + 1 - 0.5) * lco, v = (j + 1 - 0.5) * lco;
        const bool oco = std::fabs(u - cu) < wu / 2 - e && std::fabs(v - cv) < wv / 2 - e;
        return !oco;
    });
}

// Verificação independente: enumera as 12 arestas e os 8 vértices de cada cubo ativo.
struct Reference
{
    std::map<std::pair<std::int64_t, std::int64_t>, int> edgeCubes; // (ponto a, ponto b) → nº de cubos
    std::map<std::int64_t, int> vertexCubes;                         // ponto da rede → nº de cubos
    int faceNeighbours = 0;                                          // pares de cubos vizinhos por face
};

Reference reference(const Mask& m)
{
    Reference r;
    const auto point = [&](int i, int j, int k) { return (std::int64_t(k) * (m.ny + 1) + j) * (m.nx + 1) + i; };
    for (int k = 0; k < m.nz; ++k)
        for (int j = 0; j < m.ny; ++j)
            for (int i = 0; i < m.nx; ++i)
            {
                if (!m.at(i, j, k))
                    continue;
                for (int c = 0; c < 8; ++c)
                    ++r.vertexCubes[point(i + (c & 1), j + ((c >> 1) & 1), k + ((c >> 2) & 1))];
                for (int a = 0; a < 2; ++a)
                    for (int b = 0; b < 2; ++b)
                    {
                        ++r.edgeCubes[{point(i, j + a, k + b), point(i + 1, j + a, k + b)}];
                        ++r.edgeCubes[{point(i + a, j, k + b), point(i + a, j + 1, k + b)}];
                        ++r.edgeCubes[{point(i + a, j + b, k), point(i + a, j + b, k + 1)}];
                    }
                r.faceNeighbours += (m.at(i + 1, j, k) ? 1 : 0) + (m.at(i, j + 1, k) ? 1 : 0) + (m.at(i, j, k + 1) ? 1 : 0);
            }
    return r;
}

// Classifica, confere com a referência independente e com os invariantes; devolve a topologia.
LatticeTopology checkTopology(const char* name, const Mask& m, const Discretization& d)
{
    const LatticeTopology t = classifyLattice(d);
    CAD_CHECK(t.ok);
    if (!t.ok)
    {
        std::printf("  %s: %s\n", name, t.message.c_str());
        return t;
    }
    const Reference r = reference(m);
    const int nc = m.count();
    CAD_CHECK(d.cubeCount() == nc);

    // cVert por vértice.
    bool vertexOk = std::int64_t(r.vertexCubes.size()) == d.vertexNodeCount();
    std::int64_t sumCvert = 0;
    for (std::size_t v = 0; v < t.vertexCubeCount.size(); ++v)
    {
        const auto it = r.vertexCubes.find(d.vertexPoints()[v]);
        vertexOk = vertexOk && it != r.vertexCubes.end() && it->second == t.vertexCubeCount[v];
        sumCvert += t.vertexCubeCount[v];
    }
    CAD_CHECK(vertexOk);
    CAD_CHECK(sumCvert == 8LL * nc);

    // tp por ligação.
    bool linkOk = true;
    std::int64_t sumEdgeTp = 0;
    ApdlLinkSequence seq(d);
    LinkElement e;
    while (seq.next(e))
    {
        const int tp = t.linkTp[std::size_t(e.id - 1)];
        if (e.group == LinkGroup::VertexEdge)
        {
            const auto it = r.edgeCubes.find({d.vertexPoints()[std::size_t(e.n1 - 1)], d.vertexPoints()[std::size_t(e.n2 - 1)]});
            linkOk = linkOk && it != r.edgeCubes.end() && it->second == tp;
            sumEdgeTp += tp;
        }
        else if (e.group == LinkGroup::CenterCenter)
            linkOk = linkOk && tp == 4;
        else
            linkOk = linkOk && tp == 0;
    }
    CAD_CHECK(linkOk);
    CAD_CHECK(t.edgeLinks == std::int64_t(r.edgeCubes.size()));
    CAD_CHECK(sumEdgeTp == 12LL * nc);
    CAD_CHECK(t.centerCenterLinks == r.faceNeighbours);
    CAD_CHECK(t.diagonalLinks == 8LL * nc);
    CAD_CHECK(t.countByTp[0] == t.diagonalLinks);
    CAD_CHECK(t.links() == ApdlLinkSequence::count(d).total());
    std::printf("  %-28s Nc = %4d  barras = %5lld  tp0 = %4lld  tp1 = %4lld  tp2 = %4lld  tp3 = %4lld  tp4 = %4lld\n", name, nc,
                static_cast<long long>(t.links()), static_cast<long long>(t.countByTp[0]),
                static_cast<long long>(t.countByTp[1]), static_cast<long long>(t.countByTp[2]),
                static_cast<long long>(t.countByTp[3]), static_cast<long long>(t.countByTp[4]));
    return t;
}

void checkMassConservation(const Discretization& d, const LatticeTopology& t, double density)
{
    const NodalMasses m = computeNodalMasses(d, t, density);
    CAD_CHECK(m.ok);
    const double lco = d.grid().lco / 1000.0;
    CAD_CHECK(approx(m.total, density * lco * lco * lco * double(d.cubeCount()), 1e-12));
    CAD_CHECK(approx(m.cubeMass, lco * lco * lco * density / 2.0, 1e-14));
    bool ok = true;
    for (std::int64_t n = 0; n < d.totalNodeCount(); ++n)
    {
        const double expected = n < d.vertexNodeCount() ? m.cubeMass * t.vertexCubeCount[std::size_t(n)] / 8.0 : m.cubeMass;
        ok = ok && m.mass[std::size_t(n)] == expected && m.mass[std::size_t(n)] > 0.0;
    }
    CAD_CHECK(ok);
}

// ------------------------------------------------------------------------------------------------
// Referência MATLAB: DEM_DYNA.txt gerado por DEM_HIAGO_PRINCIPAL.m (caixa 4 × 4 × 2, LCO = 1e-3,
// ROH = 1240). tp de cada barra 1..490, obtido de EMODIF (lei de cada barra) e da razão FP/EEP de cada
// lei (ENR/4, ENR/2, ENR por LCO e EDG por LCO·√3/2 identificam tp = 1, 2, 4 e 0).
const char* kMatlabTp =
    "11112222222222221111222244444444444422221111222222222222111112221122211222112221244422444224442244"
    "42122211222112221122211222124442244422444212221122212444224442244421222144400000000444000000004440"
    "00000004400000000444000000004440000000044400000000440000000044400000000444000000004440000000044000"
    "00000440000000044000000004400000000400000000440000000044000000004400000000400000000440000000044000"
    "00000440000000040000000044000000004400000000440000000040000000040000000040000000040000000000000000";

// Massas R,491..597 (formato %g), na ordem dos nós 1..107.
const char* kMatlabMass =
    "7.75e-08,1.55e-07,1.55e-07,1.55e-07,7.75e-08,1.55e-07,3.1e-07,3.1e-07,3.1e-07,1.55e-07,1.55e-07,3.1e-07,"
    "3.1e-07,3.1e-07,1.55e-07,1.55e-07,3.1e-07,3.1e-07,3.1e-07,1.55e-07,7.75e-08,1.55e-07,1.55e-07,1.55e-07,"
    "7.75e-08,1.55e-07,3.1e-07,3.1e-07,3.1e-07,1.55e-07,3.1e-07,6.2e-07,6.2e-07,6.2e-07,3.1e-07,3.1e-07,"
    "6.2e-07,6.2e-07,6.2e-07,3.1e-07,3.1e-07,6.2e-07,6.2e-07,6.2e-07,3.1e-07,1.55e-07,3.1e-07,3.1e-07,3.1e-07,"
    "1.55e-07,7.75e-08,1.55e-07,1.55e-07,1.55e-07,7.75e-08,1.55e-07,3.1e-07,3.1e-07,3.1e-07,1.55e-07,1.55e-07,"
    "3.1e-07,3.1e-07,3.1e-07,1.55e-07,1.55e-07,3.1e-07,3.1e-07,3.1e-07,1.55e-07,7.75e-08,1.55e-07,1.55e-07,"
    "1.55e-07,7.75e-08,6.2e-07,6.2e-07,6.2e-07,6.2e-07,6.2e-07,6.2e-07,6.2e-07,6.2e-07,6.2e-07,6.2e-07,"
    "6.2e-07,6.2e-07,6.2e-07,6.2e-07,6.2e-07,6.2e-07,6.2e-07,6.2e-07,6.2e-07,6.2e-07,6.2e-07,6.2e-07,6.2e-07,"
    "6.2e-07,6.2e-07,6.2e-07,6.2e-07,6.2e-07,6.2e-07,6.2e-07,6.2e-07,6.2e-07";

// Início do bloco de massas de DEM_DYNA.txt (linhas 606 a 617).
const char* kMatlabMassBlockStart = "\n"
                                    "!************************NODAL DENSITY***********\n"
                                    "! Define o Elemento de massa MASS166\n"
                                    "ET,2,166\n"
                                    "R,491,7.75e-08\n"
                                    "TYPE,2\n"
                                    "REAL,491\n"
                                    "E,1\n"
                                    "R,492,1.55e-07\n"
                                    "TYPE,2\n"
                                    "REAL,492\n"
                                    "E,2\n";

void testMatlabExample()
{
    std::printf("[1] exemplo MATLAB DEM_DYNA.txt (4 x 4 x 2, LCO = 1 mm, ROH = 1240)\n");
    const Mask m = makeMask(4, 4, 2, [](int, int, int) { return true; });
    const Discretization d = build(m, 1.0);
    CAD_CHECK(d.totalNodeCount() == 107);
    const LatticeTopology t = checkTopology("caixa 4x4x2", m, d);
    CAD_CHECK(t.links() == 490);

    std::string tp;
    for (std::int8_t v : t.linkTp)
        tp += char('0' + v);
    CAD_CHECK(tp == kMatlabTp);
    CAD_CHECK(t.countByTp[0] == 256 && t.countByTp[1] == 40 && t.countByTp[2] == 88 && t.countByTp[3] == 0
              && t.countByTp[4] == 106);

    const NodalMasses masses = computeNodalMasses(d, t, 1240.0);
    CAD_CHECK(masses.ok);
    std::string list;
    for (std::size_t i = 0; i < masses.mass.size(); ++i)
        list += (i ? "," : "") + matlabG(masses.mass[i]);
    CAD_CHECK(list == kMatlabMass);
    CAD_CHECK(approx(masses.total, 1240.0 * 1e-9 * 32.0, 1e-12)); // 3,968e-5 kg
    CAD_CHECK(approx(masses.cubeMass, 6.2e-7, 1e-12));

    const std::vector<MassCommand> cmds = massCommands(masses, t.links());
    CAD_CHECK(cmds.size() == 107);
    CAD_CHECK(cmds.front().realId == 491 && cmds.front().elementId == 491 && cmds.front().nodeId == 1);
    CAD_CHECK(cmds.back().realId == 597 && cmds.back().elementId == 597 && cmds.back().nodeId == 107);

    const std::string block = matlabMassBlock(masses, t.links());
    CAD_CHECK(block.compare(0, std::string(kMatlabMassBlockStart).size(), kMatlabMassBlockStart) == 0);
    const std::string lastBlock = "R,597,6.2e-07\nTYPE,2\nREAL,597\nE,107\n"; // linhas 1034 a 1037
    CAD_CHECK(block.size() > lastBlock.size() && block.compare(block.size() - lastBlock.size(), lastBlock.size(), lastBlock) == 0);
    std::size_t lines = 0;
    for (char ch : block)
        lines += ch == '\n' ? 1 : 0;
    CAD_CHECK(lines == 4 + 4 * 107); // linhas 606 a 1037 de DEM_DYNA.txt
    std::printf("  tp identico ao MATLAB (490 barras); 107 massas identicas (%%g); massa total = %.10g kg\n", masses.total);
}

void testSections()
{
    std::printf("[2] secoes, fronteiras e tp = 3 (apenas classificado)\n");
    const double lco = 1e-3;
    // Caixa não cúbica.
    {
        const Mask m = makeMask(3, 2, 5, [](int, int, int) { return true; });
        const Discretization d = build(m, 1.0);
        const LatticeTopology t = checkTopology("caixa 3x2x5", m, d);
        CAD_CHECK(t.countByTp[3] == 0);
        checkMassConservation(d, t, 1240.0);
    }
    // Seção L (um canto reentrante ao longo de Z).
    {
        const int nz = 3;
        const Mask m = makeMask(4, 4, nz, [](int i, int j, int) { return !(i >= 2 && j >= 2); });
        const Discretization d = build(m, 1.0);
        const LatticeTopology t = checkTopology("secao L 4x4x3", m, d);
        CAD_CHECK(t.countByTp[3] == nz);
        checkMassConservation(d, t, 1240.0);
    }
    // Viga I (mascara_secao.m, tipo 'viga_I', plano Z): quatro cantos reentrantes alma/mesa.
    {
        const int nz = 3;
        const Mask m = vigaI(8, 8, nz, lco, 2e-3, 2e-3);
        const Discretization d = build(m, 1.0);
        const LatticeTopology t = checkTopology("viga I 8x8x3", m, d);
        CAD_CHECK(m.count() == (4 * 8 + 2 * 4) * nz);
        CAD_CHECK(t.countByTp[3] == 4 * nz);
        checkMassConservation(d, t, 7850.0);
    }
    // Retangular vazado (quatro cantos reentrantes no furo).
    {
        const int nz = 2;
        const Mask m = retangularVazado(6, 6, nz, lco, 1e-3);
        const Discretization d = build(m, 1.0);
        const LatticeTopology t = checkTopology("retangular vazado 6x6x2", m, d);
        CAD_CHECK(m.count() == (36 - 16) * nz);
        CAD_CHECK(t.countByTp[3] == 4 * nz);
        checkMassConservation(d, t, 1240.0);
    }
    // Cubo isolado: 12 arestas tp = 1, 8 vértices com cVert = 1, massa ROH·LCO³.
    {
        const Mask m = makeMask(1, 1, 1, [](int, int, int) { return true; });
        const Discretization d = build(m, 1.0);
        const LatticeTopology t = checkTopology("cubo isolado", m, d);
        CAD_CHECK(t.countByTp[1] == 12 && t.edgeLinks == 12 && t.centerCenterLinks == 0 && t.diagonalLinks == 8);
        const NodalMasses ms = computeNodalMasses(d, t, 1000.0);
        CAD_CHECK(approx(ms.total, 1000.0 * 1e-9, 1e-14));
        CAD_CHECK(ms.mass[0] == ms.cubeMass / 8.0 && ms.mass[8] == ms.cubeMass);
    }
    // Cubos que se tocam só por uma aresta (tp = 2 nessa aresta) e só por um vértice (cVert = 2).
    {
        const Mask a = makeMask(2, 2, 1, [](int i, int j, int) { return i == j; });
        const Discretization da = build(a, 1.0);
        const LatticeTopology ta = checkTopology("cubos por aresta", a, da);
        CAD_CHECK(ta.countByTp[2] == 1 && ta.centerCenterLinks == 0);
        checkMassConservation(da, ta, 1240.0);
        const Mask b = makeMask(2, 2, 2, [](int i, int j, int k) { return (i == 0 && j == 0 && k == 0) || (i == 1 && j == 1 && k == 1); });
        const Discretization db = build(b, 1.0);
        const LatticeTopology tb = checkTopology("cubos por vertice", b, db);
        CAD_CHECK(tb.countByTp[1] == 24 && tb.countByTp[2] == 0);
        int shared = 0;
        for (std::uint8_t c : tb.vertexCubeCount)
            shared += c == 2 ? 1 : 0;
        CAD_CHECK(shared == 1);
        checkMassConservation(db, tb, 1240.0);
    }
    // Máscara pseudoaleatória (LCG determinístico) com LCO não inteiro.
    {
        std::uint32_t s = 12345u;
        const Mask m = makeMask(7, 5, 4, [&](int, int, int) {
            s = s * 1664525u + 1013904223u;
            return (s >> 16) % 3 != 0;
        });
        const Discretization d = build(m, 0.7);
        const LatticeTopology t = checkTopology("aleatoria 7x5x4 (0,7 mm)", m, d);
        checkMassConservation(d, t, 1240.0);
        const NodalMasses ms = computeNodalMasses(d, t, 1240.0);
        CAD_CHECK(approx(ms.cubeMass, std::pow(0.7e-3, 3) * 1240.0 / 2.0, 1e-14));
    }
}

void testInvalidInputs()
{
    std::printf("[3] entradas invalidas\n");
    const Mask m = makeMask(2, 2, 2, [](int, int, int) { return true; });
    const Discretization d = build(m, 1.0);
    const LatticeTopology t = classifyLattice(d);
    for (double rho : {0.0, -1.0, std::nan(""), HUGE_VAL})
        CAD_CHECK(!computeNodalMasses(d, t, rho).ok);
    CAD_CHECK(!computeNodalMasses(d, LatticeTopology{}, 1240.0).ok);
    CAD_CHECK(!classifyLattice(Discretization{}).ok);
}

void testDiagnosticsStructures()
{
    std::printf("[4] estruturas de diagnostico das leis\n");
    using namespace cad::dem;
    LawValidationReport r;
    CAD_CHECK(!r.blocksExport());
    r.issues.push_back({LawIssueKind::ReferenceZero, 0, 3, "EEP", 0.0, "valor intermediario nulo da referencia"});
    CAD_CHECK(!r.blocksExport() && r.count(LawIssueKind::ReferenceZero) == 1);
    r.issues.push_back({LawIssueKind::InvalidExported, 17, 3, "FP", 0.0, "lei nula atribuida a elemento exportavel"});
    CAD_CHECK(r.blocksExport() && r.count(LawIssueKind::InvalidExported) == 1);
}

} // namespace

int main()
{
    testMatlabExample();
    testSections();
    testInvalidInputs();
    testDiagnosticsStructures();
    return cad::test::finish("test_e574b3");
}
