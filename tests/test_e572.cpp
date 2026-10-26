// E5.7.2 — discretização volumétrica em cubos elementares e exportação ASCII dos nós (núcleo, sem Qt).
//   A  bloco simples alinhado (100 × 100 × 10, lco = 1): contagens, coordenadas, IDs de referência,
//      conectividade e reprodutibilidade;
//   B  bloco com furo passante (peça de exemplo do documento): nenhum cubo na região vazia,
//      ordenação com intervalos sem nós, relação entre os três critérios;
//   C  cavidades internas (cúbica alinhada e esférica menor que um cubo, sem vértices dentro dela);
//   D  sólido deslocado da origem (coordenadas exportadas sem deslocamento, mm e m);
//   E  alteração de lco (1 -> 2 mm): recontagem e IDs reconstruídos;
//   F  geometria irregular: cada linha começa no menor X existente; sequência de referência do
//      usuário reproduzida em metros;
//   G  nós centrais numerados depois de todos os vértices;
//   H  exportação ASCII: releitura, cabeçalho, linhas, IDs, coordenadas, ordem, precisão,
//      recusa de sobrescrita;
//   I  desempenho do exemplo de 100.000 cubos (tempos e memória);
//   J  cancelamento sem alterar o sólido;
//   +  critério 3 não aceita cubos que só tocam a superfície; limites de lco e de grade.
#include "TestCheck.h"

#include "discretization/NodeAsciiExporter.h"
#include "discretization/VoxelDiscretizer.h"
#include "document/CadDocument.h"

#include <BRepAlgoAPI_Cut.hxx>
#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepGProp.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeSphere.hxx>
#include <GProp_GProps.hxx>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#undef near // macros do windows.h (conflitam com cad::test::near)
#undef far

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

using namespace cad::discretization;
namespace fs = std::filesystem;

namespace {

TopoDS_Shape box(double x0, double y0, double z0, double dx, double dy, double dz)
{
    return BRepPrimAPI_MakeBox(gp_Pnt(x0, y0, z0), dx, dy, dz).Shape();
}

double volume(const TopoDS_Shape& s)
{
    GProp_GProps p;
    BRepGProp::VolumeProperties(s, p);
    return std::fabs(p.Mass());
}

std::shared_ptr<Discretization> run(const TopoDS_Shape& s, double lco, InclusionCriterion c = InclusionCriterion::FullyInside)
{
    const DiscretizeResult r = VoxelDiscretizer::run(s, Parameters{lco, c});
    CAD_CHECK(r.ok);
    if (!r.ok)
        std::printf("  falha: %s\n", r.message.c_str());
    return r.ok ? r.data : std::make_shared<Discretization>();
}

bool samePoint(const gp_XYZ& a, double x, double y, double z)
{
    return a.X() == x && a.Y() == y && a.Z() == z;
}

// Verifica a ordem Z → Y → X estrita dentro dos blocos de vértices e de centros.
bool orderedZyx(const Discretization& d)
{
    auto less = [](const gp_XYZ& a, const gp_XYZ& b) {
        if (a.Z() != b.Z())
            return a.Z() < b.Z();
        if (a.Y() != b.Y())
            return a.Y() < b.Y();
        return a.X() < b.X();
    };
    for (std::int64_t id = 2; id <= d.totalNodeCount(); ++id)
        if (id != d.vertexNodeCount() + 1 && !less(d.node(id - 1).position, d.node(id).position))
            return false;
    return true;
}

std::vector<std::string> readLines(const fs::path& file)
{
    std::ifstream in(file, std::ios::binary);
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(in, line))
        lines.push_back(line);
    return lines;
}

std::size_t peakMemoryBytes()
{
    PROCESS_MEMORY_COUNTERS pmc{};
    GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc));
    return pmc.PeakWorkingSetSize;
}

double secondsSince(std::chrono::steady_clock::time_point t0)
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

fs::path outputDir()
{
    fs::path dir = fs::temp_directory_path() / "cadmecanico_e572";
    fs::create_directories(dir);
    return dir;
}

// --- A + exemplo principal ----------------------------------------------------

