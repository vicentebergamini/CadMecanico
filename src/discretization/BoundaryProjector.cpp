#include "discretization/BoundaryProjector.h"

#include "discretization/ApdlExporter.h"
#include "discretization/CubeClassifier.h"

#include <BRepAdaptor_Surface.hxx>
#include <BRepBndLib.hxx>
#include <BRepBuilderAPI_MakeVertex.hxx>
#include <BRepTopAdaptor_FClass2d.hxx>
#include <BRepExtrema_DistShapeShape.hxx>
#include <BRep_Tool.hxx>
#include <Bnd_Box.hxx>
#include <ElSLib.hxx>
#include <GeomAPI_ProjectPointOnSurf.hxx>
#include <Geom_Surface.hxx>
#include <Precision.hxx>
#include <Standard_Failure.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Vertex.hxx>
#include <gp_Pln.hxx>
#include <gp_Pnt2d.hxx>

#include <algorithm>
#include <chrono>
#include <memory>
#include <cmath>
#include <numeric>
#include <set>
#include <unordered_map>
#include <utility>
#include <vector>

namespace cad::discretization {

namespace {

using Clock = std::chrono::steady_clock;

// Deslocamentos (parâmetros a, b, c) dos vértices V1..V8 (convenção de Discretization.h).
constexpr int kCorner[8][3] = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}, {0, 0, 1}, {1, 0, 1}, {1, 1, 1}, {0, 1, 1}};
// Vizinhos por face: -X, +X, -Y, +Y, -Z, +Z.
constexpr int kDir[6][3] = {{-1, 0, 0}, {1, 0, 0}, {0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}};
// Arestas do hexaedro (pares de vértices V1..V8, índices 0-based).
constexpr int kEdges[12][2] = {{0, 1}, {3, 2}, {4, 5}, {7, 6}, {0, 3}, {1, 2}, {4, 7}, {5, 6}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};

int hexIndex(int a, int b, int c)
{
    return c * 4 + (b ? (a ? 2 : 3) : (a ? 1 : 0));
}

double seconds(Clock::time_point a, Clock::time_point b)
{
    return std::chrono::duration<double>(b - a).count();
}

} // namespace

HexQuality evaluateHex(const std::array<gp_XYZ, 8>& v)
{
    HexQuality q;
    auto P = [&v](int a, int b, int c) -> const gp_XYZ& { return v[std::size_t(hexIndex(a, b, c))]; };
    auto scaled = [](const gp_XYZ& e1, const gp_XYZ& e2, const gp_XYZ& e3) {
        const double l = e1.Modulus() * e2.Modulus() * e3.Modulus();
        return l > 0.0 ? e1.Dot(e2.Crossed(e3)) / l : -1.0;
    };
    q.minScaledJacobian = 1e300;
    for (int n = 0; n < 8; ++n)
    {
        const int a = kCorner[n][0], b = kCorner[n][1], c = kCorner[n][2];
        const gp_XYZ& o = P(a, b, c);
        const gp_XYZ e1 = (P(1 - a, b, c) - o) * (a ? -1.0 : 1.0);
        const gp_XYZ e2 = (P(a, 1 - b, c) - o) * (b ? -1.0 : 1.0);
        const gp_XYZ e3 = (P(a, b, 1 - c) - o) * (c ? -1.0 : 1.0);
        q.cornerJacobian[std::size_t(n)] = scaled(e1, e2, e3);
        q.minScaledJacobian = std::min(q.minScaledJacobian, q.cornerJacobian[std::size_t(n)]);
    }
    // Derivadas do mapeamento trilinear no ponto paramétrico (s, t, u).
    auto derivatives = [&P](double s, double t, double u, gp_XYZ& ds, gp_XYZ& dt, gp_XYZ& du) {
        ds = (P(1, 0, 0) - P(0, 0, 0)) * ((1 - t) * (1 - u)) + (P(1, 1, 0) - P(0, 1, 0)) * (t * (1 - u))
             + (P(1, 0, 1) - P(0, 0, 1)) * ((1 - t) * u) + (P(1, 1, 1) - P(0, 1, 1)) * (t * u);
        dt = (P(0, 1, 0) - P(0, 0, 0)) * ((1 - s) * (1 - u)) + (P(1, 1, 0) - P(1, 0, 0)) * (s * (1 - u))
             + (P(0, 1, 1) - P(0, 0, 1)) * ((1 - s) * u) + (P(1, 1, 1) - P(1, 0, 1)) * (s * u);
        du = (P(0, 0, 1) - P(0, 0, 0)) * ((1 - s) * (1 - t)) + (P(1, 0, 1) - P(1, 0, 0)) * (s * (1 - t))
             + (P(0, 1, 1) - P(0, 1, 0)) * ((1 - s) * t) + (P(1, 1, 1) - P(1, 1, 0)) * (s * t);
    };
    gp_XYZ ds, dt, du;
    derivatives(0.5, 0.5, 0.5, ds, dt, du);
    q.centerJacobian = scaled(ds, dt, du);
    q.minScaledJacobian = std::min(q.minScaledJacobian, q.centerJacobian);
    const gp_XYZ centerDerivative[3] = {ds, dt, du};
    // Volume: det J é no máximo quadrático em cada parâmetro; Gauss 2 × 2 × 2 é exato.
    const double g = 0.5 / std::sqrt(3.0);
    const double gauss[2] = {0.5 - g, 0.5 + g};
    q.volume = 0.0;
    for (double s : gauss)
        for (double t : gauss)
            for (double u : gauss)
            {
                derivatives(s, t, u, ds, dt, du);
                q.volume += 0.125 * ds.Dot(dt.Crossed(du));
            }
    q.minEdge = 1e300;
    for (const auto& e : kEdges)
        q.minEdge = std::min(q.minEdge, (v[std::size_t(e[1])] - v[std::size_t(e[0])]).Modulus());
    // Nó central: média dos oito vértices (imagem do centro paramétrico); deve ficar estritamente
    // do lado interno de cada face (plano médio da face, normal pelas diagonais).
    gp_XYZ center(0, 0, 0);
    for (const gp_XYZ& p : v)
        center += p;
    center /= 8.0;
    q.centerInside = true;
    for (int axis = 0; axis < 3 && q.centerInside; ++axis)
        for (int side = 0; side <= 1; ++side)
        {
            gp_XYZ quad[4];
            for (int n = 0; n < 4; ++n)
            {
                const int r = (n == 1 || n == 2) ? 1 : 0, w = (n >= 2) ? 1 : 0;
                int abc[3];
                abc[axis] = side;
                abc[(axis + 1) % 3] = r;
                abc[(axis + 2) % 3] = w;
                quad[n] = P(abc[0], abc[1], abc[2]);
            }
            const gp_XYZ fc = (quad[0] + quad[1] + quad[2] + quad[3]) / 4.0;
            gp_XYZ normal = (quad[2] - quad[0]).Crossed(quad[3] - quad[1]);
            const double len = normal.Modulus();
            if (len <= 0.0)
            {
                q.centerInside = false;
                break;
            }
            normal /= len;
            // Orientação externa: no sentido crescente do parâmetro na face 'side' = 1.
            if (normal.Dot(centerDerivative[axis]) * (side ? 1.0 : -1.0) < 0.0)
                normal.Reverse();
            if ((center - fc).Dot(normal) >= -1e-12 * std::max(1.0, q.minEdge))
            {
                q.centerInside = false;
                break;
            }
        }
    return q;
}

