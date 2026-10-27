// E5.7.2.3 — discretização com projeção dos nós sobre o contorno (ajuste geométrico) (núcleo, sem Qt).
//   P  teste principal: placa 50 × 50 × 1 mm com furo central passante Ø20, lco = 0,5 mm;
//      caso A (cubo totalmente contido) × caso B (projeção): distância dos nós de fronteira à
//      superfície cilíndrica (calculada analiticamente, independente do algoritmo), células
//      invertidas, ligações atravessando o furo, continuidade da conectividade, nós projetados,
//      volume e integridade das exportações ASCII e APDL;
//   1  bloco sem furo; 2 placa com furo deslocado; 3 placa com dois furos; 4 furo de pequeno
//   diâmetro; 5 furo próximo à borda; 6 furo inclinado; 7 cavidade interna; 8 sólidos com faces
//   curvas (cilindro e esfera); 9 sólido deslocado da origem; 10 sólido rotacionado;
//   + as três opções antigas continuam inalteradas; qualidade do hexaedro (unitário, invertido);
//   comprimentos das ligações; regressão das exportações (numeração e conectividade).
//
// Verificações independentes do algoritmo em todos os casos: cada célula reavaliada a partir das
// coordenadas dos nós (sem inversão, volume > 0, nó central interior e igual à média dos vértices),
// topologia idêntica à do critério "centro no material" (mesmos cubos, nós e ligações), nenhuma
// ligação com extremidade deslocada passando pelo vazio mais fundo que suas extremidades.
//
// Saídas em %TEMP%\cadmecanico_e5723; com CAD_E5723_OUTPUT_DIR definido, os arquivos do caso
// principal e o resumo dos resultados também são gravados nesse diretório.
#include "TestCheck.h"

#include "discretization/ApdlExporter.h"
#include "discretization/BoundaryProjector.h"
#include "discretization/NodeAsciiExporter.h"
#include "discretization/VoxelDiscretizer.h"

#include <BRepAlgoAPI_Cut.hxx>
#include <BRepBuilderAPI_MakeVertex.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepClass3d_SolidClassifier.hxx>
#include <BRepExtrema_DistShapeShape.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRepPrimAPI_MakeSphere.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <gp_Ax2.hxx>
#include <gp_Trsf.hxx>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdarg>
#include <memory>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <numeric>
#include <sstream>
#include <string>
#include <vector>

using namespace cad::discretization;
namespace fs = std::filesystem;

namespace {

constexpr double kPi = 3.14159265358979323846;

std::ostringstream g_summary; // resumo gravado em CAD_E5723_OUTPUT_DIR

void log(const char* fmt, ...)
{
    char buffer[2048];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buffer, sizeof buffer, fmt, args);
    va_end(args);
    std::fputs(buffer, stdout);
    g_summary << buffer;
}

bool same(const gp_XYZ& a, const gp_XYZ& b)
{
    return a.X() == b.X() && a.Y() == b.Y() && a.Z() == b.Z();
}

TopoDS_Shape box(double x0, double y0, double z0, double dx, double dy, double dz)
{
    return BRepPrimAPI_MakeBox(gp_Pnt(x0, y0, z0), dx, dy, dz).Shape();
}

TopoDS_Shape cut(const TopoDS_Shape& a, const TopoDS_Shape& b)
{
    BRepAlgoAPI_Cut op(a, b);
    CAD_CHECK(op.IsDone());
    return op.Shape();
}

// Cilindro de raio r com eixo passando por 'o' na direção 'dir', de comprimento 'len' centrado em o.
TopoDS_Shape cylinder(const gp_Pnt& o, const gp_Dir& dir, double r, double len)
{
    const gp_Pnt start(o.XYZ() - dir.XYZ() * (0.5 * len));
    return BRepPrimAPI_MakeCylinder(gp_Ax2(start, dir), r, len).Shape();
}

TopoDS_Shape translate(const TopoDS_Shape& s, const gp_Vec& v)
{
    gp_Trsf t;
    t.SetTranslation(v);
    return BRepBuilderAPI_Transform(s, t, true).Shape();
}

DiscretizeResult discretize(const TopoDS_Shape& s, double lco, InclusionCriterion c)
{
    const DiscretizeResult r = VoxelDiscretizer::run(s, Parameters{lco, c});
    CAD_CHECK(r.ok && r.data && !r.data->empty());
    if (!r.ok)
        std::printf("  falha: %s\n", r.message.c_str());
    return r;
}

fs::path outputDir()
{
    fs::path dir = fs::temp_directory_path() / "cadmecanico_e5723";
    fs::create_directories(dir);
    return dir;
}

fs::path userOutputDir()
{
    const char* env = std::getenv("CAD_E5723_OUTPUT_DIR");
    if (!env || !*env)
        return {};
    fs::path dir(env);
    fs::create_directories(dir);
    return dir;
}

// Furo/cavidade com geometria analítica conhecida (para medidas independentes do algoritmo).
struct Hole
{
    gp_XYZ origin;
    gp_XYZ dir; // unitário (cilindro); nulo = esfera centrada em 'origin'
    double radius = 0.0;

