// Operações booleanas paramétricas (E5.4), sem Qt: união, subtração e interseção de corpos
// extrudados, volumes, furo passante real, sólidos disjuntos/coincidentes/tangentes, operandos
// inválidos, ordem da subtração, edição, recálculo de dependências, ausência de duplicação e
// exportação STEP/STL.
#include "TestCheck.h"

#include "document/CadDocument.h"
#include "io/StepExporter.h"
#include "io/StepImporter.h"
#include "io/StlExporter.h"
#include "modeling/ShapeInspector.h"

#include <BRepClass3d_SolidClassifier.hxx>
#include <BRepTools_WireExplorer.hxx>
#include <BRep_Tool.hxx>
#include <GeomAbs_SurfaceType.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>

#include <cmath>
#include <cstdio>
#include <filesystem>

using namespace cad::document;
using namespace cad::sketch;
using cad::modeling::ShapeInspector;
namespace fs = std::filesystem;

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kBox = 100.0 * 60.0 * 20.0;
const double kHoled = kBox - 2000.0 * kPi; // 113716,8147 mm³

std::string addSketch(CadDocument& doc, PlaneType plane, std::initializer_list<SketchGeometry> geometries)
{
    Sketch s = doc.createSketch(plane);
    for (const SketchGeometry& g : geometries)
        CAD_CHECK(s.addEntity(g).ok);
    std::string error;
    CAD_CHECK(doc.commitSketch(s, error));
    return s.id();
}

std::string extrude(CadDocument& doc, const std::string& sketchId, double depth, bool reversed = false)
{
    std::string id;
    const auto r = doc.addExtrusion({"", sketchId, depth, reversed}, &id);
    if (!r.ok)
        std::printf("  extrusão recusada: %s\n", r.message.c_str());
    CAD_CHECK(r.ok);
    return id;
}

// Caixa [x0,x0+w] x [y0,y0+h] x [0,depth] a partir de um esboço XY.
std::string box(CadDocument& doc, double x0, double y0, double w, double h, double depth)
{
    return extrude(doc, addSketch(doc, PlaneType::XY, {SketchRectangle{x0, y0, w, h}}), depth);
}

// Bloco 100 x 60 x 20 com z em [5, 25] (esboço XZ, extrusão na normal -Y: y em [-60, 0]) e
// cilindro R10 de z = 0 a z = 30 centrado no bloco: o cilindro ultrapassa as duas faces.
struct BlockAndCylinder
{
    std::string block, cylinder, cylinderSketch;
};
BlockAndCylinder blockAndCylinder(CadDocument& doc)
{
    BlockAndCylinder b;
    b.block = extrude(doc, addSketch(doc, PlaneType::XZ, {SketchRectangle{0.0, 5.0, 100.0, 20.0}}), 60.0);
    b.cylinderSketch = addSketch(doc, PlaneType::XY, {SketchCircle{50.0, -30.0, 10.0}});
    b.cylinder = extrude(doc, b.cylinderSketch, 30.0);
    return b;
}

std::string boolean(CadDocument& doc, BooleanKind kind, const std::string& a, const std::string& b)
{
    std::string id;
    const auto r = doc.addBoolean({"", kind, a, b}, &id);
    if (!r.ok)
        std::printf("  booleana recusada: %s\n", r.message.c_str());
    CAD_CHECK(r.ok);
    return id;
}

double volumeOf(const CadDocument& doc, const std::string& id)
{
    const Operation* op = doc.findOperation(id);
    return op ? ShapeInspector::volume(op->result) : -1.0;
}

void report(const char* tag, const TopoDS_Shape& s, double expected)
{
    const auto sum = ShapeInspector::inspect(s);
    std::printf("  [%s] volume %.4f (esperado %.4f), solidos %d, faces %d, valido %d\n", tag, sum.volume, expected,
                sum.solids, sum.faces, sum.isValid);
    CAD_CHECK(sum.isValid);
    CAD_CHECK(std::fabs(sum.volume - expected) <= std::max(1e-6, expected * 1e-9));
}