void testeA_blocoReferencia()
{
    std::printf("[A] bloco 100 x 100 x 10, lco = 1\n");
    const TopoDS_Shape s = box(0, 0, 0, 100, 100, 10);
    const auto t0 = std::chrono::steady_clock::now();
    const auto d = run(s, 1.0);
    const double t = secondsSince(t0);
    CAD_CHECK(d->cubeCount() == 100000);
    CAD_CHECK(d->vertexNodeCount() == 112211);
    CAD_CHECK(d->centerNodeCount() == 100000);
    CAD_CHECK(d->totalNodeCount() == 212211);
    CAD_CHECK(d->grid().nx == 100 && d->grid().ny == 100 && d->grid().nz == 10);
    CAD_CHECK(samePoint(d->node(1).position, 0, 0, 0));
    CAD_CHECK(samePoint(d->node(2).position, 1, 0, 0));
    CAD_CHECK(samePoint(d->node(101).position, 100, 0, 0));
    CAD_CHECK(samePoint(d->node(102).position, 0, 1, 0));
    CAD_CHECK(samePoint(d->node(10202).position, 0, 0, 1));
    CAD_CHECK(samePoint(d->node(112211).position, 100, 100, 10));
    CAD_CHECK(!d->node(112211).center && d->node(112212).center);
    CAD_CHECK(samePoint(d->node(112212).position, 0.5, 0.5, 0.5));
    CAD_CHECK(samePoint(d->node(212211).position, 99.5, 99.5, 9.5));
    CAD_CHECK(d->node(212212).id == 0); // além do último nó
    // Conectividade do cubo 1 (convenção V1..V8 + V9).
    const CubeInfo c1 = d->cube(0);
    CAD_CHECK(c1.id == 1 && c1.i == 0 && c1.j == 0 && c1.k == 0 && c1.lco == 1.0);
    const std::array<std::int64_t, 8> v1{1, 2, 103, 102, 10202, 10203, 10304, 10303};
    CAD_CHECK(c1.vertexIds == v1);
    CAD_CHECK(c1.centerId == 112212);
    CAD_CHECK(samePoint(c1.center, 0.5, 0.5, 0.5));
    const CubeInfo last = d->cube(d->cubeCount() - 1);
    CAD_CHECK(last.id == 100000 && last.centerId == 212211 && last.vertexIds[6] == 112211);
    // Vértices compartilhados: o V2 do cubo 1 é o V1 do cubo 2.
    CAD_CHECK(d->cube(1).vertexIds[0] == c1.vertexIds[1]);
    // Todos os nós usados por algum cubo (sem nós isolados) e IDs sem duplicatas.
    std::vector<std::uint8_t> used(std::size_t(d->totalNodeCount() + 1), 0);
    for (std::int64_t n = 0; n < d->cubeCount(); ++n)
    {
        const CubeInfo c = d->cube(n);
        for (std::int64_t v : c.vertexIds)
            used[std::size_t(v)] = 1;
        used[std::size_t(c.centerId)]++;
    }
    CAD_CHECK(std::count(used.begin() + 1, used.end(), std::uint8_t(1)) == d->totalNodeCount());
    CAD_CHECK(orderedZyx(*d));
    CAD_CHECK_NEAR(d->stats().solidVolume, 100000.0, 1e-6);
    CAD_CHECK_NEAR(d->cubeVolume(), 100000.0, 1e-9);
    // Reprodutibilidade: execuções repetidas e os três critérios (bloco alinhado) idênticos.
    for (InclusionCriterion c : {InclusionCriterion::FullyInside, InclusionCriterion::CenterInside,
                                 InclusionCriterion::PositiveIntersection})
    {
        const auto again = run(s, 1.0, c);
        CAD_CHECK(again->cubeCells() == d->cubeCells());
        CAD_CHECK(again->vertexPoints() == d->vertexPoints());
    }
    std::printf("    cubos %lld, nós vértices %lld, centros %lld, total %lld; %.3f s "
                "(classificação %.3f s, numeração %.3f s); regiões uniformes %lld, testes de contorno %lld\n",
                (long long)d->cubeCount(), (long long)d->vertexNodeCount(), (long long)d->centerNodeCount(),
                (long long)d->totalNodeCount(), t, d->stats().classificationSeconds, d->stats().numberingSeconds,
                (long long)d->stats().uniformRegions, (long long)d->stats().boundaryTests);
}

// --- B -------------------------------------------------------------------------