    // Profundidade dentro do vazio (> 0 dentro do furo) e distância à superfície.
    double depth(const gp_XYZ& p) const
    {
        const gp_XYZ d = p - origin;
        const double r = dir.Modulus() > 0.0 ? (d - dir * d.Dot(dir)).Modulus() : d.Modulus();
        return radius - r;
    }
};

std::array<gp_XYZ, 8> cellPoints(const Discretization& d, std::int64_t index, std::int64_t* centerId = nullptr)
{
    const CubeInfo c = d.cube(index);
    std::array<gp_XYZ, 8> pts;
    for (int n = 0; n < 8; ++n)
        pts[std::size_t(n)] = d.node(c.vertexIds[std::size_t(n)]).position;
    if (centerId)
        *centerId = c.centerId;
    return pts;
}

// Vértices de fronteira de um furo: tocam uma célula não aceita cujo centro (grade) está no vazio.
std::vector<std::int64_t> holeBoundaryNodes(const Discretization& d, const Hole& h)
{
    std::vector<std::int64_t> out;
    const VoxelGrid& g = d.grid();
    for (std::int64_t id = 1; id <= d.vertexNodeCount(); ++id)
    {
        const NodeInfo n = d.node(id);
        bool touches = false;
        for (int dk = -1; dk <= 0 && !touches; ++dk)
            for (int dj = -1; dj <= 0 && !touches; ++dj)
                for (int di = -1; di <= 0 && !touches; ++di)
                {
                    const int i = n.i + di, j = n.j + dj, k = n.k + dk;
                    if (i < 0 || j < 0 || k < 0 || i >= g.nx || j >= g.ny || k >= g.nz || d.cellAccepted(i, j, k))
                        continue;
                    touches = h.depth(g.center(i, j, k)) > 0.0;
                }
        if (touches)
            out.push_back(id);
    }
    return out;
}

struct Deviation
{
    double max = 0.0, mean = 0.0, maxInside = 0.0;
    std::int64_t nodes = 0;
};

Deviation holeDeviation(const Discretization& d, const Hole& h)
{
    Deviation dev;
    double sum = 0.0;
    for (std::int64_t id : holeBoundaryNodes(d, h))
    {
        const double depth = h.depth(d.node(id).position);
        dev.max = std::max(dev.max, std::fabs(depth));
        dev.maxInside = std::max(dev.maxInside, depth);
        sum += std::fabs(depth);
        ++dev.nodes;
    }
    dev.mean = dev.nodes ? sum / double(dev.nodes) : 0.0;
    return dev;
}

// Ligações que atravessam o furo: algum ponto interno mais fundo no vazio que as extremidades.
std::int64_t linksCrossingHole(const Discretization& d, const Hole& h, double tol)
{
    std::int64_t crossing = 0;
    ApdlLinkSequence seq(d);
    LinkElement e;
    while (seq.next(e))
    {
        const gp_XYZ a = d.node(e.n1).position, b = d.node(e.n2).position;
        const double ends = std::max({h.depth(a), h.depth(b), 0.0});
        for (double t : {0.25, 0.5, 0.75})
            if (h.depth(a + (b - a) * t) > ends + tol)
            {
                ++crossing;
                break;
            }
    }
    return crossing;
}

// Reavaliação independente de todas as células a partir das coordenadas dos nós.
struct CellCheck
{
    std::int64_t inverted = 0, nonPositive = 0, centerOutside = 0, centerMismatch = 0, belowThreshold = 0;
    double minScaledJacobian = 1.0;
    double volume = 0.0;
};

CellCheck checkCells(const Discretization& d)
{
    CellCheck c;
    for (std::int64_t n = 0; n < d.cubeCount(); ++n)
    {
        std::int64_t centerId = 0;
        const auto pts = cellPoints(d, n, &centerId);
        const HexQuality q = evaluateHex(pts);
        gp_XYZ mean(0, 0, 0);
        for (const gp_XYZ& p : pts)
            mean += p;
        mean /= 8.0;
        c.inverted += q.minScaledJacobian <= 0.0 ? 1 : 0;
        c.belowThreshold += q.minScaledJacobian < 0.2 - 1e-12 ? 1 : 0;
        c.nonPositive += q.volume <= 0.0 ? 1 : 0;
        c.centerOutside += q.centerInside ? 0 : 1;
        c.centerMismatch += (d.node(centerId).position - mean).Modulus() > 1e-9 * d.grid().lco ? 1 : 0;
        c.minScaledJacobian = std::min(c.minScaledJacobian, q.minScaledJacobian);
        c.volume += q.volume;
    }
    return c;
}

bool sameTopology(const Discretization& a, const Discretization& b)
{
    if (a.cubeCells() != b.cubeCells() || a.vertexPoints() != b.vertexPoints())
        return false;
    ApdlLinkSequence sa(a), sb(b);
    LinkElement ea, eb;
    for (;;)
    {
        const bool ha = sa.next(ea), hb = sb.next(eb);
        if (ha != hb)
            return false;
        if (!ha)
            return true;
        if (ea.id != eb.id || ea.n1 != eb.n1 || ea.n2 != eb.n2)
            return false;
    }
}

