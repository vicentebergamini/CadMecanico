#include "modeling/ShapeInspector.h"

#include <BRepBndLib.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <NCollection_IndexedMap.hxx>
#include <TopTools_ShapeMapHasher.hxx>

namespace cad::modeling {

ShapeSummary ShapeInspector::inspect(const TopoDS_Shape& shape)
{
    ShapeSummary s;
    s.isNull = shape.IsNull();
    if (s.isNull)
        return s;

    s.isValid  = isValid(shape);
    s.solids   = countDistinct(shape, TopAbs_SOLID);
    s.shells   = countDistinct(shape, TopAbs_SHELL);
    s.faces    = countDistinct(shape, TopAbs_FACE);
    s.edges    = countDistinct(shape, TopAbs_EDGE);
    s.edgeOccurrences = countOccurrences(shape, TopAbs_EDGE);
    s.vertices = countDistinct(shape, TopAbs_VERTEX);
    s.volume   = volume(shape);
    s.area     = area(shape);
    s.boundingBox = boundingBox(shape);
    return s;
}

int ShapeInspector::countDistinct(const TopoDS_Shape& shape, TopAbs_ShapeEnum type)
{
    NCollection_IndexedMap<TopoDS_Shape, TopTools_ShapeMapHasher> map;
    TopExp::MapShapes(shape, type, map);
    return map.Extent();
}

int ShapeInspector::countOccurrences(const TopoDS_Shape& shape, TopAbs_ShapeEnum type)
{
    int n = 0;
    for (TopExp_Explorer exp(shape, type); exp.More(); exp.Next())
        ++n;
    return n;
}

double ShapeInspector::volume(const TopoDS_Shape& shape)
{
    GProp_GProps props;
    BRepGProp::VolumeProperties(shape, props);
    return props.Mass();
}

double ShapeInspector::area(const TopoDS_Shape& shape)
{
    GProp_GProps props;
    BRepGProp::SurfaceProperties(shape, props);
    return props.Mass();
}

Bnd_Box ShapeInspector::boundingBox(const TopoDS_Shape& shape)
{
    Bnd_Box box;
    BRepBndLib::AddOptimal(shape, box, false, false);
    return box;
}

bool ShapeInspector::isValid(const TopoDS_Shape& shape)
{
    return !shape.IsNull() && BRepCheck_Analyzer(shape).IsValid();
}

} // namespace cad::modeling
