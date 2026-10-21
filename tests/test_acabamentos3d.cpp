// Fillet 3D e Chamfer 3D (E5.5B), sem Qt: arredondamento e chanfro de arestas de corpos
// extrudados e booleanos, uma ou várias arestas, rejeições, integridade topológica, edição,
// recálculo após extrusão e booleana, referência persistente de arestas (remapeamento, aresta
// não remapeável, ambiguidade) e exportação STEP/STL.
#include "TestCheck.h"

#include "document/CadDocument.h"
#include "io/StepExporter.h"
#include "io/StepImporter.h"
#include "io/StlExporter.h"
#include "modeling/EdgeNaming.h"
#include "modeling/ShapeInspector.h"

#include <BRepAdaptor_Curve.hxx>
#include <BRep_Builder.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <limits>

using namespace cad::document;
using namespace cad::sketch;
using cad::modeling::EdgeSignature;
using cad::modeling::ShapeInspector;
namespace fs = std::filesystem;

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kBox = 120000.0;

// Material removido por unidade de comprimento: fillet r em aresta de 90° = r² − πr²/4;
// chanfro simétrico d = d²/2.
double filletLoss(double r) { return r * r - kPi * r * r / 4.0; }
double chamferLoss(double d) { return d * d / 2.0; }

std::string sketchRect(CadDocument& doc, double x, double y, double w, double h, PlaneType plane = PlaneType::XY)
{
    Sketch s = doc.createSketch(plane);
    CAD_CHECK(s.addEntity(SketchRectangle{x, y, w, h}).ok);
    std::string error;
    CAD_CHECK(doc.commitSketch(s, error));
    return s.id();
}

std::string extrude(CadDocument& doc, const std::string& sketch, double depth)
{
    std::string id;
    CAD_CHECK(doc.addExtrusion({"", sketch, depth, false}, &id).ok);
    return id;
}

// Aresta do corpo cujo ponto médio está em (x, y, z).
TopoDS_Edge edgeAt(const TopoDS_Shape& shape, double x, double y, double z)
{
    for (TopExp_Explorer ex(shape, TopAbs_EDGE); ex.More(); ex.Next())
    {
        const TopoDS_Edge e = TopoDS::Edge(ex.Current());
        BRepAdaptor_Curve c(e);
        if (c.Value(0.5 * (c.FirstParameter() + c.LastParameter())).Distance(gp_Pnt(x, y, z)) < 1e-6)
            return e;
    }
    std::printf("  aresta nao encontrada em (%.1f; %.1f; %.1f)\n", x, y, z);
    CAD_CHECK(false);
    return TopoDS_Edge();
}

FinishParameters finish(const CadDocument& doc, const std::string& body, std::initializer_list<TopoDS_Edge> edges,
                        double value)
{
    return FinishParameters{"", body, value, doc.edgeSignatures(body, std::vector<TopoDS_Edge>(edges))};
}

double volume(const CadDocument& doc, const std::string& id)
{
    return ShapeInspector::volume(doc.findOperation(id)->result);
}

void report(const char* tag, double v, double expected, double tol = 1e-3)
{
    std::printf("  [%s] volume %.4f (esperado %.4f)\n", tag, v, expected);
    CAD_CHECK(std::fabs(v - expected) <= tol);
}

void checkRejected(CadDocument& doc, OperationType type, const FinishParameters& p, const char* tag,
                   const char* expected = nullptr)
{
    const size_t ops = doc.operations().size();
    const int revision = doc.revision();
    const TopoDS_Shape shape = doc.shape();
    const auto r = doc.addFinish(type, p);
    std::printf("  recusado (%s): %s\n", tag, r.message.c_str());
    CAD_CHECK(!r.ok && !r.message.empty());
    if (expected)
        CAD_CHECK(r.message.find(expected) != std::string::npos);
    CAD_CHECK(doc.operations().size() == ops && doc.revision() == revision && doc.shape().IsSame(shape));
}

} // namespace

