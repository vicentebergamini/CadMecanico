// Arco circular 2D, Fillet 2D e Chamfer 2D (E5.5A), sem Qt: criação por centro/início/fim,
// raio, ângulos, sentidos, edição, exclusão, rejeições, planos XY/XZ/YZ, aresta curva exata do
// OCCT, resolução de cantos (tangência, distâncias, fechamento) e extrusão de perfis com arcos,
// fillets e chanfros, com recálculo paramétrico.
#include "TestCheck.h"

#include "document/CadDocument.h"
#include "modeling/ShapeInspector.h"
#include "sketch/SketchProfile.h"
#include "sketch/SketchResolver.h"
#include "sketch/SketchShapes.h"

#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRep_Tool.hxx>
#include <GCPnts_AbscissaPoint.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <gp_Vec2d.hxx>

#include <cmath>
#include <cstdio>
#include <limits>

using namespace cad::document;
using namespace cad::sketch;
using cad::modeling::ShapeInspector;

namespace {

constexpr double kPi = 3.14159265358979323846;

bool near2(const gp_Pnt2d& a, double x, double y, double tol = 1e-9)
{
    return std::fabs(a.X() - x) <= tol && std::fabs(a.Y() - y) <= tol;
}

const Curve2d* curveOf(const ResolvedSketch& r, const std::string& id, int part = 0)
{
    for (const Curve2d& c : r.curves)
        if (c.entityId == id && c.part == part)
            return &c;
    return nullptr;
}

void checkRejectedEntity(Sketch& s, const SketchGeometry& g, const char* tag, const char* expected = nullptr)
{
    const size_t n = s.entities().size();
    const int next = s.nextEntityNumber();
    const EntityValidation v = s.addEntity(g);
    std::printf("  recusado (%s): %s\n", tag, v.message.c_str());
    CAD_CHECK(!v.ok && !v.message.empty());
    if (expected)
        CAD_CHECK(v.message.find(expected) != std::string::npos);
    CAD_CHECK(s.entities().size() == n && s.nextEntityNumber() == next);
}

// Extrusão do esboço (único) com a profundidade dada; retorna o volume (-1 se falhar).
double extrudedVolume(const Sketch& sketch, double depth, int* faces = nullptr)
{
    CadDocument doc;
    std::string error;
    Sketch s = sketch;
    CAD_CHECK(doc.commitSketch(s, error));
    const auto r = doc.addExtrusion({"", s.id(), depth, false});
    if (!r.ok)
    {
        std::printf("  extrusão recusada: %s\n", r.message.c_str());
        return -1.0;
    }
    const auto sum = ShapeInspector::inspect(doc.shape());
    CAD_CHECK(sum.isValid && sum.solids == 1);
    if (faces)
        *faces = sum.faces;
    return sum.volume;
}

Sketch sketchOn(PlaneType plane, std::initializer_list<SketchGeometry> geometries)
{
    Sketch s("sk-1", "Esboço 1", plane);
    for (const SketchGeometry& g : geometries)
    {
        const EntityValidation v = s.addEntity(g);
        if (!v.ok)
            std::printf("  entidade recusada: %s\n", v.message.c_str());
        CAD_CHECK(v.ok);
    }
    return s;
}

} // namespace