std::int64_t components(const Discretization& d)
{
    std::vector<std::int64_t> parent(std::size_t(d.totalNodeCount() + 1));
    std::iota(parent.begin(), parent.end(), 0);
    std::function<std::int64_t(std::int64_t)> find = [&](std::int64_t x) {
        while (parent[std::size_t(x)] != x)
            x = parent[std::size_t(x)] = parent[std::size_t(parent[std::size_t(x)])];
        return x;
    };
    ApdlLinkSequence seq(d);
    LinkElement e;
    while (seq.next(e))
        parent[std::size_t(find(e.n1))] = find(e.n2);
    std::int64_t n = 0;
    for (std::int64_t id = 1; id <= d.totalNodeCount(); ++id)
        n += find(id) == id ? 1 : 0;
    return n;
}

// Ligações com extremidade deslocada que passam pelo vazio mais fundo que as extremidades
// (classificador e distância do Open CASCADE, independentes do projetor).
std::int64_t linksThroughVoid(const Discretization& d, const TopoDS_Shape& shape, double tol)
{
    BRepClass3d_SolidClassifier cls(TopoDS::Solid(TopExp_Explorer(shape, TopAbs_SOLID).Current()));
    auto depth = [&](const gp_XYZ& p) {
        cls.Perform(gp_Pnt(p), 1e-7);
        if (cls.State() != TopAbs_OUT)
            return 0.0;
        BRepExtrema_DistShapeShape dist(BRepBuilderAPI_MakeVertex(gp_Pnt(p)).Vertex(), shape);
        return dist.IsDone() ? dist.Value() : 0.0;
    };
    const VoxelGrid& g = d.grid();
    auto movedNode = [&](std::int64_t id) {
        const NodeInfo n = d.node(id);
        const gp_XYZ grid = n.center ? g.center(n.i, n.j, n.k) : g.vertex(n.i, n.j, n.k);
        return (n.position - grid).Modulus() > 0.0;
    };
    std::vector<double> nodeDepth(std::size_t(d.totalNodeCount() + 1), -1.0);
    auto endDepth = [&](std::int64_t id) {
        double& v = nodeDepth[std::size_t(id)];
        if (v < 0.0)
            v = depth(d.node(id).position);
        return v;
    };
    std::int64_t crossing = 0;
    ApdlLinkSequence seq(d);
    LinkElement e;
    while (seq.next(e))
    {
        if (!movedNode(e.n1) && !movedNode(e.n2))
            continue;
        const gp_XYZ mid = (d.node(e.n1).position + d.node(e.n2).position) * 0.5;
        // Triagem barata: fora do sólido além da tolerância 'tol' (classificação com tolerância);
        // só então a distância exata é calculada.
        cls.Perform(gp_Pnt(mid), tol);
        if (cls.State() != TopAbs_OUT)
            continue;
        if (depth(mid) > tol + std::max(endDepth(e.n1), endDepth(e.n2)))
            ++crossing;
    }
    return crossing;
}

struct ExportCheck
{
    bool ascii = false, apdl = false, coordinatesUpdated = false;
};

ExportCheck checkExports(const Discretization& d, const std::string& name, const fs::path& extraDir = {})
{
    ExportCheck out;
    const fs::path dir = outputDir();
    const fs::path ascii = dir / (name + "_nodes.txt");
    const fs::path apdl = dir / (name + "_PREP7.inp");
    const NodeExportResult ra = NodeAsciiExporter().write(d, ascii, LengthUnit::Millimeter, true);
    out.ascii = ra.ok && NodeAsciiExporter::verify(ascii, d, LengthUnit::Millimeter).ok
                && NodeAsciiExporter::verify(ascii, d, LengthUnit::Millimeter).lines == d.totalNodeCount();
    ApdlOptions opts;
    opts.unit = LengthUnit::Meter;
    opts.timestamp = "01/01/2026, 00:00:00";
    opts.partName = name;
    const ApdlExportResult rp = ApdlExporter().write(d, apdl, opts, true);
    out.apdl = rp.ok && ApdlExporter::verify(apdl, d, opts).ok && rp.nodes == d.totalNodeCount()
               && rp.links.total() == ApdlLinkSequence::count(d).total();
    // As linhas dos nós deslocados contêm as coordenadas ajustadas (e não as da grade).
    std::ifstream in(ascii);
    std::string line;
    std::getline(in, line);
    std::int64_t movedChecked = 0, mismatches = 0;
    const VoxelGrid& g = d.grid();
    while (std::getline(in, line))
    {
        std::istringstream s(line);
        std::int64_t id;
        double x, y, z;
        s >> id >> x >> y >> z;
        const NodeInfo n = d.node(id);
        const gp_XYZ grid = n.center ? g.center(n.i, n.j, n.k) : g.vertex(n.i, n.j, n.k);
        if ((n.position - grid).Modulus() > 0.0)
        {
            ++movedChecked;
            mismatches += (x != n.position.X() || y != n.position.Y() || z != n.position.Z()) ? 1 : 0;
            mismatches += (x == grid.X() && y == grid.Y() && z == grid.Z()) ? 1 : 0;
        }
    }
    out.coordinatesUpdated = mismatches == 0 && (movedChecked > 0) == (d.projection().movedNodes > 0);
    if (!extraDir.empty())
    {
        fs::copy_file(ascii, extraDir / ascii.filename(), fs::copy_options::overwrite_existing);
        fs::copy_file(apdl, extraDir / apdl.filename(), fs::copy_options::overwrite_existing);
    }
    return out;
}

