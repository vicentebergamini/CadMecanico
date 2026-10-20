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

TopoDS_Shape lineEdge(const SketchPlane& plane, const SketchLine& l)
{
    BRepBuilderAPI_MakeEdge edge(plane.toGlobal(l.x1, l.y1), plane.toGlobal(l.x2, l.y2));
    return edge.IsDone() ? edge.Shape() : TopoDS_Shape();
}

} // namespace

TopoDS_Shape buildEntityShape(const SketchPlane& plane, const SketchEntity& entity)
{
    if (!entity.isValid())
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

        const auto& c = std::get<SketchCircle>(entity.geometry);
        // Eixo do círculo = normal do plano; direção X = eixo horizontal (sentido anti-horário no plano).
        const gp_Ax2 axis(plane.toGlobal(c.cx, c.cy), plane.normal(), plane.horizontalAxis());
        BRepBuilderAPI_MakeEdge edge(gp_Circ(axis, c.radius));
        return edge.IsDone() ? edge.Shape() : TopoDS_Shape();
    }
    catch (const Standard_Failure&)
    {
        return TopoDS_Shape();
    }
}

TopoDS_Shape buildSketchShape(const Sketch& sketch)
{
    BRep_Builder builder;
    TopoDS_Compound compound;
    builder.MakeCompound(compound);
    for (const SketchEntity& e : sketch.entities())
    {
        const TopoDS_Shape s = buildEntityShape(sketch.plane(), e);
        if (!s.IsNull())
            builder.Add(compound, s);
    }
    return compound;
}

} // namespace cad::sketch
