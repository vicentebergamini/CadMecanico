// E5.6 sem Qt: esboços sobre faces planas (sistema local, faces paralelas aos planos globais e
// inclinadas, remapeamento e face não remapeável), Extrusão de Saliência, Extrusão de Corte
// (profundidade definida e atravessar tudo), Extrusão de Novo Corpo, chanfros avançados 2D
// (duas distâncias, distância–ângulo) e 3D (duas distâncias, distância–ângulo, face de
// referência), recálculo da cadeia e exportação STEP/STL.
#include "TestCheck.h"

#include "document/CadDocument.h"
#include "io/StepExporter.h"
#include "io/StepImporter.h"
#include "io/StlExporter.h"
#include "modeling/FaceNaming.h"
#include "modeling/ShapeInspector.h"
#include "sketch/SketchProfile.h"
#include "sketch/SketchResolver.h"

#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepClass3d_SolidClassifier.hxx>
#include <BRep_Tool.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>

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
constexpr double kBox = 120000.0;

std::string sketchWith(CadDocument& doc, PlaneType plane, std::initializer_list<SketchGeometry> geometries)
{
    Sketch s = doc.createSketch(plane);
    for (const SketchGeometry& g : geometries)
        CAD_CHECK(s.addEntity(g).ok);
    std::string error;
    CAD_CHECK(doc.commitSketch(s, error));
    return s.id();
}

std::string extrude(CadDocument& doc, const ExtrudeParameters& p)
{
    std::string id;
    const auto r = doc.addExtrusion(p, &id);
    if (!r.ok)
        std::printf("  extrusao recusada: %s\n", r.message.c_str());
    CAD_CHECK(r.ok);
    return id;
}

// Bloco 100 x 60 x 20 (Extrusão de Novo Corpo, esboço XY).
std::string block(CadDocument& doc, double depth = 20.0)
{
    return extrude(doc, {"", sketchWith(doc, PlaneType::XY, {SketchRectangle{0, 0, 100, 60}}), depth});
}

// Face plana do corpo com a normal dada e o centroide mais próximo do ponto.
TopoDS_Face faceWith(const TopoDS_Shape& body, const gp_Dir& normal, const gp_Pnt& near)
{
    TopoDS_Face best;
    double bestD = 1e300;
    for (TopExp_Explorer ex(body, TopAbs_FACE); ex.More(); ex.Next())
    {
        const TopoDS_Face f = TopoDS::Face(ex.Current());
        const auto pl = cad::modeling::planarFacePlane(f);
        if (!pl || pl->Axis().Direction().Angle(normal) > 1e-6)
            continue;
        const double d = cad::modeling::describeFace(f).centroid.Distance(near);
        if (d < bestD)
        {
            bestD = d;
            best = f;
        }
    }
    CAD_CHECK(!best.IsNull());
    return best;
}

// Esboço sobre face com as entidades dadas (coordenadas locais).
std::string faceSketch(CadDocument& doc, const std::string& body, const TopoDS_Face& face,
                       std::initializer_list<SketchGeometry> geometries)
{
    std::string error;
    auto s = doc.createFaceSketch(body, face, error);
    if (!s)
    {
        std::printf("  esboco sobre face recusado: %s\n", error.c_str());
        CAD_CHECK(false);
        return {};
    }
    for (const SketchGeometry& g : geometries)
        CAD_CHECK(s->addEntity(g).ok);
    CAD_CHECK(doc.commitSketch(*s, error));
    return s->id();
}

TopoDS_Edge edgeAt(const TopoDS_Shape& shape, double x, double y, double z)
{
    for (TopExp_Explorer ex(shape, TopAbs_EDGE); ex.More(); ex.Next())
    {
        BRepAdaptor_Curve c(TopoDS::Edge(ex.Current()));
        if (c.Value(0.5 * (c.FirstParameter() + c.LastParameter())).Distance(gp_Pnt(x, y, z)) < 1e-6)
            return TopoDS::Edge(ex.Current());
    }
    std::printf("  aresta nao encontrada (%.2f; %.2f; %.2f)\n", x, y, z);
    CAD_CHECK(false);
    return TopoDS_Edge();
}

bool inside(const TopoDS_Shape& s, double x, double y, double z)
{
    BRepClass3d_SolidClassifier c(s, gp_Pnt(x, y, z), 1e-7);
    return c.State() == TopAbs_IN;
}

bool dirIs(const gp_Dir& d, double x, double y, double z)
{
    return d.IsEqual(gp_Dir(x, y, z), 1e-9);
}

void report(const char* tag, double v, double expected, double tol = 1e-4)
{
    std::printf("  [%s] volume %.4f (esperado %.4f)\n", tag, v, expected);
    CAD_CHECK(std::fabs(v - expected) <= tol);
}

void checkRejected(CadDocument& doc, const ExtrudeParameters& p, const char* tag, const char* expected = nullptr)
{
    const size_t ops = doc.operations().size();
    const TopoDS_Shape shape = doc.shape();
    const int revision = doc.revision();
    const auto r = doc.addExtrusion(p);
    std::printf("  recusado (%s): %s\n", tag, r.message.c_str());
    CAD_CHECK(!r.ok && !r.message.empty());
    if (expected)
        CAD_CHECK(r.message.find(expected) != std::string::npos);
    CAD_CHECK(doc.operations().size() == ops && doc.shape().IsSame(shape) && doc.revision() == revision);
}

