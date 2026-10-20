// Exportação STL (E5.3), sem Qt: formatos binário/ASCII, qualidade, dimensões, triângulos,
// edição paramétrica, rejeições, falhas de gravação, preservação do documento e do STEP, e
// leitura do STL por leitor independente e pelo RWStl do OCCT.
#include "StlTestUtils.h"
#include "TestCheck.h"

#include "document/CadDocument.h"
#include "io/StepExporter.h"
#include "io/StepImporter.h"
#include "io/StlExporter.h"
#include "modeling/ShapeInspector.h"

#include <BRep_Tool.hxx>
#include <Poly_Triangulation.hxx>
#include <RWStl.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>

using namespace cad::document;
using namespace cad::io;
using namespace cad::sketch;
namespace fs = std::filesystem;
namespace stl = cad::test::stl;

namespace {

constexpr double kPi = 3.14159265358979323846;

std::string addSketch(CadDocument& doc, PlaneType plane, std::initializer_list<SketchGeometry> geometries)
{
    Sketch s = doc.createSketch(plane);
    for (const SketchGeometry& g : geometries)
        CAD_CHECK(s.addEntity(g).ok);
    std::string error;
    CAD_CHECK(doc.commitSketch(s, error));
    return s.id();
}

bool near3(const stl::Vec3& v, double x, double y, double z, double tol)
{
    return std::fabs(v.x - x) <= tol && std::fabs(v.y - y) <= tol && std::fabs(v.z - z) <= tol;
}

// Verifica uma malha lida: triângulos válidos, sem degenerados, coordenadas finitas e volume
// coerente com o sólido.
stl::Mesh checkMesh(const fs::path& file, const StlExportResult& r, double solidVolume, double volumeTol, bool binary)
{
    const stl::Mesh m = stl::read(file.u8string());
    std::printf("  %s: %s, %zu triangulos (exportador: %d), volume %.4f (solido %.4f), caixa [%.3f %.3f %.3f]-[%.3f %.3f %.3f]\n",
                file.filename().u8string().c_str(), m.binary ? "binario" : "ASCII", m.triangles.size(), r.triangles,
                m.volume(), solidVolume, m.min.x, m.min.y, m.min.z, m.max.x, m.max.y, m.max.z);
    CAD_CHECK(m.ok);
    CAD_CHECK(m.binary == binary);
    CAD_CHECK(int(m.triangles.size()) == r.triangles);
    CAD_CHECK(m.finite());
    CAD_CHECK(m.degenerate() == 0);
    CAD_CHECK(m.volume() > 0.0); // normais para fora
    CAD_CHECK(std::fabs(m.volume() - solidVolume) <= volumeTol);
    return m;
}

bool documentFacesHaveMesh(const CadDocument& doc)
{
    for (TopExp_Explorer ex(doc.shape(), TopAbs_FACE); ex.More(); ex.Next())
    {
        TopLoc_Location loc;
        if (!BRep_Tool::Triangulation(TopoDS::Face(ex.Current()), loc).IsNull())
            return true;
    }
    return false;
}

std::vector<fs::path> leftovers(const fs::path& dir)
{
    std::vector<fs::path> tmp;
    for (const auto& e : fs::directory_iterator(dir))
        if (e.path().filename().u8string().find(".~cadmec-") != std::string::npos)
            tmp.push_back(e.path());
    return tmp;
}

} // namespace