struct BoundaryProjector::Impl
{
    struct FaceData
    {
        TopoDS_Face face;
        Bnd_Box box; // caixa exata ampliada por lco (raio de busca)
        bool reversed = false;
        bool plane = false;
        gp_Pln pln;
        gp_XYZ planeNormal; // normal externa (plano)
        occ::handle<Geom_Surface> surface;
        std::shared_ptr<BRepTopAdaptor_FClass2d> inside; // classificador 2D da face (plano)
        std::vector<int> edges, vertices;
    };
    struct EdgeData
    {
        TopoDS_Edge edge;
        std::vector<int> faces;
    };
    struct VertexData
    {
        gp_XYZ point;
        std::vector<int> faces;
    };
    struct Closest
    {
        gp_XYZ point;
        gp_XYZ normal; // normal externa do sólido no ponto
        double distance = 0.0;
    };

    Impl(const TopoDS_Shape& shape, double lco, const ProjectionOptions& o)
        : lco(lco), opts(o), classifier(shape, lco)
    {
        TopTools_IndexedMapOfShape faceMap, edgeMap, vertexMap;
        for (TopExp_Explorer s(shape, TopAbs_SOLID); s.More(); s.Next())
            for (TopExp_Explorer f(s.Current(), TopAbs_FACE); f.More(); f.Next())
            {
                if (faceMap.Contains(f.Current()))
                    continue;
                faceMap.Add(f.Current());
                FaceData d;
                d.face = TopoDS::Face(f.Current());
                d.reversed = d.face.Orientation() == TopAbs_REVERSED;
                BRepBndLib::AddOptimal(d.face, d.box, false, false);
                if (d.box.IsVoid())
                    continue;
                d.box.Enlarge(lco);
                d.surface = BRep_Tool::Surface(d.face);
                const BRepAdaptor_Surface adaptor(d.face, false);
                if (adaptor.GetType() == GeomAbs_Plane)
                {
                    d.plane = true;
                    d.pln = adaptor.Plane();
                    gp_Pnt p;
                    gp_Vec du, dv;
                    adaptor.D1(0.0, 0.0, p, du, dv);
                    d.planeNormal = du.Crossed(dv).XYZ().Normalized();
                    if (d.reversed)
                        d.planeNormal.Reverse();
                    d.inside = std::make_shared<BRepTopAdaptor_FClass2d>(d.face, Precision::Confusion());
                }
                const int index = int(faces.size());
                for (TopExp_Explorer e(d.face, TopAbs_EDGE); e.More(); e.Next())
                {
                    const TopoDS_Edge edge = TopoDS::Edge(e.Current());
                    if (BRep_Tool::Degenerated(edge))
                        continue;
                    int ei = edgeMap.FindIndex(edge);
                    if (ei == 0)
                    {
                        ei = edgeMap.Add(edge);
                        edges.push_back(EdgeData{edge, {}});
                    }
                    auto& ef = edges[std::size_t(ei - 1)].faces;
                    if (std::find(ef.begin(), ef.end(), index) == ef.end())
                    {
                        ef.push_back(index);
                        d.edges.push_back(ei - 1);
                    }
                }
                for (TopExp_Explorer e(d.face, TopAbs_VERTEX); e.More(); e.Next())
                {
                    const TopoDS_Vertex vertex = TopoDS::Vertex(e.Current());
                    int vi = vertexMap.FindIndex(vertex);
                    if (vi == 0)
                    {
                        vi = vertexMap.Add(vertex);
                        vertices.push_back(VertexData{BRep_Tool::Pnt(vertex).XYZ(), {}});
                    }
                    auto& vf = vertices[std::size_t(vi - 1)].faces;
                    if (std::find(vf.begin(), vf.end(), index) == vf.end())
                    {
                        vf.push_back(index);
                        d.vertices.push_back(vi - 1);
                    }
                }
                faces.push_back(std::move(d));
            }
    }