void logReport(const ProjectionReport& p)
{
    log("    contorno: %lld faces expostas (%lld com face CAD), %lld vertices (%lld com destino; %lld em arestas "
        "vivas, %lld em vertices CAD; %lld sem destino, %lld acima de lco)\n",
        (long long)p.boundaryFaces, (long long)p.facesWithTarget, (long long)p.boundaryVertices,
        (long long)p.targetedVertices, (long long)p.featureEdgeTargets, (long long)p.cornerTargets,
        (long long)p.unresolvedVertices, (long long)p.inadmissibleVertices);
    log("    nos: %lld projetados, %lld parciais, %lld cancelados, %lld internos suavizados, %lld nos alterados "
        "(vertices + centros)\n",
        (long long)p.projectedNodes, (long long)p.partialNodes, (long long)p.cancelledNodes,
        (long long)p.smoothedNodes, (long long)p.movedNodes);
    log("    celulas: %lld ajustadas, %lld com deformacao reduzida/recusada, %lld invalidas; %d iteracoes; "
        "ligacoes no vazio corrigidas %lld; nos coincidentes corrigidos %lld\n",
        (long long)p.adjustedCells, (long long)p.rejectedCells, (long long)p.invalidCells, p.iterations,
        (long long)p.voidCrossingLinks, (long long)p.coincidentNodes);
    log("    desvio ao contorno (projetor): antes max %.4f / medio %.4f mm; depois max %.4f / medio %.4f mm; "
        "deslocamento max %.4f mm\n",
        p.deviationBeforeMax, p.deviationBeforeMean, p.deviationAfterMax, p.deviationAfterMean, p.maxDisplacement);
    log("    qualidade: jacobiano escalado min %.3f; volume celula min %.5f / max %.5f mm3; ligacoes min %.4f / max "
        "%.4f mm (razao %.2f); tempo %.2f s\n",
        p.minScaledJacobian, p.minCellVolume, p.maxCellVolume, p.minLink, p.maxLink,
        p.minLink > 0 ? p.maxLink / p.minLink : 0.0, p.seconds);
}

// Verificações comuns a todos os casos com projeção; retorna o resultado da projeção.
struct CaseResult
{
    std::shared_ptr<Discretization> projected, base;
    double vCad = 0.0;
};

CaseResult checkCase(const char* title, const TopoDS_Shape& shape, double lco, const Hole* hole = nullptr,
                     bool expectMoves = true)
{
    log("\n[%s] lco = %g mm\n", title, lco);
    CaseResult out;
    const DiscretizeResult rb = discretize(shape, lco, InclusionCriterion::BoundaryProjection);
    const DiscretizeResult rc = discretize(shape, lco, InclusionCriterion::CenterInside);
    const DiscretizeResult ra = discretize(shape, lco, InclusionCriterion::FullyInside);
    if (!rb.ok || !rc.ok || !ra.ok)
        return out;
    out.projected = rb.data;
    out.base = rc.data;
    const Discretization& d = *rb.data;
    const ProjectionReport& p = d.projection();
    out.vCad = d.stats().solidVolume;
    CAD_CHECK(d.adjusted());
    CAD_CHECK(!rc.data->adjusted());
    logReport(p);
    // Topologia (numeração e ligações) idêntica à da grade antes da projeção.
    CAD_CHECK(sameTopology(d, *rc.data));
    CAD_CHECK(components(d) == components(*rc.data));
    // Células válidas (reavaliação independente).
    const CellCheck cc = checkCells(d);
    CAD_CHECK(cc.inverted == 0 && cc.nonPositive == 0 && cc.centerOutside == 0 && cc.centerMismatch == 0);
    CAD_CHECK(cc.belowThreshold == 0);
    CAD_CHECK(p.invalidCells == 0);
    CAD_CHECK(std::fabs(cc.volume - d.discretizedVolume()) <= 1e-9 * std::max(1.0, cc.volume));
    // Volume das células deformadas comparado ao CAD e às grades cartesianas. O critério "centro no
    // material" se beneficia da compensação entre degraus para dentro e para fora; a comparação
    // obrigatória é com a grade cartesiana "totalmente contido" (caso A do teste principal).
    const double errB = d.discretizedVolume() - out.vCad, errC = rc.data->cubeVolume() - out.vCad;
    const double errA = ra.data->cubeVolume() - out.vCad;
    log("    volume: CAD %.4f; totalmente contido %.4f (%.3f %%); centro no material %.4f (%.3f %%); projecao %.4f "
        "(%.3f %%)\n",
        out.vCad, ra.data->cubeVolume(), 100.0 * errA / out.vCad, rc.data->cubeVolume(), 100.0 * errC / out.vCad,
        d.discretizedVolume(), 100.0 * errB / out.vCad);
    CAD_CHECK(std::fabs(errB) <= std::fabs(errA) + 1e-9 * out.vCad);
    // Desvio ao contorno não aumenta.
    CAD_CHECK(p.deviationAfterMax <= p.deviationBeforeMax + 1e-12);
    CAD_CHECK(p.deviationAfterMean <= p.deviationBeforeMean + 1e-12);
    if (expectMoves)
    {
        CAD_CHECK(p.projectedNodes > 0);
        CAD_CHECK(p.deviationAfterMean < p.deviationBeforeMean);
        CAD_CHECK(std::fabs(errB) < std::fabs(errA));
    }
    // Ligações pelo vazio (classificador OCCT, independente).
    const std::int64_t voids = linksThroughVoid(d, shape, 0.1 * lco);
    log("    ligacoes com extremidade deslocada atravessando vazio: %lld\n", (long long)voids);
    CAD_CHECK(voids == 0);
    if (hole)
    {
        const Deviation a = holeDeviation(*rc.data, *hole), b = holeDeviation(d, *hole);
        const std::int64_t crossing = linksCrossingHole(d, *hole, 0.1 * lco);
        log("    furo (analitico): %lld nos de fronteira; desvio max %.4f -> %.4f mm, medio %.4f -> %.4f mm; "
            "ligacoes atravessando o furo %lld\n",
            (long long)b.nodes, a.max, b.max, a.mean, b.mean, (long long)crossing);
        CAD_CHECK(b.mean < a.mean);
        CAD_CHECK(b.max <= a.max + 1e-12);
        CAD_CHECK(crossing == 0);
    }
    const ExportCheck ex = checkExports(d, std::string("caso_") + std::to_string(std::hash<std::string>{}(title) % 100000));
    CAD_CHECK(ex.ascii && ex.apdl && ex.coordinatesUpdated);
    return out;
}

