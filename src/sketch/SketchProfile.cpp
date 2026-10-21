#include "sketch/SketchProfile.h"

#include "sketch/SketchResolver.h"

#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakePolygon.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepGProp.hxx>
#include <BRep_Tool.hxx>
#include <TopoDS_Edge.hxx>
#include <GC_MakeArcOfCircle.hxx>
#include <Geom_TrimmedCurve.hxx>
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

// Aresta do grafo do perfil: segmento ou arco resolvido entre dois nós.
struct GraphEdge
{
    const Curve2d* curve = nullptr;
    int a = 0, b = 0; // nós do início e do fim da curva
};

// Contorno: circunferência isolada ou sequência de arestas percorridas (aresta, sentido direto).
struct Loop
{
    const Curve2d* circle = nullptr;
    std::vector<std::pair<int, bool>> edges;
};

// Passo angular máximo da amostragem usada só na validação (autointerseção); a geometria B-Rep
// usa os arcos exatos.
constexpr double kSampleStep = 2.0 * kPi / 180.0;

} // namespace

ProfileResult buildProfile(const Sketch& sketch)
{
    if (sketch.entities().empty())
        return fail("O esboço não contém entidades.");
    for (const SketchEntity& e : sketch.entities())
        if (!e.isValid())
            return fail("Entidade " + e.id + " inválida.");

    // Curvas após aplicar fillets e chanfros (E5.5).
    const ResolvedSketch resolved = resolveSketch(sketch);
    if (!resolved.ok)
        return fail(resolved.message);

    // Grafo de nós (extremidades coincidentes dentro da tolerância).
    std::vector<gp_Pnt2d> nodes;
    std::vector<GraphEdge> edges;
    std::vector<Loop> loops;
    auto nodeOf = [&nodes](const gp_Pnt2d& p) {
        for (size_t i = 0; i < nodes.size(); ++i)
            if (nodes[i].Distance(p) <= kClosureTolerance)
                return int(i);
        nodes.push_back(p);
        return int(nodes.size() - 1);
    };
    for (const Curve2d& c : resolved.curves)
    {
        if (c.kind == Curve2d::Kind::Circle)
        {
            Loop l;
            l.circle = &c;
            loops.push_back(l);
            continue;
        }
        const int a = nodeOf(c.start);
        const int b = nodeOf(c.end);
        if (a == b)
            return fail(std::string(c.kind == Curve2d::Kind::Arc ? "Arco degenerado (" : "Segmento degenerado (")
                        + c.entityId + "): extremidades coincidentes.");
        if (c.kind == Curve2d::Kind::Segment)
            for (const GraphEdge& ed : edges)
                if (ed.curve->kind == Curve2d::Kind::Segment
                    && ((ed.a == a && ed.b == b) || (ed.a == b && ed.b == a)))
                    return fail("Segmentos duplicados entre " + point(nodes[a]) + " e " + point(nodes[b]) + ".");
        edges.push_back(GraphEdge{&c, a, b});
    }

    if (!edges.empty())
    {
        std::vector<std::vector<int>> adjacency(nodes.size());
        for (size_t i = 0; i < edges.size(); ++i)
        {
            adjacency[edges[i].a].push_back(int(i));
            adjacency[edges[i].b].push_back(int(i));
        }
        for (size_t n = 0; n < nodes.size(); ++n)
        {
            if (adjacency[n].size() == 1)
                return fail("Perfil aberto: extremidade livre em " + point(nodes[n]) + ".");
            if (adjacency[n].size() > 2)
                return fail("Contorno ambíguo: " + std::to_string(adjacency[n].size()) + " curvas se encontram em "
                            + point(nodes[n]) + ".");
        }
        // Todos os nós com grau 2: cada componente conexa é um ciclo.
        std::vector<bool> used(edges.size(), false);
        for (size_t start = 0; start < edges.size(); ++start)
        {
            if (used[start])
                continue;
            Loop l;
            int node = edges[start].a;
            int edge = int(start);
            while (!used[edge])
            {
                used[edge] = true;
                const bool forward = edges[edge].a == node;
                l.edges.emplace_back(edge, forward);
                node = forward ? edges[edge].b : edges[edge].a;
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
    bool hasArc = false;
    double perimeter = 0.0;
    try
    {
        TopoDS_Wire wire;
        if (loop.circle)
        {
            const Curve2d& c = *loop.circle;
            const gp_Ax2 axis(plane.toGlobal(c.center), plane.normal(), plane.horizontalAxis());
            wire = BRepBuilderAPI_MakeWire(BRepBuilderAPI_MakeEdge(gp_Circ(axis, c.radius)).Edge()).Wire();
            result.area = kPi * c.radius * c.radius;
        }
        else
        {
            if (loop.edges.size() < 2 || (loop.edges.size() < 3 && std::all_of(loop.edges.begin(), loop.edges.end(),
                [&](const std::pair<int, bool>& e) { return edges[e.first].curve->kind == Curve2d::Kind::Segment; })))
                return fail("Contorno com menos de três vértices.", 1);
            auto entry = [&](const std::pair<int, bool>& e) { return e.second ? edges[e.first].a : edges[e.first].b; };
            auto exit = [&](const std::pair<int, bool>& e) { return e.second ? edges[e.first].b : edges[e.first].a; };

            // Área exata: polígono das cordas + segmentos circulares (r²/2 · (s − sen s)).
            std::vector<gp_Pnt2d> chords;
            std::vector<gp_Pnt2d> samples; // validação de autointerseção
            double area = 0.0;
            for (const auto& e : loop.edges)
            {
                const Curve2d& c = *edges[e.first].curve;
                chords.push_back(nodes[entry(e)]);
                samples.push_back(nodes[entry(e)]);
                perimeter += c.length();
                if (c.kind == Curve2d::Kind::Arc)
                {
                    hasArc = true;
                    const double s = e.second ? c.sweep : -c.sweep;
                    area += 0.5 * c.radius * c.radius * (s - std::sin(s));
                    int n = int(std::ceil(std::fabs(c.sweep) / kSampleStep));
                    n = std::max(1, std::min(n, int(c.length() / (5.0 * kClosureTolerance))));
                    for (int k = 1; k < n; ++k)
                        samples.push_back(c.pointAt(e.second ? double(k) / n : 1.0 - double(k) / n));
                }
            }
            area += signedArea(chords);

            if (!hasArc)
            {
                // Todos os vértices colineares: área nula. Caso contrário, autointerseção é verificada
                // antes da área (um contorno cruzado "em gravata" também tem área com sinal nula).
                bool collinear = true;
                const gp_Pnt2d& p0 = chords[0];
                const gp_Pnt2d& p1 = *std::max_element(chords.begin(), chords.end(),
                    [&p0](const gp_Pnt2d& a, const gp_Pnt2d& b) { return p0.Distance(a) < p0.Distance(b); });
                for (const gp_Pnt2d& q : chords)
                    if (std::fabs(cross(p0, p1, q)) / std::max(p0.Distance(p1), kClosureTolerance) > kClosureTolerance)
                        collinear = false;
                if (collinear)
                    return fail("Perfil com área nula (vértices colineares).", 1);
            }
            std::string where;
            if (samples.size() >= 3 && polygonSelfIntersects(samples, where))
                return fail("Perfil autointersectante em " + where + ".", 1);
            if (std::fabs(area) < kMinProfileArea)
                return fail("Perfil com área nula.", 1);
            if (area < 0.0)
            {
                // Sentido anti-horário: normal da face = normal do plano.
                std::reverse(loop.edges.begin(), loop.edges.end());
                for (auto& e : loop.edges)
                    e.second = !e.second;
                area = -area;
            }

            // Arestas exatas, ligadas pelos nós do grafo (extremidades idênticas entre curvas).
            BRepBuilderAPI_MakeWire mw;
            for (const auto& e : loop.edges)
            {
                const Curve2d& c = *edges[e.first].curve;
                const gp_Pnt from = plane.toGlobal(nodes[entry(e)]);
                const gp_Pnt to = plane.toGlobal(nodes[exit(e)]);
                TopoDS_Edge edge;
                if (c.kind == Curve2d::Kind::Segment)
                    edge = BRepBuilderAPI_MakeEdge(from, to).Edge();
                else
                {
                    GC_MakeArcOfCircle arc(from, plane.toGlobal(c.pointAt(0.5)), to);
                    if (!arc.IsDone())
                        return fail("Falha ao construir o arco " + c.entityId + ".", 1);
                    edge = BRepBuilderAPI_MakeEdge(arc.Value()).Edge();
                }
                mw.Add(edge);
                if (!mw.IsDone())
                    return fail("Falha ao conectar as arestas do contorno (wire).", 1);
            }
            wire = mw.Wire();
            if (!BRep_Tool::IsClosed(wire))
                return fail("Contorno não fechado após a construção das arestas.", 1);
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
    // Arcos passam pelos nós do grafo (tolerância de fechamento): diferença de área limitada.
    const double tolerance = 1e-6 * std::max(1.0, result.area) + (hasArc ? kClosureTolerance * perimeter : 0.0);
    if (std::fabs(props.Mass() - result.area) > tolerance)
        return fail("Área da face difere da área do contorno.", 1);
    result.ok = true;
    return result;
}

} // namespace cad::sketch