int main()
{
    std::printf("Arco 1-5. Centro, inicio e fim; raio, angulos e sentidos\n");
    {
        const SketchArc ccw = SketchArc::fromCenterStartEnd(gp_Pnt2d(0, 0), gp_Pnt2d(10, 0), gp_Pnt2d(0, 25), false);
        std::printf("  anti-horario: R %.4f, %.4f -> %.4f, varredura %.4f, comprimento %.4f\n", ccw.radius,
                    ccw.startAngle, ccw.endAngle, ccw.sweep(), ccw.length());
        CAD_CHECK_NEAR(ccw.radius, 10.0, 1e-12);       // raio = |início − centro|
        CAD_CHECK_NEAR(ccw.startAngle, 0.0, 1e-12);
        CAD_CHECK_NEAR(ccw.endAngle, 90.0, 1e-12);     // fim projetado na circunferência
        CAD_CHECK_NEAR(ccw.sweep(), 90.0, 1e-12);
        CAD_CHECK(near2(ccw.endPoint(), 0.0, 10.0, 1e-9));
        CAD_CHECK(near2(ccw.pointAt(0.5), 10.0 * std::cos(kPi / 4), 10.0 * std::sin(kPi / 4), 1e-9));
        CAD_CHECK_NEAR(ccw.length(), 5.0 * kPi, 1e-9);
        const SketchArc cw = SketchArc::fromCenterStartEnd(gp_Pnt2d(0, 0), gp_Pnt2d(10, 0), gp_Pnt2d(0, 25), true);
        std::printf("  horario: varredura %.4f, ponto medio (%.4f; %.4f)\n", cw.sweep(), cw.pointAt(0.5).X(),
                    cw.pointAt(0.5).Y());
        CAD_CHECK_NEAR(cw.sweep(), 270.0, 1e-12);
        CAD_CHECK(near2(cw.pointAt(0.5), -10.0 * std::cos(kPi / 4), -10.0 * std::sin(kPi / 4), 1e-9));
        CAD_CHECK_NEAR(cw.signedSweepRadians(), -1.5 * kPi, 1e-12);
        CAD_CHECK(near2(cw.endPoint(), 0.0, 10.0, 1e-9));
        // Ângulos fora de [0, 360) normalizados na criação; entrada direta aceita até ±720°.
        const SketchArc neg = SketchArc::fromCenterStartEnd(gp_Pnt2d(5, 5), gp_Pnt2d(5, -5), gp_Pnt2d(15, 5), false);
        CAD_CHECK_NEAR(neg.startAngle, 270.0, 1e-9);
        CAD_CHECK_NEAR(neg.endAngle, 0.0, 1e-9);
        CAD_CHECK_NEAR(neg.sweep(), 90.0, 1e-9);
        CAD_CHECK(SketchEntity::validate(SketchArc{0, 0, 10, -90, 450, false}).ok);
        CAD_CHECK_NEAR((SketchArc{0, 0, 10, -90, 450, false}).sweep(), 180.0, 1e-9);
    }

    std::printf("Arco 7-12. Edicao, exclusao e rejeicoes\n");
    {
        Sketch s = sketchOn(PlaneType::XY, {SketchArc{0, 0, 10, 0, 90, false}});
        const std::string id = s.entities()[0].id;
        CAD_CHECK(s.setDimension(id, "cx", 5.0).ok && s.setDimension(id, "cy", -2.0).ok);       // 7. centro
        CAD_CHECK(s.setDimension(id, "radius", 12.5).ok);                                      // 8. raio
        CAD_CHECK(s.setDimension(id, "startAngle", 30.0).ok && s.setDimension(id, "endAngle", 300.0).ok); // 9.
        CAD_CHECK(s.setDimension(id, "clockwise", 1.0).ok);
        const auto& a = std::get<SketchArc>(s.findEntity(id)->geometry);
        std::printf("  editado: centro (%.1f; %.1f) R %.2f, %.1f -> %.1f horario, varredura %.1f\n", a.cx, a.cy, a.radius,
                    a.startAngle, a.endAngle, a.sweep());
        CAD_CHECK(a.cx == 5.0 && a.cy == -2.0 && a.radius == 12.5 && a.clockwise);
        CAD_CHECK_NEAR(a.sweep(), 90.0, 1e-12); // 30° → 300° no sentido horário
        const auto dims = dimensionsOf(*s.findEntity(id));
        CAD_CHECK(dims.size() == 8 && dims[3].kind == DimensionKind::Angle && dims[5].kind == DimensionKind::Flag);
        CAD_CHECK(!dims[6].editable && !dims[7].editable); // varredura e comprimento calculados
        CAD_CHECK(s.removeEntity(id) && s.entities().empty()); // 10. exclusão

        Sketch r("sk-2", "E", PlaneType::XY);
        checkRejectedEntity(r, SketchArc{0, 0, 0.0, 0, 90, false}, "raio zero", "Raio");                    // 11.
        checkRejectedEntity(r, SketchArc{0, 0, -5.0, 0, 90, false}, "raio negativo", "Raio");
        checkRejectedEntity(r, SketchArc{0, 0, 10, 45, 45, false}, "varredura nula", "Varredura");          // 12.
        checkRejectedEntity(r, SketchArc{0, 0, 10, 0, 359.95, false}, "circunferencia completa", "Círculo");
        checkRejectedEntity(r, SketchArc{0, 0, 0.01, 0, 1.0, false}, "arco degenerado", "degenerado");
        checkRejectedEntity(r, SketchArc{0, std::numeric_limits<double>::quiet_NaN(), 10, 0, 90, false}, "NaN", "não numérico");
        checkRejectedEntity(r, SketchArc{0, 0, 10, std::numeric_limits<double>::infinity(), 90, false}, "infinito");
        checkRejectedEntity(r, SketchArc{0, 0, 10, 0, 1000, false}, "angulo fora dos limites", "720");
        // Edição inválida não altera a entidade.
        Sketch e = sketchOn(PlaneType::XY, {SketchArc{0, 0, 10, 0, 90, false}});
        CAD_CHECK(!e.setDimension(e.entities()[0].id, "radius", 0.0).ok);
        CAD_CHECK(std::get<SketchArc>(e.entities()[0].geometry).radius == 10.0);
        CAD_CHECK(std::string(typeName(e.entities()[0].type())) == "arc");
        CAD_CHECK(e.entities()[0].type() == EntityType::Arc); // distinto do círculo completo
    }

    std::printf("Arco 13/16. Planos XY, XZ, YZ e aresta curva exata do OCCT\n");
    for (PlaneType plane : {PlaneType::XY, PlaneType::XZ, PlaneType::YZ})
    {
        SketchEntity e;
        e.id = "e-1";
        e.geometry = SketchArc{20, 10, 15, 30, 150, false};
        const SketchPlane p(plane);
        const TopoDS_Shape shape = buildEntityShape(p, e);
        CAD_CHECK(!shape.IsNull() && shape.ShapeType() == TopAbs_EDGE);
        const TopoDS_Edge edge = TopoDS::Edge(shape);
        BRepAdaptor_Curve curve(edge);
        const double len = GCPnts_AbscissaPoint::Length(curve);
        const auto& arc = std::get<SketchArc>(e.geometry);
        const gp_Pnt first = curve.Value(curve.FirstParameter());
        const gp_Pnt last = curve.Value(curve.LastParameter());
        const gp_Pnt s3 = p.toGlobal(arc.startPoint()), e3 = p.toGlobal(arc.endPoint());
        std::printf("  [%s] tipo %s, R %.4f, comprimento %.4f (esperado %.4f), extremos %.2e / %.2e\n",
                    SketchPlane::name(plane), curve.GetType() == GeomAbs_Circle ? "Circle" : "outro",
                    curve.GetType() == GeomAbs_Circle ? curve.Circle().Radius() : 0.0, len, arc.length(),
                    first.Distance(s3), last.Distance(e3));
        CAD_CHECK(curve.GetType() == GeomAbs_Circle);
        CAD_CHECK_NEAR(curve.Circle().Radius(), 15.0, 1e-9);
        CAD_CHECK_NEAR(len, arc.length(), 1e-9);
        CAD_CHECK(first.Distance(s3) < 1e-9 && last.Distance(e3) < 1e-9);
        CAD_CHECK(std::fabs(p.distance(curve.Value(0.5 * (curve.FirstParameter() + curve.LastParameter())))) < 1e-9);
        CAD_CHECK(curve.Circle().Axis().Direction().IsParallel(p.normal(), 1e-12)); // no plano do esboço
        CAD_CHECK(p.toGlobal(arc.pointAt(0.5)).Distance(curve.Value(0.5 * (curve.FirstParameter() + curve.LastParameter())))
                  < 1e-9); // percurso anti-horário no sistema local
    }
    {
        // Arco horário: mesma geometria do anti-horário complementar.
        SketchEntity e;
        e.geometry = SketchArc{0, 0, 10, 90, 0, true}; // 90° → 0° horário = quarto superior direito
        BRepAdaptor_Curve curve(TopoDS::Edge(buildEntityShape(SketchPlane(PlaneType::XY), e)));
        const gp_Pnt mid = curve.Value(0.5 * (curve.FirstParameter() + curve.LastParameter()));
        CAD_CHECK(mid.Distance(gp_Pnt(10 * std::cos(kPi / 4), 10 * std::sin(kPi / 4), 0)) < 1e-9);
        CAD_CHECK_NEAR(GCPnts_AbscissaPoint::Length(curve), 5.0 * kPi, 1e-9);
    }

    std::printf("Arco 17. Extrusao de perfis fechados com arcos\n");
    {
        // Meio disco: arco 0° → 180° (R10, centro (10, 0)) + diâmetro de (0, 0) a (20, 0).
        const Sketch d = sketchOn(PlaneType::XY, {SketchArc{10, 0, 10, 0, 180, false}, SketchLine{0, 0, 20, 0}});
        const ProfileResult pr = buildProfile(d);
        CAD_CHECK(pr.ok);
        std::printf("  meio disco: area %.6f (esperado %.6f)\n", pr.area, 50.0 * kPi);
        CAD_CHECK_NEAR(pr.area, 50.0 * kPi, 1e-6);
        int faces = 0;
        const double v = extrudedVolume(d, 10.0, &faces);
        std::printf("  volume %.6f (esperado %.6f), faces %d\n", v, 500.0 * kPi, faces);
        CAD_CHECK_NEAR(v, 500.0 * kPi, 1e-6);
        CAD_CHECK(faces == 4); // topo, base, plana e cilíndrica (aresta curva real)
        // Mesmo meio disco descrito por um arco horário (180° → 0°).
        const Sketch dcw = sketchOn(PlaneType::XY, {SketchArc{10, 0, 10, 180, 0, true}, SketchLine{20, 0, 0, 0}});
        CAD_CHECK_NEAR(extrudedVolume(dcw, 10.0), 500.0 * kPi, 1e-6);
        // Rasgo (oblongo): duas linhas e dois arcos, nos três planos.
        for (PlaneType plane : {PlaneType::XY, PlaneType::XZ, PlaneType::YZ})
        {
            const Sketch slot = sketchOn(plane, {SketchLine{0, 0, 40, 0}, SketchArc{40, 10, 10, 270, 90, false},
                                                 SketchLine{40, 20, 0, 20}, SketchArc{0, 10, 10, 90, 270, false}});
            const double expected = (40.0 * 20.0 + kPi * 100.0) * 5.0;
            const double vs = extrudedVolume(slot, 5.0, &faces);
            std::printf("  rasgo [%s]: volume %.6f (esperado %.6f), faces %d\n", SketchPlane::name(plane), vs, expected,
                        faces);
            CAD_CHECK_NEAR(vs, expected, 1e-6);
            CAD_CHECK(faces == 6);
        }
        // Arco isolado (aberto) e arco + linha que não fecham: recusados.
        CAD_CHECK(!buildProfile(sketchOn(PlaneType::XY, {SketchArc{0, 0, 10, 0, 90, false}})).ok);
        const ProfileResult open = buildProfile(sketchOn(PlaneType::XY, {SketchArc{10, 0, 10, 0, 180, false},
                                                                          SketchLine{0, 0, 19, 0}}));
        std::printf("  recusado (aberto): %s\n", open.message.c_str());
        CAD_CHECK(!open.ok && open.message.find("aberto") != std::string::npos);
        // Arco que cruza a linha de fechamento: autointersectante.
        const ProfileResult cross = buildProfile(sketchOn(PlaneType::XY, {SketchArc{10, 0, 10, 180, 360, true},
                                                                           SketchLine{0, 0, 10, 15},
                                                                           SketchLine{10, 15, 20, 0}}));
        std::printf("  recusado (cruzamento): %s\n", cross.message.c_str());
        CAD_CHECK(!cross.ok);
    }

    std::printf("Fillet 1-4. R10 em canto de 90 graus do retangulo 100 x 60\n");
    {
        Sketch s = sketchOn(PlaneType::XY, {SketchRectangle{0, 0, 100, 60}});
        const std::string rect = s.entities()[0].id;
        CAD_CHECK(s.addEntity(SketchFillet{rect, rect, 2, 10.0}).ok); // canto (100, 60)
        const ResolvedSketch r = resolveSketch(s);
        CAD_CHECK(r.ok);
        const Curve2d* arc = curveOf(r, "e-2");
        const Curve2d* right = curveOf(r, rect, 1);
        const Curve2d* top = curveOf(r, rect, 2);
        CAD_CHECK(arc && right && top && arc->kind == Curve2d::Kind::Arc);
        std::printf("  arco: centro (%.4f; %.4f) R %.4f, de (%.4f; %.4f) a (%.4f; %.4f), varredura %.4f graus\n",
                    arc->center.X(), arc->center.Y(), arc->radius, arc->start.X(), arc->start.Y(), arc->end.X(),
                    arc->end.Y(), arc->sweep * 180.0 / kPi);
        CAD_CHECK(near2(arc->center, 90.0, 50.0, 1e-9));
        CAD_CHECK_NEAR(arc->radius, 10.0, 1e-12); // 2. raio geométrico
        CAD_CHECK_NEAR(std::fabs(arc->sweep), kPi / 2.0, 1e-12);
        CAD_CHECK(near2(right->end, 100.0, 50.0, 1e-9) && near2(top->start, 90.0, 60.0, 1e-9)); // lados recortados
        // 3. Tangência: raio perpendicular ao lado em cada ponto de tangência; pontos sobre os lados.
        const gp_Vec2d rA(arc->center, arc->start), rB(arc->center, arc->end);
        const gp_Vec2d dRight(right->start, right->end), dTop(top->start, top->end);
        CAD_CHECK(std::fabs(rA.Dot(dRight)) < 1e-9 && std::fabs(rB.Dot(dTop)) < 1e-9);
        CAD_CHECK(arc->start.Distance(right->end) < 1e-12 && arc->end.Distance(top->start) < 1e-12);
        CAD_CHECK_NEAR(arc->center.Distance(arc->start), 10.0, 1e-12);
        // 4. Contorno fechado: perfil único com a área exata.
        const ProfileResult pr = buildProfile(s);
        const double area = 6000.0 - (100.0 - 25.0 * kPi);
        std::printf("  perfil: %s, area %.6f (esperado %.6f)\n", pr.ok ? "fechado" : pr.message.c_str(), pr.area, area);
        CAD_CHECK(pr.ok && pr.loops == 1);
        CAD_CHECK_NEAR(pr.area, area, 1e-6);
        // Forma exibida: lado recortado e arco do fillet como forma própria.
        int edges = 0;
        for (const auto& [id, shape] : buildResolvedShapes(s))
            for (TopExp_Explorer ex(shape, TopAbs_EDGE); ex.More(); ex.Next())
                ++edges;
        CAD_CHECK(edges == 5);
    }

    std::printf("Chamfer 5-7. 5 mm em canto do retangulo\n");
    {
        Sketch s = sketchOn(PlaneType::XY, {SketchRectangle{0, 0, 100, 60}});
        CAD_CHECK(s.addEntity(SketchChamfer{"e-1", "e-1", 0, 5.0}).ok); // canto (0, 0)
        const ResolvedSketch r = resolveSketch(s);
        const Curve2d* c = curveOf(r, "e-2");
        CAD_CHECK(c && c->kind == Curve2d::Kind::Segment);
        std::printf("  chanfro de (%.4f; %.4f) a (%.4f; %.4f), comprimento %.4f\n", c->start.X(), c->start.Y(), c->end.X(),
                    c->end.Y(), c->length());
        // 6. Pontos a 5 mm do vértice original, medidos ao longo dos lados.
        CAD_CHECK((near2(c->start, 0.0, 5.0) && near2(c->end, 5.0, 0.0)) || (near2(c->start, 5.0, 0.0) && near2(c->end, 0.0, 5.0)));
        CAD_CHECK_NEAR(c->length(), 5.0 * std::sqrt(2.0), 1e-12);
        CAD_CHECK(near2(curveOf(r, "e-1", 0)->start, 5.0, 0.0) && near2(curveOf(r, "e-1", 3)->end, 0.0, 5.0));
        const ProfileResult pr = buildProfile(s); // 7. fechamento
        std::printf("  perfil: area %.6f (esperado %.6f)\n", pr.area, 6000.0 - 12.5);
        CAD_CHECK(pr.ok && pr.loops == 1);
        CAD_CHECK_NEAR(pr.area, 6000.0 - 12.5, 1e-9);
    }

    std::printf("Fillet/Chamfer entre duas linhas conectadas\n");
    {
        // Quadrilátero de linhas; fillet R5 entre e-1 e e-2 (canto (40, 0), 90°); chanfro 3 entre e-3 e e-4.
        Sketch s = sketchOn(PlaneType::XY, {SketchLine{0, 0, 40, 0}, SketchLine{40, 0, 40, 30}, SketchLine{40, 30, 0, 30},
                                            SketchLine{0, 30, 0, 0}});
        CAD_CHECK(s.addEntity(SketchFillet{"e-1", "e-2", -1, 5.0}).ok);
        CAD_CHECK(s.addEntity(SketchChamfer{"e-3", "e-4", -1, 3.0}).ok);
        const ProfileResult pr = buildProfile(s);
        const double area = 1200.0 - (25.0 - 25.0 * kPi / 4.0) - 4.5;
        std::printf("  perfil de linhas: area %.6f (esperado %.6f)\n", pr.area, area);
        CAD_CHECK(pr.ok);
        CAD_CHECK_NEAR(pr.area, area, 1e-6);
        // Canto não reto (60°) entre linhas: triângulo equilátero, fillet R2.
        const double h = 20.0 * std::sqrt(3.0) / 2.0;
        Sketch t = sketchOn(PlaneType::XY, {SketchLine{0, 0, 20, 0}, SketchLine{20, 0, 10, h}, SketchLine{10, h, 0, 0}});
        CAD_CHECK(t.addEntity(SketchFillet{"e-1", "e-2", -1, 2.0}).ok);
        const ResolvedSketch rt = resolveSketch(t);
        const Curve2d* a = curveOf(rt, "e-4");
        const double tdist = 2.0 / std::tan(kPi / 6.0); // θ = 60°: t = r / tan(30°)
        std::printf("  canto de 60 graus: varredura %.4f graus (esperado 120), recorte %.6f (esperado %.6f)\n",
                    std::fabs(a->sweep) * 180.0 / kPi, 20.0 - curveOf(rt, "e-1")->end.X(), tdist);
        CAD_CHECK_NEAR(std::fabs(a->sweep), 2.0 * kPi / 3.0, 1e-12);
        CAD_CHECK_NEAR(20.0 - curveOf(rt, "e-1")->end.X(), tdist, 1e-9);
        CAD_CHECK(buildProfile(t).ok);
    }

    std::printf("Fillet/Chamfer 8-10. Rejeicoes\n");
    {
        Sketch s = sketchOn(PlaneType::XY, {SketchLine{0, 0, 40, 0}, SketchLine{50, 0, 50, 30}, SketchRectangle{100, 0, 50, 30},
                                            SketchArc{0, 50, 10, 0, 90, false}, SketchLine{0, 10, 0, 20},
                                            SketchLine{0, 20, 0, 40}});
        checkRejectedEntity(s, SketchFillet{"e-1", "e-2", -1, 2.0}, "linhas desconectadas", "não estão conectadas"); // 8.
        checkRejectedEntity(s, SketchFillet{"e-3", "e-3", 1, 0.0}, "raio zero", "Raio");                            // 9.
        checkRejectedEntity(s, SketchFillet{"e-3", "e-3", 1, -1.0}, "raio negativo", "Raio");
        checkRejectedEntity(s, SketchChamfer{"e-3", "e-3", 1, std::numeric_limits<double>::quiet_NaN()}, "distancia NaN", "não numérico");
        checkRejectedEntity(s, SketchFillet{"e-3", "e-3", 4, 2.0}, "canto inexistente", "canto");
        checkRejectedEntity(s, SketchFillet{"e-3", "e-3", 1, 40.0}, "raio inviavel", "grande demais");             // 10.
        checkRejectedEntity(s, SketchChamfer{"e-3", "e-3", 1, 30.0}, "chanfro inviavel", "grande demais");
        checkRejectedEntity(s, SketchFillet{"e-5", "e-6", -1, 1.0}, "linhas colineares", "colineares");
        checkRejectedEntity(s, SketchFillet{"e-4", "e-1", -1, 1.0}, "arco como lado", "segmentos retos");
        checkRejectedEntity(s, SketchFillet{"e-1", "e-99", -1, 1.0}, "referencia inexistente", "inexistente");
        CAD_CHECK(s.addEntity(SketchFillet{"e-3", "e-3", 1, 5.0}).ok);
        checkRejectedEntity(s, SketchChamfer{"e-3", "e-3", 1, 2.0}, "canto ja modificado", "já foi modificado");
        // Exclusão de um lado referenciado: recusada.
        std::string error;
        CAD_CHECK(!s.removeEntity("e-3", error));
        std::printf("  exclusao recusada: %s\n", error.c_str());
        CAD_CHECK(error.find("e-7") != std::string::npos);
        // Edição de um lado que inviabiliza o fillet: recusada, geometria mantida.
        const EntityValidation shrink = s.updateEntity("e-3", SketchRectangle{100, 0, 4, 30});
        std::printf("  edicao recusada: %s\n", shrink.message.c_str());
        CAD_CHECK(!shrink.ok && std::get<SketchRectangle>(s.findEntity("e-3")->geometry).width == 50.0);
        // Excluir o fillet libera o canto e o lado.
        CAD_CHECK(s.removeEntity("e-7") && s.removeEntity("e-3"));
    }

    std::printf("Fillet/Chamfer 11-12. Edicao do raio e da distancia\n");
    {
        Sketch s = sketchOn(PlaneType::XY, {SketchRectangle{0, 0, 100, 60}, SketchFillet{"e-1", "e-1", 2, 10.0},
                                            SketchChamfer{"e-1", "e-1", 0, 5.0}});
        CAD_CHECK(s.setDimension("e-2", "radius", 20.0).ok);
        CAD_CHECK(s.setDimension("e-3", "distance", 8.0).ok);
        const ProfileResult pr = buildProfile(s);
        const double area = 6000.0 - (400.0 - 100.0 * kPi) - 32.0;
        std::printf("  R20 + chanfro 8: area %.6f (esperado %.6f)\n", pr.area, area);
        CAD_CHECK_NEAR(pr.area, area, 1e-6);
        const EntityValidation big = s.setDimension("e-2", "radius", 70.0);
        std::printf("  edicao recusada: %s\n", big.message.c_str());
        CAD_CHECK(!big.ok && std::get<SketchFillet>(s.findEntity("e-2")->geometry).radius == 20.0);
    }

    std::printf("Fillet/Chamfer 13-16. Planos e extrusao\n");
    {
        const double filletArea = 6000.0 - (100.0 - 25.0 * kPi);
        const double chamferArea = 6000.0 - 12.5;
        const double bothArea = filletArea - 12.5;
        for (PlaneType plane : {PlaneType::XY, PlaneType::XZ, PlaneType::YZ})
        {
            int faces = 0;
            const double vf = extrudedVolume(sketchOn(plane, {SketchRectangle{0, 0, 100, 60}, SketchFillet{"e-1", "e-1", 2, 10.0}}), 20.0, &faces);
            std::printf("  [%s] fillet R10: volume %.4f (esperado %.4f), faces %d\n", SketchPlane::name(plane), vf,
                        filletArea * 20.0, faces);
            CAD_CHECK_NEAR(vf, filletArea * 20.0, 1e-5);
            CAD_CHECK(faces == 7); // face cilíndrica real do arredondamento
            const double vc = extrudedVolume(sketchOn(plane, {SketchRectangle{0, 0, 100, 60}, SketchChamfer{"e-1", "e-1", 0, 5.0}}), 20.0, &faces);
            std::printf("  [%s] chamfer 5: volume %.4f (esperado %.4f), faces %d\n", SketchPlane::name(plane), vc,
                        chamferArea * 20.0, faces);
            CAD_CHECK_NEAR(vc, chamferArea * 20.0, 1e-6);
            CAD_CHECK(faces == 7);
            const double vb = extrudedVolume(sketchOn(plane, {SketchRectangle{0, 0, 100, 60}, SketchFillet{"e-1", "e-1", 2, 10.0},
                                                              SketchChamfer{"e-1", "e-1", 0, 5.0}}), 20.0, &faces);
            std::printf("  [%s] fillet + chamfer: volume %.4f (esperado %.4f), faces %d\n", SketchPlane::name(plane), vb,
                        bothArea * 20.0, faces);
            CAD_CHECK_NEAR(vb, bothArea * 20.0, 1e-5);
            CAD_CHECK(faces == 8);
        }
    }

    std::printf("Fillet/Chamfer 18. Recalculo parametrico da extrusao dependente\n");
    {
        CadDocument doc;
        Sketch s = doc.createSketch(PlaneType::XY);
        CAD_CHECK(s.addEntity(SketchRectangle{0, 0, 100, 60}).ok);
        CAD_CHECK(s.addEntity(SketchFillet{"e-1", "e-1", 2, 10.0}).ok);
        std::string error;
        CAD_CHECK(doc.commitSketch(s, error));
        std::string ext;
        CAD_CHECK(doc.addExtrusion({"", s.id(), 20.0, false}, &ext).ok);
        CAD_CHECK_NEAR(ShapeInspector::volume(doc.shape()), (6000.0 - (100.0 - 25.0 * kPi)) * 20.0, 1e-5);
        CAD_CHECK(s.setDimension("e-2", "radius", 20.0).ok);
        CAD_CHECK(doc.commitSketch(s, error));
        const double expected = (6000.0 - (400.0 - 100.0 * kPi)) * 20.0;
        std::printf("  raio 10 -> 20: volume %.4f (esperado %.4f)\n", ShapeInspector::volume(doc.shape()), expected);
        CAD_CHECK_NEAR(ShapeInspector::volume(doc.shape()), expected, 1e-5);
        // Arco independente num esboço separado não entra no perfil da extrusão.
        Sketch arcSketch = doc.createSketch(PlaneType::XY);
        CAD_CHECK(arcSketch.addEntity(SketchArc{150, 0, 10, 0, 120, false}).ok);
        CAD_CHECK(doc.commitSketch(arcSketch, error));
        CAD_CHECK_NEAR(ShapeInspector::volume(doc.shape()), expected, 1e-5);
        CAD_CHECK(!doc.addExtrusion({"", arcSketch.id(), 5.0, false}).ok); // arco aberto: sem perfil
    }

    return cad::test::finish("test_arcos_cantos2d");
}