// --- Testes ---------------------------------------------------------------------------------------

void testeQualidadeHexaedro()
{
    std::array<gp_XYZ, 8> cube = {gp_XYZ(0, 0, 0), gp_XYZ(1, 0, 0), gp_XYZ(1, 1, 0), gp_XYZ(0, 1, 0),
                                  gp_XYZ(0, 0, 1), gp_XYZ(1, 0, 1), gp_XYZ(1, 1, 1), gp_XYZ(0, 1, 1)};
    HexQuality q = evaluateHex(cube);
    CAD_CHECK_NEAR(q.minScaledJacobian, 1.0, 1e-12);
    CAD_CHECK_NEAR(q.volume, 1.0, 1e-12);
    CAD_CHECK(q.centerInside);
    // Prisma inclinado (cisalhamento): volume preservado, jacobiano reduzido.
    auto sheared = cube;
    for (int n = 4; n < 8; ++n)
        sheared[std::size_t(n)] += gp_XYZ(0.5, 0, 0);
    q = evaluateHex(sheared);
    CAD_CHECK_NEAR(q.volume, 1.0, 1e-12);
    CAD_CHECK(q.minScaledJacobian > 0.8 && q.minScaledJacobian < 1.0);
    // Trapézio (volume exato do hexaedro trilinear): topo com aresta X de 0,5.
    auto tapered = cube;
    tapered[5] = gp_XYZ(0.5, 0, 1);
    tapered[6] = gp_XYZ(0.5, 1, 1);
    CAD_CHECK_NEAR(evaluateHex(tapered).volume, 0.75, 1e-12);
    // Canto empurrado através da face oposta: célula invertida.
    auto inverted = cube;
    inverted[6] = gp_XYZ(-0.2, -0.2, -0.2);
    q = evaluateHex(inverted);
    CAD_CHECK(q.minScaledJacobian < 0.0);
    // Ordem dos vértices espelhada: volume negativo.
    std::array<gp_XYZ, 8> mirrored;
    for (int n = 0; n < 8; ++n)
        mirrored[std::size_t(n)] = gp_XYZ(-cube[std::size_t(n)].X(), cube[std::size_t(n)].Y(), cube[std::size_t(n)].Z());
    q = evaluateHex(mirrored);
    CAD_CHECK(q.volume < 0.0 && q.minScaledJacobian < 0.0);
    // Aresta degenerada.
    auto collapsed = cube;
    collapsed[1] = collapsed[0];
    CAD_CHECK(evaluateHex(collapsed).minEdge == 0.0);
}