// Recuo do chanfro na face superior (y do vértice mais à frente em z = 20) e na face frontal
// (profundidade abaixo de z = 20 em y = 0) da aresta superior frontal do bloco.
void chamferSetbacks(const TopoDS_Shape& s, double& onTop, double& onFront)
{
    onTop = 1e9;
    double zFront = -1e9;
    for (TopExp_Explorer v(s, TopAbs_VERTEX); v.More(); v.Next())
    {
        const gp_Pnt q = BRep_Tool::Pnt(TopoDS::Vertex(v.Current()));
        if (std::fabs(q.Z() - 20.0) < 1e-6)
            onTop = std::min(onTop, q.Y());
        if (std::fabs(q.Y()) < 1e-6 && q.Z() < 20.0 - 1e-6)
            zFront = std::max(zFront, q.Z());
    }
    onFront = 20.0 - zFront;
}

} // namespace

int main()
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const fs::path out = fs::temp_directory_path() / "cadmecanico_e56";
    fs::create_directories(out);

    std::printf("Faces 1-3/5-8. Selecao de face plana, rejeicao de face curva e sistema local\n");
    {
        CadDocument doc;
        const std::string b = block(doc);
        const TopoDS_Shape s = doc.shape();
        struct Case
        {
            const char* tag;
            gp_Dir normal;
            gp_Pnt near;
            gp_Dir x, y;
            gp_Pnt origin;
        } cases[] = {
            {"superior (paralela a XY)", gp_Dir(0, 0, 1), gp_Pnt(50, 30, 20), gp_Dir(1, 0, 0), gp_Dir(0, 1, 0), gp_Pnt(0, 0, 20)},
            {"frontal (paralela a XZ)", gp_Dir(0, -1, 0), gp_Pnt(50, 0, 10), gp_Dir(1, 0, 0), gp_Dir(0, 0, 1), gp_Pnt(0, 0, 0)},
            {"traseira (paralela a XZ)", gp_Dir(0, 1, 0), gp_Pnt(50, 60, 10), gp_Dir(-1, 0, 0), gp_Dir(0, 0, 1), gp_Pnt(0, 60, 0)},
            {"direita (paralela a YZ)", gp_Dir(1, 0, 0), gp_Pnt(100, 30, 10), gp_Dir(0, 1, 0), gp_Dir(0, 0, 1), gp_Pnt(100, 0, 0)},
            {"inferior", gp_Dir(0, 0, -1), gp_Pnt(50, 30, 0), gp_Dir(1, 0, 0), gp_Dir(0, -1, 0), gp_Pnt(0, 0, 0)},
        };
        for (const Case& c : cases)
        {
            const TopoDS_Face f = faceWith(s, c.normal, c.near);
            std::string error;
            const auto sk = doc.createFaceSketch(b, f, error);
            CAD_CHECK(sk.has_value());
            const SketchPlane& p = sk->plane();
            std::printf("  [%s] origem (%.1f; %.1f; %.1f) H (%.0f; %.0f; %.0f) V (%.0f; %.0f; %.0f) N (%.0f; %.0f; %.0f)\n", c.tag,
                        p.origin().X(), p.origin().Y(), p.origin().Z(), p.horizontalAxis().X(), p.horizontalAxis().Y(),
                        p.horizontalAxis().Z(), p.verticalAxis().X(), p.verticalAxis().Y(), p.verticalAxis().Z(),
                        p.normal().X(), p.normal().Y(), p.normal().Z());
            CAD_CHECK(p.type() == PlaneType::Face);
            CAD_CHECK(p.normal().IsEqual(c.normal, 1e-9));
            CAD_CHECK(p.horizontalAxis().IsEqual(c.x, 1e-9) && p.verticalAxis().IsEqual(c.y, 1e-9));
            CAD_CHECK(p.origin().Distance(c.origin) < 1e-9);
            CAD_CHECK(sk->support() && sk->support()->bodyId == b);
            // Coordenadas locais → globais sobre a face.
            const gp_Pnt g = p.toGlobal(10.0, 5.0);
            CAD_CHECK(std::fabs(p.distance(g)) < 1e-9 && p.toLocal(g).Distance(gp_Pnt2d(10.0, 5.0)) < 1e-9);
        }
        // Face superior: mesmos eixos do plano XY deslocados para z = 20.
        std::string topError;
        const auto topSketch = doc.createFaceSketch(b, faceWith(s, gp_Dir(0, 0, 1), gp_Pnt(50, 30, 20)), topError);
        CAD_CHECK(topSketch && topSketch->plane().toGlobal(50, 30).Distance(gp_Pnt(50, 30, 20)) < 1e-9);
        // 2. Face curva: recusada.
        CadDocument cyl;
        const std::string c = extrude(cyl, {"", sketchWith(cyl, PlaneType::XY, {SketchCircle{0, 0, 10}}), 20});
        TopoDS_Face curved;
        for (TopExp_Explorer ex(cyl.shape(), TopAbs_FACE); ex.More(); ex.Next())
            if (BRepAdaptor_Surface(TopoDS::Face(ex.Current())).GetType() == GeomAbs_Cylinder)
                curved = TopoDS::Face(ex.Current());
        std::string error;
        CAD_CHECK(!cyl.createFaceSketch(c, curved, error));
        std::printf("  face curva recusada: %s\n", error.c_str());
        CAD_CHECK(error.find("não é plana") != std::string::npos);
        // Face de outro corpo: recusada.
        CAD_CHECK(!doc.createFaceSketch(b, curved, error));
        CAD_CHECK(doc.bodyContainingFace(faceWith(s, gp_Dir(0, 0, 1), gp_Pnt(50, 30, 20))) == b);
    }

    std::printf("Faces 9. Face inclinada (fora dos planos globais)\n");
    {
        CadDocument doc;
        // Prisma triangular: triângulo no plano XZ (catetos 40 em X e 30 em Z), extrudado 50 mm em −Y.
        const std::string w = extrude(doc, {"", sketchWith(doc, PlaneType::XZ, {SketchLine{0, 0, 40, 0}, SketchLine{40, 0, 0, 30},
                                                                                SketchLine{0, 30, 0, 0}}), 50});
        const gp_Dir n(3.0 / 5.0, 0.0, 4.0 / 5.0); // normal externa da face inclinada
        const TopoDS_Face f = faceWith(doc.shape(), n, gp_Pnt(20, -25, 15));
        const std::string sk = faceSketch(doc, w, f, {SketchCircle{0.0, 0.0, 5.0}});
        const SketchPlane& p = doc.findSketch(sk)->plane();
        std::printf("  N (%.3f; %.3f; %.3f) H (%.3f; %.3f; %.3f) V (%.3f; %.3f; %.3f)\n", p.normal().X(), p.normal().Y(),
                    p.normal().Z(), p.horizontalAxis().X(), p.horizontalAxis().Y(), p.horizontalAxis().Z(),
                    p.verticalAxis().X(), p.verticalAxis().Y(), p.verticalAxis().Z());
        CAD_CHECK(p.normal().IsEqual(n, 1e-9));
        CAD_CHECK(std::fabs(p.horizontalAxis().Z()) < 1e-12 && p.verticalAxis().Z() > 0.0); // horizontal, "para cima"
        CAD_CHECK(std::fabs(p.horizontalAxis().Dot(p.verticalAxis())) < 1e-12);
        // Ponto do plano: o centro da face inclinada fica sobre o plano do esboço.
        CAD_CHECK(std::fabs(p.distance(gp_Pnt(20, -25, 15))) < 1e-9);
        // Saliência cilíndrica perpendicular à face inclinada: 25π·10 adicionados (se o círculo
        // estiver dentro da face). Centro do círculo no centro da face: origem local deslocada.
        Sketch s = *doc.findSketch(sk);
        const gp_Pnt2d c = p.toLocal(gp_Pnt(20, -25, 15));
        CAD_CHECK(s.updateEntity("e-1", SketchCircle{c.X(), c.Y(), 5.0}).ok);
        std::string error;
        CAD_CHECK(doc.commitSketch(s, error));
        const double before = ShapeInspector::volume(doc.shape());
        const std::string boss = extrude(doc, {"", sk, 10.0, false, ExtrudeMode::Boss, w});
        report("saliencia na face inclinada", ShapeInspector::volume(doc.shape()), before + 250.0 * kPi, 1e-3);
        CAD_CHECK(doc.solidCount() == 1 && ShapeInspector::isValid(doc.shape()));
        CAD_CHECK(inside(doc.shape(), 20 + 0.6 * 5, -25, 15 + 0.8 * 5)); // material 5 mm acima da face, na normal

        std::printf("Faces 12. Face nao remapeavel\n");
        // O triângulo vira retângulo: a face inclinada deixa de existir.
        Sketch base = *doc.findSketch("sk-1");
        CAD_CHECK(base.removeEntity("e-1") && base.removeEntity("e-2") && base.removeEntity("e-3"));
        CAD_CHECK(base.addEntity(SketchRectangle{0, 0, 40, 30}).ok);
        const TopoDS_Shape keep = doc.shape();
        const auto planeBefore = doc.findSketch(sk)->plane().origin();
        CAD_CHECK(!doc.commitSketch(base, error));
        std::printf("  recusado: %s\n", error.c_str());
        CAD_CHECK(error.find("não remapeável") != std::string::npos);
        CAD_CHECK(doc.shape().IsSame(keep) && doc.findSketch(sk)->plane().origin().Distance(planeBefore) < 1e-12);
        CAD_CHECK(doc.findOperation(boss)->status == OperationStatus::Valid);
    }

    std::printf("Saliencia 1-9\n");
    {
        CadDocument doc;
        const std::string b = block(doc);
        const TopoDS_Face top = faceWith(doc.shape(), gp_Dir(0, 0, 1), gp_Pnt(50, 30, 20));
        const std::string sk = faceSketch(doc, b, top, {SketchRectangle{10, 10, 20, 15}});
        const std::string boss = extrude(doc, {"", sk, 10.0, false, ExtrudeMode::Boss, b});
        report("1. saliencia retangular 20 x 15 x 10", ShapeInspector::volume(doc.shape()), kBox + 3000.0);
        CAD_CHECK(doc.findOperation(boss)->name == "Extrusão de Saliência 1");
        CAD_CHECK(doc.solidCount() == 1 && doc.activeBodies() == std::vector<std::string>{boss}); // 3. união
        CAD_CHECK(doc.consumerOf(b) == boss);
        double x0, y0, z0, x1, y1, z1;
        ShapeInspector::boundingBox(doc.shape()).Get(x0, y0, z0, x1, y1, z1);
        std::printf("  5. caixa z [%.3f; %.3f] (saliencia ate z = 30, na normal da face)\n", z0, z1);
        CAD_CHECK(std::fabs(z1 - 30.0) < 1e-6 && inside(doc.shape(), 20, 17.5, 25));
        CAD_CHECK(ShapeInspector::isValid(doc.shape()) && ShapeInspector::inspect(doc.shape()).shells == 1); // 8.
        // 4. Profundidade editável.
        CAD_CHECK(doc.updateExtrusion(boss, {"", sk, 15.0, false, ExtrudeMode::Boss, b}).ok);
        report("4. profundidade 15", ShapeInspector::volume(doc.shape()), kBox + 4500.0);
        // 9. Recálculo: bloco com 30 mm; a face superior sobe e a saliência acompanha.
        CAD_CHECK(doc.updateExtrusion(b, {"", "sk-1", 30.0}).ok);
        ShapeInspector::boundingBox(doc.shape()).Get(x0, y0, z0, x1, y1, z1);
        report("9. bloco com 30 mm", ShapeInspector::volume(doc.shape()), 180000.0 + 4500.0);
        CAD_CHECK(std::fabs(z1 - 45.0) < 1e-6);
        CAD_CHECK(std::fabs(doc.findSketch(sk)->plane().origin().Z() - 30.0) < 1e-9);
        // 2. Saliência circular.
        CadDocument d2;
        const std::string b2 = block(d2);
        const std::string sk2 = faceSketch(d2, b2, faceWith(d2.shape(), gp_Dir(0, 0, 1), gp_Pnt(50, 30, 20)),
                                           {SketchCircle{50, 30, 5}});
        extrude(d2, {"", sk2, 10.0, false, ExtrudeMode::Boss, b2});
        report("2. saliencia circular R5 x 10", ShapeInspector::volume(d2.shape()), kBox + 250.0 * kPi);
        // 6-7. Perfil aberto e profundidade inválida.
        const std::string open = faceSketch(d2, d2.activeBodies()[0], faceWith(d2.shape(), gp_Dir(0, -1, 0), gp_Pnt(50, 0, 10)),
                                            {SketchLine{10, 5, 30, 5}});
        checkRejected(d2, {"", open, 5.0, false, ExtrudeMode::Boss, d2.activeBodies()[0]}, "6. perfil aberto", "aberto");
        checkRejected(d2, {"", sk2, 0.0, false, ExtrudeMode::Boss, d2.activeBodies()[0]}, "7. profundidade zero", "maior que zero");
        checkRejected(d2, {"", sk2, 5.0, false, ExtrudeMode::Boss, ""}, "sem corpo de destino", "destino");
        // Saliência que não toca o corpo e saliência para dentro do corpo.
        CadDocument d3;
        const std::string b3 = block(d3);
        const std::string far = faceSketch(d3, b3, faceWith(d3.shape(), gp_Dir(0, 0, 1), gp_Pnt(50, 30, 20)),
                                           {SketchRectangle{200, 0, 10, 10}});
        checkRejected(d3, {"", far, 5.0, false, ExtrudeMode::Boss, b3}, "nao toca o corpo", "Novo Corpo");
        const std::string in = faceSketch(d3, b3, faceWith(d3.shape(), gp_Dir(0, 0, 1), gp_Pnt(50, 30, 20)),
                                          {SketchRectangle{10, 10, 10, 10}});
        checkRejected(d3, {"", in, 5.0, true, ExtrudeMode::Boss, b3}, "para dentro do corpo", "não adiciona");
    }

    std::printf("Corte 1-10\n");
    {
        // 1. Furo circular com profundidade definida (10 mm a partir da face superior).
        CadDocument doc;
        const std::string b = block(doc);
        const std::string sk = faceSketch(doc, b, faceWith(doc.shape(), gp_Dir(0, 0, 1), gp_Pnt(50, 30, 20)),
                                          {SketchCircle{50, 30, 10}});
        const std::string blind = extrude(doc, {"", sk, 10.0, false, ExtrudeMode::Cut, b});
        report("1. furo cego R10 x 10", ShapeInspector::volume(doc.shape()), kBox - 1000.0 * kPi);
        CAD_CHECK(!inside(doc.shape(), 50, 30, 15) && inside(doc.shape(), 50, 30, 5)); // fundo preservado
        CAD_CHECK(doc.findOperation(blind)->name == "Extrusão de Corte 1");
        // 2. Atravessar tudo.
        CAD_CHECK(doc.updateExtrusion(blind, {"", sk, 10.0, false, ExtrudeMode::Cut, b, EndCondition::ThroughAll}).ok);
        report("2. furo atravessando tudo", ShapeInspector::volume(doc.shape()), kBox - 2000.0 * kPi);
        bool free = true;
        for (double z = 0.25; z < 20.0; z += 0.5)
            free = free && !inside(doc.shape(), 50, 30, z);
        CAD_CHECK(free); // 8. sem resíduo de material ao longo do furo
        const auto s = ShapeInspector::inspect(doc.shape());
        CAD_CHECK(s.faces == 7 && s.isValid && s.solids == 1);
        // 9-10. Espessura do bloco alterada: atravessar tudo recalculado.
        CAD_CHECK(doc.updateExtrusion(b, {"", "sk-1", 35.0}).ok);
        report("9/10. bloco com 35 mm, furo recalculado", ShapeInspector::volume(doc.shape()), 210000.0 - 3500.0 * kPi);
        free = true;
        for (double z = 0.25; z < 35.0; z += 0.5)
            free = free && !inside(doc.shape(), 50, 30, z);
        CAD_CHECK(free && ShapeInspector::inspect(doc.shape()).faces == 7);
        // 12. STEP/STL do bloco furado.
        const fs::path step = out / "corte.step";
        CAD_CHECK(cad::io::StepExporter().write(doc.shape(), step).ok);
        const auto read = cad::io::StepImporter().read(step);
        CAD_CHECK(read.ok && std::fabs(ShapeInspector::volume(read.shape) - ShapeInspector::volume(doc.shape())) < 1e-6);
        for (auto f : {cad::io::StlFormat::Binary, cad::io::StlFormat::Ascii})
        {
            cad::io::StlExportOptions o;
            o.format = f;
            CAD_CHECK(cad::io::StlExporter().write(doc.shape(), out / (f == cad::io::StlFormat::Binary ? "corte_b.stl" : "corte_a.stl"), o).ok);
        }

        // 3. Corte retangular atravessando tudo.
        CadDocument d2;
        const std::string b2 = block(d2);
        const std::string r = faceSketch(d2, b2, faceWith(d2.shape(), gp_Dir(0, 0, 1), gp_Pnt(50, 30, 20)),
                                         {SketchRectangle{60, 10, 30, 20}});
        extrude(d2, {"", r, 1.0, false, ExtrudeMode::Cut, b2, EndCondition::ThroughAll});
        report("3. corte retangular passante", ShapeInspector::volume(d2.shape()), kBox - 600.0 * 20.0);
        // 4. Corte em face lateral (frontal, normal −Y): furo R5 atravessando tudo em +Y.
        const std::string side = faceSketch(d2, d2.activeBodies()[0], faceWith(d2.shape(), gp_Dir(0, -1, 0), gp_Pnt(50, 0, 10)),
                                            {SketchCircle{25, 10, 5}});
        extrude(d2, {"", side, 1.0, false, ExtrudeMode::Cut, d2.activeBodies()[0], EndCondition::ThroughAll});
        report("4. furo lateral passante", ShapeInspector::volume(d2.shape()), kBox - 12000.0 - 25.0 * kPi * 60.0);
        CAD_CHECK(!inside(d2.shape(), 25, 59.5, 10) && !inside(d2.shape(), 25, 0.5, 10));
        // 5. Inversão de direção: para fora do corpo — recusado, documento mantido.
        checkRejected(d2, {"", side, 1.0, true, ExtrudeMode::Cut, d2.activeBodies()[0], EndCondition::ThroughAll},
                      "5. atravessar tudo invertido", "sentido do corte");
        checkRejected(d2, {"", side, 5.0, true, ExtrudeMode::Cut, d2.activeBodies()[0]}, "5. corte cego invertido",
                      "não intersecta");
        checkRejected(d2, {"", side, 5.0, false, ExtrudeMode::NewBody, "", EndCondition::ThroughAll},
                      "atravessar tudo em novo corpo", "Atravessar tudo");
        // 6. Corpo de destino correto: só o corpo alvo é cortado.
        CadDocument d3;
        const std::string a = block(d3);
        const std::string other = extrude(d3, {"", sketchWith(d3, PlaneType::XY, {SketchRectangle{200, 0, 50, 50}}), 20});
        const std::string hole = faceSketch(d3, a, faceWith(d3.shape(), gp_Dir(0, 0, 1), gp_Pnt(50, 30, 20)), {SketchCircle{50, 30, 10}});
        const std::string cut = extrude(d3, {"", hole, 1.0, false, ExtrudeMode::Cut, a, EndCondition::ThroughAll});
        CAD_CHECK(std::fabs(ShapeInspector::volume(d3.findOperation(cut)->result) - (kBox - 2000.0 * kPi)) < 1e-4);
        CAD_CHECK(std::fabs(ShapeInspector::volume(d3.findOperation(other)->result) - 50000.0) < 1e-6);
        CAD_CHECK(d3.solidCount() == 2 && d3.consumerOf(other).empty() && d3.consumerOf(a) == cut);
    }

    std::printf("Novo corpo 1-8\n");
    {
        CadDocument doc;
        const std::string b = block(doc);
        const std::string sk = faceSketch(doc, b, faceWith(doc.shape(), gp_Dir(0, 0, 1), gp_Pnt(50, 30, 20)),
                                          {SketchRectangle{40, 20, 20, 20}});
        const std::string nb = extrude(doc, {"", sk, 10.0, false, ExtrudeMode::NewBody});
        CAD_CHECK(doc.solidCount() == 2); // 1-2. independente, sem união automática
        CAD_CHECK(doc.activeBodies() == (std::vector<std::string>{b, nb})); // 3.
        CAD_CHECK(doc.consumerOf(b).empty());
        CAD_CHECK(std::fabs(ShapeInspector::volume(doc.findOperation(nb)->result) - 4000.0) < 1e-6);
        // 5-7. Participa de booleanas.
        for (BooleanKind kind : {BooleanKind::Fuse, BooleanKind::Cut})
        {
            CadDocument d = doc;
            std::string id;
            CAD_CHECK(d.addBoolean({"", kind, b, nb}, &id).ok);
            const double expected = kind == BooleanKind::Fuse ? kBox + 4000.0 : kBox;
            const double v = ShapeInspector::volume(d.shape());
            std::printf("  %s com o novo corpo (encostado na face): volume %.4f\n", booleanKindLabel(kind), v);
            CAD_CHECK(std::fabs(v - expected) < 1e-6 && d.solidCount() == 1);
        }
        {
            CadDocument d = doc; // encostados: interseção sem volume, recusada
            CAD_CHECK(!d.addBoolean({"", BooleanKind::Common, b, nb}).ok);
        }
        // Interseção com sobreposição real: novo corpo que penetra o bloco (invertido, 10 mm).
        CadDocument d = doc;
        CAD_CHECK(d.updateExtrusion(nb, {"", sk, 10.0, true, ExtrudeMode::NewBody}).ok);
        std::string common;
        CAD_CHECK(d.addBoolean({"", BooleanKind::Common, b, nb}, &common).ok);
        report("7. intersecao", ShapeInspector::volume(d.shape()), 4000.0);
        // 8. Exportação: dois corpos ativos no STEP.
        const fs::path step = out / "novo_corpo.step";
        CAD_CHECK(cad::io::StepExporter().write(doc.shape(), step).ok);
        const auto read = cad::io::StepImporter().read(step);
        const auto rs = ShapeInspector::inspect(read.shape);
        std::printf("  8. STEP: %d solidos, volume %.4f\n", rs.solids, rs.volume);
        CAD_CHECK(read.ok && rs.solids == 2 && std::fabs(rs.volume - (kBox + 4000.0)) < 1e-6);
    }

    std::printf("Chamfer 2D avancado\n");
    {
        auto areaOf = [](std::initializer_list<SketchGeometry> g) {
            Sketch s("sk-1", "E", PlaneType::XY);
            for (const SketchGeometry& x : g)
            {
                const auto v = s.addEntity(x);
                if (!v.ok)
                {
                    std::printf("  recusado: %s\n", v.message.c_str());
                    return -1.0;
                }
            }
            const ProfileResult p = buildProfile(s);
            return p.ok ? p.area : -2.0;
        };
        const double eq = areaOf({SketchRectangle{0, 0, 100, 60}, SketchChamfer{"e-1", "e-1", 0, 5.0}});
        CAD_CHECK(std::fabs(eq - (6000.0 - 12.5)) < 1e-9); // 1. distâncias iguais
        SketchChamfer two{"e-1", "e-1", 0, 5.0, ChamferMode::TwoDistances, 10.0};
        const double a2 = areaOf({SketchRectangle{0, 0, 100, 60}, two});
        std::printf("  2. D1 = 5, D2 = 10: area %.6f (esperado %.6f)\n", a2, 6000.0 - 25.0);
        CAD_CHECK(std::fabs(a2 - (6000.0 - 25.0)) < 1e-9);
        // Pontos de recorte: D1 no lado de referência (lado 3, que termina no canto 0: x = 0),
        // D2 no outro lado (lado 0: y = 0).
        Sketch s("sk-1", "E", PlaneType::XY);
        CAD_CHECK(s.addEntity(SketchRectangle{0, 0, 100, 60}).ok && s.addEntity(two).ok);
        const ResolvedSketch r = resolveSketch(s);
        const Curve2d* seg = nullptr;
        for (const Curve2d& c : r.curves)
            if (c.entityId == "e-2")
                seg = &c;
        std::printf("  segmento do chanfro: (%.3f; %.3f) -> (%.3f; %.3f)\n", seg->start.X(), seg->start.Y(), seg->end.X(), seg->end.Y());
        CAD_CHECK((seg->start.Distance(gp_Pnt2d(0, 5)) < 1e-9 && seg->end.Distance(gp_Pnt2d(10, 0)) < 1e-9));
        // Referência no segundo lado: distâncias trocadas.
        two.flip = true;
        Sketch s2("sk-1", "E", PlaneType::XY);
        CAD_CHECK(s2.addEntity(SketchRectangle{0, 0, 100, 60}).ok && s2.addEntity(two).ok);
        for (const Curve2d& c : resolveSketch(s2).curves)
            if (c.entityId == "e-2")
                CAD_CHECK(c.start.Distance(gp_Pnt2d(0, 10)) < 1e-9 && c.end.Distance(gp_Pnt2d(5, 0)) < 1e-9);
        // 3-4. Distância–ângulo num canto de 90°: D1 = 5, α = 30° → D2 = 5·sen30/sen120 = 5·tan30.
        SketchChamfer da{"e-1", "e-1", 0, 5.0, ChamferMode::DistanceAngle, 0.0, 30.0};
        const double d2 = 5.0 * std::tan(30.0 * kPi / 180.0);
        const double a3 = areaOf({SketchRectangle{0, 0, 100, 60}, da});
        std::printf("  3/4. D = 5, angulo 30 graus (90 graus): area %.6f (esperado %.6f; D2 = %.6f)\n", a3, 6000.0 - 2.5 * d2, d2);
        CAD_CHECK(std::fabs(a3 - (6000.0 - 2.5 * d2)) < 1e-9);
        // 5. Canto de 60° (triângulo equilátero de lado 20): D1 = 4, α = 60° → D2 = 4 (corte equilátero).
        const double h = 20.0 * std::sqrt(3.0) / 2.0;
        const double tri = 0.5 * 20.0 * h;
        const double a5 = areaOf({SketchLine{0, 0, 20, 0}, SketchLine{20, 0, 10, h}, SketchLine{10, h, 0, 0},
                                  SketchChamfer{"e-1", "e-2", -1, 4.0, ChamferMode::DistanceAngle, 0.0, 60.0}});
        const double cutArea = 0.5 * 4.0 * 4.0 * std::sin(kPi / 3.0);
        std::printf("  5. canto de 60 graus: area %.6f (esperado %.6f)\n", a5, tri - cutArea);
        CAD_CHECK(std::fabs(a5 - (tri - cutArea)) < 1e-9);
        // Duas distâncias no canto de 60°: área removida = D1·D2·sen(60°)/2.
        const double a5b = areaOf({SketchLine{0, 0, 20, 0}, SketchLine{20, 0, 10, h}, SketchLine{10, h, 0, 0},
                                   SketchChamfer{"e-1", "e-2", -1, 3.0, ChamferMode::TwoDistances, 6.0}});
        CAD_CHECK(std::fabs(a5b - (tri - 0.5 * 3.0 * 6.0 * std::sin(kPi / 3.0))) < 1e-9);
        // 6. Validação: ângulo 0, ângulo inviável (90° + 100°), D2 grande demais, D2 zero.
        CAD_CHECK(areaOf({SketchRectangle{0, 0, 100, 60}, SketchChamfer{"e-1", "e-1", 0, 5, ChamferMode::DistanceAngle, 0, 0.0}}) == -1.0);
        CAD_CHECK(areaOf({SketchRectangle{0, 0, 100, 60}, SketchChamfer{"e-1", "e-1", 0, 5, ChamferMode::DistanceAngle, 0, 100.0}}) == -1.0);
        CAD_CHECK(areaOf({SketchRectangle{0, 0, 100, 60}, SketchChamfer{"e-1", "e-1", 0, 5, ChamferMode::TwoDistances, 100.0}}) == -1.0);
        CAD_CHECK(areaOf({SketchRectangle{0, 0, 100, 60}, SketchChamfer{"e-1", "e-1", 0, 5, ChamferMode::TwoDistances, 0.0}}) == -1.0);
        // 7-8. Fechamento e extrusão do perfil chanfrado.
        CadDocument doc;
        const std::string sk = sketchWith(doc, PlaneType::XY, {SketchRectangle{0, 0, 100, 60}, SketchChamfer{"e-1", "e-1", 0, 5.0, ChamferMode::TwoDistances, 10.0}});
        extrude(doc, {"", sk, 20.0});
        report("8. extrusao do perfil com D1 = 5 / D2 = 10", ShapeInspector::volume(doc.shape()), (6000.0 - 25.0) * 20.0);
        CAD_CHECK(ShapeInspector::inspect(doc.shape()).faces == 7);
    }

    std::printf("Chamfer 3D avancado\n");
    {
        auto boxWithChamfer = [](Chamfer3DMode mode, double d1, double second, bool flip, double* setbackTop,
                                 double* setbackFront, std::string* message = nullptr) {
            CadDocument doc;
            const std::string b = block(doc);
            const TopoDS_Edge e = edgeAt(doc.shape(), 50, 0, 20);
            FinishParameters p{"", b, d1, doc.edgeSignatures(b, {e})};
            p.chamferMode = mode;
            p.value2 = second;
            p.angle = second;
            p.referenceNormals = doc.referenceNormals(b, {e}, flip);
            std::string id;
            const auto r = doc.addFinish(OperationType::Chamfer3D, p, &id);
            if (message)
                *message = r.message;
            if (!r.ok)
                return -1.0;
            chamferSetbacks(doc.shape(), *setbackTop, *setbackFront);
            CAD_CHECK(ShapeInspector::isValid(doc.shape()));
            return kBox - ShapeInspector::volume(doc.shape());
        };
        double top = 0, front = 0;
        // 1. Distâncias iguais.
        double loss = boxWithChamfer(Chamfer3DMode::Equal, 3.0, 0.0, false, &top, &front);
        std::printf("  1. iguais 3: perda %.4f, recuo superior %.4f, frontal %.4f\n", loss, top, front);
        CAD_CHECK(std::fabs(loss - 450.0) < 1e-6);
        // 2/4. D1 = 3 na face de referência (superior, padrão), D2 = 6 na frontal.
        loss = boxWithChamfer(Chamfer3DMode::TwoDistances, 3.0, 6.0, false, &top, &front);
        std::printf("  2. D1 = 3 / D2 = 6 (ref. superior): perda %.4f, recuo superior %.4f, frontal %.4f\n", loss, top, front);
        CAD_CHECK(std::fabs(loss - 900.0) < 1e-6 && std::fabs(top - 3.0) < 1e-6 && std::fabs(front - 6.0) < 1e-6);
        loss = boxWithChamfer(Chamfer3DMode::TwoDistances, 3.0, 6.0, true, &top, &front);
        std::printf("  4. referencia na face frontal: perda %.4f, recuo superior %.4f, frontal %.4f\n", loss, top, front);
        CAD_CHECK(std::fabs(loss - 900.0) < 1e-6 && std::fabs(top - 6.0) < 1e-6 && std::fabs(front - 3.0) < 1e-6);
        // 3. Distância 5 e ângulo 30° (convenção do OCCT: distância na face de referência, ângulo
        // entre a face de referência e a face do chanfro).
        loss = boxWithChamfer(Chamfer3DMode::DistanceAngle, 5.0, 30.0, false, &top, &front);
        std::printf("  3. D = 5, angulo 30 (ref. superior): perda %.4f, recuo superior %.4f, frontal %.4f\n", loss, top, front);
        CAD_CHECK(std::fabs(top - 5.0) < 1e-6);
        CAD_CHECK(std::fabs(front - 5.0 * std::tan(30.0 * kPi / 180.0)) < 1e-6);
        CAD_CHECK(std::fabs(loss - 0.5 * 5.0 * front * 100.0) < 1e-4);
        // Validação: ângulo 0 e 90, D2 zero, distância excessiva.
        std::string message;
        CAD_CHECK(boxWithChamfer(Chamfer3DMode::DistanceAngle, 5.0, 0.0, false, &top, &front, &message) < 0);
        std::printf("  recusado (angulo 0): %s\n", message.c_str());
        CAD_CHECK(boxWithChamfer(Chamfer3DMode::DistanceAngle, 5.0, 90.0, false, &top, &front, &message) < 0);
        CAD_CHECK(boxWithChamfer(Chamfer3DMode::TwoDistances, 5.0, 0.0, false, &top, &front, &message) < 0);
        CAD_CHECK(boxWithChamfer(Chamfer3DMode::TwoDistances, 3.0, 30.0, false, &top, &front, &message) < 0);
        std::printf("  recusado (D2 30 em face de 20): %s\n", message.c_str());
        // 6/8. Edição (iguais → duas distâncias → distância–ângulo) e recálculo.
        CadDocument doc;
        const std::string b = block(doc);
        const TopoDS_Edge e = edgeAt(doc.shape(), 50, 0, 20);
        std::string id;
        CAD_CHECK(doc.addFinish(OperationType::Chamfer3D, {"", b, 3.0, doc.edgeSignatures(b, {e})}, &id).ok);
        FinishParameters p = doc.finishParameters(id);
        p.chamferMode = Chamfer3DMode::TwoDistances;
        p.value2 = 6.0;
        p.referenceNormals = doc.referenceNormals(b, {doc.resolvedFinishEdges(id)[0]}, false);
        CAD_CHECK(doc.updateFinish(id, p).ok);
        report("6. editado para duas distancias", ShapeInspector::volume(doc.shape()), kBox - 900.0);
        p = doc.finishParameters(id);
        CAD_CHECK(p.chamferMode == Chamfer3DMode::TwoDistances && p.value2 == 6.0 && p.referenceNormals.size() == 1);
        p.chamferMode = Chamfer3DMode::DistanceAngle;
        p.value = 5.0;
        p.angle = 30.0;
        CAD_CHECK(doc.updateFinish(id, p).ok);
        report("6. editado para distancia-angulo", ShapeInspector::volume(doc.shape()),
               kBox - 0.5 * 5.0 * 5.0 * std::tan(kPi / 6.0) * 100.0);
        CAD_CHECK(doc.updateExtrusion(b, {"", "sk-1", 30.0}).ok); // 8. recálculo com a aresta remapeada
        report("8. bloco com 30 mm", ShapeInspector::volume(doc.shape()), 180000.0 - 0.5 * 25.0 * std::tan(kPi / 6.0) * 100.0);
        CAD_CHECK(doc.finishParameters(id).referenceNormals[0].IsEqual(gp_Dir(0, 0, 1), 1e-9));
        // 10. STEP do chanfro inclinado.
        const fs::path step = out / "chanfro_da.step";
        CAD_CHECK(cad::io::StepExporter().write(doc.shape(), step).ok);
        const auto read = cad::io::StepImporter().read(step);
        CAD_CHECK(read.ok && std::fabs(ShapeInspector::volume(read.shape) - ShapeInspector::volume(doc.shape())) < 1e-6);
        cad::io::StlExportOptions o;
        o.format = cad::io::StlFormat::Ascii;
        CAD_CHECK(cad::io::StlExporter().write(doc.shape(), out / "chanfro_da.stl", o).ok);
    }

    std::printf("Cadeia 17 / fluxo 25 (sem interface)\n");
    {
        CadDocument doc;
        const std::string b = block(doc); // Extrusão de Novo Corpo 20 mm
        const std::string hole = faceSketch(doc, b, faceWith(doc.shape(), gp_Dir(0, 0, 1), gp_Pnt(50, 30, 20)), {SketchCircle{50, 30, 10}});
        const std::string cut = extrude(doc, {"", hole, 1.0, false, ExtrudeMode::Cut, b, EndCondition::ThroughAll});
        // Saliência 20 x 15 x 10 na face frontal (normal −Y).
        const std::string sideSk = faceSketch(doc, cut, faceWith(doc.shape(), gp_Dir(0, -1, 0), gp_Pnt(50, 0, 10)),
                                              {SketchRectangle{70, 2, 20, 15}});
        const std::string boss = extrude(doc, {"", sideSk, 10.0, false, ExtrudeMode::Boss, cut});
        const double afterBoss = kBox - 2000.0 * kPi + 3000.0;
        report("corte passante + saliencia frontal", ShapeInspector::volume(doc.shape()), afterBoss);
        double x0, y0, z0, x1, y1, z1;
        ShapeInspector::boundingBox(doc.shape()).Get(x0, y0, z0, x1, y1, z1);
        CAD_CHECK(std::fabs(y0 + 10.0) < 1e-6); // saliência para fora da face frontal (−Y)
        // Chamfer 3D (duas distâncias) na aresta superior traseira e Fillet 3D na aresta lateral esquerda.
        const TopoDS_Edge backTop = edgeAt(doc.shape(), 50, 60, 20);
        FinishParameters cp{"", boss, 3.0, doc.edgeSignatures(boss, {backTop})};
        cp.chamferMode = Chamfer3DMode::TwoDistances;
        cp.value2 = 6.0;
        cp.referenceNormals = doc.referenceNormals(boss, {backTop}, false);
        std::string ch;
        CAD_CHECK(doc.addFinish(OperationType::Chamfer3D, cp, &ch).ok);
        std::string fi;
        CAD_CHECK(doc.addFinish(OperationType::Fillet3D, {"", ch, 2.0, doc.edgeSignatures(ch, {edgeAt(doc.shape(), 0, 30, 0)})}, &fi).ok);
        const double finalV = ShapeInspector::volume(doc.shape());
        std::printf("  volume final %.4f; ativos %zu; solidos %d\n", finalV, doc.activeBodies().size(), doc.solidCount());
        CAD_CHECK(doc.activeBodies() == std::vector<std::string>{fi} && doc.solidCount() == 1);
        // Alterações na cadeia: espessura do bloco, diâmetro do furo, profundidade da saliência,
        // dimensão do chanfro.
        CAD_CHECK(doc.updateExtrusion(b, {"", "sk-1", 25.0}).ok);
        CAD_CHECK(ShapeInspector::volume(doc.shape()) > finalV + 25000.0);
        Sketch hs = *doc.findSketch(hole);
        CAD_CHECK(hs.updateEntity("e-1", SketchCircle{50, 30, 12}).ok);
        std::string error;
        CAD_CHECK(doc.commitSketch(hs, error));
        CAD_CHECK(doc.updateExtrusion(boss, {"", sideSk, 15.0, false, ExtrudeMode::Boss, cut}).ok);
        FinishParameters cp2 = doc.finishParameters(ch);
        cp2.value = 4.0;
        CAD_CHECK(doc.updateFinish(ch, cp2).ok);
        const double edited = ShapeInspector::volume(doc.shape());
        // Volume esperado aproximado: bloco 25 mm − furo R12 + saliência 15 − chanfro 4/6 − fillet 2.
        const double approx = 150000.0 - 144.0 * kPi * 25.0 + 4500.0 - 0.5 * 4.0 * 6.0 * 100.0 - 60.0 * (4.0 - kPi);
        std::printf("  apos editar a cadeia: volume %.4f (aprox. %.4f)\n", edited, approx);
        CAD_CHECK(std::fabs(edited - approx) < 5.0);
        CAD_CHECK(ShapeInspector::isValid(doc.shape()) && doc.solidCount() == 1);
        // Falha no meio da cadeia (chanfro inviável): último estado válido preservado.
        cp2.value = 50.0;
        const TopoDS_Shape keep = doc.shape();
        const auto bad = doc.updateFinish(ch, cp2);
        std::printf("  recusado: %s\n", bad.message.c_str());
        CAD_CHECK(!bad.ok && doc.shape().IsSame(keep));
    }

    return cad::test::finish("test_e56");
}
