// E5.7.2.2 — exportação da geometria discretizada em comandos ANSYS APDL /PREP7 (núcleo, sem Qt).
//   R  teste de referência obrigatório: cubo 4 × 4 × 4 mm, lco = 1 mm, metros — 189 nós, 956
//      elementos; comparação automática com o arquivo do usuário (DEM_DYNA.txt, somente leitura);
//      relatório de divergência (casos negativos: unidade, contagem e ordem);
//   1  cubo simples; 2 bloco retangular; 3 placa; 4 sólido com furo (prismático e cilíndrico);
//   5  sólido com cavidade; 6 sólido deslocado da origem; 7 diferentes valores de lco;
//   8/9 exportação em metros e em milímetros; 10 IDs duplicados; 11 elementos duplicados;
//   12 conectividade inválida; 13 elementos atravessando vazios; 14 preservação da exportação
//   ASCII; + opção /CLEAR, ausência de comandos de solução, verificação por releitura que
//   detecta adulteração, geometria booleana (união em L) e desempenho (100.000 cubos).
//
// Saídas em %TEMP%\cadmecanico_e5722; com CAD_E5722_OUTPUT_DIR definido, o arquivo APDL do caso de
// referência e o relatório da comparação também são gravados nesse diretório.
#include "TestCheck.h"

#include "discretization/ApdlExporter.h"
#include "discretization/NodeAsciiExporter.h"
#include "discretization/VoxelDiscretizer.h"

#include <BRepAlgoAPI_Cut.hxx>
#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepClass3d_SolidClassifier.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <gp_Ax2.hxx>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

using namespace cad::discretization;
namespace fs = std::filesystem;

#ifndef CAD_APDL_REFERENCE
#define CAD_APDL_REFERENCE ""
#endif

