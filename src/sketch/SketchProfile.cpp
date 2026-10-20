#include "sketch/SketchProfile.h"

#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakePolygon.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <Standard_Failure.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Wire.hxx>
#include <gp_Ax2.hxx>
#include <gp_Circ.hxx>
#include <gp_Pln.hxx>
#include <gp_Vec2d.hxx>

#include <algorithm>
#include <cmath>
#include <sstream>

namespace cad::sketch {

namespace {

constexpr double kPi = 3.14159265358979323846;

std::string point(const gp_Pnt2d& p)
{
    std::ostringstream s;
    s.precision(2);
    s << std::fixed << "(" << p.X() << "; " << p.Y() << ")";
    std::string t = s.str();
    std::replace(t.begin(), t.end(), '.', ',');
    return t;
}

struct Loop
{
    bool circle = false;
    std::vector<gp_Pnt2d> points; // polígono, sem repetir o primeiro ponto
    SketchCircle c;
};

double signedArea(const std::vector<gp_Pnt2d>& p)
{
    double a = 0.0;
    for (size_t i = 0; i < p.size(); ++i)
    {
        const gp_Pnt2d& u = p[i];
        const gp_Pnt2d& v = p[(i + 1) % p.size()];
        a += u.X() * v.Y() - v.X() * u.Y();
    }
    return a / 2.0;
}

double cross(const gp_Pnt2d& o, const gp_Pnt2d& a, const gp_Pnt2d& b)
{
    return (a.X() - o.X()) * (b.Y() - o.Y()) - (a.Y() - o.Y()) * (b.X() - o.X());
}

bool onSegment(const gp_Pnt2d& p, const gp_Pnt2d& a, const gp_Pnt2d& b, double tol)
{
    const double len = a.Distance(b);
    if (len < tol)
        return p.Distance(a) <= tol;
    if (std::fabs(cross(a, b, p)) / len > tol)
        return false;
    const double t = ((p.X() - a.X()) * (b.X() - a.X()) + (p.Y() - a.Y()) * (b.Y() - a.Y())) / (len * len);
    return t >= -tol / len && t <= 1.0 + tol / len;
}

// Interseção (inclusive toque ou sobreposição) entre os segmentos ab e cd.
bool segmentsIntersect(const gp_Pnt2d& a, const gp_Pnt2d& b, const gp_Pnt2d& c, const gp_Pnt2d& d, double tol)
{
    const double d1 = cross(c, d, a), d2 = cross(c, d, b), d3 = cross(a, b, c), d4 = cross(a, b, d);
    if (((d1 > 0 && d2 < 0) || (d1 < 0 && d2 > 0)) && ((d3 > 0 && d4 < 0) || (d3 < 0 && d4 > 0)))
        return true;
    return onSegment(a, c, d, tol) || onSegment(b, c, d, tol) || onSegment(c, a, b, tol) || onSegment(d, a, b, tol);
}

// Polígono simples: lados não adjacentes não se tocam e lados adjacentes não se dobram sobre si.
bool polygonSelfIntersects(const std::vector<gp_Pnt2d>& p, std::string& where)
{
    const size_t n = p.size();
    for (size_t i = 0; i < n; ++i)
    {
        const gp_Pnt2d& a = p[i];
        const gp_Pnt2d& b = p[(i + 1) % n];
        const gp_Pnt2d& c = p[(i + 2) % n];
        // Adjacentes colineares em sentidos opostos (retorno sobre o mesmo segmento).
        const gp_Vec2d u(a, b), v(b, c);
        if (std::fabs(u.Crossed(v)) <= kClosureTolerance * std::max(u.Magnitude(), v.Magnitude()) && u.Dot(v) < 0.0)
        {
            where = point(b);
            return true;
        }
        for (size_t j = i + 2; j < n; ++j)
        {
            if (i == 0 && j == n - 1)
                continue; // adjacentes (último e primeiro lado)
            if (segmentsIntersect(a, b, p[j], p[(j + 1) % n], kClosureTolerance))
            {
                where = point(a) + "-" + point(b);
                return true;
            }
        }
    }
    return false;
}

ProfileResult fail(const std::string& message, int loops = 0)
{
    ProfileResult r;
    r.message = message;
    r.loops = loops;
    return r;
}

} // namespace

ProfileResult buildProfile(const Sketch& sketch)
{
    if (sketch.entities().empty())
        return fail("O esboço não contém entidades.");

    std::vector<Loop> loops;
    // Linhas: grafo de nós (extremidades coincidentes dentro da tolerância).
    std::vector<gp_Pnt2d> nodes;
    std::vector<std::pair<int, int>> edges;
    auto nodeOf = [&nodes](const gp_Pnt2d& p) {
        for (size_t i = 0; i < nodes.size(); ++i)
            if (nodes[i].Distance(p) <= kClosureTolerance)
                return int(i);
        nodes.push_back(p);
        return int(nodes.size() - 1);
    };

    for (const SketchEntity& e : sketch.entities())
    {
        if (!e.isValid())
            return fail("Entidade " + e.id + " inválida.");
        if (const auto* r = std::get_if<SketchRectangle>(&e.geometry))
        {
            Loop l;
            for (const gp_Pnt2d& c : r->corners())
                l.points.push_back(c);
            loops.push_back(l);
        }
        else if (const auto* c = std::get_if<SketchCircle>(&e.geometry))
        {
            Loop l;
            l.circle = true;
            l.c = *c;
            loops.push_back(l);
        }
        else
        {
            const auto& s = std::get<SketchLine>(e.geometry);
            const int a = nodeOf(gp_Pnt2d(s.x1, s.y1));
            const int b = nodeOf(gp_Pnt2d(s.x2, s.y2));
            if (a == b)
                return fail("Segmento degenerado (" + e.id + "): extremidades coincidentes.");
            for (const auto& ed : edges)
                if ((ed.first == a && ed.second == b) || (ed.first == b && ed.second == a))
                    return fail("Segmentos duplicados entre " + point(nodes[a]) + " e " + point(nodes[b]) + ".");
            edges.emplace_back(a, b);
        }
    }

    if (!edges.empty())
    {
        std::vector<std::vector<int>> adjacency(nodes.size());
        for (size_t i = 0; i < edges.size(); ++i)
        {
            adjacency[edges[i].first].push_back(int(i));
            adjacency[edges[i].second].push_back(int(i));
        }
        for (size_t n = 0; n < nodes.size(); ++n)
        {
            if (adjacency[n].size() == 1)
                return fail("Perfil aberto: extremidade livre em " + point(nodes[n]) + ".");
            if (adjacency[n].size() > 2)
                return fail("Contorno ambíguo: " + std::to_string(adjacency[n].size()) + " segmentos se encontram em "
                            + point(nodes[n]) + ".");
        }
        // Todos os nós com grau 2: cada componente conexa é um ciclo.
        std::vector<bool> used(edges.size(), false);
        for (size_t start = 0; start < edges.size(); ++start)
        {
            if (used[start])
                continue;
            Loop l;
            int node = edges[start].first;
            int edge = int(start);
            while (!used[edge])
            {
                used[edge] = true;
                l.points.push_back(nodes[node]);
                node = edges[edge].first == node ? edges[edge].second : edges[edge].first;
                const auto& adj = adjacency[node];
                edge = adj[0] == edge ? adj[1] : adj[0];
            }
            loops.push_back(l);
        }
    }

    const int loopCount = int(loops.size());
    if (loopCount > 1)
        return fail("O esboço contém " + std::to_string(loopCount)
                        + " contornos fechados. Nesta etapa a extrusão aceita um único contorno fechado "
                          "(múltiplos contornos e furos pertencem a etapas futuras).",
                    loopCount);

    Loop& loop = loops.front();
    const SketchPlane& plane = sketch.plane();
    ProfileResult result;
    result.loops = 1;
    try
    {
        TopoDS_Wire wire;
        if (loop.circle)
        {
            const gp_Ax2 axis(plane.toGlobal(loop.c.cx, loop.c.cy), plane.normal(), plane.horizontalAxis());
            wire = BRepBuilderAPI_MakeWire(BRepBuilderAPI_MakeEdge(gp_Circ(axis, loop.c.radius)).Edge()).Wire();
            result.area = kPi * loop.c.radius * loop.c.radius;
        }
        else
        {
            if (loop.points.size() < 3)
                return fail("Contorno com menos de três vértices.", 1);
            double area = signedArea(loop.points);
            // Todos os vértices colineares: área nula. Caso contrário, autointerseção é verificada
            // antes da área (um contorno cruzado "em gravata" também tem área com sinal nula).
            bool collinear = true;
            const gp_Pnt2d& p0 = loop.points[0];
            const gp_Pnt2d& p1 = *std::max_element(loop.points.begin(), loop.points.end(),
                [&p0](const gp_Pnt2d& a, const gp_Pnt2d& b) { return p0.Distance(a) < p0.Distance(b); });
            for (const gp_Pnt2d& q : loop.points)
                if (std::fabs(cross(p0, p1, q)) / std::max(p0.Distance(p1), kClosureTolerance) > kClosureTolerance)
                    collinear = false;
            if (collinear)
                return fail("Perfil com área nula (vértices colineares).", 1);
            std::string where;
            if (polygonSelfIntersects(loop.points, where))
                return fail("Perfil autointersectante em " + where + ".", 1);
            if (std::fabs(area) < kMinProfileArea)
                return fail("Perfil com área nula.", 1);
            if (area < 0.0)
            {
                std::reverse(loop.points.begin(), loop.points.end()); // anti-horário: normal = plano
                area = -area;
            }
            BRepBuilderAPI_MakePolygon polygon;
            for (const gp_Pnt2d& p : loop.points)
                polygon.Add(plane.toGlobal(p));
            polygon.Close();
            if (!polygon.IsDone())
                return fail("Falha ao construir o contorno (wire).", 1);
            wire = polygon.Wire();
            result.area = area;
        }

        BRepBuilderAPI_MakeFace face(gp_Pln(plane.ax3()), wire, true);
        if (!face.IsDone())
            return fail("Falha ao construir a face plana do perfil.", 1);
        result.face = face.Face();
    }
    catch (const Standard_Failure& e)
    {
        return fail(std::string("Falha do Open CASCADE ao construir o perfil: ") + e.what(), 1);
    }

    if (!BRepCheck_Analyzer(result.face).IsValid())
        return fail("Face do perfil inválida (BRepCheck).", 1);
    GProp_GProps props;
    BRepGProp::SurfaceProperties(result.face, props);
    if (std::fabs(props.Mass() - result.area) > 1e-6 * std::max(1.0, result.area))
        return fail("Área da face difere da área do contorno.", 1);
    result.ok = true;
    return result;
}

} // namespace cad::sketch
