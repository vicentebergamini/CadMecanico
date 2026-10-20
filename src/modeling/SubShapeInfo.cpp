#include "modeling/SubShapeInfo.h"

#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepGProp.hxx>
#include <BRep_Tool.hxx>
#include <GProp_GProps.hxx>
#include <NCollection_IndexedMap.hxx>
#include <TopExp.hxx>
#include <TopTools_ShapeMapHasher.hxx>
#include <TopoDS.hxx>

namespace cad::modeling {

namespace {

void locate(const TopoDS_Shape& part, const TopoDS_Shape& sub, TopAbs_ShapeEnum type, SubShapeInfo& info)
{
    NCollection_IndexedMap<TopoDS_Shape, TopTools_ShapeMapHasher> map;
    if (!part.IsNull())
        TopExp::MapShapes(part, type, map);
    info.count = map.Extent();
    info.index = map.FindIndex(sub);
}

void describeFace(const TopoDS_Face& face, SubShapeInfo& info)
{
    const BRepAdaptor_Surface surface(face);
    switch (surface.GetType())
    {
    case GeomAbs_Plane:
        info.geometry = GeometryKind::Plane;
        break;
    case GeomAbs_Cylinder:
        info.geometry = GeometryKind::Cylinder;
        info.radius = surface.Cylinder().Radius();
        break;
    case GeomAbs_Cone:
        info.geometry = GeometryKind::Cone;
        info.radius = surface.Cone().RefRadius();
        break;
    case GeomAbs_Sphere:
        info.geometry = GeometryKind::Sphere;
        info.radius = surface.Sphere().Radius();
        break;
    case GeomAbs_Torus:
        info.geometry = GeometryKind::Torus;
        break;
    default:
        info.geometry = GeometryKind::OtherSurface;
        break;
    }
    GProp_GProps props;
    BRepGProp::SurfaceProperties(face, props);
    info.area = props.Mass();
}

void describeEdge(const TopoDS_Edge& edge, SubShapeInfo& info)
{
    GProp_GProps props;
    BRepGProp::LinearProperties(edge, props);
    info.length = props.Mass();
    if (BRep_Tool::Degenerated(edge))
    {
        info.geometry = GeometryKind::OtherCurve;
        return;
    }
    const BRepAdaptor_Curve curve(edge);
    switch (curve.GetType())
    {
    case GeomAbs_Line:
        info.geometry = GeometryKind::Line;
        break;
    case GeomAbs_Circle:
        info.geometry = GeometryKind::Circle;
        info.radius = curve.Circle().Radius();
        break;
    case GeomAbs_Ellipse:
        info.geometry = GeometryKind::Ellipse;
        break;
    default:
        info.geometry = GeometryKind::OtherCurve;
        break;
    }
}

} // namespace

SubShapeInfo describeSubShape(const TopoDS_Shape& part, const TopoDS_Shape& sub)
{
    SubShapeInfo info;
    if (sub.IsNull())
        return info;

    switch (sub.ShapeType())
    {
    case TopAbs_FACE:
        info.kind = SubShapeKind::Face;
        locate(part, sub, TopAbs_FACE, info);
        describeFace(TopoDS::Face(sub), info);
        break;
    case TopAbs_EDGE:
        info.kind = SubShapeKind::Edge;
        locate(part, sub, TopAbs_EDGE, info);
        describeEdge(TopoDS::Edge(sub), info);
        break;
    case TopAbs_VERTEX:
        info.kind = SubShapeKind::Vertex;
        info.geometry = GeometryKind::Point;
        locate(part, sub, TopAbs_VERTEX, info);
        info.point = BRep_Tool::Pnt(TopoDS::Vertex(sub));
        break;
    default:
        info.kind = SubShapeKind::Other;
        break;
    }
    return info;
}

} // namespace cad::modeling