namespace {

TopoDS_Shape box(double x0, double y0, double z0, double dx, double dy, double dz)
{
    return BRepPrimAPI_MakeBox(gp_Pnt(x0, y0, z0), dx, dy, dz).Shape();
}

std::shared_ptr<Discretization> run(const TopoDS_Shape& s, double lco, InclusionCriterion c = InclusionCriterion::FullyInside)
{
    const DiscretizeResult r = VoxelDiscretizer::run(s, Parameters{lco, c});
    CAD_CHECK(r.ok);
    if (!r.ok)
        std::printf("  falha: %s\n", r.message.c_str());
    return r.ok ? r.data : std::make_shared<Discretization>();
}

fs::path outputDir()
{
    fs::path dir = fs::temp_directory_path() / "cadmecanico_e5722";
    fs::create_directories(dir);
    return dir;
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

std::string readAll(const fs::path& file)
{
    std::ifstream in(file, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void writeAll(const fs::path& file, const std::string& text)
{
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out << text;
}

ApdlExportResult exportTo(const Discretization& d, const fs::path& file, LengthUnit unit = LengthUnit::Meter,
                          bool clear = true)
{
    ApdlOptions o;
    o.unit = unit;
    o.includeClear = clear;
    o.partName = "teste";
    const ApdlExportResult r = ApdlExporter().write(d, file, o, true);
    CAD_CHECK(r.ok);
    if (!r.ok)
        std::printf("  falha na exportação: %s\n", r.message.c_str());
    return r;
}

// Conteúdo do arquivo exportado (leitura independente do exportador).
struct FileModel
{
    std::vector<std::string> header; // comandos antes do primeiro N
    std::vector<std::int64_t> nodeIds;
    std::vector<std::array<double, 3>> coords;
    std::vector<std::string> nodeLines;
    std::vector<std::string> elementTypes;
    std::vector<std::array<std::int64_t, 3>> elements; // id, n1, n2
    std::vector<std::string> others;                   // comandos inesperados
    bool malformed = false;
};

FileModel readModel(const fs::path& file)
{
    FileModel m;
    for (const std::string& raw : readLines(file))
    {
        const std::string line = raw;
        if (line.empty() || line[0] == '!')
            continue;
        std::vector<std::string> f;
        std::stringstream ss(line);
        std::string item;
        while (std::getline(ss, item, ','))
            f.push_back(item);
        if (f[0] == "N")
        {
            if (f.size() != 5)
            {
                m.malformed = true;
                continue;
            }
            m.nodeIds.push_back(std::stoll(f[1]));
            m.coords.push_back({std::stod(f[2]), std::stod(f[3]), std::stod(f[4])});
            m.nodeLines.push_back(line);
        }
        else if (f[0] == "EN")
        {
            if (f.size() != 4)
            {
                m.malformed = true;
                continue;
            }
            m.elements.push_back({std::stoll(f[1]), std::stoll(f[2]), std::stoll(f[3])});
        }
        else if (f[0] == "ET")
            m.elementTypes.push_back(line);
        else if (m.nodeIds.empty())
            m.header.push_back(line);
        else
            m.others.push_back(line);
    }
    return m;
}

using Pair = std::pair<std::int64_t, std::int64_t>;
Pair key(std::int64_t a, std::int64_t b)
{
    return a < b ? Pair{a, b} : Pair{b, a};
}

// Conjuntos esperados de ligações, calculados por força bruta a partir dos cubos aceitos
// (independente de ApdlLinkSequence): 12 arestas por cubo (deduplicadas), vizinhos por face, 8
// ligações centro-vértice por cubo.
struct ExpectedLinks
{
    std::set<Pair> vertexEdges, centerCenter, centerVertex;
};

ExpectedLinks bruteForce(const Discretization& d)
{
    ExpectedLinks e;
    std::map<std::tuple<int, int, int>, std::int64_t> centerAt;
    for (std::int64_t n = 0; n < d.cubeCount(); ++n)
    {
        const CubeInfo c = d.cube(n);
        centerAt[{c.i, c.j, c.k}] = c.centerId;
    }
    static constexpr int kEdges[12][2] = {{0, 1}, {1, 2}, {2, 3}, {3, 0}, {4, 5}, {5, 6},
                                          {6, 7}, {7, 4}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
    for (std::int64_t n = 0; n < d.cubeCount(); ++n)
    {
        const CubeInfo c = d.cube(n);
        for (const auto& edge : kEdges)
            e.vertexEdges.insert(key(c.vertexIds[std::size_t(edge[0])], c.vertexIds[std::size_t(edge[1])]));
        for (std::int64_t v : c.vertexIds)
            e.centerVertex.insert({c.centerId, v});
        for (const auto& nb : {std::tuple<int, int, int>{c.i + 1, c.j, c.k}, std::tuple<int, int, int>{c.i, c.j + 1, c.k},
                               std::tuple<int, int, int>{c.i, c.j, c.k + 1}})
        {
            const auto it = centerAt.find(nb);
            if (it != centerAt.end())
                e.centerCenter.insert(key(c.centerId, it->second));
        }
    }
    return e;
}

double dist(const gp_XYZ& a, const gp_XYZ& b)
{
    return (a - b).Modulus();
}

// Validações gerais de um arquivo exportado (itens 10, 11, 12, 13 e ordem).
void checkFile(const char* name, const TopoDS_Shape& solid, const Discretization& d, const fs::path& file,
               LengthUnit unit)
{
    const FileModel m = readModel(file);
    CAD_CHECK(!m.malformed);
    CAD_CHECK(m.others.empty());
    CAD_CHECK(m.elementTypes.size() == 1 && m.elementTypes[0] == "ET,1,165");
    // 10. IDs de nós: 1..N, contínuos, sem duplicatas; coordenadas da discretização.
    CAD_CHECK(std::int64_t(m.nodeIds.size()) == d.totalNodeCount());
    bool contiguous = true, coordsOk = true;
    for (std::size_t n = 0; n < m.nodeIds.size(); ++n)
    {
        contiguous = contiguous && m.nodeIds[n] == std::int64_t(n) + 1;
        const gp_XYZ p = d.node(std::int64_t(n) + 1, unit).position;
        coordsOk = coordsOk && m.coords[n][0] == p.X() && m.coords[n][1] == p.Y() && m.coords[n][2] == p.Z();
    }
    CAD_CHECK(contiguous);
    CAD_CHECK(coordsOk);
    // Elementos: IDs contínuos a partir de 1.
    bool elementIds = true;
    for (std::size_t n = 0; n < m.elements.size(); ++n)
        elementIds = elementIds && m.elements[n][0] == std::int64_t(n) + 1;
    CAD_CHECK(elementIds);

    // 11 + 12: sem duplicatas; conectividade válida e geometricamente coerente.
    const std::int64_t nv = d.vertexNodeCount();
    const std::int64_t total = d.totalNodeCount();
    const double lco = d.grid().lco;
    std::set<Pair> seen;
    std::set<Pair> vv, cc, cv;
    bool duplicates = false, invalid = false, geometry = false, crossesVoid = false, order = false;
    BRepClass3d_SolidClassifier classifier(solid);
    int stage = 0; // 0 VV (eixo crescente), 1 centros
    int lastAxis = 0;
    std::int64_t lastFirst = 0;
    for (const auto& e : m.elements)
    {
        const std::int64_t a = e[1], b = e[2];
        if (a == b || a < 1 || b < 1 || a > total || b > total)
        {
            invalid = true;
            continue;
        }
        if (!seen.insert(key(a, b)).second)
            duplicates = true;
        const gp_XYZ pa = d.node(a).position, pb = d.node(b).position;
        const gp_XYZ delta = pb - pa;
        if (a <= nv && b <= nv)
        {
            vv.insert(key(a, b));
            // Aresta paralela a um eixo, comprimento lco, sentido positivo.
            int axis = -1, nonzero = 0;
            for (int ax = 0; ax < 3; ++ax)
                if (std::fabs(delta.Coord(ax + 1)) > 1e-9)
                {
                    ++nonzero;
                    axis = ax;
                }
            if (nonzero != 1 || std::fabs(delta.Coord(axis + 1) - lco) > 1e-9 * std::max(1.0, lco))
                geometry = true;
            if (stage != 0 || axis < lastAxis || (axis == lastAxis && a <= lastFirst))
                order = true;
            lastAxis = axis;
            lastFirst = a;
        }
        else if (a > nv && b > nv)
        {
            cc.insert(key(a, b));
            if (std::fabs(dist(pa, pb) - lco) > 1e-9 * std::max(1.0, lco) || b <= a)
                geometry = true;
            if (stage == 1 && a < lastFirst)
                order = true;
            stage = 1;
            lastFirst = a;
        }
        else
        {
            cv.insert({a, b});
            if (a <= nv || std::fabs(dist(pa, pb) - lco * std::sqrt(3.0) / 2.0) > 1e-9 * std::max(1.0, lco))
                geometry = true;
            if (stage == 1 && a < lastFirst)
                order = true;
            stage = 1;
            lastFirst = a;
        }
        // 13. Nenhuma ligação atravessa região vazia: o ponto médio está no material.
        classifier.Perform(gp_Pnt((pa + pb) / 2.0), 1e-7);
        if (classifier.State() == TopAbs_OUT)
            crossesVoid = true;
    }
    CAD_CHECK(!invalid);
    CAD_CHECK(!duplicates);
    CAD_CHECK(!geometry);
    CAD_CHECK(!crossesVoid);
    CAD_CHECK(!order);
    // Conjuntos iguais aos obtidos por força bruta a partir dos cubos aceitos.
    const ExpectedLinks exp = bruteForce(d);
    CAD_CHECK(vv == exp.vertexEdges);
    CAD_CHECK(cc == exp.centerCenter);
    CAD_CHECK(cv == exp.centerVertex);
    CAD_CHECK(std::int64_t(cv.size()) == 8 * d.cubeCount());
    std::printf("  %s: %lld cubos, %lld nós, %lld elementos (%zu VV + %zu CC + %zu CV)%s%s%s%s\n", name,
                (long long)d.cubeCount(), (long long)d.totalNodeCount(), (long long)m.elements.size(), vv.size(),
                cc.size(), cv.size(), duplicates ? " DUPLICADOS" : "", invalid ? " INVALIDOS" : "",
                crossesVoid ? " ATRAVESSA-VAZIO" : "", order ? " FORA-DE-ORDEM" : "");
}

std::int64_t vvFormula(std::int64_t nx, std::int64_t ny, std::int64_t nz)
{
    return nx * (ny + 1) * (nz + 1) + (nx + 1) * ny * (nz + 1) + (nx + 1) * (ny + 1) * nz;
}

std::int64_t ccFormula(std::int64_t nx, std::int64_t ny, std::int64_t nz)
{
    return (nx - 1) * ny * nz + nx * (ny - 1) * nz + nx * ny * (nz - 1);
}

void checkBoxCounts(const char* name, double dx, double dy, double dz, double lco)
{
    const TopoDS_Shape s = box(0, 0, 0, dx, dy, dz);
    const auto d = run(s, lco);
    const std::int64_t nx = std::llround(dx / lco), ny = std::llround(dy / lco), nz = std::llround(dz / lco);
    CAD_CHECK(d->cubeCount() == nx * ny * nz);
    const LinkCounts c = ApdlLinkSequence::count(*d);
    CAD_CHECK(c.vertexEdges == vvFormula(nx, ny, nz));
    CAD_CHECK(c.centerCenter == ccFormula(nx, ny, nz));
    CAD_CHECK(c.centerVertex == 8 * nx * ny * nz);
    const fs::path file = outputDir() / (std::string(name) + ".inp");
    exportTo(*d, file);
    checkFile(name, s, *d, file, LengthUnit::Meter);
}

// --- R: teste de referência obrigatório -----------------------------------------

void testeR_referencia()
{
    std::printf("[R] cubo 4 x 4 x 4 mm, lco = 1 mm, metros (referência DEM_DYNA.txt)\n");
    const TopoDS_Shape s = box(0, 0, 0, 4, 4, 4);
    const auto d = run(s, 1.0);
    CAD_CHECK(d->cubeCount() == 64);
    CAD_CHECK(d->vertexNodeCount() == 125);
    CAD_CHECK(d->centerNodeCount() == 64);
    CAD_CHECK(d->totalNodeCount() == 189);
    const LinkCounts c = ApdlLinkSequence::count(*d);
    CAD_CHECK(c.vertexEdges == 300);
    CAD_CHECK(c.centerCenter == 144);
    CAD_CHECK(c.centerVertex == 512);
    CAD_CHECK(c.total() == 956);

    const fs::path file = outputDir() / "cubo_4x4x4_PREP7.inp";
    const ApdlExportResult r = exportTo(*d, file);
    CAD_CHECK(r.nodes == 189 && r.links.total() == 956);
    const std::vector<std::string> lines = readLines(file);
    CAD_CHECK(lines.size() > 6);
    CAD_CHECK(lines[0] == "!seed == 10");
    CAD_CHECK(lines[1] == "FINISH");
    CAD_CHECK(lines[2] == "/CLEAR");
    CAD_CHECK(std::regex_match(lines[3], std::regex(R"(!Code generate on \d\d/\d\d/\d{4}, \d\d:\d\d:\d\d)")));
    CAD_CHECK(lines[4] == "/TITLE, DEM-DYNA");
    CAD_CHECK(lines[5] == "/PREP7");
    const auto has = [&](const std::string& l) { return std::find(lines.begin(), lines.end(), l) != lines.end(); };
    CAD_CHECK(has("! Unidade das coordenadas: metros (m). O APDL nao impoe um sistema de unidades: os dados"));
    const FileModel m = readModel(file);
    CAD_CHECK(m.nodeLines.size() == 189);
    if (m.nodeLines.size() == 189)
    {
        CAD_CHECK(m.nodeLines[0] == "N,1,0,0,0");
        CAD_CHECK(m.nodeLines[1] == "N,2,0.001,0,0");
        CAD_CHECK(m.nodeLines[2] == "N,3,0.002,0,0");
        CAD_CHECK(m.nodeLines[3] == "N,4,0.003,0,0");
        CAD_CHECK(m.nodeLines[4] == "N,5,0.004,0,0");
        CAD_CHECK(m.nodeLines[124] == "N,125,0.004,0.004,0.004");
        CAD_CHECK(m.nodeLines[125] == "N,126,0.0005,0.0005,0.0005");
        CAD_CHECK(m.nodeLines[188] == "N,189,0.0035,0.0035,0.0035");
    }
    CAD_CHECK(has("ET,1,165"));
    CAD_CHECK(has("EN,1,1,2"));
    CAD_CHECK(has("EN,956,189,125"));
    CAD_CHECK(lines.back() == "EN,956,189,125");
    checkFile("referencia", s, *d, file, LengthUnit::Meter);

    // Comparação automática com o arquivo do usuário (somente leitura).
    const fs::path reference = fs::u8path(CAD_APDL_REFERENCE).lexically_normal().make_preferred();
    std::string comparisonText;
    if (reference.empty() || !fs::exists(reference))
    {
        std::printf("  AVISO: arquivo de referência não encontrado (%s); comparação automática não executada\n",
                    reference.u8string().c_str());
        comparisonText = "Arquivo de referência não encontrado: " + reference.u8string() + "\n";
    }
    else
    {
        std::error_code ec;
        const auto timeBefore = fs::last_write_time(reference, ec);
        const std::string bytesBefore = readAll(reference);
        const ApdlComparison cmp = ApdlExporter::compare(file, reference);
        std::printf("%s", cmp.summary.c_str());
        CAD_CHECK(cmp.ok);
        CAD_CHECK(cmp.nodesReference == 189 && cmp.elementsReference == 956);
        CAD_CHECK(cmp.nodesGenerated == 189 && cmp.elementsGenerated == 956);
        CAD_CHECK(cmp.elementTypeMatches);
        comparisonText = cmp.summary;

        // Casos negativos: o relatório aponta o primeiro ID divergente e a causa.
        const fs::path mm = outputDir() / "cubo_4x4x4_mm_PREP7.inp";
        exportTo(*d, mm, LengthUnit::Millimeter);
        const ApdlComparison cmpMm = ApdlExporter::compare(mm, reference);
        CAD_CHECK(!cmpMm.ok && cmpMm.divergenceKind == "N" && cmpMm.divergenceId == 2);
        CAD_CHECK(cmpMm.expected == "N,2,0.001,0,0" && cmpMm.obtained == "N,2,1,0,0");
        CAD_CHECK(cmpMm.cause.find("unidade") != std::string::npos);
        comparisonText += "\nCaso negativo 1 (mesma geometria exportada em mm):\n" + cmpMm.summary;

        const auto d5 = run(box(0, 0, 0, 5, 4, 4), 1.0);
        const fs::path other = outputDir() / "bloco_5x4x4_PREP7.inp";
        exportTo(*d5, other);
        const ApdlComparison cmp5 = ApdlExporter::compare(other, reference);
        CAD_CHECK(!cmp5.ok && cmp5.divergenceKind == "N" && cmp5.divergenceId == 6);
        comparisonText += "\nCaso negativo 2 (bloco 5 x 4 x 4 mm):\n" + cmp5.summary;

        std::string text = readAll(file);
        const std::size_t at = text.find("EN,301,126,127\n");
        CAD_CHECK(at != std::string::npos);
        if (at != std::string::npos)
        {
            text.replace(at, std::strlen("EN,301,126,127\n"), "EN,301,126,142\n");
            const fs::path changed = outputDir() / "cubo_4x4x4_alterado.inp";
            writeAll(changed, text);
            const ApdlComparison cmpEn = ApdlExporter::compare(changed, reference);
            CAD_CHECK(!cmpEn.ok && cmpEn.divergenceKind == "EN" && cmpEn.divergenceId == 301);
            CAD_CHECK(cmpEn.expected == "EN,301,126,127" && cmpEn.obtained == "EN,301,126,142");
            comparisonText += "\nCaso negativo 3 (elemento 301 alterado manualmente):\n" + cmpEn.summary;
        }

        // A referência não foi alterada.
        CAD_CHECK(readAll(reference) == bytesBefore);
        CAD_CHECK(fs::last_write_time(reference, ec) == timeBefore);
    }

    if (const char* dir = std::getenv("CAD_E5722_OUTPUT_DIR"); dir && *dir)
    {
        const fs::path out = fs::u8path(dir);
        fs::create_directories(out);
        std::error_code ec;
        fs::copy_file(file, out / "cubo_4x4x4_lco1_PREP7.inp", fs::copy_options::overwrite_existing, ec);
        CAD_CHECK(!ec);
        writeAll(out / "comparacao_referencia.txt", comparisonText);
        std::printf("  amostra e comparação gravadas em %s\n", out.u8string().c_str());
    }
}

// --- 1 a 7: geometrias -------------------------------------------------------------

void teste1a3_blocos()
{
    std::printf("[1-3] cubo simples, bloco retangular, placa\n");
    checkBoxCounts("cubo_2x2x2", 2, 2, 2, 1.0);
    checkBoxCounts("bloco_6x3x2", 6, 3, 2, 1.0);
    checkBoxCounts("placa_20x10x1", 20, 10, 1, 1.0);
    // Um único cubo: 12 arestas, nenhuma ligação entre centros, 8 centro-vértice.
    const auto d = run(box(0, 0, 0, 1, 1, 1), 1.0);
    const LinkCounts c = ApdlLinkSequence::count(*d);
    CAD_CHECK(c.vertexEdges == 12 && c.centerCenter == 0 && c.centerVertex == 8);
}

void teste4_furo()
{
    std::printf("[4] sólido com furo passante (prismático e cilíndrico)\n");
    // Furo prismático alinhado à grade: 10 × 10 × 4 com furo 4 × 4 no centro.
    const TopoDS_Shape plate = BRepAlgoAPI_Cut(box(0, 0, 0, 10, 10, 4), box(3, 3, -1, 4, 4, 6)).Shape();
    const auto d = run(plate, 1.0);
    CAD_CHECK(d->cubeCount() == (100 - 16) * 4);
    // Nenhum centro de cubo no furo e nenhuma ligação centro-centro atravessando-o.
    const LinkCounts c = ApdlLinkSequence::count(*d);
    // Contagem direta: ligações por face entre cubos aceitos.
    std::int64_t faces = 0;
    for (std::int64_t n = 0; n < d->cubeCount(); ++n)
    {
        const CubeInfo q = d->cube(n);
        faces += d->cellAccepted(q.i + 1, q.j, q.k) + d->cellAccepted(q.i, q.j + 1, q.k) + d->cellAccepted(q.i, q.j, q.k + 1);
    }
    CAD_CHECK(c.centerCenter == faces);
    const fs::path file = outputDir() / "furo_prismatico.inp";
    exportTo(*d, file);
    checkFile("furo prismatico", plate, *d, file, LengthUnit::Meter);

    // Furo cilíndrico (contorno curvo, aproximado por degraus).
    const TopoDS_Shape cyl = BRepPrimAPI_MakeCylinder(gp_Ax2(gp_Pnt(10, 10, -1), gp::DZ()), 4.0, 7.0).Shape();
    const TopoDS_Shape plate2 = BRepAlgoAPI_Cut(box(0, 0, 0, 20, 20, 5), cyl).Shape();
    const auto d2 = run(plate2, 1.0);
    const fs::path file2 = outputDir() / "furo_cilindrico.inp";
    exportTo(*d2, file2);
    checkFile("furo cilindrico", plate2, *d2, file2, LengthUnit::Meter);
    // Critério "centro no material": cubos parcialmente fora; ligações dentro da discretização.
    const auto d3 = run(plate2, 1.0, InclusionCriterion::CenterInside);
    const fs::path file3 = outputDir() / "furo_cilindrico_centro.inp";
    exportTo(*d3, file3);
    const FileModel m3 = readModel(file3);
    CAD_CHECK(std::int64_t(m3.elements.size()) == ApdlLinkSequence::count(*d3).total());
}

void teste5_cavidade()
{
    std::printf("[5] sólido com cavidade interna\n");
    const TopoDS_Shape s = BRepAlgoAPI_Cut(box(0, 0, 0, 10, 10, 10), box(3, 3, 3, 4, 4, 4)).Shape();
    const auto d = run(s, 1.0);
    CAD_CHECK(d->cubeCount() == 1000 - 64);
    const LinkCounts c = ApdlLinkSequence::count(*d);
    CAD_CHECK(c.centerCenter == ccFormula(10, 10, 10) - (ccFormula(4, 4, 4) + 6 * 16));
    // Arestas: todas as do bloco menos as estritamente internas à cavidade (as da superfície da
    // cavidade pertencem aos cubos vizinhos).
    // Arestas: as do bloco menos as 108 estritamente internas à cavidade 4 × 4 × 4 (300 arestas da
    // rede 4 × 4 × 4 menos as 192 da sua superfície, que pertencem aos cubos vizinhos).
    CAD_CHECK(c.vertexEdges == vvFormula(10, 10, 10) - (vvFormula(4, 4, 4) - 192));
    CAD_CHECK(d->vertexId(5, 5, 5) == 0); // ponto no interior da cavidade
    const fs::path file = outputDir() / "cavidade.inp";
    exportTo(*d, file);
    checkFile("cavidade", s, *d, file, LengthUnit::Meter);
}

void teste6_deslocado()
{
    std::printf("[6] sólido deslocado da origem\n");
    const TopoDS_Shape s = box(12.5, -7, 3, 3, 2, 2);
    const auto d = run(s, 1.0);
    const fs::path file = outputDir() / "deslocado.inp";
    exportTo(*d, file);
    const FileModel m = readModel(file);
    CAD_CHECK(!m.nodeLines.empty() && m.nodeLines[0] == "N,1,0.0125,-0.007,0.003");
    CAD_CHECK(m.nodeLines.size() > 1 && m.nodeLines[1] == "N,2,0.0135,-0.007,0.003");
    CAD_CHECK(d->vertexNodeCount() == 36);
    CAD_CHECK(std::int64_t(m.nodeLines.size()) == d->totalNodeCount()
              && m.nodeLines[std::size_t(d->vertexNodeCount())] == "N,37,0.013,-0.0065,0.0035");
    checkFile("deslocado", s, *d, file, LengthUnit::Meter);
    const fs::path fileMm = outputDir() / "deslocado_mm.inp";
    exportTo(*d, fileMm, LengthUnit::Millimeter);
    const FileModel mm = readModel(fileMm);
    CAD_CHECK(!mm.nodeLines.empty() && mm.nodeLines[0] == "N,1,12.5,-7,3");
    checkFile("deslocado mm", s, *d, fileMm, LengthUnit::Millimeter);
}

void teste7_lco()
{
    std::printf("[7] diferentes valores de lco (bloco 4 x 4 x 2 mm)\n");
    for (double lco : {0.5, 1.0, 2.0})
    {
        const TopoDS_Shape s = box(0, 0, 0, 4, 4, 2);
        const auto d = run(s, lco);
        const std::int64_t nx = std::llround(4 / lco), nz = std::llround(2 / lco);
        const LinkCounts c = ApdlLinkSequence::count(*d);
        CAD_CHECK(c.vertexEdges == vvFormula(nx, nx, nz));
        CAD_CHECK(c.centerCenter == ccFormula(nx, nx, nz));
        CAD_CHECK(c.centerVertex == 8 * nx * nx * nz);
        const fs::path file = outputDir() / ("lco_" + std::to_string(int(lco * 10)) + ".inp");
        exportTo(*d, file);
        checkFile(("lco " + NodeAsciiExporter::formatCoordinate(lco)).c_str(), s, *d, file, LengthUnit::Meter);
    }
    // lco = 0,1 mm (mínimo atual) em um bloco pequeno.
    const TopoDS_Shape s = box(0, 0, 0, 1, 0.5, 0.3);
    const auto d = run(s, 0.1);
    CAD_CHECK(d->cubeCount() == 10 * 5 * 3);
    const fs::path file = outputDir() / "lco_01.inp";
    exportTo(*d, file);
    const FileModel m = readModel(file);
    CAD_CHECK(m.nodeLines.size() > 1 && m.nodeLines[1] == "N,2,0.0001,0,0");
    checkFile("lco 0.1", s, *d, file, LengthUnit::Meter);
}

void teste8e9_unidades()
{
    std::printf("[8/9] metros e milímetros: mesmos IDs e elementos; coordenadas x 1000\n");
    const TopoDS_Shape s = box(0, 0, 0, 3, 2, 2);
    const auto d = run(s, 0.5);
    const fs::path m = outputDir() / "unidade_m.inp";
    const fs::path mm = outputDir() / "unidade_mm.inp";
    exportTo(*d, m, LengthUnit::Meter);
    exportTo(*d, mm, LengthUnit::Millimeter);
    const FileModel a = readModel(m);
    const FileModel b = readModel(mm);
    CAD_CHECK(a.nodeIds == b.nodeIds);
    CAD_CHECK(a.elements == b.elements);
    bool scaled = a.coords.size() == b.coords.size();
    for (std::size_t n = 0; scaled && n < a.coords.size(); ++n)
        for (int ax = 0; ax < 3; ++ax)
            scaled = scaled && std::fabs(a.coords[n][std::size_t(ax)] * 1000.0 - b.coords[n][std::size_t(ax)]) < 1e-9;
    CAD_CHECK(scaled);
    CAD_CHECK(b.nodeLines.size() > 1 && b.nodeLines[1] == "N,2,0.5,0,0");
    CAD_CHECK(a.nodeLines.size() > 1 && a.nodeLines[1] == "N,2,0.0005,0,0");
    const std::vector<std::string> lines = readLines(mm);
    CAD_CHECK(std::find(lines.begin(), lines.end(),
                        "! Unidade das coordenadas: milimetros (mm). O APDL nao impoe um sistema de unidades: os dados")
              != lines.end());
    checkFile("milimetros", s, *d, mm, LengthUnit::Millimeter);
}

// --- 10 a 14 e opções --------------------------------------------------------------

void teste10a13_verificacao()
{
    std::printf("[10-13] verificação por releitura detecta IDs/elementos duplicados e conectividade inválida\n");
    const TopoDS_Shape s = box(0, 0, 0, 3, 3, 3);
    const auto d = run(s, 1.0);
    const fs::path file = outputDir() / "verificacao.inp";
    exportTo(*d, file);
    ApdlOptions o;
    CAD_CHECK(ApdlExporter::verify(file, *d, o).ok);
    const std::string text = readAll(file);
    auto tampered = [&](const std::string& from, const std::string& to) {
        std::string t = text;
        const std::size_t at = t.find(from);
        CAD_CHECK(at != std::string::npos);
        if (at != std::string::npos)
            t.replace(at, from.size(), to);
        const fs::path f = outputDir() / "verificacao_alterado.inp";
        writeAll(f, t);
        const ApdlFileCheck c = ApdlExporter::verify(f, *d, o);
        std::printf("  adulteração \"%s\" -> %s\n", to.substr(0, to.size() - 1).c_str(), c.message.c_str());
        return c.ok;
    };
    CAD_CHECK(!tampered("N,2,", "N,1,"));                       // ID de nó duplicado
    CAD_CHECK(!tampered("EN,2,2,3\n", "EN,2,1,2\n"));           // elemento duplicado
    CAD_CHECK(!tampered("EN,2,2,3\n", "EN,2,2,2\n"));           // conectividade inválida
    CAD_CHECK(!tampered("EN,2,2,3\n", "EN,2,2,3,4\n"));         // mais de dois nós
    CAD_CHECK(!tampered("EN,3,3,4\n", "EN,3,3,4\nEN,3,3,4\n")); // ID de elemento repetido
    CAD_CHECK(!tampered("/PREP7\n", "/PREP7\nMP,EX,1,2e11\n")); // propriedade física
    CAD_CHECK(!tampered("EN,2,2,3\n", ""));                     // elemento ausente
}

void teste14_ascii()
{
    std::printf("[14] exportação ASCII preservada e coerente com o APDL\n");
    const TopoDS_Shape s = BRepAlgoAPI_Cut(box(0, 0, 0, 6, 6, 3), box(2, 2, -1, 2, 2, 5)).Shape();
    const auto d = run(s, 1.0);
    const fs::path ascii = outputDir() / "nos_ascii.txt";
    const NodeExportResult ra = NodeAsciiExporter().write(*d, ascii, LengthUnit::Meter, true);
    CAD_CHECK(ra.ok);
    CAD_CHECK(NodeAsciiExporter::verify(ascii, *d, LengthUnit::Meter).ok);
    const fs::path apdl = outputDir() / "nos_apdl.inp";
    exportTo(*d, apdl);
    const std::vector<std::string> asciiLines = readLines(ascii);
    const FileModel m = readModel(apdl);
    CAD_CHECK(asciiLines.size() == m.nodeLines.size() + 1);
    CAD_CHECK(!asciiLines.empty() && asciiLines[0] == kNodeFileHeader);
    bool same = asciiLines.size() == m.nodeLines.size() + 1;
    for (std::size_t n = 0; same && n < m.nodeLines.size(); ++n)
    {
        std::string a = asciiLines[n + 1];
        std::replace(a.begin(), a.end(), ' ', ',');
        same = "N," + a == m.nodeLines[n];
    }
    CAD_CHECK(same); // mesmos IDs, mesma ordem e mesma formatação numérica
    CAD_CHECK(NodeAsciiExporter::suggestedFileName("Peça 1") == "Peça 1_nodes.txt");
    CAD_CHECK(ApdlExporter::suggestedFileName("Peça 1") == "Peça 1_PREP7.inp");
    CAD_CHECK(ApdlExporter::suggestedFileName("") == "peca_PREP7.inp");
}

void testeOpcoes()
{
    std::printf("[+] /CLEAR opcional, sem comandos de solução, texto ASCII, sobrescrita, booleana em L\n");
    const TopoDS_Shape s = box(0, 0, 0, 2, 2, 2);
    const auto d = run(s, 1.0);
    const fs::path file = outputDir() / "sem_clear.inp";
    ApdlOptions o;
    o.includeClear = false;
    o.timestamp = "01/02/2026, 03:04:05";
    o.partName = "Peça com acentuação";
    CAD_CHECK(ApdlExporter().write(*d, file, o, true).ok);
    const std::vector<std::string> lines = readLines(file);
    CAD_CHECK(std::find(lines.begin(), lines.end(), "/CLEAR") == lines.end());
    CAD_CHECK(lines.size() > 3 && lines[2].rfind("! /CLEAR omitido", 0) == 0);
    CAD_CHECK(lines.size() > 3 && lines[3] == "!Code generate on 01/02/2026, 03:04:05");
    CAD_CHECK(std::find(lines.begin(), lines.end(), "! Peca: Pe_a com acentua__o") != lines.end());
    const std::string text = readAll(file);
    CAD_CHECK(std::all_of(text.begin(), text.end(), [](char c) { return static_cast<unsigned char>(c) < 0x80; }));
    for (const char* forbidden : {"\nSOLVE", "\n/SOLU", "\nMP,", "\nR,", "\nMAT,", "\nTB,", "\nD,", "\nF,", "\nEDWRITE"})
        CAD_CHECK(text.find(forbidden) == std::string::npos);
    CAD_CHECK(ApdlExporter::verify(file, *d, o).ok);
    // Com a opção errada a releitura acusa diferença de cabeçalho.
    ApdlOptions withClear = o;
    withClear.includeClear = true;
    CAD_CHECK(!ApdlExporter::verify(file, *d, withClear).ok);
    // Arquivo existente não é substituído sem autorização; discretização vazia é recusada.
    CAD_CHECK(!ApdlExporter().write(*d, file, o, false).ok);
    CAD_CHECK(!ApdlExporter().write(Discretization(), outputDir() / "vazio.inp", o, true).ok);
    CAD_CHECK(!fs::exists(outputDir() / "vazio.inp"));
    CAD_CHECK(!ApdlExporter().write(*d, outputDir() / "inexistente" / "x.inp", o, true).ok);

    // Geometria obtida por operação booleana (união em L) e por subtração (degrau).
    const TopoDS_Shape l = BRepAlgoAPI_Fuse(box(0, 0, 0, 6, 2, 2), box(0, 2, 0, 2, 4, 2)).Shape();
    const auto dl = run(l, 1.0);
    CAD_CHECK(dl->cubeCount() == 24 + 16);
    const fs::path fl = outputDir() / "uniao_L.inp";
    exportTo(*dl, fl);
    checkFile("uniao em L", l, *dl, fl, LengthUnit::Meter);
    // Degrau obtido por subtração: cantos reentrantes sem ligações diagonais entre centros.
    const TopoDS_Shape step = BRepAlgoAPI_Cut(box(0, 0, 0, 4, 4, 2), box(2, 0, -1, 3, 2, 4)).Shape();
    const auto ds = run(step, 1.0);
    const fs::path fs2 = outputDir() / "degrau.inp";
    exportTo(*ds, fs2);
    checkFile("degrau", step, *ds, fs2, LengthUnit::Meter);
}

void testeDesempenho()
{
    std::printf("[D] desempenho: bloco 100 x 100 x 10, lco = 1 (100.000 cubos)\n");
    const TopoDS_Shape s = box(0, 0, 0, 100, 100, 10);
    const auto d = run(s, 1.0);
    const fs::path file = outputDir() / "bloco_100k.inp";
    const auto t0 = std::chrono::steady_clock::now();
    const ApdlExportResult r = exportTo(*d, file);
    const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    CAD_CHECK(r.links.vertexEdges == vvFormula(100, 100, 10));
    CAD_CHECK(r.links.centerCenter == ccFormula(100, 100, 10));
    CAD_CHECK(r.links.centerVertex == 800000);
    std::printf("  %lld nós, %lld elementos, %.1f MB, exportação + verificação %.2f s\n", (long long)r.nodes,
                (long long)r.links.total(), double(r.bytes) / 1048576.0, t);
    std::error_code ec;
    fs::remove(file, ec);
}

} // namespace

int main()
{
    testeR_referencia();
    teste1a3_blocos();
    teste4_furo();
    teste5_cavidade();
    teste6_deslocado();
    teste7_lco();
    teste8e9_unidades();
    teste10a13_verificacao();
    teste14_ascii();
    testeOpcoes();
    testeDesempenho();
    return cad::test::finish("e5722");
}
