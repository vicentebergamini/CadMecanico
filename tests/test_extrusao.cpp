// Extrusão paramétrica de esboços (E5.3), sem Qt: volumes, planos, sentido, validação de perfis,
// edição, dependências, sólidos múltiplos e exportação/round-trip STEP.
#include "TestCheck.h"

#include "document/CadDocument.h"
#include "io/StepExporter.h"
#include "io/StepImporter.h"
#include "modeling/ShapeInspector.h"
#include "sketch/SketchProfile.h"

#include <TopAbs_ShapeEnum.hxx>

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <limits>

using namespace cad::document;
using namespace cad::sketch;
using cad::modeling::ShapeInspector;
namespace fs = std::filesystem;

namespace {

constexpr double kPi = 3.14159265358979323846;

// Cria um esboço com as geometrias dadas e o inclui no documento.
std::string addSketch(CadDocument& doc, PlaneType plane, std::initializer_list<SketchGeometry> geometries)
{
    Sketch s = doc.createSketch(plane);
    for (const SketchGeometry& g : geometries)
        CAD_CHECK(s.addEntity(g).ok);
    std::string error;
    CAD_CHECK(doc.commitSketch(s, error));
    return s.id();
}

void box(const TopoDS_Shape& s, double& x0, double& y0, double& z0, double& x1, double& y1, double& z1)
{
    ShapeInspector::boundingBox(s).Get(x0, y0, z0, x1, y1, z1);
}

bool boxIs(const TopoDS_Shape& s, double x0, double y0, double z0, double x1, double y1, double z1, double tol = 1e-6)
{
    double a, b, c, d, e, f;
    box(s, a, b, c, d, e, f);
    const bool ok = std::fabs(a - x0) < tol && std::fabs(b - y0) < tol && std::fabs(c - z0) < tol
                    && std::fabs(d - x1) < tol && std::fabs(e - y1) < tol && std::fabs(f - z1) < tol;
    if (!ok)
        std::printf("  caixa [%.4f %.4f %.4f]-[%.4f %.4f %.4f]\n", a, b, c, d, e, f);
    return ok;
}

void checkSolid(const char* tag, const TopoDS_Shape& s, double volume, int faces)
{
    const auto sum = ShapeInspector::inspect(s);
    std::printf("  [%s] volume %.4f (esperado %.4f), faces %d, valido %d\n", tag, sum.volume, volume, sum.faces,
                sum.isValid);
    CAD_CHECK(sum.isValid);
    CAD_CHECK(sum.solids == 1);
    CAD_CHECK(sum.faces == faces);
    CAD_CHECK(std::fabs(sum.volume - volume) <= std::max(1e-6, volume * 1e-9));
}

// Tenta extrudar e espera recusa sem alterar o documento.
void checkRejected(CadDocument& doc, const ExtrudeParameters& p, const char* tag, const char* expectedText = nullptr)
{
    const size_t ops = doc.operations().size();
    const int revision = doc.revision();
    const TopoDS_Shape shape = doc.shape();
    const auto preview = doc.previewExtrusion(p);
    const ValidationResult r = doc.addExtrusion(p);
    std::printf("  recusado (%s): %s\n", tag, r.message.c_str());
    CAD_CHECK(!r.ok && !r.message.empty());
    CAD_CHECK(!preview.ok && !preview.message.empty());
    if (expectedText)
        CAD_CHECK(r.message.find(expectedText) != std::string::npos);
    CAD_CHECK(doc.operations().size() == ops && doc.revision() == revision && doc.shape().IsSame(shape));
}

double stepRoundTripVolume(const TopoDS_Shape& shape, const fs::path& file, int& solids, bool& valid)
{
    CAD_CHECK(cad::io::StepExporter().write(shape, file).ok);
    const auto read = cad::io::StepImporter().read(file);
    CAD_CHECK(read.ok);
    const auto s = ShapeInspector::inspect(read.shape);
    solids = s.solids;
    valid = s.isValid;
    return s.volume;
}

} // namespace