void testeB_furoPassante()
{
    std::printf("[B] bloco 100 x 60 x 20 com furo passante R10 (peça do documento)\n");
    const auto doc = cad::document::CadDocument::withTestPart();
    const TopoDS_Shape s = doc.shape();
    const double vCad = volume(s);
    const auto d1 = run(s, 1.0, InclusionCriterion::FullyInside);
    const auto d2 = run(s, 1.0, InclusionCriterion::CenterInside);
    const auto d3 = run(s, 1.0, InclusionCriterion::PositiveIntersection);
    const double r = 10.0, cx = 50.0, cy = 30.0;
    auto nearest = [&](const CubeInfo& c) { // distância do eixo ao ponto mais próximo do quadrado XY
        const double h = c.lco / 2;
        const double dx = std::max(0.0, std::fabs(c.center.X() - cx) - h);
        const double dy = std::max(0.0, std::fabs(c.center.Y() - cy) - h);
        return std::hypot(dx, dy);
    };
    auto farthest = [&](const CubeInfo& c) {
        const double h = c.lco / 2;
        return std::hypot(std::fabs(c.center.X() - cx) + h, std::fabs(c.center.Y() - cy) + h);
    };
    bool c1ok = true, c2ok = true, c3ok = true;
    for (std::int64_t n = 0; n < d1->cubeCount(); ++n)
        c1ok = c1ok && nearest(d1->cube(n)) >= r - 1e-9; // inteiramente fora do furo
    for (std::int64_t n = 0; n < d2->cubeCount(); ++n)
    {
        const CubeInfo c = d2->cube(n);
        c2ok = c2ok && std::hypot(c.center.X() - cx, c.center.Y() - cy) > r; // centro no material
    }
    for (std::int64_t n = 0; n < d3->cubeCount(); ++n)
        c3ok = c3ok && farthest(d3->cube(n)) > r; // nenhum cubo inteiramente no vazio
    CAD_CHECK(c1ok && c2ok && c3ok);
    CAD_CHECK(d1->cubeCount() < d2->cubeCount() && d2->cubeCount() < d3->cubeCount());
    // Contagem exata do critério 1: células cujo quadrado não entra no círculo.
    std::int64_t expected1 = 0, expected3 = 0;
    for (int j = 0; j < 60; ++j)
        for (int i = 0; i < 100; ++i)
        {
            CubeInfo c;
            c.lco = 1.0;
            c.center = gp_XYZ(i + 0.5, j + 0.5, 0.5);
            expected1 += nearest(c) >= r ? 1 : 0;
            expected3 += farthest(c) > r ? 1 : 0;
        }
    CAD_CHECK(d1->cubeCount() == expected1 * 20);
    CAD_CHECK(d3->cubeCount() == expected3 * 20);
    CAD_CHECK(d1->cubeVolume() <= vCad && vCad <= d3->cubeVolume());
    // Ordenação com intervalos sem nós: na linha y = 30, z = 0, X salta o furo.
    CAD_CHECK(orderedZyx(*d1) && orderedZyx(*d2) && orderedZyx(*d3));
    bool gap = false;
    for (std::int64_t id = 2; id <= d1->vertexNodeCount(); ++id)
    {
        const gp_XYZ a = d1->node(id - 1).position, b = d1->node(id).position;
        if (a.Z() == 0 && a.Y() == 30 && b.Y() == 30 && b.X() - a.X() > 1.0)
            gap = b.X() - a.X() == 20.0; // de x = 40 a x = 60 (cubos tangentes ao furo são aceitos)
    }
    CAD_CHECK(gap);
    // A forma do documento continua válida e intacta.
    CAD_CHECK(BRepCheck_Analyzer(s).IsValid());
    CAD_CHECK_NEAR(volume(doc.shape()), vCad, 1e-9);
    std::printf("    V_CAD = %.3f mm³; critério 1: %lld cubos (%.0f mm³), critério 2: %lld, critério 3: %lld "
                "(%.0f mm³); testes de contorno %lld (%.2f s), falhas %lld; pontos %lld (%.2f s)\n",
                vCad, (long long)d1->cubeCount(), d1->cubeVolume(), (long long)d2->cubeCount(),
                (long long)d3->cubeCount(), d3->cubeVolume(), (long long)d1->stats().boundaryTests,
                d1->stats().boundarySeconds, (long long)d1->stats().extremaFailures,
                (long long)d1->stats().pointClassifications, d1->stats().pointSeconds);
}

// --- C -------------------------------------------------------------------------

