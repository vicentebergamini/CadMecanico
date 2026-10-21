#include "sketch/SketchShapes.h"

#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRep_Builder.hxx>
#include <Standard_Failure.hxx>
#include <TopoDS_Compound.hxx>
#include <gp_Ax2.hxx>
#include <gp_Circ.hxx>

namespace cad::sketch {

namespace {

constexpr double kPi = 3.14159265358979323846;

TopoDS_Shape lineEdge(const SketchPlane& plane, const SketchLine& l)
{
    BRepBuilderAPI_MakeEdge edge(plane.toGlobal(l.x1, l.y1), plane.toGlobal(l.x2, l.y2));
    return edge.IsDone() ? edge.Shape() : TopoDS_Shape();
}

gp_Circ circleOf(const SketchPlane& plane, const gp_Pnt2d& center, double radius)
{
    // Eixo do círculo = normal do plano; direção X = eixo horizontal (sentido anti-horário no plano).
    return gp_Circ(gp_Ax2(plane.toGlobal(center), plane.normal(), plane.horizontalAxis()), radius);
}

} // namespace

TopoDS_Edge buildCurveEdge(const SketchPlane& plane, const Curve2d& curve)
{
    try
    {
        switch (curve.kind)
        {
        case Curve2d::Kind::Segment:
        {
            BRepBuilderAPI_MakeEdge edge(plane.toGlobal(curve.start), plane.toGlobal(curve.end));
            return edge.IsDone() ? edge.Edge() : TopoDS_Edge();
        }
        case Curve2d::Kind::Circle:
        {
            BRepBuilderAPI_MakeEdge edge(circleOf(plane, curve.center, curve.radius));
            return edge.IsDone() ? edge.Edge() : TopoDS_Edge();
        }
        case Curve2d::Kind::Arc:
        {
            // Parâmetros crescentes no sentido anti-horário: o arco horário usa o intervalo invertido.
            double a = curve.startAngle, b = curve.startAngle + curve.sweep;
            if (b < a)
                std::swap(a, b);
            BRepBuilderAPI_MakeEdge edge(circleOf(plane, curve.center, curve.radius), a, b);
            return edge.IsDone() ? edge.Edge() : TopoDS_Edge();
        }
        }
    }
    catch (const Standard_Failure&)
    {
    }
    return TopoDS_Edge();
}

TopoDS_Shape buildEntityShape(const SketchPlane& plane, const SketchEntity& entity)
{
    if (!entity.isValid() || isCornerModifier(entity.geometry))
        return TopoDS_Shape();
    try
    {
        if (const auto* l = std::get_if<SketchLine>(&entity.geometry))
            return lineEdge(plane, *l);

        if (const auto* r = std::get_if<SketchRectangle>(&entity.geometry))
        {
            // Arestas consecutivas compartilham vértices: contorno fechado.
            const auto c = r->corners();
            gp_Pnt p[4];
            for (int i = 0; i < 4; ++i)
                p[i] = plane.toGlobal(c[i]);
            BRepBuilderAPI_MakeWire wire;
            for (int i = 0; i < 4; ++i)
                wire.Add(BRepBuilderAPI_MakeEdge(p[i], p[(i + 1) % 4]).Edge());
            return wire.IsDone() ? wire.Shape() : TopoDS_Shape();
        }

        if (const auto* a = std::get_if<SketchArc>(&entity.geometry))
        {
            Curve2d k;
            k.kind = Curve2d::Kind::Arc;
            k.center = gp_Pnt2d(a->cx, a->cy);
            k.radius = a->radius;
            k.startAngle = a->startAngle * kPi / 180.0;
            k.sweep = a->signedSweepRadians();
            return buildCurveEdge(plane, k);
        }

        const auto& c = std::get<SketchCircle>(entity.geometry);
        BRepBuilderAPI_MakeEdge edge(circleOf(plane, gp_Pnt2d(c.cx, c.cy), c.radius));
        return edge.IsDone() ? edge.Shape() : TopoDS_Shape();
    }
    catch (const Standard_Failure&)
    {
        return TopoDS_Shape();
    }
}

std::vector<std::pair<std::string, TopoDS_Shape>> buildResolvedShapes(const Sketch& sketch)
{
    std::vector<std::pair<std::string, TopoDS_Shape>> shapes;
    const ResolvedSketch resolved = resolveSketch(sketch);
    BRep_Builder builder;
    for (const SketchEntity& e : sketch.entities())
    {
        TopoDS_Compound compound;
        builder.MakeCompound(compound);
        bool any = false;
        for (const Curve2d& c : resolved.curves)
            if (c.entityId == e.id)
            {
                const TopoDS_Edge edge = buildCurveEdge(sketch.plane(), c);
                if (!edge.IsNull())
                {
                    builder.Add(compound, edge);
                    any = true;
                }
            }
        if (any)
            shapes.emplace_back(e.id, compound);
    }
    return shapes;
}

TopoDS_Shape buildSketchShape(const Sketch& sketch)
{
    BRep_Builder builder;
    TopoDS_Compound compound;
    builder.MakeCompound(compound);
    for (const auto& [id, shape] : buildResolvedShapes(sketch))
        builder.Add(compound, shape);
    return compound;
}

} // namespace cad::sketch