void testePrincipalPlacaComFuro()
{
    log("\n=== Teste principal: placa 50 x 50 x 1 mm, furo central passante D = 20 mm, lco = 0,5 mm ===\n");
    const TopoDS_Shape plate =
        cut(box(-25, -25, 0, 50, 50, 1), cylinder(gp_Pnt(0, 0, 0.5), gp_Dir(0, 0, 1), 10.0, 3.0));
    const Hole hole{gp_XYZ(0, 0, 0), gp_XYZ(0, 0, 1), 10.0};
    const double lco = 0.5;
    const DiscretizeResult ra = discretize(plate, lco, InclusionCriterion::FullyInside);
    const DiscretizeResult rb = discretize(plate, lco, InclusionCriterion::BoundaryProjection);
    const DiscretizeResult rc = discretize(plate, lco, InclusionCriterion::CenterInside);
    if (!ra.ok || !rb.ok || !rc.ok)
        return;
    const Discretization& a = *ra.data;
    const Discretization& b = *rb.data;
    const double vCad = b.stats().solidVolume;
    CAD_CHECK_NEAR(vCad, 2500.0 - 100.0 * kPi, 1e-6);
    log("Volume CAD: %.4f mm3 (50 x 50 x 1 - pi x 10^2 x 1)\n", vCad);
    log("Caso A (cubo totalmente contido): %lld cubos, %lld nos (%lld vertices + %lld centrais), %lld ligacoes\n",
        (long long)a.cubeCount(), (long long)a.totalNodeCount(), (long long)a.vertexNodeCount(),
        (long long)a.centerNodeCount(), (long long)ApdlLinkSequence::count(a).total());
    log("Caso B (projecao dos nos):        %lld cubos, %lld nos (%lld vertices + %lld centrais), %lld ligacoes\n",
        (long long)b.cubeCount(), (long long)b.totalNodeCount(), (long long)b.vertexNodeCount(),
        (long long)b.centerNodeCount(), (long long)ApdlLinkSequence::count(b).total());
    logReport(b.projection());

    // 1/2. Distância dos nós de fronteira do furo à superfície cilíndrica (analítica).
    const Deviation da = holeDeviation(a, hole), db = holeDeviation(b, hole), dc = holeDeviation(*rc.data, hole);
    log("Distancia dos nos de fronteira do furo ao cilindro CAD (|r - 10|):\n");
    log("  caso A: %lld nos, max %.5f mm, medio %.5f mm\n", (long long)da.nodes, da.max, da.mean);
    log("  centro no material (partida de B): %lld nos, max %.5f mm, medio %.5f mm\n", (long long)dc.nodes, dc.max,
        dc.mean);
    log("  caso B: %lld nos, max %.5f mm, medio %.5f mm (max dentro do furo %.5f mm)\n", (long long)db.nodes, db.max,
        db.mean, db.maxInside);
    CAD_CHECK(db.mean < 0.25 * da.mean);
    CAD_CHECK(db.max < da.max);
    std::int64_t onSurface = 0;
    for (std::int64_t id : holeBoundaryNodes(b, hole))
        onSurface += std::fabs(hole.depth(b.node(id).position)) <= 1e-6 ? 1 : 0;
    log("  caso B: %lld de %lld nos de fronteira sobre o cilindro (|r - 10| <= 1e-6 mm)\n", (long long)onSurface,
        (long long)db.nodes);
    CAD_CHECK(onSurface > db.nodes / 2);

    // 3. Células invertidas.
    const CellCheck cb = checkCells(b);
    log("Celulas (reavaliacao independente) caso B: invertidas %lld, volume <= 0 %lld, centro fora %lld, centro != "
        "media %lld, jacobiano escalado min %.3f\n",
        (long long)cb.inverted, (long long)cb.nonPositive, (long long)cb.centerOutside, (long long)cb.centerMismatch,
        cb.minScaledJacobian);
    CAD_CHECK(cb.inverted == 0 && cb.nonPositive == 0 && cb.centerOutside == 0 && cb.centerMismatch == 0);

    // 4. Ligações atravessando o furo.
    const std::int64_t crossA = linksCrossingHole(a, hole, 0.1 * lco), crossB = linksCrossingHole(b, hole, 0.1 * lco);
    const std::int64_t voids = linksThroughVoid(b, plate, 0.1 * lco);
    log("Ligacoes atravessando o furo: caso A %lld; caso B %lld (classificador OCCT: %lld)\n", (long long)crossA,
        (long long)crossB, (long long)voids);
    CAD_CHECK(crossA == 0 && crossB == 0 && voids == 0);

    // 5. Continuidade da conectividade.
    CAD_CHECK(sameTopology(b, *rc.data));
    const std::int64_t compA = components(a), compB = components(b);
    log("Conectividade: componentes conexas A %lld, B %lld; topologia de B identica a grade de partida: %s\n",
        (long long)compA, (long long)compB, sameTopology(b, *rc.data) ? "sim" : "NAO");
    CAD_CHECK(compA == 1 && compB == 1);

    // 6. Nós projetados.
    CAD_CHECK(b.projection().projectedNodes > 0);
    CAD_CHECK(b.projection().featureEdgeTargets > 0); // bordas superior e inferior do furo

    // 7. Volume.
    const double vA = a.cubeVolume(), vB = b.discretizedVolume(), vC = rc.data->cubeVolume();
    log("Volume: A %.4f (%.3f %%); centro no material %.4f (%.3f %%); B %.4f (%.3f %%) [CAD %.4f]\n", vA,
        100 * (vA - vCad) / vCad, vC, 100 * (vC - vCad) / vCad, vB, 100 * (vB - vCad) / vCad, vCad);
    CAD_CHECK(std::fabs(vB - vCad) < std::fabs(vA - vCad));
    CAD_CHECK(std::fabs(vB - vCad) < std::fabs(vC - vCad));
    CAD_CHECK(std::fabs(cb.volume - vB) < 1e-9 * vB);
    CAD_CHECK(std::fabs(vB - double(b.cubeCount()) * lco * lco * lco) > 1e-6); // não é Nc × lco³

    // 8. Exportações ASCII e APDL.
    const fs::path extra = userOutputDir();
    const ExportCheck ea = checkExports(a, "placa_furo_casoA_totalmente_contido", extra);
    const ExportCheck eb = checkExports(b, "placa_furo_casoB_projecao", extra);
    log("Exportacoes: caso A ASCII %s / APDL %s; caso B ASCII %s / APDL %s; coordenadas ajustadas no arquivo: %s\n",
        ea.ascii ? "ok" : "FALHA", ea.apdl ? "ok" : "FALHA", eb.ascii ? "ok" : "FALHA", eb.apdl ? "ok" : "FALHA",
        eb.coordinatesUpdated ? "sim" : "NAO");
    CAD_CHECK(ea.ascii && ea.apdl && eb.ascii && eb.apdl && eb.coordinatesUpdated);
    const auto lengths = linkLengths(b);
    CAD_CHECK(std::int64_t(lengths.size()) == ApdlLinkSequence::count(b).total());
    CAD_CHECK_NEAR(*std::min_element(lengths.begin(), lengths.end()), b.projection().minLink, 1e-12);
    CAD_CHECK_NEAR(*std::max_element(lengths.begin(), lengths.end()), b.projection().maxLink, 1e-12);
    const auto lengthsA = linkLengths(a);
    CAD_CHECK_NEAR(*std::min_element(lengthsA.begin(), lengthsA.end()), 0.5 * std::sqrt(3.0) * lco, 1e-12);
    CAD_CHECK_NEAR(*std::max_element(lengthsA.begin(), lengthsA.end()), lco, 1e-12);
}