int main()
{
    const fs::path dir = fs::temp_directory_path() / "cadmecanico_stl";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);

    CadDocument doc;
    const std::string rect = addSketch(doc, PlaneType::XY, {SketchRectangle{0.0, 0.0, 100.0, 60.0}});
    std::string blockId;
    CAD_CHECK(doc.addExtrusion({"", rect, 20.0, false}, &blockId).ok);

    // 1. STL binário de bloco extrudado (padrão binário); 6. dimensões externas; 7. triângulos válidos
    std::printf("1/6/7. Bloco binario\n");
    {
        StlExportOptions o; // padrão: binário, média
        CAD_CHECK(o.format == StlFormat::Binary && o.quality == StlQuality::Medium);
        const fs::path f = dir / "bloco.stl";
        const StlExportResult r = StlExporter().write(doc.shape(), f, o);
        std::printf("  %s; deflexao %.4f mm / %.1f graus\n", r.message.c_str(), r.parameters.linearDeflection,
                    r.parameters.angularDeflectionDeg);
        CAD_CHECK(r.ok && r.solids == 1 && r.faces == 6 && r.triangles >= 12);
        const stl::Mesh m = checkMesh(f, r, 120000.0, 1e-3, true);
        CAD_CHECK(near3(m.min, 0, 0, 0, 1e-4) && near3(m.max, 100, 60, 20, 1e-4)); // mm, sem conversão
        CAD_CHECK(fs::file_size(f) == 84u + 50u * std::uintmax_t(r.triangles));
    }

    // 2. ASCII
    std::printf("2. ASCII\n");
    {
        StlExportOptions o;
        o.format = StlFormat::Ascii;
        const fs::path f = dir / "bloco_ascii.stl";
        const StlExportResult r = StlExporter().write(doc.shape(), f, o);
        CAD_CHECK(r.ok);
        const stl::Mesh m = checkMesh(f, r, 120000.0, 1e-3, false);
        CAD_CHECK(near3(m.min, 0, 0, 0, 1e-4) && near3(m.max, 100, 60, 20, 1e-4));
    }

    // 3-5. Cilindro extrudado em três qualidades: Alta aproxima melhor que Baixa
    std::printf("3/4/5. Cilindro e qualidades\n");
    {
        CadDocument c;
        const std::string s = addSketch(c, PlaneType::XY, {SketchCircle{0.0, 0.0, 10.0}});
        CAD_CHECK(c.addExtrusion({"", s, 20.0, false}).ok);
        const double exact = 2000.0 * kPi;
        double error[3] = {0, 0, 0};
        int triangles[3] = {0, 0, 0};
        const char* names[3] = {"baixa", "media", "alta"};
        const StlQuality qualities[3] = {StlQuality::Low, StlQuality::Medium, StlQuality::High};
        for (int i = 0; i < 3; ++i)
        {
            StlExportOptions o;
            o.quality = qualities[i];
            const fs::path f = dir / (std::string("cilindro_") + names[i] + ".stl");
            const StlExportResult r = StlExporter().write(c.shape(), f, o);
            std::printf("  %s: deflexao linear %.4f mm, angular %.1f graus\n", qualityName(qualities[i]),
                        r.parameters.linearDeflection, r.parameters.angularDeflectionDeg);
            CAD_CHECK(r.ok);
            const stl::Mesh m = checkMesh(f, r, exact, 0.05 * exact, true);
            // Aproximação geométrica: a malha está inscrita no cilindro, então o volume é menor que
            // o exato e o erro diminui com a qualidade.
            CAD_CHECK(m.volume() < exact);
            error[i] = std::fabs(m.volume() - exact);
            triangles[i] = r.triangles;
            // 6. Dimensões externas: caixa do cilindro (a malha está inscrita no círculo).
            CAD_CHECK(m.min.z > -1e-6 && std::fabs(m.max.z - 20.0) < 1e-4);
            CAD_CHECK(m.max.x <= 10.0 + 1e-4 && m.max.x >= 10.0 - r.parameters.linearDeflection - 1e-4);
        }
        std::printf("  erro de volume: baixa %.4f, media %.4f, alta %.4f mm3 (%.3f%%, %.3f%%, %.3f%%); triangulos %d/%d/%d\n",
                    error[0], error[1], error[2], 100 * error[0] / exact, 100 * error[1] / exact, 100 * error[2] / exact,
                    triangles[0], triangles[1], triangles[2]);
        CAD_CHECK(error[2] < error[0]);
        CAD_CHECK(error[1] <= error[0] && error[2] <= error[1]);
        CAD_CHECK(StlExporter::presetParameters(StlQuality::High, c.shape()).linearDeflection
                  < StlExporter::presetParameters(StlQuality::Low, c.shape()).linearDeflection);
        // Tolerâncias personalizadas e limites.
        StlExportOptions custom;
        custom.linearDeflection = 0.02;
        custom.angularDeflectionDeg = 3.0;
        const StlExportResult r = StlExporter().write(c.shape(), dir / "cilindro_personalizado.stl", custom);
        CAD_CHECK(r.ok && r.parameters.linearDeflection == 0.02 && r.parameters.angularDeflectionDeg == 3.0);
        custom.linearDeflection = 0.0;
        CAD_CHECK(!StlExporter().write(c.shape(), dir / "invalido.stl", custom).ok);
        custom.linearDeflection = 0.02;
        custom.angularDeflectionDeg = 180.0;
        CAD_CHECK(!StlExporter().write(c.shape(), dir / "invalido.stl", custom).ok);
        CAD_CHECK(!fs::exists(dir / "invalido.stl"));
    }

    // 8. Após alterar a profundidade da extrusão (20 -> 35)
    std::printf("8. Apos editar a profundidade\n");
    {
        CAD_CHECK(doc.updateExtrusion(blockId, {"", rect, 35.0, false}).ok);
        const fs::path f = dir / "bloco_35.stl";
        const StlExportResult r = StlExporter().write(doc.shape(), f);
        CAD_CHECK(r.ok);
        const stl::Mesh m = checkMesh(f, r, 210000.0, 1e-3, true);
        CAD_CHECK(near3(m.max, 100, 60, 35, 1e-4));
    }

    // 9-10. Documento vazio e documento só com esboços
    std::printf("9-10. Sem solido\n");
    {
        CadDocument empty;
        StlExportResult r = StlExporter().write(empty.shape(), dir / "vazio.stl");
        std::printf("  vazio: %s\n", r.message.c_str());
        CAD_CHECK(!r.ok && !fs::exists(dir / "vazio.stl"));
        CadDocument sketches;
        addSketch(sketches, PlaneType::XY, {SketchRectangle{0, 0, 10, 10}});
        r = StlExporter().write(sketches.shape(), dir / "esbocos.stl");
        std::printf("  so esbocos: %s\n", r.message.c_str());
        CAD_CHECK(!r.ok && !fs::exists(dir / "esbocos.stl"));
    }

    // 11. Caminho inválido
    std::printf("11. Caminho invalido\n");
    {
        StlExportResult r = StlExporter().write(doc.shape(), dir / "nao_existe" / "a.stl");
        std::printf("  %s\n", r.message.c_str());
        CAD_CHECK(!r.ok && !fs::exists(dir / "nao_existe"));
        r = StlExporter().write(doc.shape(), dir); // é um diretório
        CAD_CHECK(!r.ok);
        r = StlExporter().write(doc.shape(), fs::path());
        CAD_CHECK(!r.ok);
    }

    // 12. Falha de gravação: destino somente leitura não é alterado e não sobra temporário
    std::printf("12. Falha de gravacao\n");
    {
        const fs::path f = dir / "somente_leitura.stl";
        {
            std::ofstream out(f, std::ios::binary);
            out << "conteudo original";
        }
        fs::permissions(f, fs::perms::owner_write | fs::perms::group_write | fs::perms::others_write,
                        fs::perm_options::remove);
        const StlExportResult r = StlExporter().write(doc.shape(), f);
        std::printf("  %s\n", r.message.c_str());
        CAD_CHECK(!r.ok);
        std::ifstream in(f, std::ios::binary);
        const std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        CAD_CHECK(content == "conteudo original");
        in.close();
        fs::permissions(f, fs::perms::owner_write, fs::perm_options::add);
        CAD_CHECK(leftovers(dir).empty());
        // Substituição de arquivo existente gravável (a confirmação fica na interface).
        const StlExportResult again = StlExporter().write(doc.shape(), f);
        CAD_CHECK(again.ok && fs::file_size(f) == 84u + 50u * std::uintmax_t(again.triangles));
        CAD_CHECK(leftovers(dir).empty());
    }

    // 13-14. STEP preservado; documento paramétrico intacto (malha só na cópia)
    std::printf("13-14. STEP e documento preservados\n");
    {
        const TopoDS_Shape before = doc.shape();
        const int rev = doc.revision();
        CAD_CHECK(!documentFacesHaveMesh(doc));
        CAD_CHECK(StlExporter().write(doc.shape(), dir / "outra.stl").ok);
        CAD_CHECK(doc.shape().IsSame(before) && doc.revision() == rev);
        CAD_CHECK(!documentFacesHaveMesh(doc)); // a triangulação não tocou o B-Rep do documento
        CAD_CHECK(std::fabs(cad::modeling::ShapeInspector::volume(doc.shape()) - 210000.0) < 1e-6);
        CAD_CHECK(doc.extrudeParameters(blockId).depth == 35.0);
        const fs::path step = dir / "bloco_35.step";
        CAD_CHECK(StepExporter().write(doc.shape(), step).ok);
        const auto read = StepImporter().read(step);
        CAD_CHECK(read.ok);
        CAD_CHECK(std::fabs(cad::modeling::ShapeInspector::volume(read.shape) - 210000.0) < 1e-3);
    }

    // Nome de arquivo com acentos (caminho Unicode).
    std::printf("Nome com acentos\n");
    {
        const fs::path f = dir / fs::u8path(u8"peça_média_ç.stl");
        const StlExportResult r = StlExporter().write(doc.shape(), f);
        CAD_CHECK(r.ok && fs::exists(f));
        CAD_CHECK(fs::file_size(f) == 84u + 50u * std::uintmax_t(r.triangles));
        CAD_CHECK(leftovers(dir).empty());
    }

    // 15. Leitor independente (acima) e RWStl do OCCT
    std::printf("15. Leitura pelo RWStl\n");
    {
        for (const char* name : {"bloco.stl", "bloco_ascii.stl", "cilindro_alta.stl"})
        {
            const auto tri = RWStl::ReadFile((dir / name).u8string().c_str());
            const stl::Mesh m = stl::read((dir / name).u8string());
            std::printf("  %s: RWStl %d triangulos, leitor proprio %zu\n", name, tri.IsNull() ? -1 : tri->NbTriangles(),
                        m.triangles.size());
            CAD_CHECK(!tri.IsNull() && tri->NbTriangles() == int(m.triangles.size()));
        }
    }

    return cad::test::finish("test_stl");
}