    // Normal externa da face no ponto 'q' (sobre a face).
    bool normalAt(const FaceData& d, const gp_XYZ& q, gp_XYZ& normal) const
    {
        GeomAPI_ProjectPointOnSurf proj(gp_Pnt(q), d.surface);
        if (!proj.IsDone() || proj.NbPoints() < 1)
            return false;
        double u, v;
        proj.LowerDistanceParameters(u, v);
        double u1, u2, v1, v2;
        d.surface->Bounds(u1, u2, v1, v2);
        const double um = std::isfinite(u1) && std::isfinite(u2) ? 0.5 * (u1 + u2) : u;
        const double vm = std::isfinite(v1) && std::isfinite(v2) ? 0.5 * (v1 + v2) : v;
        for (int attempt = 0; attempt < 4; ++attempt)
        {
            // Ponto singular (polo): desloca o parâmetro levemente em direção ao meio do domínio.
            const double f = attempt == 0 ? 0.0 : 1e-6 * std::pow(10.0, attempt);
            gp_Pnt p;
            gp_Vec du, dv;
            d.surface->D1(u + (um - u) * f, v + (vm - v) * f, p, du, dv);
            const gp_Vec n = du.Crossed(dv);
            if (n.Magnitude() > 1e-12)
            {
                normal = n.XYZ().Normalized();
                if (d.reversed)
                    normal.Reverse();
                return true;
            }
        }
        return false;
    }

    // Ponto mais próximo de 'p' sobre a face LIMITADA 'f' e a normal externa nesse ponto.
    bool closestOnFace(int f, const gp_XYZ& p, Closest& out) const
    {
        const FaceData& d = faces[std::size_t(f)];
        try
        {
            if (d.plane)
            {
                // Projeção ortogonal sobre o plano; aceita se o ponto cair dentro da face.
                const gp_XYZ o = d.pln.Location().XYZ();
                const double s = (p - o).Dot(d.planeNormal);
                const gp_XYZ q = p - d.planeNormal * s;
                double u, v;
                ElSLib::Parameters(d.pln, gp_Pnt(q), u, v);
                const TopAbs_State state = d.inside->Perform(gp_Pnt2d(u, v));
                if (state == TopAbs_IN || state == TopAbs_ON)
                {
                    out.point = q;
                    out.normal = d.planeNormal;
                    out.distance = std::fabs(s);
                    return true;
                }
            }
            const TopoDS_Vertex vertex = BRepBuilderAPI_MakeVertex(gp_Pnt(p)).Vertex();
            BRepExtrema_DistShapeShape ext(vertex, d.face, Extrema_ExtFlag_MIN);
            if (!ext.IsDone() || ext.NbSolution() < 1)
                return false;
            out.point = ext.PointOnShape2(1).XYZ();
            out.distance = ext.Value();
            if (d.plane)
            {
                out.normal = d.planeNormal;
                return true;
            }
            return normalAt(d, out.point, out.normal);
        }
        catch (const Standard_Failure&)
        {
            return false;
        }
    }

    bool closestOnEdge(int e, const gp_XYZ& p, gp_XYZ& q, double& distance) const
    {
        try
        {
            const TopoDS_Vertex vertex = BRepBuilderAPI_MakeVertex(gp_Pnt(p)).Vertex();
            BRepExtrema_DistShapeShape ext(vertex, edges[std::size_t(e)].edge, Extrema_ExtFlag_MIN);
            if (!ext.IsDone() || ext.NbSolution() < 1)
                return false;
            q = ext.PointOnShape2(1).XYZ();
            distance = ext.Value();
            return true;
        }
        catch (const Standard_Failure&)
        {
            return false;
        }
    }

    // Distância de 'p' ao contorno do sólido (limitada: >= lco quando nenhuma face está próxima).
    double distanceToBoundary(const gp_XYZ& p) const
    {
        double best = 10.0 * lco;
        Closest c;
        for (int f = 0; f < int(faces.size()); ++f)
            if (!faces[std::size_t(f)].box.IsOut(gp_Pnt(p)) && closestOnFace(f, p, c))
                best = std::min(best, c.distance);
        return best;
    }

    // Face CAD de destino de uma face exposta (centro 'center', normal externa 'normal'): -1 se
    // nenhuma face próxima tem a normal externa no mesmo sentido.
    int targetFace(const gp_XYZ& center, const gp_XYZ& normal) const
    {
        int best = -1;
        double bestDistance = lco * (1.0 + 1e-9);
        Closest c;
        for (int pass = 0; pass < 2; ++pass) // planos primeiro: faces coplanares decidem sem extrema
            for (int f = 0; f < int(faces.size()); ++f)
            {
                const FaceData& d = faces[std::size_t(f)];
                if (d.plane != (pass == 0) || d.box.IsOut(gp_Pnt(center)) || !closestOnFace(f, center, c))
                    continue;
                if (c.normal.Dot(normal) <= 1e-9)
                    continue; // face voltada para o outro lado (lado oposto de furo/cavidade/parede)
                if (c.distance < bestDistance - 1e-12)
                {
                    bestDistance = c.distance;
                    best = f;
                    if (bestDistance <= 1e-9 * lco)
                        return best;
                }
            }
        return best;
    }