// Tenta a booleana e espera recusa sem alterar o documento.
void checkRejected(CadDocument& doc, const BooleanParameters& p, const char* tag, const char* expected = nullptr)
{
    const size_t ops = doc.operations().size();
    const int revision = doc.revision();
    const int next = doc.nextOperationNumber();
    const TopoDS_Shape shape = doc.shape();
    const auto r = doc.addBoolean(p);
    std::printf("  recusado (%s): %s\n", tag, r.message.c_str());
    CAD_CHECK(!r.ok && !r.message.empty());
    if (expected)
        CAD_CHECK(r.message.find(expected) != std::string::npos);
    CAD_CHECK(doc.operations().size() == ops && doc.revision() == revision && doc.shape().IsSame(shape));
    CAD_CHECK(doc.nextOperationNumber() == next);
}

bool inside(const TopoDS_Shape& s, double x, double y, double z)
{
    BRepClass3d_SolidClassifier c(s, gp_Pnt(x, y, z), 1e-7);
    return c.State() == TopAbs_IN;
}

} // namespace

int main()
{
    const fs::path out = fs::temp_directory_path() / "cadmecanico_booleanas";
    fs::create_directories(out);

    std::printf("1-2. Uniao de solidos sobrepostos\n");
    {
        CadDocument doc;
        const std::string a = box(doc, 0, 0, 100, 60, 20);
        const std::string b = box(doc, 50, 30, 100, 60, 20);
        CAD_CHECK(doc.solidCount() == 2); // antes da booleana: dois corpos ativos
        const std::string u = boolean(doc, BooleanKind::Fuse, a, b);
        // V(A ∪ B) = V(A) + V(B) − V(A ∩ B), com a interseção calculada de forma independente.
        CadDocument other;
        const std::string a2 = box(other, 0, 0, 100, 60, 20);
        const std::string b2 = box(other, 50, 30, 100, 60, 20);
        const std::string c2 = boolean(other, BooleanKind::Common, a2, b2);
        const double common = volumeOf(other, c2);
        std::printf("  V(A)=%.4f V(B)=%.4f V(A∩B)=%.4f\n", kBox, kBox, common);
        CAD_CHECK(std::fabs(common - 30000.0) < 1e-6);
        report("A ∪ B", doc.findOperation(u)->result, kBox + kBox - common);
        CAD_CHECK(cad::modeling::countSolids(doc.shape()) == 1);
        CAD_CHECK(doc.activeBodies() == std::vector<std::string>{u});
        CAD_CHECK(doc.consumerOf(a) == u && doc.consumerOf(b) == u);
        CAD_CHECK(doc.findOperation(u)->name == "União 1");
    }

    std::printf("3-5. Subtracao de cilindro (furo passante real)\n");
    {
        CadDocument doc;
        const BlockAndCylinder bc = blockAndCylinder(doc);
        const std::string cut = boolean(doc, BooleanKind::Cut, bc.block, bc.cylinder);
        const TopoDS_Shape& s = doc.findOperation(cut)->result;
        report("bloco − cilindro", s, kHoled);
        std::printf("  volume esperado 120000 − 2000π = %.4f mm³\n", kHoled);
        const auto sum = ShapeInspector::inspect(s);
        CAD_CHECK(sum.solids == 1);
        CAD_CHECK(sum.faces == 7); // 6 planas + parede cilíndrica do furo
        // Furo passante: o eixo do furo está fora do material em toda a altura do bloco.
        bool axisFree = true;
        for (double z = 5.5; z < 25.0; z += 1.0)
            axisFree = axisFree && !inside(s, 50.0, -30.0, z);
        CAD_CHECK(axisFree);
        CAD_CHECK(inside(s, 50.0, -30.0 + 15.0, 15.0)); // material ao lado do furo
        // Faces superior e inferior com dois contornos (externo + furo); parede cilíndrica.
        int planarWithHole = 0, cylinders = 0;
        for (TopExp_Explorer f(s, TopAbs_FACE); f.More(); f.Next())
        {
            const TopoDS_Face face = TopoDS::Face(f.Current());
            int wires = 0;
            for (TopExp_Explorer w(face, TopAbs_WIRE); w.More(); w.Next())
                ++wires;
            const GeomAbs_SurfaceType type = BRepAdaptor_Surface(face).GetType();
            if (type == GeomAbs_Plane && wires == 2)
                ++planarWithHole;
            if (type == GeomAbs_Cylinder)
                ++cylinders;
        }
        std::printf("  faces planas com furo: %d, faces cilindricas: %d\n", planarWithHole, cylinders);
        CAD_CHECK(planarWithHole == 2 && cylinders == 1);
        CAD_CHECK(doc.findOperation(cut)->name == "Subtração 1");

        std::printf("12. Ordem da subtracao\n");
        CadDocument rev;
        const BlockAndCylinder bc2 = blockAndCylinder(rev);
        const std::string cut2 = boolean(rev, BooleanKind::Cut, bc2.cylinder, bc2.block);
        // Cilindro − bloco: dois discos (z em [0,5] e [25,30]) desconectados.
        report("cilindro − bloco", rev.findOperation(cut2)->result, 100.0 * kPi * 10.0);
        CAD_CHECK(cad::modeling::countSolids(rev.findOperation(cut2)->result) == 2);
        CAD_CHECK(rev.solidCount() == 2);

        std::printf("17. STEP do bloco furado\n");
        const fs::path step = out / "bloco_furado.step";
        CAD_CHECK(cad::io::StepExporter().write(doc.shape(), step).ok);
        const auto read = cad::io::StepImporter().read(step);
        CAD_CHECK(read.ok);
        const auto rs = ShapeInspector::inspect(read.shape);
        std::printf("  STEP relido: %d solido(s), volume %.4f, valido %d\n", rs.solids, rs.volume, rs.isValid);
        CAD_CHECK(rs.solids == 1 && rs.isValid && std::fabs(rs.volume - kHoled) < 1e-6);
        CAD_CHECK(rs.faces == 7);

        std::printf("18. STL do bloco furado\n");
        for (auto format : {cad::io::StlFormat::Binary, cad::io::StlFormat::Ascii})
            for (auto quality : {cad::io::StlQuality::Low, cad::io::StlQuality::Medium, cad::io::StlQuality::High})
            {
                cad::io::StlExportOptions o;
                o.format = format;
                o.quality = quality;
                const fs::path f = out / (std::string("bloco_furado_") + (format == cad::io::StlFormat::Binary ? "bin_" : "ascii_")
                                          + cad::io::qualityName(quality) + ".stl");
                const auto r = cad::io::StlExporter().write(doc.shape(), f, o);
                std::printf("  %s: %s, %d triangulos\n", f.filename().string().c_str(), r.ok ? "ok" : r.message.c_str(),
                            r.triangles);
                CAD_CHECK(r.ok && r.solids == 1 && r.triangles > 12);
            }

        std::printf("19. Ausencia de solidos duplicados\n");
        CAD_CHECK(doc.activeBodies() == std::vector<std::string>{cut});
        CAD_CHECK(doc.solidCount() == 1);
        CAD_CHECK(doc.operations().size() == 3); // operandos preservados no histórico
        CAD_CHECK(doc.findOperation(bc.block)->consumedBy == cut);
        CAD_CHECK(!doc.findOperation(bc.block)->result.IsNull()); // resultado do operando mantido
    }

    std::printf("6-7. Intersecao\n");
    {
        CadDocument doc;
        const std::string a = box(doc, 0, 0, 100, 60, 20);
        const std::string b = box(doc, 50, 30, 100, 60, 20);
        const std::string c = boolean(doc, BooleanKind::Common, a, b);
        report("A ∩ B", doc.findOperation(c)->result, 30000.0);
        CAD_CHECK(doc.findOperation(c)->name == "Interseção 1");
        // Cilindro ∩ bloco = cilindro dentro do bloco (20 mm de altura).
        CadDocument doc2;
        const BlockAndCylinder bc = blockAndCylinder(doc2);
        const std::string c2 = boolean(doc2, BooleanKind::Common, bc.block, bc.cylinder);
        report("bloco ∩ cilindro", doc2.findOperation(c2)->result, 2000.0 * kPi);
    }

    std::printf("8. Solidos disjuntos\n");
    {
        CadDocument doc;
        const std::string a = box(doc, 0, 0, 100, 60, 20);
        const std::string b = box(doc, 200, 0, 100, 60, 20);
        checkRejected(doc, {"", BooleanKind::Common, a, b}, "intersecao vazia", "Interseção vazia");
        const std::string u = boolean(doc, BooleanKind::Fuse, a, b);
        const TopoDS_Shape& s = doc.findOperation(u)->result;
        report("disjuntos A ∪ B", s, 2.0 * kBox);
        const int solids = cad::modeling::countSolids(s);
        std::printf("  uniao de disjuntos: %d solidos (composto), tipo %s\n", solids,
                    s.ShapeType() == TopAbs_COMPOUND ? "COMPOUND" : "outro");
        CAD_CHECK(solids == 2 && s.ShapeType() == TopAbs_COMPOUND); // não é informado como sólido único
        CAD_CHECK(doc.solidCount() == 2);
        // Subtração de disjuntos: A inalterado.
        CadDocument doc2;
        const std::string a2 = box(doc2, 0, 0, 100, 60, 20);
        const std::string b2 = box(doc2, 200, 0, 100, 60, 20);
        const std::string c2 = boolean(doc2, BooleanKind::Cut, a2, b2);
        report("disjuntos A − B", doc2.findOperation(c2)->result, kBox);
    }

    std::printf("9. Solidos coincidentes\n");
    {
        for (BooleanKind kind : {BooleanKind::Fuse, BooleanKind::Common})
        {
            CadDocument doc;
            const std::string a = box(doc, 0, 0, 100, 60, 20);
            const std::string b = box(doc, 0, 0, 100, 60, 20);
            const std::string r = boolean(doc, kind, a, b);
            report(kind == BooleanKind::Fuse ? "coincidentes A ∪ B" : "coincidentes A ∩ B", doc.findOperation(r)->result,
                   kBox);
            CAD_CHECK(cad::modeling::countSolids(doc.shape()) == 1);
            CAD_CHECK(ShapeInspector::inspect(doc.shape()).faces == 6);
        }
        CadDocument doc;
        const std::string a = box(doc, 0, 0, 100, 60, 20);
        const std::string b = box(doc, 0, 0, 100, 60, 20);
        checkRejected(doc, {"", BooleanKind::Cut, a, b}, "coincidentes A − B", "eliminou");
    }

    std::printf("10. Solidos tangentes (face comum em x = 100)\n");
    {
        CadDocument doc;
        const std::string a = box(doc, 0, 0, 100, 60, 20);
        const std::string b = box(doc, 100, 0, 100, 60, 20);
        checkRejected(doc, {"", BooleanKind::Common, a, b}, "tangentes A ∩ B", "Interseção vazia");
        const std::string u = boolean(doc, BooleanKind::Fuse, a, b);
        report("tangentes A ∪ B", doc.findOperation(u)->result, 2.0 * kBox);
        CAD_CHECK(cad::modeling::countSolids(doc.shape()) == 1);
        CAD_CHECK(ShapeInspector::inspect(doc.shape()).faces == 6); // faces coplanares unificadas
        CadDocument doc2;
        const std::string a2 = box(doc2, 0, 0, 100, 60, 20);
        const std::string b2 = box(doc2, 100, 0, 100, 60, 20);
        const std::string c2 = boolean(doc2, BooleanKind::Cut, a2, b2);
        report("tangentes A − B", doc2.findOperation(c2)->result, kBox);
    }

    std::printf("11. Operandos invalidos\n");
    {
        CadDocument doc;
        const std::string a = box(doc, 0, 0, 100, 60, 20);
        const std::string b = box(doc, 50, 30, 100, 60, 20);
        const std::string c = box(doc, 0, 0, 10, 10, 50);
        checkRejected(doc, {"", BooleanKind::Fuse, a, "op-99"}, "inexistente", "inexistente");
        checkRejected(doc, {"", BooleanKind::Fuse, a, a}, "A = B", "diferentes");
        checkRejected(doc, {"", BooleanKind::Fuse, a, ""}, "B vazio", "selecione");
        checkRejected(doc, {"", BooleanKind::Fuse, "sk-1", b}, "esboco como operando", "inexistente");
        const std::string u = boolean(doc, BooleanKind::Fuse, a, b);
        checkRejected(doc, {"", BooleanKind::Cut, a, c}, "corpo ja consumido", "já é usado");
        // Edição que faria a booleana depender de uma operação posterior (ciclo).
        const std::string later = boolean(doc, BooleanKind::Cut, u, c);
        const TopoDS_Shape before = doc.shape();
        const auto r = doc.updateBoolean(u, {"", BooleanKind::Fuse, a, later});
        std::printf("  recusado (ciclo): %s\n", r.message.c_str());
        CAD_CHECK(!r.ok && r.message.find("circular") != std::string::npos);
        CAD_CHECK(doc.shape().IsSame(before));
        const auto self = doc.updateBoolean(u, {"", BooleanKind::Fuse, a, u});
        std::printf("  recusado (proprio resultado): %s\n", self.message.c_str());
        CAD_CHECK(!self.ok);
        // Pré-visualização também recusa.
        CAD_CHECK(!doc.previewBoolean({"", BooleanKind::Fuse, a, c}).ok);
        CAD_CHECK(!doc.previewBoolean({"", BooleanKind::Fuse, "x", c}).ok);
    }

    std::printf("13. Cancelamento / pre-visualizacao sem alterar o documento\n");
    {
        CadDocument doc;
        const std::string a = box(doc, 0, 0, 100, 60, 20);
        const std::string b = box(doc, 50, 30, 100, 60, 20);
        const int revision = doc.revision();
        const auto p = doc.previewBoolean({"", BooleanKind::Cut, a, b});
        CAD_CHECK(p.ok && std::fabs(ShapeInspector::volume(p.shape) - 90000.0) < 1e-6);
        CAD_CHECK(doc.revision() == revision && doc.operations().size() == 2 && doc.solidCount() == 2);
        CAD_CHECK(doc.consumerOf(a).empty());
    }

    std::printf("14. Edicao parametrica (tipo e operandos)\n");
    {
        CadDocument doc;
        const BlockAndCylinder bc = blockAndCylinder(doc);
        const std::string op = boolean(doc, BooleanKind::Cut, bc.block, bc.cylinder);
        CAD_CHECK(doc.updateBoolean(op, {"Bloco ∪ cilindro", BooleanKind::Fuse, bc.block, bc.cylinder}).ok);
        // União: bloco + partes do cilindro fora do bloco (z 0..5 e 25..30).
        report("editado para uniao", doc.findOperation(op)->result, kBox + 1000.0 * kPi);
        CAD_CHECK(doc.findOperation(op)->name == "Bloco ∪ cilindro");
        CAD_CHECK(doc.updateBoolean(op, {"", BooleanKind::Common, bc.block, bc.cylinder}).ok);
        report("editado para intersecao", doc.findOperation(op)->result, 2000.0 * kPi);
        CAD_CHECK(doc.updateBoolean(op, {"", BooleanKind::Cut, bc.cylinder, bc.block}).ok); // operandos trocados
        report("operandos trocados", doc.findOperation(op)->result, 1000.0 * kPi);
        CAD_CHECK(doc.updateBoolean(op, {"", BooleanKind::Cut, bc.block, bc.cylinder}).ok);
        report("de volta a A − B", doc.findOperation(op)->result, kHoled);
        CAD_CHECK(doc.booleanParameters(op).kind == BooleanKind::Cut);
    }

    std::printf("15. Recalculo de dependencias (raio do cilindro e profundidade)\n");
    {
        CadDocument doc;
        const BlockAndCylinder bc = blockAndCylinder(doc);
        const std::string cut = boolean(doc, BooleanKind::Cut, bc.block, bc.cylinder);
        Sketch s = *doc.findSketch(bc.cylinderSketch);
        CAD_CHECK(s.updateEntity(s.entities()[0].id, SketchCircle{50.0, -30.0, 15.0}).ok);
        std::string error;
        CAD_CHECK(doc.commitSketch(s, error));
        report("raio 15", doc.findOperation(cut)->result, kBox - 225.0 * kPi * 20.0);
        CAD_CHECK(doc.shape().IsSame(doc.findOperation(cut)->result));
        // Profundidade do cilindro 15: furo cego a partir da face inferior (z 5..15).
        CAD_CHECK(doc.updateExtrusion(bc.cylinder, {"", bc.cylinderSketch, 15.0, false}).ok);
        report("profundidade 15 (furo cego)", doc.findOperation(cut)->result, kBox - 225.0 * kPi * 10.0);
        CAD_CHECK(doc.updateExtrusion(bc.cylinder, {"", bc.cylinderSketch, 30.0, false}).ok);

        // Falha na reconstrução: interseção que fica vazia. O último estado válido é mantido e a
        // mensagem identifica a operação responsável.
        CadDocument doc2;
        const BlockAndCylinder bc2 = blockAndCylinder(doc2);
        const std::string common = boolean(doc2, BooleanKind::Common, bc2.block, bc2.cylinder);
        const TopoDS_Shape before = doc2.shape();
        const int revision = doc2.revision();
        Sketch far = *doc2.findSketch(bc2.cylinderSketch);
        CAD_CHECK(far.updateEntity(far.entities()[0].id, SketchCircle{500.0, -30.0, 10.0}).ok);
        std::string error2;
        CAD_CHECK(!doc2.commitSketch(far, error2));
        std::printf("  recusado: %s\n", error2.c_str());
        CAD_CHECK(error2.find("Interseção 1") != std::string::npos && error2.find("vazia") != std::string::npos);
        CAD_CHECK(doc2.shape().IsSame(before) && doc2.revision() == revision);
        CAD_CHECK(std::get<SketchCircle>(doc2.findSketch(bc2.cylinderSketch)->entities()[0].geometry).cx == 50.0);
        const auto depth = doc2.updateExtrusion(bc2.cylinder, {"", bc2.cylinderSketch, 2.0, false});
        std::printf("  recusado (profundidade 2): %s\n", depth.message.c_str());
        CAD_CHECK(!depth.ok && doc2.shape().IsSame(before));
        CAD_CHECK(doc2.findOperation(common)->status == OperationStatus::Valid);
    }

    std::printf("Cadeia de booleanas e peca de exemplo como operando\n");
    {
        CadDocument doc = CadDocument::withTestPart();
        const std::string ext = box(doc, 100, 0, 50, 60, 20); // encostado na face x = 100 da peça
        const std::string u = boolean(doc, BooleanKind::Fuse, "op-2", ext);
        report("peca ∪ bloco", doc.findOperation(u)->result, kBox - 2000.0 * kPi + 60000.0);
        const std::string tool = box(doc, 140, 20, 5, 5, 20);
        const std::string cut = boolean(doc, BooleanKind::Cut, u, tool);
        report("(peca ∪ bloco) − prisma", doc.findOperation(cut)->result, kBox - 2000.0 * kPi + 60000.0 - 500.0);
        CAD_CHECK(doc.activeBodies() == std::vector<std::string>{cut});
        // Bloco base não é corpo (consumido pelo furo): recusado como operando.
        checkRejected(doc, {"", BooleanKind::Fuse, "op-1", cut}, "bloco base", "não produz");
        // Parâmetros da peça propagam pela cadeia.
        PartParameters pp;
        pp.holeRadius = 5.0;
        CAD_CHECK(doc.setParameters(pp).ok);
        report("raio do furo 5", doc.findOperation(cut)->result, kBox - 500.0 * kPi + 60000.0 - 500.0);
    }

    std::printf("20. Integridade topologica\n");
    {
        CadDocument doc;
        const BlockAndCylinder bc = blockAndCylinder(doc);
        const std::string cut = boolean(doc, BooleanKind::Cut, bc.block, bc.cylinder);
        const auto s = ShapeInspector::inspect(doc.shape());
        std::printf("  solidos %d, cascas %d, faces %d, arestas %d, vertices %d, valido %d\n", s.solids, s.shells,
                    s.faces, s.edges, s.vertices, s.isValid);
        CAD_CHECK(s.isValid && s.solids == 1 && s.shells == 1);
        CAD_CHECK(doc.findOperation(cut)->status == OperationStatus::Valid);
        CAD_CHECK(s.volume > 0.0);
    }

    return cad::test::finish("test_booleanas");
}
