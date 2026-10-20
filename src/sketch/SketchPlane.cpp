#include "sketch/SketchPlane.h"

#include <gp_Vec.hxx>

namespace cad::sketch {

SketchPlane::SketchPlane(PlaneType type)
    : m_type(type)
    , m_origin(0.0, 0.0, 0.0)
    , m_xAxis(1.0, 0.0, 0.0)
    , m_yAxis(0.0, 1.0, 0.0)
    , m_normal(0.0, 0.0, 1.0)
{
    switch (type)
    {
    case PlaneType::XY:
        m_xAxis = gp_Dir(1.0, 0.0, 0.0);
        m_yAxis = gp_Dir(0.0, 1.0, 0.0);
        break;
    case PlaneType::XZ:
        m_xAxis = gp_Dir(1.0, 0.0, 0.0);
        m_yAxis = gp_Dir(0.0, 0.0, 1.0);
        break;
    case PlaneType::YZ:
        m_xAxis = gp_Dir(0.0, 1.0, 0.0);
        m_yAxis = gp_Dir(0.0, 0.0, 1.0);
        break;
    }
    m_normal = m_xAxis.Crossed(m_yAxis);
}

gp_Ax3 SketchPlane::ax3() const
{
    return gp_Ax3(m_origin, m_normal, m_xAxis);
}

gp_Pnt SketchPlane::toGlobal(double u, double v) const
{
    return m_origin.Translated(gp_Vec(m_xAxis) * u + gp_Vec(m_yAxis) * v);
}

gp_Pnt2d SketchPlane::toLocal(const gp_Pnt& p) const
{
    const gp_Vec d(m_origin, p);
    return gp_Pnt2d(d.Dot(gp_Vec(m_xAxis)), d.Dot(gp_Vec(m_yAxis)));
}

double SketchPlane::distance(const gp_Pnt& p) const
{
    return gp_Vec(m_origin, p).Dot(gp_Vec(m_normal));
}

const char* SketchPlane::name(PlaneType type)
{
    switch (type)
    {
    case PlaneType::XY: return "XY";
    case PlaneType::XZ: return "XZ";
    case PlaneType::YZ: return "YZ";
    }
    return "";
}

std::optional<PlaneType> SketchPlane::fromName(const std::string& name)
{
    if (name == "XY") return PlaneType::XY;
    if (name == "XZ") return PlaneType::XZ;
    if (name == "YZ") return PlaneType::YZ;
    return std::nullopt;
}

} // namespace cad::sketch