    bool run(Discretization& data, std::string& message, const std::function<bool()>& keepGoing);

    double lco;
    ProjectionOptions opts;
    CubeClassifier classifier;
    std::vector<FaceData> faces;
    std::vector<EdgeData> edges;
    std::vector<VertexData> vertices;
};

BoundaryProjector::BoundaryProjector(const TopoDS_Shape& shape, double lco, const ProjectionOptions& options)
    : m(std::make_unique<Impl>(shape, lco, options))
{
}

BoundaryProjector::~BoundaryProjector() = default;

bool BoundaryProjector::run(Discretization& data, std::string& message, const std::function<bool()>& keepGoing)
{
    try
    {
        return m->run(data, message, keepGoing);
    }
    catch (const Standard_Failure& e)
    {
        message = std::string("Falha geométrica na projeção dos nós: ") + e.what();
        return false;
    }
    catch (const std::bad_alloc&)
    {
        message = "Memória insuficiente para a projeção dos nós; aumente lco";
        return false;
    }
}

bool BoundaryProjector::Impl::run(Discretization& data, std::string& message, const std::function<bool()>& keepGoing)
{
    const auto t0 = Clock::now();
    const VoxelGrid& g = data.grid();
    const std::int64_t nv = data.vertexNodeCount();
    const std::int64_t nc = data.cubeCount();
    ProjectionReport rep;
    std::int64_t ticks = 0;
    auto cancelled = [&] { return (++ticks & 0xFF) == 0 && keepGoing && !keepGoing(); };
    auto cancel = [&] {
        message = "Discretização cancelada pelo usuário";
        return false;
    };

    std::vector<gp_XYZ> base(static_cast<std::size_t>(nv)); // posições cartesianas (mm) por ID de vértice
    for (std::int64_t id = 1; id <= nv; ++id)
        base[std::size_t(id - 1)] = data.node(id).position;
    auto cellVertices = [&](std::int64_t cube, std::array<std::int64_t, 8>& ids) {
        int i, j, k;
        g.cellIjk(data.cubeCells()[std::size_t(cube)], i, j, k);
        for (int n = 0; n < 8; ++n)
            ids[std::size_t(n)] = data.vertexId(i + kCorner[n][0], j + kCorner[n][1], k + kCorner[n][2]);
    };

    // 1–2. Faces expostas e face CAD de destino de cada uma.
    struct VertexTargets
    {
        std::vector<int> faces;
        gp_XYZ normalSum; // soma das normais externas das faces expostas que contêm o vértice
    };
    std::unordered_map<std::int64_t, VertexTargets> boundary;
    for (std::int64_t c = 0; c < nc; ++c)
    {
        if (cancelled())
            return cancel();
        int i, j, k;
        g.cellIjk(data.cubeCells()[std::size_t(c)], i, j, k);
        for (int d = 0; d < 6; ++d)
        {
            if (data.cellAccepted(i + kDir[d][0], j + kDir[d][1], k + kDir[d][2]))
                continue;
            ++rep.boundaryFaces;
            const int axis = d / 2, side = d % 2;
            std::int64_t ids[4];
            gp_XYZ center(0, 0, 0);
            for (int n = 0; n < 4; ++n)
            {
                int p[3] = {i, j, k};
                p[axis] += side;
                p[(axis + 1) % 3] += (n == 1 || n == 2) ? 1 : 0;
                p[(axis + 2) % 3] += n >= 2 ? 1 : 0;
                ids[n] = data.vertexId(p[0], p[1], p[2]);
                center += base[std::size_t(ids[n] - 1)];
            }
            center /= 4.0;
            const gp_XYZ normal(kDir[d][0], kDir[d][1], kDir[d][2]);
            const int f = targetFace(center, normal);
            if (f >= 0)
                ++rep.facesWithTarget;
            for (std::int64_t id : ids)
            {
                VertexTargets& t = boundary[id];
                t.normalSum += normal;
                if (f >= 0 && std::find(t.faces.begin(), t.faces.end(), f) == t.faces.end())
                    t.faces.push_back(f);
            }
        }
    }
    std::vector<std::int64_t> boundaryIds;
    boundaryIds.reserve(boundary.size());
    for (const auto& [id, t] : boundary)
        boundaryIds.push_back(id);
    std::sort(boundaryIds.begin(), boundaryIds.end());
    rep.boundaryVertices = std::int64_t(boundaryIds.size());
    std::vector<std::uint8_t> isBoundary(std::size_t(nv), 0);
    for (std::int64_t id : boundaryIds)
        isBoundary[std::size_t(id - 1)] = 1;

    // 3. Destino de cada vértice de contorno pela topologia CAD.
    enum Kind : std::uint8_t
    {
        Fixed = 0,
        Projected = 1,
        Smoothed = 2
    };
    std::vector<gp_XYZ> disp(std::size_t(nv), gp_XYZ(0, 0, 0));
    std::vector<double> alpha(std::size_t(nv), 0.0);
    std::vector<std::uint8_t> kind(std::size_t(nv), Fixed);
    std::vector<double> initialDeviation(std::size_t(nv), 0.0);
    const double cosSmooth = std::cos(opts.smoothAngleDeg * 3.14159265358979323846 / 180.0);
    const double zero = 1e-7 * lco;
    for (std::int64_t id : boundaryIds)
    {
        if (cancelled())
            return cancel();
        const VertexTargets& t = boundary[id];
        const gp_XYZ p0 = base[std::size_t(id - 1)];
        std::vector<int> fs;
        std::vector<Closest> cl;
        for (int f : t.faces)
        {
            Closest c;
            if (closestOnFace(f, p0, c))
            {
                fs.push_back(f);
                cl.push_back(c);
            }
        }
        if (fs.empty())
        {
            ++rep.unresolvedVertices;
            continue;
        }
        // Grupos de faces suaves (normais próximas): união simples.
        std::vector<int> group(fs.size());
        std::iota(group.begin(), group.end(), 0);
        for (std::size_t a = 0; a < fs.size(); ++a)
            for (std::size_t b = a + 1; b < fs.size(); ++b)
                if (cl[a].normal.Dot(cl[b].normal) >= cosSmooth)
                {
                    const int ga = group[a], gb = group[b];
                    for (int& x : group)
                        if (x == gb)
                            x = ga;
                }
        std::vector<int> labels(group);
        std::sort(labels.begin(), labels.end());
        labels.erase(std::unique(labels.begin(), labels.end()), labels.end());
        auto inGroup = [&](int face, int label) {
            for (std::size_t n = 0; n < fs.size(); ++n)
                if (fs[n] == face && group[n] == label)
                    return true;
            return false;
        };
        bool found = false;
        gp_XYZ q;
        double best = 1e300;
        if (labels.size() == 1)
        {
            for (std::size_t n = 0; n < cl.size(); ++n)
                if (cl[n].distance < best)
                {
                    best = cl[n].distance;
                    q = cl[n].point;
                    found = true;
                }
        }
        else if (labels.size() == 2)
        {
            // Aresta viva: arestas CAD compartilhadas por uma face de cada grupo.
            std::set<int> candidates;
            for (std::size_t n = 0; n < fs.size(); ++n)
                if (group[n] == labels[0])
                    for (int e : faces[std::size_t(fs[n])].edges)
                        for (int other : edges[std::size_t(e)].faces)
                            if (inGroup(other, labels[1]))
                                candidates.insert(e);
            for (int e : candidates)
            {
                gp_XYZ pe;
                double de;
                if (closestOnEdge(e, p0, pe, de) && de < best)
                {
                    best = de;
                    q = pe;
                    found = true;
                }
            }
            if (found)
                ++rep.featureEdgeTargets;
        }
        else
        {
            // Canto: vértice CAD que pertence a uma face de cada grupo.
            std::set<int> candidates;
            for (int f : fs)
                for (int v : faces[std::size_t(f)].vertices)
                    candidates.insert(v);
            for (int v : candidates)
            {
                bool all = true;
                for (int label : labels)
                {
                    bool any = false;
                    for (int f : vertices[std::size_t(v)].faces)
                        any = any || inGroup(f, label);
                    all = all && any;
                }
                const double dv = (vertices[std::size_t(v)].point - p0).Modulus();
                if (all && dv < best)
                {
                    best = dv;
                    q = vertices[std::size_t(v)].point;
                    found = true;
                }
            }
            if (found)
                ++rep.cornerTargets;
        }
        if (!found)
        {
            ++rep.unresolvedVertices;
            continue;
        }
        ++rep.targetedVertices;
        const gp_XYZ d = q - p0;
        const double dist = d.Modulus();
        if (dist <= zero)
            continue; // já sobre o contorno
        if (dist > opts.maxDisplacement * lco)
        {
            ++rep.inadmissibleVertices;
            continue;
        }
        // Sentido: nó no material deve ir em direção ao vazio (normais externas das faces
        // expostas) e nó no vazio em direção ao material; o contrário indica o lado errado.
        const PointState state = classifier.classify(p0);
        const double along = d.Dot(t.normalSum);
        if ((state == PointState::In && along < -0.05 * dist * t.normalSum.Modulus())
            || (state == PointState::Out && along > 0.05 * dist * t.normalSum.Modulus()))
        {
            ++rep.unresolvedVertices;
            continue;
        }
        disp[std::size_t(id - 1)] = d;
        alpha[std::size_t(id - 1)] = 1.0;
        kind[std::size_t(id - 1)] = Projected;
        initialDeviation[std::size_t(id - 1)] = dist;
    }

    auto position = [&](std::int64_t id) {
        return base[std::size_t(id - 1)] + disp[std::size_t(id - 1)] * alpha[std::size_t(id - 1)];
    };
    auto moved = [&](std::int64_t id) { return alpha[std::size_t(id - 1)] > 0.0; };
    auto quality = [&](std::int64_t cube) {
        std::array<std::int64_t, 8> ids;
        cellVertices(cube, ids);
        std::array<gp_XYZ, 8> pts;
        for (int n = 0; n < 8; ++n)
            pts[std::size_t(n)] = position(ids[std::size_t(n)]);
        return evaluateHex(pts);
    };
    auto valid = [&](const HexQuality& q) {
        return q.minScaledJacobian >= opts.minScaledJacobian && q.volume > 0.0 && q.minEdge >= opts.minEdge * lco
               && q.centerInside;
    };
    // Células (índices 0-based) que contêm o ponto (i, j, k) da rede.
    auto incidentCells = [&](std::int64_t id, std::vector<std::int64_t>& out) {
        out.clear();
        int i, j, k;
        g.latticeIjk(data.vertexPoints()[std::size_t(id - 1)], i, j, k);
        for (int dk = -1; dk <= 0; ++dk)
            for (int dj = -1; dj <= 0; ++dj)
                for (int di = -1; di <= 0; ++di)
                {
                    const std::int64_t cid = data.cubeId(i + di, j + dj, k + dk);
                    if (cid > 0)
                        out.push_back(cid - 1);
                }
    };
    // Aresta da rede a partir de (i, j, k) na direção 'axis' pertence a algum cubo aceito.
    auto edgeInMaterial = [&](int i, int j, int k, int axis) {
        for (int a = -1; a <= 0; ++a)
            for (int b = -1; b <= 0; ++b)
            {
                const bool acc = axis == 0   ? data.cellAccepted(i, j + a, k + b)
                                 : axis == 1 ? data.cellAccepted(i + a, j, k + b)
                                             : data.cellAccepted(i + a, j + b, k);
                if (acc)
                    return true;
            }
        return false;
    };

    // 4. Suavização dos nós internos vizinhos dos nós projetados (aceita só se melhora o pior
    //    jacobiano escalado das células do nó).
    {
        std::set<std::int64_t> candidates;
        std::unordered_map<std::int64_t, std::vector<std::int64_t>> projectedNeighbors;
        for (std::int64_t id : boundaryIds)
        {
            if (kind[std::size_t(id - 1)] != Projected)
                continue;
            int i, j, k;
            g.latticeIjk(data.vertexPoints()[std::size_t(id - 1)], i, j, k);
            for (int d = 0; d < 6; ++d)
            {
                const int ni = i + kDir[d][0], nj = j + kDir[d][1], nk = k + kDir[d][2];
                const std::int64_t nid = data.vertexId(ni, nj, nk);
                if (nid <= 0 || isBoundary[std::size_t(nid - 1)])
                    continue;
                const int axis = d / 2;
                if (!edgeInMaterial(std::min(i, ni), std::min(j, nj), std::min(k, nk), axis))
                    continue;
                candidates.insert(nid);
                projectedNeighbors[nid].push_back(id);
            }
        }
        std::vector<std::int64_t> cells;
        for (std::int64_t id : candidates)
        {
            if (cancelled())
                return cancel();
            gp_XYZ mean(0, 0, 0);
            const auto& nbs = projectedNeighbors[id];
            for (std::int64_t n : nbs)
                mean += disp[std::size_t(n - 1)];
            mean /= double(nbs.size());
            incidentCells(id, cells);
            double before = 1e300, after = 1e300;
            for (std::int64_t c : cells)
                before = std::min(before, quality(c).minScaledJacobian);
            disp[std::size_t(id - 1)] = mean * opts.smoothing;
            alpha[std::size_t(id - 1)] = 1.0;
            for (std::int64_t c : cells)
                after = std::min(after, quality(c).minScaledJacobian);
            if (after > before + 1e-9 && disp[std::size_t(id - 1)].Modulus() > zero)
                kind[std::size_t(id - 1)] = Smoothed;
            else
            {
                disp[std::size_t(id - 1)] = gp_XYZ(0, 0, 0);
                alpha[std::size_t(id - 1)] = 0.0;
            }
        }
    }

    // Células afetadas: as que contêm algum nó deslocado.
    std::vector<std::int64_t> affected;
    {
        std::vector<std::int64_t> cells;
        for (std::int64_t id = 1; id <= nv; ++id)
            if (kind[std::size_t(id - 1)] != Fixed)
            {
                incidentCells(id, cells);
                affected.insert(affected.end(), cells.begin(), cells.end());
            }
        std::sort(affected.begin(), affected.end());
        affected.erase(std::unique(affected.begin(), affected.end()), affected.end());
    }

    // 5–6. Validação e redução do deslocamento até todas as células e ligações serem válidas.
    auto reduce = [&](std::int64_t id) {
        double& a = alpha[std::size_t(id - 1)];
        if (a <= 0.0)
            return false;
        a *= opts.reduction;
        if (a < opts.minFraction)
            a = 0.0;
        return true;
    };
    std::vector<std::uint8_t> rejected(affected.size(), 0);
    std::unordered_map<std::int64_t, double> depthCache; // profundidade no vazio por ID de nó
    auto cellCenter = [&](std::int64_t cube) {
        std::array<std::int64_t, 8> ids;
        cellVertices(cube, ids);
        gp_XYZ c(0, 0, 0);
        for (std::int64_t id : ids)
            c += position(id);
        return c / 8.0;
    };
    // Profundidade de um ponto no vazio (0 se no material ou sobre o contorno).
    auto depth = [&](const gp_XYZ& p) {
        return classifier.classify(p) == PointState::Out ? distanceToBoundary(p) : 0.0;
    };
    for (;;)
    {
        if (cancelled())
            return cancel();
        bool changed = false;
        for (std::size_t n = 0; n < affected.size(); ++n)
        {
            const std::int64_t cube = affected[n];
            const HexQuality q = quality(cube);
            if (valid(q))
                continue;
            rejected[n] = 1;
            std::array<std::int64_t, 8> ids;
            cellVertices(cube, ids);
            bool any = false;
            const bool cornerOnly = q.volume > 0.0 && q.minEdge >= opts.minEdge * lco && q.centerInside
                                    && q.centerJacobian >= opts.minScaledJacobian;
            if (cornerOnly)
                for (int v = 0; v < 8; ++v)
                    if (q.cornerJacobian[std::size_t(v)] < opts.minScaledJacobian)
                        any = reduce(ids[std::size_t(v)]) || any;
            if (!any)
                for (std::int64_t id : ids)
                    any = reduce(id) || any;
            if (!any)
            {
                message = "Célula inválida sem nós deslocados (cubo " + std::to_string(cube + 1) + ")";
                return false;
            }
            changed = true;
        }
        if (!changed)
        {
            // Ligações (as mesmas do APDL) com extremidade deslocada que passam pelo vazio mais fundo
            // que suas extremidades, e nós coincidentes.
            depthCache.clear();
            auto nodeDepth = [&](std::int64_t key, const gp_XYZ& p) {
                const auto it = depthCache.find(key);
                if (it != depthCache.end())
                    return it->second;
                const double dd = depth(p);
                depthCache.emplace(key, dd);
                return dd;
            };
            std::set<std::pair<std::int64_t, std::int64_t>> seen;
            std::vector<std::int64_t> toReduce;
            auto checkLink = [&](std::int64_t k1, const gp_XYZ& p1, std::int64_t k2, const gp_XYZ& p2,
                                 const std::vector<std::int64_t>& owners) {
                if (!seen.insert({std::min(k1, k2), std::max(k1, k2)}).second)
                    return;
                const gp_XYZ mid = (p1 + p2) * 0.5;
                if (classifier.classify(mid) != PointState::Out)
                    return;
                const double limit = opts.voidDepth * lco + std::max(nodeDepth(k1, p1), nodeDepth(k2, p2));
                if (distanceToBoundary(mid) > limit)
                {
                    ++rep.voidCrossingLinks;
                    toReduce.insert(toReduce.end(), owners.begin(), owners.end());
                }
            };
            for (std::int64_t cube : affected)
            {
                std::array<std::int64_t, 8> ids;
                cellVertices(cube, ids);
                bool cellMoved = false;
                for (std::int64_t id : ids)
                    cellMoved = cellMoved || moved(id);
                if (!cellMoved)
                    continue;
                const std::vector<std::int64_t> owners(ids.begin(), ids.end());
                const std::int64_t centerKey = nv + cube + 1;
                const gp_XYZ center = cellCenter(cube);
                for (const auto& e : kEdges)
                {
                    const std::int64_t a = ids[std::size_t(e[0])], b = ids[std::size_t(e[1])];
                    if (moved(a) || moved(b))
                        checkLink(a, position(a), b, position(b), {a, b});
                }
                for (std::int64_t id : ids)
                    checkLink(centerKey, center, id, position(id), owners);
                int i, j, k;
                g.cellIjk(data.cubeCells()[std::size_t(cube)], i, j, k);
                for (const auto& d : kDir)
                {
                    const std::int64_t other = data.cubeId(i + d[0], j + d[1], k + d[2]);
                    if (other > 0)
                        checkLink(centerKey, center, nv + other, cellCenter(other - 1), owners);
                }
            }
            // Nós coincidentes: vértices e centros das células afetadas (hash espacial).
            {
                const double h = opts.minEdge * lco;
                std::unordered_map<std::int64_t, std::vector<std::pair<std::int64_t, gp_XYZ>>> buckets;
                auto key = [h](const gp_XYZ& p, int dx, int dy, int dz) {
                    const std::int64_t x = std::int64_t(std::floor(p.X() / h)) + dx;
                    const std::int64_t y = std::int64_t(std::floor(p.Y() / h)) + dy;
                    const std::int64_t z = std::int64_t(std::floor(p.Z() / h)) + dz;
                    return (x * 73856093) ^ (y * 19349663) ^ (z * 83492791);
                };
                std::set<std::int64_t> nodes;
                std::unordered_map<std::int64_t, gp_XYZ> where;
                for (std::int64_t cube : affected)
                {
                    std::array<std::int64_t, 8> ids;
                    cellVertices(cube, ids);
                    for (std::int64_t id : ids)
                        where.emplace(id, position(id));
                    where.emplace(nv + cube + 1, cellCenter(cube));
                }
                std::vector<std::int64_t> keys;
                for (const auto& [id, p] : where)
                    keys.push_back(id);
                std::sort(keys.begin(), keys.end());
                for (std::int64_t id : keys)
                {
                    const gp_XYZ& p = where[id];
                    for (int dx = -1; dx <= 1; ++dx)
                        for (int dy = -1; dy <= 1; ++dy)
                            for (int dz = -1; dz <= 1; ++dz)
                            {
                                const auto it = buckets.find(key(p, dx, dy, dz));
                                if (it == buckets.end())
                                    continue;
                                for (const auto& [other, po] : it->second)
                                    if ((po - p).Modulus() < h)
                                    {
                                        ++rep.coincidentNodes;
                                        for (std::int64_t n : {id, other})
                                        {
                                            if (n <= nv)
                                                toReduce.push_back(n);
                                            else
                                            {
                                                std::array<std::int64_t, 8> ids;
                                                cellVertices(n - nv - 1, ids);
                                                toReduce.insert(toReduce.end(), ids.begin(), ids.end());
                                            }
                                        }
                                    }
                            }
                    buckets[key(p, 0, 0, 0)].push_back({id, p});
                }
            }
            std::sort(toReduce.begin(), toReduce.end());
            toReduce.erase(std::unique(toReduce.begin(), toReduce.end()), toReduce.end());
            for (std::int64_t id : toReduce)
                changed = reduce(id) || changed;
            if (!toReduce.empty() && !changed)
            {
                message = "Ligações atravessando vazios ou nós coincidentes sem nós deslocados para corrigir";
                return false;
            }
        }
        if (!changed)
            break;
        ++rep.iterations;
    }

    // 7. Resultado: posições, centros, verificação final e indicadores de qualidade.
    std::vector<gp_XYZ> vpos(static_cast<std::size_t>(nv));
    std::vector<std::uint8_t> movedFlags(std::size_t(nv + nc), 0);
    double sumBefore = 0.0, sumAfter = 0.0;
    std::int64_t deviations = 0;
    for (std::int64_t id = 1; id <= nv; ++id)
    {
        const std::size_t n = std::size_t(id - 1);
        vpos[n] = position(id);
        if (alpha[n] > 0.0)
        {
            movedFlags[n] = 1;
            ++rep.movedNodes;
            rep.maxDisplacement = std::max(rep.maxDisplacement, (disp[n] * alpha[n]).Modulus());
        }
        if (kind[n] == Projected)
        {
            if (alpha[n] >= 1.0)
                ++rep.projectedNodes;
            else if (alpha[n] > 0.0)
                ++rep.partialNodes;
            else
                ++rep.cancelledNodes;
            const double before = initialDeviation[n], after = (1.0 - alpha[n]) * initialDeviation[n];
            rep.deviationBeforeMax = std::max(rep.deviationBeforeMax, before);
            rep.deviationAfterMax = std::max(rep.deviationAfterMax, after);
            sumBefore += before;
            sumAfter += after;
            ++deviations;
        }
        else if (kind[n] == Smoothed && alpha[n] > 0.0)
            ++rep.smoothedNodes;
    }
    if (deviations > 0)
    {
        rep.deviationBeforeMean = sumBefore / double(deviations);
        rep.deviationAfterMean = sumAfter / double(deviations);
    }
    std::vector<gp_XYZ> cpos(static_cast<std::size_t>(nc));
    for (std::int64_t c = 0; c < nc; ++c)
        cpos[std::size_t(c)] = data.node(nv + c + 1).position;
    const double cube3 = lco * lco * lco;
    double volume = 0.0;
    std::int64_t regular = nc;
    rep.minCellVolume = 1e300;
    rep.maxCellVolume = 0.0;
    rep.minScaledJacobian = 1.0;
    for (std::size_t n = 0; n < affected.size(); ++n)
    {
        const std::int64_t cube = affected[n];
        std::array<std::int64_t, 8> ids;
        cellVertices(cube, ids);
        bool cellMoved = false;
        for (std::int64_t id : ids)
            cellMoved = cellMoved || moved(id);
        if (!cellMoved)
            continue; // voltou a ser o cubo cartesiano
        --regular;
        ++rep.adjustedCells;
        const HexQuality q = quality(cube);
        if (!valid(q))
            ++rep.invalidCells;
        rep.minScaledJacobian = std::min(rep.minScaledJacobian, q.minScaledJacobian);
        rep.minCellVolume = std::min(rep.minCellVolume, q.volume);
        rep.maxCellVolume = std::max(rep.maxCellVolume, q.volume);
        volume += q.volume;
        cpos[std::size_t(cube)] = cellCenter(cube);
        movedFlags[std::size_t(nv + cube)] = 1;
        ++rep.movedNodes;
    }
    for (std::uint8_t r : rejected)
        rep.rejectedCells += r ? 1 : 0;
    if (regular > 0)
    {
        volume += double(regular) * cube3;
        rep.minCellVolume = std::min(rep.minCellVolume, cube3);
        rep.maxCellVolume = std::max(rep.maxCellVolume, cube3);
    }
    if (nc == 0)
        rep.minCellVolume = 0.0;
    rep.discretizedVolume = volume;
    if (rep.invalidCells > 0)
    {
        message = std::to_string(rep.invalidCells)
                  + " células permaneceram inválidas após o ajuste; a discretização ajustada foi rejeitada";
        return false;
    }

    // Comprimentos das ligações (mesma sequência da exportação APDL).
    {
        auto nodePos = [&](std::int64_t id) -> const gp_XYZ& {
            return id <= nv ? vpos[std::size_t(id - 1)] : cpos[std::size_t(id - nv - 1)];
        };
        ApdlLinkSequence seq(data);
        LinkElement link;
        rep.minLink = 1e300;
        rep.maxLink = 0.0;
        while (seq.next(link))
        {
            const double len = (nodePos(link.n2) - nodePos(link.n1)).Modulus();
            rep.minLink = std::min(rep.minLink, len);
            rep.maxLink = std::max(rep.maxLink, len);
        }
        if (rep.maxLink == 0.0)
            rep.minLink = 0.0;
    }
    rep.seconds = seconds(t0, Clock::now());
    if (!data.setAdjustedGeometry(std::move(vpos), std::move(cpos), std::move(movedFlags), rep))
    {
        message = "Falha interna ao instalar a geometria ajustada";
        return false;
    }
    return true;
}

} // namespace cad::discretization