void testesAdicionais()
{
    log("\n=== Testes adicionais ===\n");
    // 1. Bloco sem furo (faces alinhadas à grade): nada a projetar; coordenadas iguais à grade.
    {
        const CaseResult r = checkCase("1 bloco sem furo 10 x 6 x 4", box(0, 0, 0, 10, 6, 4), 1.0, nullptr, false);
        if (r.projected)
        {
            CAD_CHECK(r.projected->projection().movedNodes == 0);
            CAD_CHECK_NEAR(r.projected->discretizedVolume(), 240.0, 1e-9);
            bool equal = true;
            for (std::int64_t id = 1; id <= r.projected->totalNodeCount(); ++id)
                equal = equal && same(r.projected->node(id).position, r.base->node(id).position);
            CAD_CHECK(equal);
        }
    }
    // 2. Placa com furo deslocado.
    {
        const Hole h{gp_XYZ(3.3, -2.1, 0), gp_XYZ(0, 0, 1), 4.0};
        checkCase("2 placa 20 x 20 x 2 com furo deslocado D8 em (3,3; -2,1)",
                  cut(box(-10, -10, 0, 20, 20, 2), cylinder(gp_Pnt(3.3, -2.1, 1), gp_Dir(0, 0, 1), 4.0, 4.0)), 0.5, &h);
    }
    // 3. Placa com dois furos.
    {
        const TopoDS_Shape s = cut(cut(box(-15, -10, 0, 30, 20, 2), cylinder(gp_Pnt(-7, 0, 1), gp_Dir(0, 0, 1), 3.0, 4.0)),
                                   cylinder(gp_Pnt(7, 0, 1), gp_Dir(0, 0, 1), 3.0, 4.0));
        const CaseResult r = checkCase("3 placa 30 x 20 x 2 com dois furos D6", s, 0.5);
        if (r.projected)
            for (double cx : {-7.0, 7.0})
            {
                const Hole h{gp_XYZ(cx, 0, 0), gp_XYZ(0, 0, 1), 3.0};
                const Deviation a = holeDeviation(*r.base, h), b = holeDeviation(*r.projected, h);
                log("    furo em x = %g: desvio medio %.4f -> %.4f mm\n", cx, a.mean, b.mean);
                CAD_CHECK(b.mean < a.mean);
                CAD_CHECK(linksCrossingHole(*r.projected, h, 0.05) == 0);
            }
    }
    // 4. Furo de pequeno diâmetro (3 lco).
    {
        const Hole h{gp_XYZ(0.1, 0.2, 0), gp_XYZ(0, 0, 1), 0.75};
        checkCase("4 furo pequeno D1,5 (3 lco)",
                  cut(box(-5, -5, 0, 10, 10, 2), cylinder(gp_Pnt(0.1, 0.2, 1), gp_Dir(0, 0, 1), 0.75, 4.0)), 0.5, &h);
    }
    // 5. Furo próximo à borda (parede de 0,6 mm).
    {
        const Hole h{gp_XYZ(5.4, 0, 0), gp_XYZ(0, 0, 1), 4.0};
        checkCase("5 furo D8 proximo a borda (parede 0,6 mm)",
                  cut(box(-10, -10, 0, 20, 20, 2), cylinder(gp_Pnt(5.4, 0, 1), gp_Dir(0, 0, 1), 4.0, 4.0)), 0.5, &h);
    }
    // 6. Furo inclinado (eixo a 30° de Z, no plano YZ).
    {
        const gp_Dir dir(0, std::sin(kPi / 6), std::cos(kPi / 6));
        const Hole h{gp_XYZ(0, 0, 1.5), dir.XYZ(), 2.0};
        checkCase("6 furo inclinado 30 graus D4 em placa 12 x 12 x 3",
                  cut(box(-6, -6, 0, 12, 12, 3), cylinder(gp_Pnt(0, 0, 1.5), dir, 2.0, 16.0)), 0.5, &h);
    }
    // 7. Cavidade interna (esfera R2,3 no interior de um cubo).
    {
        const Hole h{gp_XYZ(4, 4, 4), gp_XYZ(0, 0, 0), 2.3};
        checkCase("7 cavidade esferica interna R2,3 em cubo 8 x 8 x 8",
                  cut(box(0, 0, 0, 8, 8, 8), BRepPrimAPI_MakeSphere(gp_Pnt(4, 4, 4), 2.3).Shape()), 0.5, &h);
    }
    // 8. Sólidos com faces curvas.
    checkCase("8a cilindro macico R3 x 4", BRepPrimAPI_MakeCylinder(gp_Ax2(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1)), 3.0, 4.0).Shape(),
              0.5);
    checkCase("8b esfera R3", BRepPrimAPI_MakeSphere(gp_Pnt(0, 0, 0), 3.0).Shape(), 0.5);
    // 9. Sólido deslocado da origem (placa com furo transladada).
    {
        const gp_Vec offset(1000.25, -500.4, 250.1);
        const TopoDS_Shape s = translate(
            cut(box(-10, -10, 0, 20, 20, 2), cylinder(gp_Pnt(3.3, -2.1, 1), gp_Dir(0, 0, 1), 4.0, 4.0)), offset);
        const Hole h{gp_XYZ(3.3, -2.1, 0) + offset.XYZ(), gp_XYZ(0, 0, 1), 4.0};
        const CaseResult r = checkCase("9 placa com furo deslocada para (1000,25; -500,4; 250,1)", s, 0.5, &h);
        if (r.projected)
        {
            const VoxelGrid& g = r.projected->grid();
            CAD_CHECK(std::fabs(g.origin.X() - (-10 + offset.X())) < 1e-6);
            CAD_CHECK(std::fabs(g.origin.Z() - offset.Z()) < 1e-6);
        }
    }
    // 10. Sólido rotacionado (bloco girado 25° em torno de (1, 1, 1)).
    {
        gp_Trsf t;
        t.SetRotation(gp_Ax1(gp_Pnt(0, 0, 0), gp_Dir(1, 1, 1)), 25.0 * kPi / 180.0);
        const TopoDS_Shape s = BRepBuilderAPI_Transform(box(0, 0, 0, 4, 3, 2), t, true).Shape();
        checkCase("10 bloco 4 x 3 x 2 rotacionado 25 graus em torno de (1,1,1)", s, 0.5);
    }
}