void testeC_cavidades()
{
    std::printf("[C] cavidades internas\n");
    // Cavidade cúbica alinhada [5, 15]³ em um bloco 20³.
    const TopoDS_Shape hollow = BRepAlgoAPI_Cut(box(0, 0, 0, 20, 20, 20), box(5, 5, 5, 10, 10, 10)).Shape();
    for (InclusionCriterion c : {InclusionCriterion::FullyInside, InclusionCriterion::CenterInside,
                                 InclusionCriterion::PositiveIntersection})
    {
        const auto d = run(hollow, 1.0, c);
        CAD_CHECK(d->cubeCount() == 8000 - 1000);
        bool none = true;
        for (std::int64_t n = 0; n < d->cubeCount(); ++n)
        {
            const gp_XYZ p = d->cube(n).center;
            none = none && !(p.X() > 5 && p.X() < 15 && p.Y() > 5 && p.Y() < 15 && p.Z() > 5 && p.Z() < 15);
        }
        CAD_CHECK(none);
        // Vértices dentro da cavidade (não usados) não existem; as faces da cavidade têm nós.
        CAD_CHECK(d->vertexNodeCount() == 21 * 21 * 21 - 9 * 9 * 9);
    }
    // Cavidade esférica R 0,3 centrada no cubo (4, 4, 4) de um bloco 10³: nenhum vértice da
    // grade está dentro dela — um teste só com os oito vértices aceitaria o cubo.
    const TopoDS_Shape sphere = BRepPrimAPI_MakeSphere(gp_Pnt(4.5, 4.5, 4.5), 0.3).Shape();
    const TopoDS_Shape bubble = BRepAlgoAPI_Cut(box(0, 0, 0, 10, 10, 10), sphere).Shape();
    const auto b1 = run(bubble, 1.0, InclusionCriterion::FullyInside);
    const auto b2 = run(bubble, 1.0, InclusionCriterion::CenterInside);
    const auto b3 = run(bubble, 1.0, InclusionCriterion::PositiveIntersection);
    CAD_CHECK(b1->cubeCount() == 999 && !b1->cellAccepted(4, 4, 4));
    CAD_CHECK(b2->cubeCount() == 999 && !b2->cellAccepted(4, 4, 4)); // centro no vazio
    CAD_CHECK(b3->cubeCount() == 1000 && b3->cellAccepted(4, 4, 4)); // há material no cubo
    // Bolha pequena fora do centro: só o critério 1 rejeita.
    const TopoDS_Shape off = BRepAlgoAPI_Cut(box(0, 0, 0, 10, 10, 10),
                                             BRepPrimAPI_MakeSphere(gp_Pnt(4.25, 4.5, 4.5), 0.15).Shape())
                                 .Shape();
    CAD_CHECK(run(off, 1.0, InclusionCriterion::FullyInside)->cubeCount() == 999);
    CAD_CHECK(run(off, 1.0, InclusionCriterion::CenterInside)->cubeCount() == 1000);
    CAD_CHECK(run(off, 1.0, InclusionCriterion::PositiveIntersection)->cubeCount() == 1000);
    std::printf("    cavidade cúbica: 7000 cubos; bolha R0,3: critérios 1/2/3 = %lld/%lld/%lld\n",
                (long long)b1->cubeCount(), (long long)b2->cubeCount(), (long long)b3->cubeCount());
}

// --- D -------------------------------------------------------------------------

void testeD_deslocado()
{
    std::printf("[D] sólido deslocado da origem\n");
    const TopoDS_Shape s = box(20, -5, 3.5, 10, 4, 2);
    const auto d = run(s, 1.0);
    CAD_CHECK(d->cubeCount() == 10 * 4 * 2);
    CAD_CHECK(d->grid().origin.X() == 20 && d->grid().origin.Y() == -5 && d->grid().origin.Z() == 3.5);
    CAD_CHECK(samePoint(d->node(1).position, 20, -5, 3.5));
    CAD_CHECK(samePoint(d->node(1, LengthUnit::Meter).position, 0.02, -0.005, 0.0035));
    const fs::path mm = outputDir() / "deslocado_mm.txt";
    const fs::path m = outputDir() / "deslocado_m.txt";
    NodeAsciiExporter ex;
    CAD_CHECK(ex.write(*d, mm, LengthUnit::Millimeter, true).ok);
    CAD_CHECK(ex.write(*d, m, LengthUnit::Meter, true).ok);
    const auto lmm = readLines(mm), lm = readLines(m);
    CAD_CHECK(lmm.size() == std::size_t(d->totalNodeCount() + 1) && lm.size() == lmm.size());
    CAD_CHECK(lmm.size() > 2 && lmm[1] == "1 20 -5 3.5" && lmm[2] == "2 21 -5 3.5");
    CAD_CHECK(lm.size() > 2 && lm[1] == "1 0.02 -0.005 0.0035" && lm[2] == "2 0.021 -0.005 0.0035");
    // A unidade não altera IDs nem ordem: só a escala.
    bool same = true;
    for (std::size_t n = 1; n < lm.size(); ++n)
    {
        long long a = 0, b = 0;
        double ax, ay, az, bx, by, bz;
        std::sscanf(lmm[n].c_str(), "%lld %lf %lf %lf", &a, &ax, &ay, &az);
        std::sscanf(lm[n].c_str(), "%lld %lf %lf %lf", &b, &bx, &by, &bz);
        same = same && a == b && std::fabs(ax / 1000 - bx) < 1e-15 && std::fabs(ay / 1000 - by) < 1e-15
               && std::fabs(az / 1000 - bz) < 1e-15;
    }
    CAD_CHECK(same);
}

