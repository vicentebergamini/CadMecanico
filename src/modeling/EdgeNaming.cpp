#include "modeling/EdgeNaming.h"

#include <BRepAdaptor_Curve.hxx>
#include <BRepGProp_Face.hxx>
#include <BRep_Tool.hxx>
#include <GCPnts_AbscissaPoint.hxx>
#include <Geom2d_Curve.hxx>
#include <NCollection_IndexedDataMap.hxx>
#include <NCollection_IndexedMap.hxx>
#include <NCollection_List.hxx>
#include <Standard_Failure.hxx>
#include <TopExp.hxx>
#include <TopTools_ShapeMapHasher.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>

#include <algorithm>
#include <cmath>

namespace cad::modeling {

namespace {

using EdgeFaceMap = NCollection_IndexedDataMap<TopoDS_Shape, NCollection_List<TopoDS_Shape>, TopTools_ShapeMapHasher>;
using ShapeSet = NCollection_IndexedMap<TopoDS_Shape, TopTools_ShapeMapHasher>;

constexpr double kExactDistance = 1e-4;  // mm
constexpr double kParallel = 1e-6;       // 1 − |cos|
constexpr double kNormalAngle = 1e-3;    // rad

// Direção sem sinal: primeira componente significativa positiva.
gp_Dir canonical(const gp_Dir& d)
{
    const double c[3] = {d.X(), d.Y(), d.Z()};
    for (double v : c)
        if (std::fabs(v) > 1e-9)
            return v < 0.0 ? d.Reversed() : d;
    return d;
}

EdgeSignature describe(const TopoDS_Edge& edge, const EdgeFaceMap& faces)
{
    EdgeSignature s;
    BRepAdaptor_Curve curve(edge);
    const double f = curve.FirstParameter(), l = curve.LastParameter();
    const double m = 0.5 * (f + l);
    s.mid = curve.Value(m);
    s.length = GCPnts_AbscissaPoint::Length(curve);
    switch (curve.GetType())
    {
    case GeomAbs_Line:
        s.curve = EdgeSignature::Curve::Line;
        s.direction = canonical(curve.Line().Direction());
        break;
    case GeomAbs_Circle:
        s.curve = EdgeSignature::Curve::Circle;
        s.direction = canonical(curve.Circle().Axis().Direction());
        s.radius = curve.Circle().Radius();
        break;
    default:
    {
        s.curve = EdgeSignature::Curve::Other;
        gp_Pnt p;
        gp_Vec t;
        curve.D1(m, p, t);
        if (t.Magnitude() > 1e-12)
            s.direction = canonical(gp_Dir(t));
        break;
    }
    }
    if (const NCollection_List<TopoDS_Shape>* adjacent = faces.Seek(edge))
    {
        for (const TopoDS_Shape& fs : *adjacent)
        {
            const TopoDS_Face face = TopoDS::Face(fs);
            double pf = 0.0, pl = 0.0;
            const occ::handle<Geom2d_Curve> pcurve = BRep_Tool::CurveOnSurface(edge, face, pf, pl);
            if (pcurve.IsNull())
                continue;
            // Parâmetro médio da pcurve (mesma parametrização da curva 3D da aresta).
            const gp_Pnt2d uv = pcurve->Value(0.5 * (pf + pl));
            gp_Pnt p;
            gp_Vec n;
            BRepGProp_Face(face).Normal(uv.X(), uv.Y(), p, n);
            if (n.Magnitude() <= 1e-12)
                continue;
            const gp_Dir nd(n);
            // Faces duplicadas na lista (aresta de costura) contam uma vez.
            if (std::none_of(s.normals.begin(), s.normals.end(), [&](const gp_Dir& o) { return o.Angle(nd) < kNormalAngle; }))
                s.normals.push_back(nd);
        }
    }
    return s;
}

bool sameRole(const EdgeSignature& a, const EdgeSignature& b)
{
    if (a.curve != b.curve)
        return false;
    if (1.0 - std::fabs(a.direction.Dot(b.direction)) > kParallel)
        return false;
    if (a.normals.size() != b.normals.size())
        return false;
    for (const gp_Dir& n : a.normals)
        if (std::none_of(b.normals.begin(), b.normals.end(), [&](const gp_Dir& o) { return o.Angle(n) < kNormalAngle; }))
            return false;
    return true;
}

EdgeFaceMap edgeFaces(const TopoDS_Shape& solid)
{
    EdgeFaceMap map;
    TopExp::MapShapesAndAncestors(solid, TopAbs_EDGE, TopAbs_FACE, map);
    return map;
}

} // namespace

const char* curveName(EdgeSignature::Curve curve)
{
    switch (curve)
    {
    case EdgeSignature::Curve::Line:   return "line";
    case EdgeSignature::Curve::Circle: return "circle";
    case EdgeSignature::Curve::Other:  return "other";
    }
    return "";
}

EdgeSignature describeEdge(const TopoDS_Shape& solid, const TopoDS_Edge& edge)
{
    if (solid.IsNull() || edge.IsNull() || BRep_Tool::Degenerated(edge))
        return EdgeSignature(); // sem normais: nunca corresponde a uma aresta real
    try
    {
        return describe(edge, edgeFaces(solid));
    }
    catch (const Standard_Failure&)
    {
        return EdgeSignature();
    }
}

bool containsEdge(const TopoDS_Shape& shape, const TopoDS_Shape& edge)
{
    if (shape.IsNull() || edge.IsNull() || edge.ShapeType() != TopAbs_EDGE)
        return false;
    ShapeSet edges;
    TopExp::MapShapes(shape, TopAbs_EDGE, edges);
    return edges.Contains(edge);
}

EdgeMatch resolveEdge(const TopoDS_Shape& solid, const EdgeSignature& signature)
{
    EdgeMatch result;
    if (solid.IsNull())
    {
        result.message = "sólido de origem sem geometria";
        return result;
    }
    try
    {
        const EdgeFaceMap faces = edgeFaces(solid);
        struct Candidate
        {
            TopoDS_Edge edge;
            double distance;
        };
        std::vector<Candidate> candidates;
        for (int i = 1; i <= faces.Extent(); ++i)
        {
            const TopoDS_Edge edge = TopoDS::Edge(faces.FindKey(i));
            if (BRep_Tool::Degenerated(edge))
                continue;
            const EdgeSignature s = describe(edge, faces);
            if (sameRole(signature, s))
                candidates.push_back(Candidate{edge, s.mid.Distance(signature.mid)});
        }
        if (candidates.empty())
        {
            result.message = "aresta não remapeável: nenhuma aresta do sólido reconstruído tem o mesmo tipo, direção e "
                             "faces adjacentes";
            return result;
        }
        std::sort(candidates.begin(), candidates.end(),
                  [](const Candidate& a, const Candidate& b) { return a.distance < b.distance; });
        const Candidate& best = candidates.front();
        const bool unique = candidates.size() == 1;
        const bool clear = unique || best.distance < 0.5 * candidates[1].distance;
        if (best.distance <= kExactDistance && (unique || candidates[1].distance > kExactDistance))
        {
            result.ok = result.exact = true;
            result.edge = best.edge;
            return result;
        }
        if (!clear)
        {
            result.message = "aresta ambígua: " + std::to_string(candidates.size())
                             + " arestas equivalentes no sólido reconstruído, sem correspondência segura";
            return result;
        }
        result.ok = true;
        result.edge = best.edge;
        return result;
    }
    catch (const Standard_Failure& e)
    {
        result.message = std::string("falha ao remapear a aresta: ") + e.what();
        return result;
    }
}

} // namespace cad::modeling