void testeOpcoesAntigas()
{
    log("\n=== Opcoes antigas inalteradas ===\n");
    const TopoDS_Shape s = cut(box(-10, -10, 0, 20, 20, 2), cylinder(gp_Pnt(3.3, -2.1, 1), gp_Dir(0, 0, 1), 4.0, 4.0));
    for (InclusionCriterion c : {InclusionCriterion::FullyInside, InclusionCriterion::CenterInside,
                                 InclusionCriterion::PositiveIntersection})
    {
        const DiscretizeResult r = discretize(s, 0.5, c);
        if (!r.ok)
            continue;
        const Discretization& d = *r.data;
        CAD_CHECK(!d.adjusted());
        CAD_CHECK(d.discretizedVolume() == d.cubeVolume());
        bool grid = true;
        for (std::int64_t id = 1; id <= d.totalNodeCount(); ++id)
        {
            const NodeInfo n = d.node(id);
            const gp_XYZ expected = n.center ? d.grid().center(n.i, n.j, n.k) : d.grid().vertex(n.i, n.j, n.k);
            grid = grid && same(n.position, expected);
        }
        CAD_CHECK(grid);
        log("  %s: %lld cubos, %lld nos, coordenadas da grade: %s\n", criterionLabel(c), (long long)d.cubeCount(),
            (long long)d.totalNodeCount(), grid ? "sim" : "NAO");
    }
    // Rótulos e chaves: as três opções originais mantêm os textos; a quarta é nova.
    CAD_CHECK(std::string(criterionLabel(InclusionCriterion::FullyInside)) == "Cubo totalmente contido no sólido");
    CAD_CHECK(std::string(criterionKey(InclusionCriterion::BoundaryProjection)) == "projecao_contorno");
    CAD_CHECK(std::string(criterionLabel(InclusionCriterion::BoundaryProjection))
              == "Projeção dos nós sobre o contorno (ajuste geométrico)");
}

} // namespace

int main()
{
    const auto t0 = std::chrono::steady_clock::now();
    auto elapsed = [&t0] { return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(); };
    testeQualidadeHexaedro();
    testePrincipalPlacaComFuro();
    std::printf("(tempo acumulado: %.1f s)\n", elapsed());
    testesAdicionais();
    std::printf("(tempo acumulado: %.1f s)\n", elapsed());
    testeOpcoesAntigas();
    std::printf("(tempo acumulado: %.1f s)\n", elapsed());
    const fs::path extra = userOutputDir();
    if (!extra.empty())
    {
        std::ofstream out(extra / "resultados_test_e5723.txt", std::ios::binary);
        out << g_summary.str();
    }
    return cad::test::finish("e5723");
}