// --- E -------------------------------------------------------------------------

void testeE_alteracaoLco()
{
    std::printf("[E] alteração de lco (1 -> 2 mm)\n");
    const TopoDS_Shape s = box(0, 0, 0, 100, 100, 10);
    const auto d1 = run(s, 1.0);
    const auto d2 = run(s, 2.0);
    CAD_CHECK(d1->cubeCount() == 100000 && d2->cubeCount() == 12500);
    CAD_CHECK(d2->vertexNodeCount() == 51 * 51 * 6 && d2->totalNodeCount() == 15606 + 12500);
    CAD_CHECK(samePoint(d2->node(2).position, 2, 0, 0));
    CAD_CHECK(samePoint(d2->node(52).position, 0, 2, 0));
    CAD_CHECK(samePoint(d2->node(51 * 51 + 1).position, 0, 0, 2));
    CAD_CHECK(samePoint(d2->node(15607).position, 1, 1, 1));
    CAD_CHECK(d2->cube(0).centerId == 15607 && d2->cube(0).vertexIds[2] == 53);
    CAD_CHECK_NEAR(d2->cubeVolume(), 100000.0, 1e-9);
    // Bloco não múltiplo de lco = 3 (100/3): a última camada não cabe inteira no sólido.
    const auto d3 = run(s, 3.0);
    CAD_CHECK(d3->grid().nx == 34 && d3->grid().nz == 4);
    CAD_CHECK(d3->cubeCount() == 33 * 33 * 3);
    std::printf("    lco = 3: %.2f s; regiões uniformes %lld, cubos de contorno %lld, pontos %lld (%.2f s), "
                "testes de distância %lld (%.2f s)\n",
                d3->stats().totalSeconds, (long long)d3->stats().uniformRegions, (long long)d3->stats().boundaryCubes,
                (long long)d3->stats().pointClassifications, d3->stats().pointSeconds,
                (long long)d3->stats().boundaryTests, d3->stats().boundarySeconds);
    CAD_CHECK(run(s, 3.0, InclusionCriterion::PositiveIntersection)->cubeCount() == 34 * 34 * 4);
    // Decimal: lco = 0,1 mm -> coordenadas sem erro acumulado (0,3 e não 0,30000000000000004).
    const auto dd = run(box(0, 0, 0, 1, 1, 1), 0.1);
    CAD_CHECK(dd->cubeCount() == 1000 && dd->vertexNodeCount() == 1331);
    CAD_CHECK(dd->node(4).position.X() == 0.3 && dd->node(11).position.X() == 1.0);
    CAD_CHECK(NodeAsciiExporter::formatCoordinate(dd->node(4).position.X()) == "0.3");
    CAD_CHECK(NodeAsciiExporter::formatCoordinate(dd->node(1332).position.X()) == "0.05");
}

// --- F -------------------------------------------------------------------------

