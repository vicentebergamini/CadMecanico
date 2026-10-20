// Núcleo dos esboços 2D (E5.2): planos, transformações, entidades, dimensões, identificadores e
// integração com o documento (sem Qt).
#include "ExpectedTestPart.h"
#include "TestCheck.h"

#include "document/CadDocument.h"
#include "modeling/ShapeInspector.h"
#include "sketch/Sketch.h"
#include "sketch/SketchShapes.h"

#include <BRepAdaptor_Curve.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepGProp.hxx>
#include <BRep_Tool.hxx>
#include <GProp_GProps.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Wire.hxx>

#include <cmath>
#include <cstdio>
#include <limits>

using namespace cad::sketch;
using cad::document::CadDocument;

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kTol = 1e-9;

bool samePoint(const gp_Pnt& a, const gp_Pnt& b, double tol = 1e-9)
{
    return a.Distance(b) <= tol;
}

int count(const TopoDS_Shape& s, TopAbs_ShapeEnum type)
{
    int n = 0;
    for (TopExp_Explorer e(s, type); e.More(); e.Next())
        ++n;
    return n;
}

// Todas as arestas da forma estão no plano.
bool liesOnPlane(const TopoDS_Shape& s, const SketchPlane& plane)
{
    for (TopExp_Explorer e(s, TopAbs_EDGE); e.More(); e.Next())
    {
        BRepAdaptor_Curve c(TopoDS::Edge(e.Current()));
        for (int i = 0; i <= 8; ++i)
        {
            const double t = c.FirstParameter() + (c.LastParameter() - c.FirstParameter()) * i / 8.0;
            if (std::fabs(plane.distance(c.Value(t))) > 1e-9)
                return false;
        }
    }
    return true;
}

void checkPlane(PlaneType type, const gp_Dir& h, const gp_Dir& v, const gp_Dir& n)
{
    const SketchPlane p(type);
    std::printf("  plano %s\n", SketchPlane::name(type));
    CAD_CHECK(p.type() == type);
    CAD_CHECK(samePoint(p.origin(), gp_Pnt(0, 0, 0)));
    CAD_CHECK(p.horizontalAxis().IsEqual(h, kTol));
    CAD_CHECK(p.verticalAxis().IsEqual(v, kTol));
    CAD_CHECK(p.normal().IsEqual(n, kTol));
    // Ortonormal e dextrógiro (normal = horizontal x vertical): sem espelhamento.
    CAD_CHECK(std::fabs(p.horizontalAxis().Dot(p.verticalAxis())) < kTol);
    CAD_CHECK(p.horizontalAxis().Crossed(p.verticalAxis()).IsEqual(p.normal(), kTol));
    // Sistema OCCT coerente.
    const gp_Ax3 ax = p.ax3();
    CAD_CHECK(ax.Direction().IsEqual(n, kTol) && ax.XDirection().IsEqual(h, kTol) && ax.YDirection().IsEqual(v, kTol));
    CAD_CHECK(ax.Direct());
    CAD_CHECK(SketchPlane::fromName(SketchPlane::name(type)) == type);
}

} // namespace

