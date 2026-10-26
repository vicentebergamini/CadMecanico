#include "discretization/CubeClassifier.h"

#include <BRepAdaptor_Surface.hxx>
#include <BRepBndLib.hxx>
#include <BRepClass3d_SolidClassifier.hxx>
#include <BRepExtrema_DistShapeShape.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <Standard_Failure.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>

#include <algorithm>
#include <cmath>
#include <memory>

namespace cad::discretization {

CubeClassifier::CubeClassifier(const TopoDS_Shape& shape, double lco)
{
    m_delta = std::max(1e-4 * lco, 1e-6);
    m_pointTol = std::max(1e-7, 1e-6 * lco);
    for (TopExp_Explorer ex(shape, TopAbs_SOLID); ex.More(); ex.Next())
        m_solids.push_back(TopoDS::Solid(ex.Current()));
    for (const TopoDS_Solid& solid : m_solids)
        for (TopExp_Explorer ex(solid, TopAbs_FACE); ex.More(); ex.Next())
        {
            FaceEntry e;
            e.face = TopoDS::Face(ex.Current());
            // Caixa exata da face (sem tolerância da forma nem triangulação).
            BRepBndLib::AddOptimal(e.face, e.box, false, false);
            if (e.box.IsVoid())
                continue;
            const BRepAdaptor_Surface surface(e.face, false);
            switch (surface.GetType())
            {
            case GeomAbs_Plane:
                e.type = GeomAbs_Plane;
                e.position = surface.Plane().Position();
                break;
            case GeomAbs_Cylinder:
            {
                const gp_Cylinder c = surface.Cylinder();
                const gp_Dir d = c.Axis().Direction();
                for (int a = 0; a < 3; ++a)
                    if (std::fabs(std::fabs(d.Coord(a + 1)) - 1.0) < 1e-12)
                        e.axis = a;
                if (e.axis >= 0)
                {
                    e.type = GeomAbs_Cylinder;
                    e.position = c.Position();
                    e.radius = c.Radius();
                }
                break;
            }
            case GeomAbs_Sphere:
                e.type = GeomAbs_Sphere;
                e.position = surface.Sphere().Position();
                e.radius = surface.Sphere().Radius();
                break;
            default: break;
            }
            m_faces.push_back(e);
        }
    if (!m_solids.empty())
        BRepBndLib::AddOptimal(shape, m_box, false, false);
}

CubeClassifier::~CubeClassifier() = default;

PointState CubeClassifier::classify(const gp_XYZ& point) const
{
    if (m_classifiers.empty())
        for (const TopoDS_Solid& solid : m_solids)
            m_classifiers.push_back(std::make_unique<BRepClass3d_SolidClassifier>(solid));
    bool on = false;
    for (auto& c : m_classifiers)
    {
        c->Perform(gp_Pnt(point), m_pointTol);
        const TopAbs_State s = c->State();
        if (s == TopAbs_IN)
            return PointState::In;
        if (s == TopAbs_ON)
            on = true;
    }
    return on ? PointState::On : PointState::Out;
}

std::vector<int> CubeClassifier::allFaces() const
{
    std::vector<int> all(m_faces.size());
    for (std::size_t f = 0; f < all.size(); ++f)
        all[f] = int(f);
    return all;
}

void CubeClassifier::facesNear(const gp_XYZ& pmin, const gp_XYZ& pmax, const std::vector<int>& candidates,
                               std::vector<int>& out) const
{
    out.clear();
    Bnd_Box box;
    box.Update(pmin.X(), pmin.Y(), pmin.Z(), pmax.X(), pmax.Y(), pmax.Z());
    for (int f : candidates)
        if (!m_faces[std::size_t(f)].box.IsOut(box) && !surfaceMissesBox(m_faces[std::size_t(f)], pmin, pmax))
            out.push_back(f);
}

bool CubeClassifier::surfaceMissesBox(const FaceEntry& f, const gp_XYZ& pmin, const gp_XYZ& pmax)
{
    // Margem: só exclui com folga acima da precisão geométrica do OCCT.
    constexpr double eps = 1e-7;
    switch (f.type)
    {
    case GeomAbs_Plane:
    {
        // Cantos todos do mesmo lado do plano.
        const gp_XYZ o = f.position.Location().XYZ();
        const gp_XYZ n = f.position.Direction().XYZ();
        double lo = 1e300, hi = -1e300;
        for (int c = 0; c < 8; ++c)
        {
            const gp_XYZ p((c & 1) ? pmax.X() : pmin.X(), (c & 2) ? pmax.Y() : pmin.Y(), (c & 4) ? pmax.Z() : pmin.Z());
            const double s = n.Dot(p - o);
            lo = std::min(lo, s);
            hi = std::max(hi, s);
        }
        return lo > eps || hi < -eps;
    }
    case GeomAbs_Cylinder:
    {
        // Eixo paralelo ao eixo 'axis' da grade: distância ao eixo = distância 2D no plano normal.
        const int u = (f.axis + 1) % 3, v = (f.axis + 2) % 3;
        const gp_XYZ o = f.position.Location().XYZ();
        const double cu = o.Coord(u + 1), cv = o.Coord(v + 1);
        const double du = std::max({pmin.Coord(u + 1) - cu, 0.0, cu - pmax.Coord(u + 1)});
        const double dv = std::max({pmin.Coord(v + 1) - cv, 0.0, cv - pmax.Coord(v + 1)});
        const double dmin = std::hypot(du, dv);
        const double dmax = std::hypot(std::max(std::fabs(pmin.Coord(u + 1) - cu), std::fabs(pmax.Coord(u + 1) - cu)),
                                       std::max(std::fabs(pmin.Coord(v + 1) - cv), std::fabs(pmax.Coord(v + 1) - cv)));
        return f.radius < dmin - eps || f.radius > dmax + eps;
    }
    case GeomAbs_Sphere:
    {
        const gp_XYZ c = f.position.Location().XYZ();
        double dmin2 = 0.0, dmax2 = 0.0;
        for (int a = 1; a <= 3; ++a)
        {
            const double lo = pmin.Coord(a) - c.Coord(a), hi = pmax.Coord(a) - c.Coord(a);
            const double near = lo > 0 ? lo : (hi < 0 ? -hi : 0.0);
            const double far = std::max(std::fabs(lo), std::fabs(hi));
            dmin2 += near * near;
            dmax2 += far * far;
        }
        return f.radius < std::sqrt(dmin2) - eps || f.radius > std::sqrt(dmax2) + eps;
    }
    default: return false;
    }
}

bool CubeClassifier::boundaryCrosses(const gp_XYZ& pmin, const gp_XYZ& pmax, const std::vector<int>& faces,
                                     long long* tests, long long* failures) const
{
    // Exclusão analítica antes do teste de distância (que é caro).
    std::vector<int> remaining;
    for (int f : faces)
        if (!surfaceMissesBox(m_faces[std::size_t(f)], pmin, pmax))
            remaining.push_back(f);
    if (remaining.empty())
        return false;
    TopoDS_Shape boxSolid;
    try
    {
        boxSolid = BRepPrimAPI_MakeBox(gp_Pnt(pmin), gp_Pnt(pmax)).Solid();
    }
    catch (const Standard_Failure&)
    {
        if (failures)
            ++*failures;
        return true;
    }
    const double limit = 0.5 * m_delta;
    for (int f : remaining)
    {
        if (tests)
            ++*tests;
        try
        {
            BRepExtrema_DistShapeShape dist(boxSolid, m_faces[std::size_t(f)].face, Extrema_ExtFlag_MIN);
            if (!dist.IsDone())
            {
                if (failures)
                    ++*failures;
                return true; // sem resultado: conservador (o bloco é subdividido / cubo de contorno)
            }
            if (dist.InnerSolution() || dist.Value() <= limit)
                return true;
        }
        catch (const Standard_Failure&)
        {
            if (failures)
                ++*failures;
            return true;
        }
    }
    return false;
}

} // namespace cad::discretization