void testeF_irregular()
{
    std::printf("[F] ordenação em geometria irregular (sequência de referência)\n");
    // Linha y ∈ [0, 1]: x de 20 a 30; linha y ∈ [1, 2]: x de 16 a 33 (mm). Espessura 1.
    const TopoDS_Shape s = BRepAlgoAPI_Fuse(box(20, 0, 0, 10, 1, 1), box(16, 1, 0, 17, 1, 1)).Shape();
    const auto d = run(s, 1.0);
    CAD_CHECK(d->cubeCount() == 10 + 17);
    CAD_CHECK(d->vertexNodeCount() == 2 * (11 + 18 + 18));
    // Cada linha (y, z) começa no menor X efetivamente existente naquela linha.
    std::map<std::pair<double, double>, double> rowMin;
    for (std::int64_t n = 0; n < d->cubeCount(); ++n)
    {
        const CubeInfo c = d->cube(n);
        for (int dy = 0; dy <= 1; ++dy)
            for (int dz = 0; dz <= 1; ++dz)
            {
                const auto key = std::make_pair(c.center.Z() - 0.5 + dz, c.center.Y() - 0.5 + dy);
                const double x = c.center.X() - 0.5;
                auto it = rowMin.find(key);
                if (it == rowMin.end() || x < it->second)
                    rowMin[key] = x;
            }
    }
    bool rowsOk = true;
    gp_XYZ prev;
    for (std::int64_t id = 1; id <= d->vertexNodeCount(); ++id)
    {
        const gp_XYZ p = d->node(id).position;
        if (id == 1 || p.Y() != prev.Y() || p.Z() != prev.Z())
            rowsOk = rowsOk && p.X() == rowMin[std::make_pair(p.Z(), p.Y())];
        prev = p;
    }
    CAD_CHECK(rowsOk);
    // Sequência fornecida pelo usuário (sem o prefixo "n" do APDL), em metros.
    const char* reference[29][3] = {
        {"0.02", "0", "0"},      {"0.021", "0", "0"},     {"0.022", "0", "0"},     {"0.023", "0", "0"},
        {"0.024", "0", "0"},     {"0.025", "0", "0"},     {"0.026", "0", "0"},     {"0.027", "0", "0"},
        {"0.028", "0", "0"},     {"0.029", "0", "0"},     {"0.03", "0", "0"},      {"0.016", "0.001", "0"},
        {"0.017", "0.001", "0"}, {"0.018", "0.001", "0"}, {"0.019", "0.001", "0"}, {"0.02", "0.001", "0"},
        {"0.021", "0.001", "0"}, {"0.022", "0.001", "0"}, {"0.023", "0.001", "0"}, {"0.024", "0.001", "0"},
        {"0.025", "0.001", "0"}, {"0.026", "0.001", "0"}, {"0.027", "0.001", "0"}, {"0.028", "0.001", "0"},
        {"0.029", "0.001", "0"}, {"0.03", "0.001", "0"},  {"0.031", "0.001", "0"}, {"0.032", "0.001", "0"},
        {"0.033", "0.001", "0"}};
    const fs::path file = outputDir() / "referencia_m.txt";
    CAD_CHECK(NodeAsciiExporter().write(*d, file, LengthUnit::Meter, true).ok);
    const auto lines = readLines(file);
    bool seqOk = lines.size() > 29 && lines[0] == kNodeFileHeader;
    for (int n = 0; seqOk && n < 29; ++n)
    {
        const std::string expected = std::to_string(n + 1) + " " + reference[n][0] + " " + reference[n][1] + " "
                                     + reference[n][2];
        if (lines[std::size_t(n + 1)] != expected)
        {
            std::printf("  linha %d: \"%s\" (esperado \"%s\")\n", n + 2, lines[std::size_t(n + 1)].c_str(),
                        expected.c_str());
            seqOk = false;
        }
        // Valor numérico idêntico ao da sequência do usuário.
        const NodeInfo node = d->node(n + 1, LengthUnit::Meter);
        seqOk = seqOk && node.position.X() == std::stod(reference[n][0]) && node.position.Y() == std::stod(reference[n][1]);
    }
    CAD_CHECK(seqOk);
    std::printf("    primeiras linhas: %s | %s | %s | %s\n", lines.size() > 12 ? lines[1].c_str() : "",
                lines.size() > 12 ? lines[11].c_str() : "", lines.size() > 12 ? lines[12].c_str() : "",
                lines.size() > 29 ? lines[29].c_str() : "");
}

// --- G -------------------------------------------------------------------------

void testeG_centros()
{
    std::printf("[G] nós centrais após os vértices\n");
    const auto doc = cad::document::CadDocument::withTestPart();
    const auto d = run(doc.shape(), 2.0, InclusionCriterion::CenterInside);
    const std::int64_t nv = d->vertexNodeCount();
    bool ok = true;
    for (std::int64_t n = 0; n < d->cubeCount(); ++n)
    {
        const CubeInfo c = d->cube(n);
        ok = ok && c.centerId == nv + c.id && c.centerId > nv;
        for (std::int64_t v : c.vertexIds)
            ok = ok && v >= 1 && v <= nv;
    }
    for (std::int64_t id = 1; id <= d->totalNodeCount(); ++id)
        ok = ok && d->node(id).center == (id > nv);
    CAD_CHECK(ok);
    CAD_CHECK(d->node(nv + 1).center && !d->node(nv).center);
}

// --- H -------------------------------------------------------------------------

