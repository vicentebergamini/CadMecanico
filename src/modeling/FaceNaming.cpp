#include "modeling/FaceNaming.h"

#include <BRepAdaptor_Surface.hxx>
#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <NCollection_IndexedMap.hxx>
#include <Standard_Failure.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_ShapeMapHasher.hxx>
#include <TopoDS.hxx>

#include <algorithm>
#include <cmath>
#include <vector>

namespace cad::modeling {

namespace {

constexpr double kNormalAngle = 1e-6; // rad
constexpr double kSamePlane = 1e-4;   // mm

} // namespace

std::optional<gp_Pln> planarFacePlane(const TopoDS_Face& face)
{
    if (face.IsNull())
        return std::nullopt;
    try
    {
        BRepAdaptor_Surface surface(face);
        if (surface.GetType() != GeomAbs_Plane)
            return std::nullopt;
        gp_Pln plane = surface.Plane();
        // A normal geométrica do plano segue a superfície; a face invertida aponta para o outro lado.
        if (face.Orientation() == TopAbs_REVERSED)
        {
            gp_Ax3 a = plane.Position();
            a.ZReverse();
            plane.SetPosition(a);
        }
        return plane;
    }
    catch (const Standard_Failure&)
    {
        return std::nullopt;
    }
}

FaceSignature describeFace(const TopoDS_Face& face)
{
    FaceSignature s;
    const auto plane = planarFacePlane(face);
    if (!plane)
        return s;
    s.normal = plane->Axis().Direction();
    GProp_GProps props;
    BRepGProp::SurfaceProperties(face, props);
    s.area = props.Mass();
    s.centroid = props.CentreOfMass();
    s.offset = gp_Vec(s.normal).Dot(gp_Vec(s.centroid.XYZ()));
    return s;
}

bool containsFace(const TopoDS_Shape& shape, const TopoDS_Shape& face)
{
    if (shape.IsNull() || face.IsNull() || face.ShapeType() != TopAbs_FACE)
        return false;
    NCollection_IndexedMap<TopoDS_Shape, TopTools_ShapeMapHasher> faces;
    TopExp::MapShapes(shape, TopAbs_FACE, faces);
    return faces.Contains(face);
}

FaceMatch resolvePlanarFace(const TopoDS_Shape& body, const FaceSignature& signature)
{
    FaceMatch result;
    if (body.IsNull())
    {
        result.message = "corpo de origem sem geometria";
        return result;
    }
    struct Candidate
    {
        TopoDS_Face face;
        FaceSignature s;
        double distance;
    };
    std::vector<Candidate> candidates;
    try
    {
        for (TopExp_Explorer ex(body, TopAbs_FACE); ex.More(); ex.Next())
        {
            const TopoDS_Face f = TopoDS::Face(ex.Current());
            const auto plane = planarFacePlane(f);
            if (!plane || plane->Axis().Direction().Angle(signature.normal) > kNormalAngle)
                continue;
            const FaceSignature s = describeFace(f);
            candidates.push_back(Candidate{f, s, s.centroid.Distance(signature.centroid)});
        }
    }
    catch (const Standard_Failure& e)
    {
        result.message = std::string("falha ao remapear a face: ") + e.what();
        return result;
    }
    if (candidates.empty())
    {
        result.message = "face de suporte não remapeável: nenhuma face plana do corpo reconstruído tem a mesma normal";
        return result;
    }
    auto pick = [&result](std::vector<Candidate>& list, bool exact, const char* what) {
        std::sort(list.begin(), list.end(), [](const Candidate& a, const Candidate& b) { return a.distance < b.distance; });
        if (list.size() > 1 && !(list[0].distance < 0.5 * list[1].distance))
        {
            result.message = std::string("face de suporte ambígua: ") + std::to_string(list.size()) + " faces " + what
                             + " sem correspondência segura";
            return;
        }
        result.ok = true;
        result.exact = exact; // mesmo plano
        result.face = list[0].face;
    };
    std::vector<Candidate> samePlane;
    for (const Candidate& c : candidates)
        if (std::fabs(c.s.offset - signature.offset) <= kSamePlane)
            samePlane.push_back(c);
    if (!samePlane.empty())
        pick(samePlane, true, "no mesmo plano");
    else
        pick(candidates, false, "paralelas equivalentes");
    return result;
}

} // namespace cad::modeling