int main()
{
    std::setvbuf(stdout, nullptr, _IONBF, 0); // progresso visível mesmo se o processo falhar
    const fs::path out = fs::temp_directory_path() / "cadmecanico_acabamentos";
    fs::create_directories(out);

    std::printf("1/3. Fillet R5 em uma aresta do bloco 100 x 60 x 20\n");
    {
        CadDocument doc;
        const std::string box = extrude(doc, sketchRect(doc, 0, 0, 100, 60), 20);
        const TopoDS_Edge top = edgeAt(doc.shape(), 50, 0, 20); // aresta superior frontal (X)
        std::string id;
        const auto r = doc.addFinish(OperationType::Fillet3D, finish(doc, box, {top}, 5.0), &id);
        CAD_CHECK(r.ok);
        report("fillet R5", ShapeInspector::volume(doc.shape()), kBox - 100.0 * filletLoss(5.0));
        const auto s = ShapeInspector::inspect(doc.shape());
        std::printf("  faces %d (6 + 1 cilindrica), valido %d\n", s.faces, s.isValid);
        CAD_CHECK(s.faces == 7 && s.isValid && s.solids == 1);
        CAD_CHECK(doc.findOperation(id)->name == "Fillet 3D 1");
        CAD_CHECK(doc.consumerOf(box) == id && doc.activeBodies() == std::vector<std::string>{id});
        CAD_CHECK(doc.findOperation(id)->edges.size() == 1);
        const EdgeSignature& sig = doc.findOperation(id)->edges[0];
        std::printf("  assinatura: %s, meio (%.1f; %.1f; %.1f), comprimento %.1f, %zu normais\n",
                    cad::modeling::curveName(sig.curve), sig.mid.X(), sig.mid.Y(), sig.mid.Z(), sig.length,
                    sig.normals.size());
        CAD_CHECK(sig.curve == EdgeSignature::Curve::Line && sig.normals.size() == 2 && std::fabs(sig.length - 100.0) < 1e-9);
    }

    std::printf("2. Chamfer 3 mm em uma aresta\n");
    {
        CadDocument doc;
        const std::string box = extrude(doc, sketchRect(doc, 0, 0, 100, 60), 20);
        std::string id;
        CAD_CHECK(doc.addFinish(OperationType::Chamfer3D, finish(doc, box, {edgeAt(doc.shape(), 100, 30, 20)}, 3.0), &id).ok);
        report("chamfer 3", ShapeInspector::volume(doc.shape()), kBox - 60.0 * chamferLoss(3.0));
        CAD_CHECK(ShapeInspector::inspect(doc.shape()).faces == 7);
        CAD_CHECK(doc.findOperation(id)->name == "Chamfer 3D 1");
    }

    std::printf("4. Selecao de multiplas arestas\n");
    {
        CadDocument doc;
        const std::string box = extrude(doc, sketchRect(doc, 0, 0, 100, 60), 20);
        const TopoDS_Shape s = doc.shape();
        // Quatro arestas verticais: arredondamentos independentes (sem interação nos cantos).
        std::string id;
        CAD_CHECK(doc.addFinish(OperationType::Fillet3D,
                                finish(doc, box, {edgeAt(s, 0, 0, 10), edgeAt(s, 100, 0, 10), edgeAt(s, 100, 60, 10),
                                                  edgeAt(s, 0, 60, 10)},
                                       5.0),
                                &id)
                      .ok);
        report("4 arestas verticais R5", ShapeInspector::volume(doc.shape()), kBox - 4.0 * 20.0 * filletLoss(5.0));
        CAD_CHECK(doc.findOperation(id)->edges.size() == 4);
        CAD_CHECK(ShapeInspector::inspect(doc.shape()).faces == 10);
        // Duas arestas que se encontram num vértice (canto arredondado com concordância).
        CadDocument doc2;
        const std::string box2 = extrude(doc2, sketchRect(doc2, 0, 0, 100, 60), 20);
        const TopoDS_Shape s2 = doc2.shape();
        std::string id2;
        CAD_CHECK(doc2.addFinish(OperationType::Chamfer3D, finish(doc2, box2, {edgeAt(s2, 50, 0, 20), edgeAt(s2, 100, 30, 20)}, 2.0), &id2).ok);
        const double v2 = ShapeInspector::volume(doc2.shape());
        std::printf("  chanfro 2 em 2 arestas concorrentes: volume %.4f (< %.4f)\n", v2, kBox - 100.0 * chamferLoss(2.0));
        CAD_CHECK(v2 < kBox - 100.0 * chamferLoss(2.0) && v2 > kBox - 160.0 * chamferLoss(2.0));
        CAD_CHECK(ShapeInspector::isValid(doc2.shape()));
        // Aresta repetida na seleção.
        CadDocument doc3;
        const std::string box3 = extrude(doc3, sketchRect(doc3, 0, 0, 100, 60), 20);
        const TopoDS_Edge e = edgeAt(doc3.shape(), 50, 0, 20);
        checkRejected(doc3, OperationType::Fillet3D, finish(doc3, box3, {e, e}, 2.0), "aresta repetida", "repetida");
    }

    std::printf("5-8. Rejeicoes\n");
    {
        CadDocument doc;
        const std::string box = extrude(doc, sketchRect(doc, 0, 0, 100, 60), 20);
        const TopoDS_Edge e = edgeAt(doc.shape(), 50, 0, 20);
        checkRejected(doc, OperationType::Fillet3D, finish(doc, box, {e}, 0.0), "raio zero", "maior que zero");      // 5.
        checkRejected(doc, OperationType::Fillet3D, finish(doc, box, {e}, -2.0), "raio negativo", "maior que zero"); // 6.
        checkRejected(doc, OperationType::Fillet3D, finish(doc, box, {e}, 25.0), "raio excessivo", "inviável");     // 7.
        checkRejected(doc, OperationType::Fillet3D, finish(doc, box, {e}, std::numeric_limits<double>::quiet_NaN()), "raio NaN");
        checkRejected(doc, OperationType::Chamfer3D, finish(doc, box, {e}, 0.0), "distancia zero", "maior que zero"); // 8.
        checkRejected(doc, OperationType::Chamfer3D, finish(doc, box, {e}, 25.0), "distancia excessiva", "inviável");
        checkRejected(doc, OperationType::Chamfer3D, FinishParameters{"", box, 2.0, {}}, "sem arestas", "aresta");
        checkRejected(doc, OperationType::Fillet3D, FinishParameters{"", "op-9", 2.0, doc.edgeSignatures(box, {e})},
                      "corpo inexistente", "inexistente");
        CAD_CHECK(!doc.previewFinish(OperationType::Fillet3D, finish(doc, box, {e}, 25.0)).ok);
        CAD_CHECK(doc.previewFinish(OperationType::Fillet3D, finish(doc, box, {e}, 5.0)).ok);
        CAD_CHECK(doc.operations().size() == 1); // pré-visualização não altera o documento
    }

    std::printf("9-10. Integridade topologica e edicao de parametros\n");
    {
        CadDocument doc;
        const std::string box = extrude(doc, sketchRect(doc, 0, 0, 100, 60), 20);
        std::string id;
        CAD_CHECK(doc.addFinish(OperationType::Fillet3D, finish(doc, box, {edgeAt(doc.shape(), 50, 0, 20)}, 5.0), &id).ok);
        const auto s = ShapeInspector::inspect(doc.shape());
        std::printf("  solidos %d, cascas %d, faces %d, arestas %d, vertices %d, valido %d\n", s.solids, s.shells, s.faces,
                    s.edges, s.vertices, s.isValid);
        CAD_CHECK(s.isValid && s.solids == 1 && s.shells == 1 && s.faces == 7 && s.edges == 15 && s.vertices == 10);
        FinishParameters p = doc.finishParameters(id);
        p.value = 8.0;
        CAD_CHECK(doc.updateFinish(id, p).ok);
        report("raio editado 8", ShapeInspector::volume(doc.shape()), kBox - 100.0 * filletLoss(8.0));
        p.value = 30.0;
        const TopoDS_Shape before = doc.shape();
        const auto bad = doc.updateFinish(id, p);
        std::printf("  edicao recusada: %s\n", bad.message.c_str());
        CAD_CHECK(!bad.ok && doc.shape().IsSame(before) && doc.finishParameters(id).value == 8.0);
        // Seleção de arestas editada: troca para a aresta lateral direita.
        p = doc.finishParameters(id);
        const TopoDS_Shape source = doc.findOperation(box)->result;
        p.edges = doc.edgeSignatures(box, {edgeAt(source, 100, 30, 20)});
        p.value = 5.0;
        CAD_CHECK(doc.updateFinish(id, p).ok);
        report("aresta trocada", ShapeInspector::volume(doc.shape()), kBox - 60.0 * filletLoss(5.0));
        // Chamfer encadeado sobre o resultado do fillet (outra aresta).
        std::string ch;
        // A aresta superior traseira foi encurtada pelo fillet da aresta x = 100 (vai de x = 0 a 95).
        CAD_CHECK(doc.addFinish(OperationType::Chamfer3D, finish(doc, id, {edgeAt(doc.shape(), 47.5, 60, 20)}, 2.0), &ch).ok);
        // O OCCT propaga o chanfro pelas arestas tangentes: continua pelo arco do fillet na face
        // traseira (≈ π/2 · 5 mm), removendo um pouco mais que os 95 mm retos.
        const double chained = ShapeInspector::volume(doc.shape());
        const double straight = kBox - 60.0 * filletLoss(5.0) - 95.0 * chamferLoss(2.0);
        std::printf("  [fillet + chamfer encadeados] volume %.4f (95 mm retos: %.4f; com o arco tangente: menor)\n",
                    chained, straight);
        CAD_CHECK(chained < straight && chained > straight - 100.0);
        CAD_CHECK(ShapeInspector::isValid(doc.shape()) && doc.solidCount() == 1);
        CAD_CHECK(doc.activeBodies() == std::vector<std::string>{ch});
    }

    std::printf("11. Recalculo apos alterar a extrusao (remapeamento da aresta)\n");
    {
        CadDocument doc;
        const std::string sk = sketchRect(doc, 0, 0, 100, 60);
        const std::string box = extrude(doc, sk, 20);
        std::string id;
        CAD_CHECK(doc.addFinish(OperationType::Fillet3D, finish(doc, box, {edgeAt(doc.shape(), 50, 0, 20)}, 5.0), &id).ok);
        CAD_CHECK(doc.updateExtrusion(box, {"", sk, 30.0, false}).ok); // a aresta superior sobe para z = 30
        report("profundidade 30", ShapeInspector::volume(doc.shape()), 180000.0 - 100.0 * filletLoss(5.0));
        const EdgeSignature& sig = doc.findOperation(id)->edges[0];
        std::printf("  assinatura atualizada: meio (%.1f; %.1f; %.1f)\n", sig.mid.X(), sig.mid.Y(), sig.mid.Z());
        CAD_CHECK(std::fabs(sig.mid.Z() - 30.0) < 1e-9);
        // Largura do esboço alterada: aresta de 80 mm.
        Sketch s = *doc.findSketch(sk);
        CAD_CHECK(s.updateEntity("e-1", SketchRectangle{0, 0, 80, 60}).ok);
        std::string error;
        CAD_CHECK(doc.commitSketch(s, error));
        report("largura 80", ShapeInspector::volume(doc.shape()), 80.0 * 60.0 * 30.0 - 80.0 * filletLoss(5.0));
    }

    std::printf("12. Recalculo apos operacao booleana\n");
    {
        CadDocument doc;
        const std::string block = extrude(doc, sketchRect(doc, 0, 5, 100, 20, PlaneType::XZ), 60); // z 5..25, y -60..0
        Sketch c = doc.createSketch(PlaneType::XY);
        CAD_CHECK(c.addEntity(SketchCircle{50, -30, 10}).ok);
        std::string error;
        CAD_CHECK(doc.commitSketch(c, error));
        const std::string cyl = extrude(doc, c.id(), 30);
        std::string cut;
        CAD_CHECK(doc.addBoolean({"", BooleanKind::Cut, block, cyl}, &cut).ok);
        // Fillet R3 na aresta superior frontal (y = −60, z = 25) e chamfer 2 na circunferência
        // superior do furo.
        std::string f;
        CAD_CHECK(doc.addFinish(OperationType::Fillet3D, finish(doc, cut, {edgeAt(doc.shape(), 50, -60, 25)}, 3.0), &f).ok);
        TopoDS_Edge holeTop;
        for (TopExp_Explorer ex(doc.shape(), TopAbs_EDGE); ex.More(); ex.Next())
        {
            BRepAdaptor_Curve k(TopoDS::Edge(ex.Current()));
            if (k.GetType() == GeomAbs_Circle && std::fabs(k.Value(k.FirstParameter()).Z() - 25.0) < 1e-6)
                holeTop = TopoDS::Edge(ex.Current());
        }
        CAD_CHECK(!holeTop.IsNull());
        std::string ch;
        const auto rc = doc.addFinish(OperationType::Chamfer3D, finish(doc, f, {holeTop}, 2.0), &ch);
        if (!rc.ok)
            std::printf("  chanfro do furo recusado: %s\n", rc.message.c_str());
        CAD_CHECK(rc.ok);
        const double base = kBox - 2000.0 * kPi - 100.0 * filletLoss(3.0);
        const double v = ShapeInspector::volume(doc.shape());
        // Chanfro do furo: anel triangular de seção 2 x 2 / 2 com centroide em r = 10 + 2/3.
        const double ring = 2.0 * kPi * (10.0 + 2.0 / 3.0) * chamferLoss(2.0);
        report("bloco furado + fillet R3 + chanfro 2 no furo", v, base - ring, 1e-2);
        // Raio do cilindro 12: corte, fillet e chanfro recalculados (a circunferência é remapeada).
        Sketch c2 = *doc.findSketch(c.id());
        CAD_CHECK(c2.updateEntity("e-1", SketchCircle{50, -30, 12}).ok);
        CAD_CHECK(doc.commitSketch(c2, error));
        const double base12 = kBox - 144.0 * kPi * 20.0 - 100.0 * filletLoss(3.0);
        const double ring12 = 2.0 * kPi * (12.0 + 2.0 / 3.0) * chamferLoss(2.0);
        report("raio do furo 12", ShapeInspector::volume(doc.shape()), base12 - ring12, 1e-2);
        CAD_CHECK(ShapeInspector::isValid(doc.shape()) && doc.solidCount() == 1);

        std::printf("15-16. STEP e STL do corpo com acabamentos\n");
        const fs::path step = out / "acabamentos.step";
        CAD_CHECK(cad::io::StepExporter().write(doc.shape(), step).ok);
        const auto read = cad::io::StepImporter().read(step);
        const auto rs = ShapeInspector::inspect(read.shape);
        std::printf("  STEP relido: %d solido(s), %d faces, volume %.4f, valido %d\n", rs.solids, rs.faces, rs.volume,
                    rs.isValid);
        CAD_CHECK(read.ok && rs.solids == 1 && rs.isValid && std::fabs(rs.volume - ShapeInspector::volume(doc.shape())) < 1e-4);
        for (auto format : {cad::io::StlFormat::Binary, cad::io::StlFormat::Ascii})
        {
            cad::io::StlExportOptions o;
            o.format = format;
            o.quality = cad::io::StlQuality::High;
            const auto r = cad::io::StlExporter().write(doc.shape(), out / (format == cad::io::StlFormat::Binary ? "acab_bin.stl" : "acab_ascii.stl"), o);
            std::printf("  STL %s: %s, %d triangulos\n", format == cad::io::StlFormat::Binary ? "binario" : "ASCII",
                        r.ok ? "ok" : r.message.c_str(), r.triangles);
            CAD_CHECK(r.ok && r.triangles > 100);
        }
    }

    std::printf("13. Aresta nao remapeavel e referencia ambigua\n");
    {
        CadDocument doc;
        const std::string sk = sketchRect(doc, 0, 0, 100, 60);
        const std::string box = extrude(doc, sk, 20);
        std::string id;
        CAD_CHECK(doc.addFinish(OperationType::Fillet3D, finish(doc, box, {edgeAt(doc.shape(), 50, 0, 20)}, 5.0), &id).ok);
        const TopoDS_Shape before = doc.shape();
        const int revision = doc.revision();
        // Retângulo trocado por círculo: a aresta reta deixa de existir.
        Sketch s = *doc.findSketch(sk);
        CAD_CHECK(s.removeEntity("e-1"));
        CAD_CHECK(s.addEntity(SketchCircle{50, 30, 25}).ok);
        std::string error;
        CAD_CHECK(!doc.commitSketch(s, error));
        std::printf("  recusado: %s\n", error.c_str());
        CAD_CHECK(error.find("não remapeável") != std::string::npos && error.find("Fillet 3D 1") != std::string::npos);
        CAD_CHECK(doc.shape().IsSame(before) && doc.revision() == revision);
        CAD_CHECK(doc.findOperation(id)->status == OperationStatus::Valid);

        // Dois corpos equivalentes (compound): referência no meio dos dois é ambígua.
        CadDocument two;
        const std::string a = extrude(two, sketchRect(two, 0, 0, 100, 60), 20);
        const std::string b = extrude(two, sketchRect(two, 200, 0, 100, 60), 20);
        std::string u;
        CAD_CHECK(two.addBoolean({"", BooleanKind::Fuse, a, b}, &u).ok); // composto: 2 sólidos
        EdgeSignature sig = cad::modeling::describeEdge(two.shape(), edgeAt(two.shape(), 50, 0, 20));
        sig.mid = gp_Pnt(150, 0, 20); // equidistante das duas arestas equivalentes
        const auto amb = cad::modeling::resolveEdge(two.shape(), sig);
        std::printf("  ambigua: %s\n", amb.message.c_str());
        CAD_CHECK(!amb.ok && amb.message.find("ambígua") != std::string::npos);
        sig.mid = gp_Pnt(60, 0, 20); // claramente mais próxima da primeira
        const auto closest = cad::modeling::resolveEdge(two.shape(), sig);
        CAD_CHECK(closest.ok && !closest.exact);
        sig.mid = gp_Pnt(250, 0, 20);
        const auto exact = cad::modeling::resolveEdge(two.shape(), sig);
        CAD_CHECK(exact.ok && exact.exact);
        BRepAdaptor_Curve k(exact.edge);
        CAD_CHECK(k.Value(0.5 * (k.FirstParameter() + k.LastParameter())).Distance(gp_Pnt(250, 0, 20)) < 1e-9);
    }

    return cad::test::finish("test_acabamentos3d");
}