void testeH_exportacao()
{
    std::printf("[H] exportação ASCII\n");
    const auto doc = cad::document::CadDocument::withTestPart();
    const auto d = run(doc.shape(), 2.5, InclusionCriterion::PositiveIntersection);
    const fs::path file = outputDir() / "peca_nodes.txt";
    std::error_code ec;
    fs::remove(file, ec);
    NodeAsciiExporter ex;
    const NodeExportResult r = ex.write(*d, file, LengthUnit::Millimeter);
    CAD_CHECK(r.ok && r.lines == d->totalNodeCount());
    const NodeFileCheck check = NodeAsciiExporter::verify(file, *d, LengthUnit::Millimeter);
    CAD_CHECK(check.ok && check.vertexLines == d->vertexNodeCount() && check.centerLines == d->cubeCount());
    // Releitura independente.
    const auto lines = readLines(file);
    CAD_CHECK(lines.size() == std::size_t(d->totalNodeCount() + 1));
    CAD_CHECK(!lines.empty() && lines[0] == "ID_node Coord_X Coord_Y Coord_Z");
    bool cols = true, ids = true, values = true, noApdl = true;
    for (std::size_t n = 1; n < lines.size(); ++n)
    {
        std::istringstream in(lines[n]);
        long long id;
        double x, y, z;
        std::string extra;
        cols = cols && bool(in >> id >> x >> y >> z) && !(in >> extra);
        ids = ids && id == (long long)n;
        const gp_XYZ p = d->node(id).position;
        values = values && x == p.X() && y == p.Y() && z == p.Z(); // precisão completa (ida e volta)
        noApdl = noApdl && lines[n].find('n') == std::string::npos && lines[n].find(',') == std::string::npos;
    }
    CAD_CHECK(cols && ids && values && noApdl);
    // Sem sobrescrita implícita: o arquivo existente é mantido.
    const auto before = fs::file_size(file);
    const NodeExportResult again = ex.write(*d, file, LengthUnit::Meter);
    CAD_CHECK(!again.ok && fs::file_size(file) == before);
    CAD_CHECK(!fs::exists(fs::path(file.string() + ".parcial.tmp")));
    CAD_CHECK(ex.write(*d, file, LengthUnit::Meter, true).ok);
    CAD_CHECK(NodeAsciiExporter::verify(file, *d, LengthUnit::Meter).ok);
    CAD_CHECK(!NodeAsciiExporter::verify(file, *d, LengthUnit::Millimeter).ok); // unidade conferida
    // Diretório inexistente: falha sem criar nada.
    CAD_CHECK(!ex.write(*d, outputDir() / "nao_existe" / "x.txt", LengthUnit::Meter).ok);
    // Arquivo corrompido (linha removida) é detectado.
    {
        std::ofstream out(outputDir() / "corrompido.txt", std::ios::binary);
        for (std::size_t n = 0; n < lines.size(); ++n)
            if (n != 5)
                out << lines[n] << '\n';
    }
    CAD_CHECK(!NodeAsciiExporter::verify(outputDir() / "corrompido.txt", *d, LengthUnit::Millimeter).ok);
    CAD_CHECK(NodeAsciiExporter::suggestedFileName("Peça: A/B") == "Peça_ A_B_nodes.txt");
    CAD_CHECK(NodeAsciiExporter::formatCoordinate(-0.0) == "0");
    CAD_CHECK(NodeAsciiExporter::formatCoordinate(0.1 + 0.2) == "0.30000000000000004"); // sem truncamento
    std::printf("    %lld linhas, %llu bytes, %.3f s\n", (long long)r.lines, (unsigned long long)r.bytes, r.seconds);
}

// --- I -------------------------------------------------------------------------

void testeI_desempenho()
{
    std::printf("[I] desempenho (100.000 cubos)\n");
    const TopoDS_Shape s = box(0, 0, 0, 100, 100, 10);
    const auto t0 = std::chrono::steady_clock::now();
    const auto d = run(s, 1.0);
    const double tGen = secondsSince(t0);
    const fs::path mm = outputDir() / "bloco_100x100x10_nodes_mm.txt";
    const fs::path m = outputDir() / "bloco_100x100x10_nodes_m.txt";
    const NodeExportResult rmm = NodeAsciiExporter().write(*d, mm, LengthUnit::Millimeter, true);
    const NodeExportResult rm = NodeAsciiExporter().write(*d, m, LengthUnit::Meter, true);
    CAD_CHECK(rmm.ok && rm.ok);
    CAD_CHECK(rmm.lines == 212211);
    const auto lines = readLines(m);
    CAD_CHECK(lines.size() == 212212);
    CAD_CHECK(lines.size() == 212212 && lines[1] == "1 0 0 0" && lines[2] == "2 0.001 0 0" && lines[101] == "101 0.1 0 0"
              && lines[102] == "102 0 0.001 0" && lines[10202] == "10202 0 0 0.001" && lines[112211] == "112211 0.1 0.1 0.01"
              && lines[112212] == "112212 0.0005 0.0005 0.0005" && lines[212211] == "212211 0.0995 0.0995 0.0095");
    CAD_CHECK(tGen < 60.0);
    std::printf("    geração %.3f s; exportação mm %.3f s (%llu bytes), m %.3f s; memória da discretização %.1f MiB; "
                "pico do processo %.1f MiB\n",
                tGen, rmm.seconds, (unsigned long long)rmm.bytes, rm.seconds, double(d->memoryBytes()) / 1048576.0,
                double(peakMemoryBytes()) / 1048576.0);
    std::printf("    arquivos: %s | %s\n", mm.u8string().c_str(), m.u8string().c_str());
}