int main()
{
    // 1-3. Criação de esboço em cada plano (documento)
    std::printf("1-3. Esbocos nos planos XY, XZ e YZ\n");
    CadDocument doc = CadDocument::withTestPart();
    const TopoDS_Shape solidBefore = doc.shape();
    const double volumeBefore = cad::modeling::ShapeInspector::volume(solidBefore);
    const PlaneType planes[] = {PlaneType::XY, PlaneType::XZ, PlaneType::YZ};
    for (int i = 0; i < 3; ++i)
    {
        Sketch s = doc.createSketch(planes[i]);
        CAD_CHECK(s.id() == "sk-" + std::to_string(i + 1));
        CAD_CHECK(s.name() == "Esboço " + std::to_string(i + 1));
        CAD_CHECK(s.plane().type() == planes[i]);
        CAD_CHECK(s.entities().empty());
        std::string error;
        CAD_CHECK(doc.commitSketch(s, error));
        CAD_CHECK(doc.findSketch(s.id()) != nullptr);
    }
    CAD_CHECK(doc.sketches().size() == 3);
    CAD_CHECK(doc.isModified());
    CAD_CHECK(doc.nextSketchNumber() == 4);

    // 4. Orientação dos sistemas locais (coerente com as vistas Superior, Frente e Direita)
    std::printf("4. Orientacao dos sistemas locais\n");
    checkPlane(PlaneType::XY, gp_Dir(1, 0, 0), gp_Dir(0, 1, 0), gp_Dir(0, 0, 1));
    checkPlane(PlaneType::XZ, gp_Dir(1, 0, 0), gp_Dir(0, 0, 1), gp_Dir(0, -1, 0));
    checkPlane(PlaneType::YZ, gp_Dir(0, 1, 0), gp_Dir(0, 0, 1), gp_Dir(1, 0, 0));

    // 5. Transformação 2D -> 3D e volta
    std::printf("5. Transformacao de coordenadas\n");
    CAD_CHECK(samePoint(SketchPlane(PlaneType::XY).toGlobal(12.5, -3.0), gp_Pnt(12.5, -3.0, 0.0)));
    CAD_CHECK(samePoint(SketchPlane(PlaneType::XZ).toGlobal(12.5, -3.0), gp_Pnt(12.5, 0.0, -3.0)));
    CAD_CHECK(samePoint(SketchPlane(PlaneType::YZ).toGlobal(12.5, -3.0), gp_Pnt(0.0, 12.5, -3.0)));
    for (PlaneType t : planes)
    {
        const SketchPlane p(t);
        for (const gp_Pnt2d& q : {gp_Pnt2d(0, 0), gp_Pnt2d(10, 20), gp_Pnt2d(-37.25, 4.5), gp_Pnt2d(9999, -9999)})
        {
            const gp_Pnt g = p.toGlobal(q);
            CAD_CHECK(std::fabs(p.distance(g)) < kTol);
            CAD_CHECK(p.toLocal(g).Distance(q) < 1e-9);
        }
        // Ponto fora do plano: projeção ortogonal.
        const gp_Pnt off = p.toGlobal(5.0, 7.0).Translated(gp_Vec(p.normal()) * 3.0);
        CAD_CHECK(std::fabs(p.distance(off) - 3.0) < kTol);
        CAD_CHECK(p.toLocal(off).Distance(gp_Pnt2d(5.0, 7.0)) < kTol);
    }

    // 6-7. Linha: criação e edição
    std::printf("6-7. Linha\n");
    Sketch s("sk-9", "Teste", PlaneType::XZ);
    std::string lineId;
    CAD_CHECK(s.addEntity(SketchLine{0.0, 0.0, 30.0, 40.0}, &lineId).ok);
    CAD_CHECK(lineId == "e-1");
    const SketchEntity* line = s.findEntity(lineId);
    CAD_CHECK(line && line->type() == EntityType::Line && line->sketchId == "sk-9" && line->isValid());
    CAD_CHECK(std::fabs(std::get<SketchLine>(line->geometry).length() - 50.0) < kTol);
    {
        const TopoDS_Shape e = buildEntityShape(s.plane(), *line);
        CAD_CHECK(count(e, TopAbs_EDGE) == 1);
        CAD_CHECK(liesOnPlane(e, s.plane()));
        TopoDS_Vertex v1, v2;
        TopExp::Vertices(TopoDS::Edge(e), v1, v2);
        CAD_CHECK(samePoint(BRep_Tool::Pnt(v1), gp_Pnt(0, 0, 0)) && samePoint(BRep_Tool::Pnt(v2), gp_Pnt(30, 0, 40)));
    }
    CAD_CHECK(s.setDimension(lineId, "x2", 60.0).ok);
    CAD_CHECK(s.setDimension(lineId, "y2", 80.0).ok);
    CAD_CHECK(std::fabs(std::get<SketchLine>(s.findEntity(lineId)->geometry).length() - 100.0) < kTol);
    CAD_CHECK(s.updateEntity(lineId, SketchLine{-10.0, 5.0, 20.0, 5.0}).ok);
    CAD_CHECK(std::fabs(std::get<SketchLine>(s.findEntity(lineId)->geometry).length() - 30.0) < kTol);
    {
        const auto d = dimensionsOf(*s.findEntity(lineId));
        CAD_CHECK(d.size() == 5 && d[4].key == "length" && !d[4].editable && std::fabs(d[4].value - 30.0) < kTol);
        CAD_CHECK(!s.setDimension(lineId, "length", 10.0).ok); // calculado: não editável
    }

    // 8-10. Retângulo: criação, contorno fechado e edição
    std::printf("8-10. Retangulo\n");
    const SketchRectangle fromCorners = SketchRectangle::fromCorners(50.0, 30.0, 10.0, 5.0);
    CAD_CHECK(fromCorners.x == 10.0 && fromCorners.y == 5.0 && fromCorners.width == 40.0 && fromCorners.height == 25.0);
    std::string rectId;
    CAD_CHECK(s.addEntity(fromCorners, &rectId).ok);
    CAD_CHECK(rectId == "e-2");
    {
        const SketchEntity& r = *s.findEntity(rectId);
        const auto seg = std::get<SketchRectangle>(r.geometry).segments();
        for (int i = 0; i < 4; ++i)
        {
            // Fim de cada segmento = início do próximo; o último fecha no primeiro.
            CAD_CHECK(seg[i].x2 == seg[(i + 1) % 4].x1 && seg[i].y2 == seg[(i + 1) % 4].y1);
        }
        const TopoDS_Shape w = buildEntityShape(s.plane(), r);
        CAD_CHECK(w.ShapeType() == TopAbs_WIRE);
        CAD_CHECK(count(w, TopAbs_EDGE) == 4);
        CAD_CHECK(BRep_Tool::IsClosed(w));
        CAD_CHECK(liesOnPlane(w, s.plane()));
        // O contorno fechado delimita uma face plana de área largura x altura.
        BRepBuilderAPI_MakeFace face(TopoDS::Wire(w), true);
        CAD_CHECK(face.IsDone());
        GProp_GProps props;
        BRepGProp::SurfaceProperties(face.Shape(), props);
        CAD_CHECK(std::fabs(props.Mass() - 40.0 * 25.0) < 1e-6);
    }
    CAD_CHECK(s.setDimension(rectId, "width", 70.0).ok);
    CAD_CHECK(s.setDimension(rectId, "height", 12.5).ok);
    CAD_CHECK(s.setDimension(rectId, "x", -5.0).ok);
    {
        const auto& r = std::get<SketchRectangle>(s.findEntity(rectId)->geometry);
        CAD_CHECK(r.x == -5.0 && r.y == 5.0 && r.width == 70.0 && r.height == 12.5);
    }

    // 11-12. Círculo: criação e edição de centro e raio
    std::printf("11-12. Circulo\n");
    std::string circleId;
    CAD_CHECK(s.addEntity(SketchCircle{25.0, 15.0, 8.0}, &circleId).ok);
    CAD_CHECK(circleId == "e-3");
    {
        const SketchEntity& c = *s.findEntity(circleId);
        CAD_CHECK(std::get<SketchCircle>(c.geometry).diameter() == 16.0);
        const TopoDS_Shape e = buildEntityShape(s.plane(), c);
        CAD_CHECK(count(e, TopAbs_EDGE) == 1 && BRep_Tool::IsClosed(TopoDS::Edge(e)));
        CAD_CHECK(liesOnPlane(e, s.plane()));
        BRepAdaptor_Curve curve(TopoDS::Edge(e));
        CAD_CHECK(curve.GetType() == GeomAbs_Circle);
        CAD_CHECK(std::fabs(curve.Circle().Radius() - 8.0) < kTol);
        CAD_CHECK(samePoint(curve.Circle().Location(), s.plane().toGlobal(25.0, 15.0)));
        CAD_CHECK(curve.Circle().Axis().Direction().IsParallel(s.plane().normal(), kTol));
        GProp_GProps props;
        BRepGProp::LinearProperties(e, props);
        CAD_CHECK(std::fabs(props.Mass() - 2.0 * kPi * 8.0) < 1e-6);
    }
    CAD_CHECK(s.setDimension(circleId, "cx", -12.0).ok);
    CAD_CHECK(s.setDimension(circleId, "cy", 3.5).ok);
    CAD_CHECK(s.setDimension(circleId, "radius", 20.0).ok);
    {
        const auto& c = std::get<SketchCircle>(s.findEntity(circleId)->geometry);
        CAD_CHECK(c.cx == -12.0 && c.cy == 3.5 && c.radius == 20.0 && c.diameter() == 40.0);
        CAD_CHECK(!s.setDimension(circleId, "diameter", 10.0).ok);
    }

    // 13. Rejeição de dimensões inválidas (estado preservado)
    std::printf("13. Dimensoes invalidas\n");
    {
        const Sketch before = s;
        const double nan = std::numeric_limits<double>::quiet_NaN();
        struct Bad { std::string id; std::string key; double value; } bad[] = {
            {rectId, "width", 0.0},     {rectId, "height", -3.0},  {rectId, "width", nan},
            {circleId, "radius", 0.0},  {circleId, "radius", -1.0}, {circleId, "cx", 1e6},
            {lineId, "x2", -10.0},      // com y2 = y1 -> comprimento zero
            {lineId, "y1", nan},        {"e-99", "x1", 1.0},       {rectId, "radius", 1.0},
        };
        for (const Bad& b : bad)
        {
            if (b.key == "x2")
                CAD_CHECK(s.setDimension(lineId, "y2", 5.0).ok); // garante y1 == y2 antes do caso
            const auto r = s.setDimension(b.id, b.key, b.value);
            std::printf("  recusado %s.%s: %s\n", b.id.c_str(), b.key.c_str(), r.message.c_str());
            CAD_CHECK(!r.ok && !r.message.empty());
        }
        CAD_CHECK(!s.addEntity(SketchLine{1, 1, 1, 1}).ok);
        CAD_CHECK(!s.addEntity(SketchRectangle{0, 0, 10, 0}).ok);
        CAD_CHECK(!s.addEntity(SketchCircle{0, 0, -5}).ok);
        CAD_CHECK(!s.addEntity(SketchRectangle{9995, 0, 10, 10}).ok); // ultrapassa o limite
        CAD_CHECK(!s.updateEntity(circleId, SketchLine{0, 0, 1, 1}).ok); // tipo não muda
        // Nada mudou além da linha ajustada antes dos casos.
        CAD_CHECK(s.entities().size() == before.entities().size());
        CAD_CHECK(std::get<SketchRectangle>(s.findEntity(rectId)->geometry).width == 70.0);
        CAD_CHECK(std::get<SketchCircle>(s.findEntity(circleId)->geometry).radius == 20.0);
        CAD_CHECK(s.nextEntityNumber() == 4); // tentativas recusadas não consomem ids
    }

    // 14. Preservação do estado após cancelamento (edição sobre cópia de trabalho)
    std::printf("14. Cancelamento\n");
    {
        std::string error;
        CAD_CHECK(doc.commitSketch(s, error)); // novo esboço "sk-9"
        const int nextBefore = doc.nextSketchNumber();
        Sketch working = *doc.findSketch("sk-9");
        CAD_CHECK(working.addEntity(SketchCircle{0, 0, 1}).ok);
        CAD_CHECK(working.setDimension(rectId, "width", 1.0).ok);
        CAD_CHECK(working.removeEntity(lineId));
        // "Cancelar": a cópia é descartada.
        const Sketch* kept = doc.findSketch("sk-9");
        CAD_CHECK(kept->entities().size() == 3);
        CAD_CHECK(std::get<SketchRectangle>(kept->findEntity(rectId)->geometry).width == 70.0);
        CAD_CHECK(kept->findEntity(lineId) != nullptr);
        CAD_CHECK(doc.nextSketchNumber() == nextBefore);
        // Novo esboço cancelado não entra no documento nem consome número.
        const Sketch discarded = doc.createSketch(PlaneType::YZ);
        CAD_CHECK(doc.findSketch(discarded.id()) == nullptr);
        CAD_CHECK(doc.createSketch(PlaneType::XY).id() == discarded.id());
    }

    // 15. Identificadores estáveis
    std::printf("15. Identificadores estaveis\n");
    {
        Sketch t("sk-1", "Ids", PlaneType::XY);
        std::string a, b, c;
        CAD_CHECK(t.addEntity(SketchLine{0, 0, 1, 0}, &a).ok);
        CAD_CHECK(t.addEntity(SketchLine{0, 0, 0, 1}, &b).ok);
        CAD_CHECK(t.setDimension(a, "x2", 5.0).ok);
        CAD_CHECK(t.findEntity(a) && a == "e-1" && b == "e-2"); // edição não muda o id
        CAD_CHECK(t.removeEntity(b));
        CAD_CHECK(t.addEntity(SketchCircle{0, 0, 1}, &c).ok);
        CAD_CHECK(c == "e-3"); // id removido não é reutilizado
        CAD_CHECK(entityNumber("e-12") == 12 && entityNumber("x-1") == 0 && entityNumber("e-") == 0);
        CAD_CHECK(cad::document::sketchNumber("sk-7") == 7 && cad::document::sketchNumber("sk7") == 0);
        // Ordem das entidades preservada.
        CAD_CHECK(t.entities()[0].id == "e-1" && t.entities()[1].id == "e-3");
        // Leitura com contador inconsistente ou ids duplicados é recusada.
        Sketch out;
        std::string error;
        std::vector<SketchEntity> ents{t.entities()[0], t.entities()[1]};
        CAD_CHECK(Sketch::fromData("sk-1", "Ids", PlaneType::XY, ents, 4, out, error));
        CAD_CHECK(!Sketch::fromData("sk-1", "Ids", PlaneType::XY, ents, 3, out, error));
        ents[1].id = "e-1";
        CAD_CHECK(!Sketch::fromData("sk-1", "Ids", PlaneType::XY, ents, 4, out, error));
        // Dimensões do esboço = dimensões de todas as entidades, em ordem.
        CAD_CHECK(t.dimensions().size() == 5 + 4);
    }

    // 24. Geometria sólida preservada pelos esboços
    std::printf("24. Solido preservado\n");
    CAD_CHECK(doc.shape().IsSame(solidBefore));
    CAD_CHECK(std::fabs(cad::modeling::ShapeInspector::volume(doc.shape()) - volumeBefore) < 1e-9);
    cad::test::checkTestPart(cad::modeling::ShapeInspector::inspect(doc.shape()));
    // E recalcular o sólido não perde os esboços.
    CAD_CHECK(doc.setParameters({120.0, 60.0, 20.0, 10.0}).ok);
    CAD_CHECK(doc.sketches().size() == 4);
    CAD_CHECK(doc.removeSketch("sk-2"));
    CAD_CHECK(!doc.removeSketch("sk-2"));
    CAD_CHECK(doc.createSketch(PlaneType::XY).id() == "sk-10"); // números não reutilizados

    return cad::test::finish("test_esboco");
}