int main()
{
    const fs::path dir = fs::temp_directory_path() / "cadmecanico_extrusao";
    fs::create_directories(dir);

    // 1. Documento inicial vazio
    std::printf("1. Documento vazio\n");
    CadDocument doc;
    CAD_CHECK(doc.operations().empty() && doc.shape().IsNull() && doc.solidCount() == 0 && !doc.hasPart());

    // 2-4. Esboço XY e extrusão de retângulo 100 x 60, profundidade 20
    std::printf("2-4. Retangulo XY\n");
    const std::string rectSketch = addSketch(doc, PlaneType::XY, {SketchRectangle{0.0, 0.0, 100.0, 60.0}});
    CAD_CHECK(rectSketch == "sk-1");
    // Pré-visualização não altera o documento.
    {
        const auto preview = doc.previewExtrusion({"", rectSketch, 20.0, false});
        CAD_CHECK(preview.ok);
        CAD_CHECK(std::fabs(ShapeInspector::volume(preview.shape) - 120000.0) < 1e-6);
        CAD_CHECK(doc.operations().empty() && doc.shape().IsNull());
    }
    std::string blockId;
    CAD_CHECK(doc.addExtrusion({"", rectSketch, 20.0, false}, &blockId).ok);
    CAD_CHECK(blockId == "op-1");
    const Operation* ext = doc.findOperation(blockId);
    CAD_CHECK(ext && ext->type == OperationType::Extrude && ext->name == "Extrusão 1" && ext->sketchId == rectSketch);
    CAD_CHECK(ext->status == OperationStatus::Valid && !ext->result.IsNull());
    checkSolid("bloco 100x60x20", doc.shape(), 120000.0, 6);
    CAD_CHECK(boxIs(doc.shape(), 0, 0, 0, 100, 60, 20));
    CAD_CHECK(doc.solidCount() == 1 && doc.isModified());
    CAD_CHECK(!doc.hasPart()); // extrusão não é a peça de exemplo

    // 5-6. Círculo R10, profundidade 20
    std::printf("5-6. Circulo\n");
    {
        CadDocument c;
        const std::string s = addSketch(c, PlaneType::XY, {SketchCircle{50.0, 30.0, 10.0}});
        CAD_CHECK(c.addExtrusion({"Cilindro", s, 20.0, false}).ok);
        checkSolid("cilindro R10 x 20", c.shape(), 2000.0 * kPi, 3);
        CAD_CHECK(boxIs(c.shape(), 40, 20, 0, 60, 40, 20, 1e-6));
        CAD_CHECK(c.operations()[0].name == "Cilindro");
    }

    // 7. Planos XZ e YZ; 8. inversão da direção
    std::printf("7-8. Planos XZ, YZ e inversao\n");
    {
        CadDocument p;
        // XZ: u = X, v = Z, normal -Y.
        const std::string xz = addSketch(p, PlaneType::XZ, {SketchRectangle{0.0, 0.0, 100.0, 60.0}});
        CAD_CHECK(p.addExtrusion({"", xz, 20.0, false}).ok);
        checkSolid("XZ", p.operations()[0].result, 120000.0, 6);
        CAD_CHECK(boxIs(p.operations()[0].result, 0, -20, 0, 100, 0, 60));
        // YZ: u = Y, v = Z, normal +X.
        const std::string yz = addSketch(p, PlaneType::YZ, {SketchCircle{0.0, 0.0, 5.0}});
        CAD_CHECK(p.addExtrusion({"", yz, 30.0, false}).ok);
        checkSolid("YZ", p.operations()[1].result, kPi * 25.0 * 30.0, 3);
        CAD_CHECK(boxIs(p.operations()[1].result, 0, -5, -5, 30, 5, 5));
        // Inversões: sentido oposto à normal, volume positivo.
        const std::string xy = addSketch(p, PlaneType::XY, {SketchRectangle{0.0, 0.0, 10.0, 10.0}});
        CAD_CHECK(p.addExtrusion({"", xy, 5.0, true}).ok);
        checkSolid("XY invertida", p.operations()[2].result, 500.0, 6);
        CAD_CHECK(boxIs(p.operations()[2].result, 0, 0, -5, 10, 10, 0));
        CAD_CHECK(p.updateExtrusion(p.operations()[0].id, {"", xz, 20.0, true}).ok);
        CAD_CHECK(boxIs(p.operations()[0].result, 0, 0, 0, 100, 20, 60));
        CAD_CHECK(ShapeInspector::volume(p.operations()[0].result) > 0.0);
        // Sólidos independentes: composto com três sólidos, sem união.
        CAD_CHECK(p.solidCount() == 3);
        CAD_CHECK(p.shape().ShapeType() == TopAbs_COMPOUND);
        CAD_CHECK(std::fabs(ShapeInspector::volume(p.shape()) - (120000.0 + kPi * 750.0 + 500.0)) < 1e-6);
    }

    // 9-10. Profundidades inválidas
    std::printf("9-10. Profundidade invalida\n");
    {
        const double nan = std::numeric_limits<double>::quiet_NaN();
        checkRejected(doc, {"", rectSketch, 0.0, false}, "zero", "maior que zero");
        checkRejected(doc, {"", rectSketch, -5.0, false}, "negativa");
        checkRejected(doc, {"", rectSketch, nan, false}, "NaN");
        checkRejected(doc, {"", rectSketch, 20000.0, false}, "enorme");
        checkRejected(doc, {"", "sk-99", 10.0, false}, "esboco inexistente");
        CAD_CHECK(!doc.updateExtrusion(blockId, {"", rectSketch, 0.0, false}).ok);
    }

    // 11-14. Perfis inválidos e múltiplos contornos
    std::printf("11-14. Perfis\n");
    {
        CadDocument v;
        const std::string open = addSketch(v, PlaneType::XY,
            {SketchLine{0, 0, 50, 0}, SketchLine{50, 0, 50, 30}, SketchLine{50, 30, 0, 30}});
        checkRejected(v, {"", open, 10.0, false}, "aberto", "Perfil aberto");
        const std::string bowtie = addSketch(v, PlaneType::XY,
            {SketchLine{0, 0, 10, 10}, SketchLine{10, 10, 10, 0}, SketchLine{10, 0, 0, 10}, SketchLine{0, 10, 0, 0}});
        checkRejected(v, {"", bowtie, 10.0, false}, "autointersecao", "autointersectante");
        const std::string flat = addSketch(v, PlaneType::XY,
            {SketchLine{0, 0, 10, 0}, SketchLine{10, 0, 20, 0}, SketchLine{20, 0, 0, 0}});
        checkRejected(v, {"", flat, 10.0, false}, "area nula", "área nula");
        const std::string dup = addSketch(v, PlaneType::XY, {SketchLine{0, 0, 10, 0}, SketchLine{10, 0, 0, 0}});
        checkRejected(v, {"", dup, 10.0, false}, "duplicado", "duplicados");
        const std::string tee = addSketch(v, PlaneType::XY,
            {SketchLine{0, 0, 10, 0}, SketchLine{10, 0, 10, 10}, SketchLine{10, 10, 0, 0}, SketchLine{10, 0, 20, 0},
             SketchLine{20, 0, 10, 10}});
        checkRejected(v, {"", tee, 10.0, false}, "ambiguo", "ambíguo");
        const std::string two = addSketch(v, PlaneType::XY,
            {SketchRectangle{0, 0, 100, 60}, SketchCircle{50, 30, 10}});
        checkRejected(v, {"", two, 10.0, false}, "multiplos", "contornos");
        const std::string twoRects = addSketch(v, PlaneType::XY,
            {SketchRectangle{0, 0, 10, 10}, SketchRectangle{20, 0, 10, 10}});
        checkRejected(v, {"", twoRects, 10.0, false}, "dois retangulos", "contornos");
        const std::string empty = addSketch(v, PlaneType::XY, {});
        checkRejected(v, {"", empty, 10.0, false}, "vazio", "não contém entidades");
        CAD_CHECK(v.operations().empty() && v.shape().IsNull());

        // Contornos fechados de linhas: polígono em L (área 4500) e triângulo com folga < tolerância.
        const std::string ell = addSketch(v, PlaneType::XY,
            {SketchLine{0, 0, 100, 0}, SketchLine{100, 0, 100, 30}, SketchLine{100, 30, 50, 30},
             SketchLine{50, 30, 50, 60}, SketchLine{50, 60, 0, 60}, SketchLine{0, 60, 0, 0}});
        CAD_CHECK(v.addExtrusion({"", ell, 10.0, false}).ok);
        checkSolid("poligono em L", v.operations().back().result, 45000.0, 8);
        const std::string tri = addSketch(v, PlaneType::XZ,
            {SketchLine{0, 0, 40, 0}, SketchLine{40.0005, 0, 0, 30}, SketchLine{0, 30.0004, 0, 0}});
        CAD_CHECK(v.addExtrusion({"", tri, 10.0, true}).ok); // sentido horário na ordem das linhas
        checkSolid("triangulo XZ", v.operations().back().result, 0.5 * 40.0 * 30.0 * 10.0, 5);
        CAD_CHECK(kClosureTolerance == 1e-3);
    }

    // 15. Edição da profundidade: 20 -> 35 mm
    std::printf("15. Edicao da profundidade\n");
    {
        const int rev = doc.revision();
        CAD_CHECK(doc.updateExtrusion(blockId, {"Bloco extrudado", rectSketch, 35.0, false}).ok);
        checkSolid("bloco 100x60x35", doc.shape(), 210000.0, 6);
        CAD_CHECK(doc.revision() == rev + 1);
        CAD_CHECK(doc.findOperation(blockId)->name == "Bloco extrudado");
        CAD_CHECK(doc.extrudeParameters(blockId).depth == 35.0);
        CAD_CHECK(doc.operations().size() == 1 && doc.operations()[0].id == blockId); // id estável
    }

    // 16. Recálculo após edição do esboço de origem
    std::printf("16. Recalculo apos editar o esboco\n");
    {
        Sketch s = *doc.findSketch(rectSketch);
        CAD_CHECK(s.setDimension(s.entities()[0].id, "width", 80.0).ok);
        std::string error;
        CAD_CHECK(doc.commitSketch(s, error));
        checkSolid("bloco 80x60x35", doc.shape(), 80.0 * 60.0 * 35.0, 6);
        CAD_CHECK(boxIs(doc.shape(), 0, 0, 0, 80, 60, 35));
    }

    // 17. Último estado válido preservado após falha
    std::printf("17. Preservacao apos falha\n");
    {
        const TopoDS_Shape before = doc.shape();
        const int rev = doc.revision();
        Sketch broken = *doc.findSketch(rectSketch);
        CAD_CHECK(broken.removeEntity(broken.entities()[0].id));
        CAD_CHECK(broken.addEntity(SketchLine{0, 0, 10, 0}).ok); // perfil aberto
        std::string error;
        CAD_CHECK(!doc.commitSketch(broken, error));
        std::printf("  esboco recusado: %s\n", error.c_str());
        CAD_CHECK(error.find("extrusão dependente") != std::string::npos);
        CAD_CHECK(doc.shape().IsSame(before) && doc.revision() == rev);
        CAD_CHECK(doc.findSketch(rectSketch)->entities().size() == 1);
        CAD_CHECK(std::holds_alternative<SketchRectangle>(doc.findSketch(rectSketch)->entities()[0].geometry));
        CAD_CHECK(!doc.updateExtrusion(blockId, {"", rectSketch, -1.0, false}).ok);
        CAD_CHECK(doc.shape().IsSame(before));
    }

    // 18. Referências esboço <-> extrusão
    std::printf("18. Referencias\n");
    {
        std::string error;
        CAD_CHECK(doc.dependentOperations(rectSketch) == std::vector<std::string>{blockId});
        CAD_CHECK(!doc.removeSketch(rectSketch, error));
        std::printf("  remocao recusada: %s\n", error.c_str());
        CAD_CHECK(doc.findSketch(rectSketch) != nullptr);
        const std::string loose = addSketch(doc, PlaneType::XY, {SketchCircle{0, 0, 1}});
        CAD_CHECK(doc.removeSketch(loose, error)); // sem dependentes: removível
        CAD_CHECK(!doc.updateExtrusion("op-99", {"", rectSketch, 5.0, false}).ok);
        CAD_CHECK(!doc.updateExtrusion(blockId, {"", "sk-99", 5.0, false}).ok);
        CAD_CHECK(doc.nextOperationNumber() == 2);
        // Exemplo + extrusão: ids continuam a numeração (op-1, op-2 da peça; op-3 extrusão).
        CadDocument ex = CadDocument::withTestPart();
        const std::string s = addSketch(ex, PlaneType::XY, {SketchRectangle{200, 0, 10, 10}});
        std::string id;
        CAD_CHECK(ex.addExtrusion({"", s, 10.0, false}, &id).ok && id == "op-3");
        CAD_CHECK(ex.solidCount() == 2 && ex.hasPart());
        const double expected = 100.0 * 60.0 * 20.0 - kPi * 100.0 * 20.0 + 1000.0;
        CAD_CHECK(std::fabs(ShapeInspector::volume(ex.shape()) - expected) < 1e-6);
        // Alterar a peça recalcula tudo e mantém a extrusão.
        CAD_CHECK(ex.setParameters({120.0, 60.0, 20.0, 10.0}).ok);
        CAD_CHECK(ex.solidCount() == 2 && ex.findOperation("op-3")->status == OperationStatus::Valid);
    }

    // 24-25. Exportação STEP e round-trip
    std::printf("24-25. STEP\n");
    {
        int solids = 0;
        bool valid = false;
        double v = stepRoundTripVolume(doc.shape(), dir / "bloco.step", solids, valid);
        std::printf("  bloco: volume relido %.4f\n", v);
        CAD_CHECK(solids == 1 && valid && std::fabs(v - 80.0 * 60.0 * 35.0) < 1e-6 * 168000.0);

        CadDocument c;
        const std::string s = addSketch(c, PlaneType::XY, {SketchCircle{0, 0, 10}});
        CAD_CHECK(c.addExtrusion({"", s, 20.0, false}).ok);
        v = stepRoundTripVolume(c.shape(), dir / "cilindro.step", solids, valid);
        std::printf("  cilindro: volume relido %.4f\n", v);
        CAD_CHECK(solids == 1 && valid && std::fabs(v - 2000.0 * kPi) < 1e-6 * 2000.0 * kPi);

        CAD_CHECK(c.updateExtrusion(c.operations()[0].id, {"", s, 35.0, false}).ok);
        v = stepRoundTripVolume(c.shape(), dir / "cilindro_35.step", solids, valid);
        CAD_CHECK(solids == 1 && valid && std::fabs(v - 3500.0 * kPi) < 1e-6 * 3500.0 * kPi);

        CadDocument m = CadDocument::withTestPart();
        const std::string r = addSketch(m, PlaneType::XY, {SketchRectangle{200, 0, 10, 10}});
        CAD_CHECK(m.addExtrusion({"", r, 10.0, false}).ok);
        v = stepRoundTripVolume(m.shape(), dir / "dois_solidos.step", solids, valid);
        std::printf("  dois solidos: %d solidos, volume %.4f\n", solids, v);
        CAD_CHECK(solids == 2 && valid);
    }

    return cad::test::finish("test_extrusao");
}