// --- J -------------------------------------------------------------------------

void testeJ_cancelamento()
{
    std::printf("[J] cancelamento\n");
    const auto doc = cad::document::CadDocument::withTestPart();
    const TopoDS_Shape s = doc.shape();
    const double v0 = volume(s);
    int calls = 0;
    const DiscretizeResult r = VoxelDiscretizer::run(s, Parameters{0.5, InclusionCriterion::FullyInside},
                                                     [&calls](double) { return ++calls < 2; });
    CAD_CHECK(!r.ok && r.cancelled && !r.data);
    CAD_CHECK(calls >= 2);
    CAD_CHECK(BRepCheck_Analyzer(doc.shape()).IsValid());
    CAD_CHECK(doc.shape().IsSame(s));
    CAD_CHECK_NEAR(volume(doc.shape()), v0, 1e-9);
    // Depois do cancelamento uma nova geração funciona normalmente.
    CAD_CHECK(VoxelDiscretizer::run(s, Parameters{2.0, InclusionCriterion::FullyInside}).ok);
}

// --- Outros --------------------------------------------------------------------

void testeContatoELimites()
{
    std::printf("[+] contato sem interseção e limites\n");
    // L: célula (1, 1) da grade 2 × 2 está vazia e só toca o sólido nas faces x = 1 e y = 1.
    const TopoDS_Shape l = BRepAlgoAPI_Fuse(box(0, 0, 0, 2, 1, 1), box(0, 1, 0, 1, 1, 1)).Shape();
    const auto d3 = run(l, 1.0, InclusionCriterion::PositiveIntersection);
    CAD_CHECK(d3->cubeCount() == 3 && !d3->cellAccepted(1, 1, 0));
    CAD_CHECK(run(l, 1.0, InclusionCriterion::FullyInside)->cubeCount() == 3);
    // Contato por uma aresta/ponto só: cubos vizinhos em diagonal não são aceitos.
    const TopoDS_Shape diag = BRepAlgoAPI_Fuse(box(0, 0, 0, 1, 1, 1), box(1, 1, 0, 1, 1, 1)).Shape();
    CAD_CHECK(run(diag, 1.0, InclusionCriterion::PositiveIntersection)->cubeCount() == 2);
    // Limites.
    const TopoDS_Shape b = box(0, 0, 0, 100, 100, 10);
    CAD_CHECK(!VoxelDiscretizer::run(b, Parameters{0.0}).ok);
    CAD_CHECK(!VoxelDiscretizer::run(b, Parameters{-1.0}).ok);
    const GridEstimate big = VoxelDiscretizer::estimate(b, 0.01);
    CAD_CHECK(!big.ok && big.message.find("Grade grande demais") != std::string::npos);
    CAD_CHECK(!VoxelDiscretizer::run(TopoDS_Shape(), Parameters{1.0}).ok);
    // lco maior que o sólido: um único cubo candidato.
    CAD_CHECK(run(b, 1000.0, InclusionCriterion::PositiveIntersection)->cubeCount() == 1);
    CAD_CHECK(run(b, 1000.0, InclusionCriterion::FullyInside)->cubeCount() == 0);
}

} // namespace

int main()
{
    const std::pair<const char*, void (*)()> tests[] = {
        {"A", testeA_blocoReferencia}, {"B", testeB_furoPassante},   {"C", testeC_cavidades},
        {"D", testeD_deslocado},       {"E", testeE_alteracaoLco},   {"F", testeF_irregular},
        {"G", testeG_centros},         {"H", testeH_exportacao},     {"I", testeI_desempenho},
        {"J", testeJ_cancelamento},    {"+", testeContatoELimites}};
    for (const auto& [name, test] : tests)
    {
        const auto t0 = std::chrono::steady_clock::now();
        test();
        std::printf("    (teste %s: %.2f s)\n", name, secondsSince(t0));
    }
    return cad::test::finish("e572");
}
