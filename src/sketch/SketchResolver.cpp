#include "sketch/SketchResolver.h"

#include "sketch/SketchProfile.h"

#include <gp_Vec2d.hxx>

#include <algorithm>
#include <cmath>
#include <sstream>

namespace cad::sketch {

namespace {

constexpr double kPi = 3.14159265358979323846;
// Ângulo mínimo/máximo entre os lados de um canto modificável (graus).
constexpr double kMinCornerAngle = 1.0;
constexpr double kMaxCornerAngle = 179.0;

std::string num(double v)
{
    std::ostringstream s;
    s.precision(2);
    s << std::fixed << v;
    std::string t = s.str();
    std::replace(t.begin(), t.end(), '.', ',');
    return t;
}

std::string point(const gp_Pnt2d& p)
{
    return "(" + num(p.X()) + "; " + num(p.Y()) + ")";
}

Curve2d segment(const gp_Pnt2d& a, const gp_Pnt2d& b, const std::string& id, int part)
{
    Curve2d c;
    c.kind = Curve2d::Kind::Segment;
    c.start = a;
    c.end = b;
    c.entityId = id;
    c.part = part;
    return c;
}

// Extremidade de um segmento no canto: 0 = início, 1 = fim.
struct SideEnd
{
    Curve2d* curve = nullptr;
    int end = 0;
    gp_Pnt2d& at() const { return end == 0 ? curve->start : curve->end; }
    const gp_Pnt2d& far() const { return end == 0 ? curve->end : curve->start; }
};

ResolvedSketch failure(ResolvedSketch r, const std::string& id, const std::string& message)
{
    r.ok = false;
    r.failedEntity = id;
    r.message = message;
    return r;
}

Curve2d* findSegment(std::vector<Curve2d>& curves, const std::string& id, int part)
{
    for (Curve2d& c : curves)
        if (c.entityId == id && c.part == part)
            return &c;
    return nullptr;
}

} // namespace

gp_Pnt2d Curve2d::pointAt(double t) const
{
    if (kind == Kind::Segment)
        return gp_Pnt2d(start.X() + t * (end.X() - start.X()), start.Y() + t * (end.Y() - start.Y()));
    const double a = startAngle + t * sweep;
    return gp_Pnt2d(center.X() + radius * std::cos(a), center.Y() + radius * std::sin(a));
}

double Curve2d::length() const
{
    if (kind == Kind::Segment)
        return start.Distance(end);
    return radius * std::fabs(sweep);
}

ResolvedSketch resolveSketch(const Sketch& sketch)
{
    ResolvedSketch r;
    // 1. Curvas das entidades de base.
    for (const SketchEntity& e : sketch.entities())
    {
        if (const auto* l = std::get_if<SketchLine>(&e.geometry))
            r.curves.push_back(segment(gp_Pnt2d(l->x1, l->y1), gp_Pnt2d(l->x2, l->y2), e.id, 0));
        else if (const auto* rect = std::get_if<SketchRectangle>(&e.geometry))
        {
            const auto s = rect->segments();
            for (int i = 0; i < 4; ++i)
                r.curves.push_back(segment(gp_Pnt2d(s[i].x1, s[i].y1), gp_Pnt2d(s[i].x2, s[i].y2), e.id, i));
        }
        else if (const auto* c = std::get_if<SketchCircle>(&e.geometry))
        {
            Curve2d k;
            k.kind = Curve2d::Kind::Circle;
            k.center = gp_Pnt2d(c->cx, c->cy);
            k.radius = c->radius;
            k.sweep = 2.0 * kPi;
            k.start = k.end = gp_Pnt2d(c->cx + c->radius, c->cy);
            k.entityId = e.id;
            r.curves.push_back(k);
        }
        else if (const auto* a = std::get_if<SketchArc>(&e.geometry))
        {
            Curve2d k;
            k.kind = Curve2d::Kind::Arc;
            k.center = gp_Pnt2d(a->cx, a->cy);
            k.radius = a->radius;
            k.startAngle = a->startAngle * kPi / 180.0;
            k.sweep = a->signedSweepRadians();
            k.start = a->startPoint();
            k.end = a->endPoint();
            k.entityId = e.id;
            r.curves.push_back(k);
        }
    }

    // 2. Modificadores de canto, na ordem das entidades.
    for (const SketchEntity& e : sketch.entities())
    {
        if (!isCornerModifier(e.geometry))
            continue;
        const bool fillet = std::holds_alternative<SketchFillet>(e.geometry);
        const char* what = fillet ? "Fillet 2D" : "Chamfer 2D";
        std::string idA, idB;
        int corner = -1;
        double value = 0.0;
        if (fillet)
        {
            const auto& f = std::get<SketchFillet>(e.geometry);
            idA = f.entityA, idB = f.entityB, corner = f.corner, value = f.radius;
        }
        else
        {
            const auto& c = std::get<SketchChamfer>(e.geometry);
            idA = c.entityA, idB = c.entityB, corner = c.corner, value = c.distance;
        }
        const std::string label = std::string(what) + " " + e.id;
        for (const std::string& ref : {idA, idB})
        {
            const SketchEntity* target = sketch.findEntity(ref);
            if (!target)
                return failure(r, e.id, label + ": entidade de referência inexistente (" + ref + ")");
            if (!std::holds_alternative<SketchLine>(target->geometry)
                && !std::holds_alternative<SketchRectangle>(target->geometry))
                return failure(r, e.id, label + ": o canto deve ser formado por segmentos retos (linhas ou retângulo)");
        }

        SideEnd sa, sb;
        gp_Pnt2d p;
        if (idA == idB)
        {
            const SketchEntity* target = sketch.findEntity(idA);
            if (!std::holds_alternative<SketchRectangle>(target->geometry))
                return failure(r, e.id, label + ": duas entidades diferentes são necessárias para linhas");
            p = std::get<SketchRectangle>(target->geometry).corners()[corner];
            sa = SideEnd{findSegment(r.curves, idA, (corner + 3) % 4), 1}; // lado que termina no canto
            sb = SideEnd{findSegment(r.curves, idA, corner), 0};           // lado que começa no canto
            if (sa.at().Distance(p) > kClosureTolerance || sb.at().Distance(p) > kClosureTolerance)
                return failure(r, e.id, label + ": o canto " + point(p) + " já foi modificado por outra entidade");
        }
        else
        {
            if (std::holds_alternative<SketchRectangle>(sketch.findEntity(idA)->geometry)
                || std::holds_alternative<SketchRectangle>(sketch.findEntity(idB)->geometry))
                return failure(r, e.id, label + ": combine duas linhas ou use um canto do retângulo");
            Curve2d* a = findSegment(r.curves, idA, 0);
            Curve2d* b = findSegment(r.curves, idB, 0);
            int matches = 0;
            for (int ea = 0; ea < 2; ++ea)
                for (int eb = 0; eb < 2; ++eb)
                {
                    const SideEnd ca{a, ea}, cb{b, eb};
                    if (ca.at().Distance(cb.at()) <= kClosureTolerance)
                    {
                        sa = ca;
                        sb = cb;
                        ++matches;
                    }
                }
            if (matches == 0)
                return failure(r, e.id, label + ": as linhas " + idA + " e " + idB
                                            + " não estão conectadas (sem extremidade comum)");
            if (matches > 1)
                return failure(r, e.id, label + ": as linhas " + idA + " e " + idB + " coincidem nas duas extremidades");
            p = sa.at();
        }

        const gp_Pnt2d qa = sa.far(), qb = sb.far();
        const double la = p.Distance(qa), lb = p.Distance(qb);
        if (la < kClosureTolerance || lb < kClosureTolerance)
            return failure(r, e.id, label + ": lado sem comprimento disponível no canto " + point(p));
        const gp_Vec2d u(p, qa), v(p, qb);
        const gp_Vec2d un = u / la, vn = v / lb;
        const double theta = std::acos(std::clamp(un.Dot(vn), -1.0, 1.0));
        const double thetaDeg = theta * 180.0 / kPi;
        if (thetaDeg < kMinCornerAngle || thetaDeg > kMaxCornerAngle)
            return failure(r, e.id, label + ": os lados são colineares no ponto " + point(p) + " (não há canto)");

        const double trim = fillet ? value / std::tan(theta / 2.0) : value;
        const double available = std::min(la, lb) - kMinSize;
        if (trim > available)
            return failure(r, e.id, label + ": " + (fillet ? "raio " : "distância ") + num(value)
                                        + " mm grande demais para o canto " + point(p) + " (exige " + num(trim)
                                        + " mm em cada lado; disponível " + num(std::max(0.0, available)) + " mm)");
        const gp_Pnt2d pa = p.Translated(un * trim);
        const gp_Pnt2d pb = p.Translated(vn * trim);
        sa.at() = pa;
        sb.at() = pb;
        if (fillet)
        {
            gp_Vec2d w = un + vn;
            w.Normalize();
            const gp_Pnt2d c = p.Translated(w * (value / std::sin(theta / 2.0)));
            Curve2d k;
            k.kind = Curve2d::Kind::Arc;
            k.center = c;
            k.radius = value;
            k.startAngle = std::atan2(pa.Y() - c.Y(), pa.X() - c.X());
            double s = std::atan2(pb.Y() - c.Y(), pb.X() - c.X()) - k.startAngle;
            while (s > kPi)
                s -= 2.0 * kPi;
            while (s <= -kPi)
                s += 2.0 * kPi;
            k.sweep = s; // arco curto: |s| = π − θ
            k.start = pa;
            k.end = pb;
            k.entityId = e.id;
            r.curves.push_back(k);
        }
        else
        {
            r.curves.push_back(segment(pa, pb, e.id, 0));
        }
    }
    return r;
}

std::vector<SketchCorner> findCorners(const Sketch& sketch)
{
    std::vector<SketchCorner> corners;
    const ResolvedSketch r = resolveSketch(sketch);
    if (!r.ok)
        return corners;
    auto curveOf = [&r](const std::string& id, int part) -> const Curve2d* {
        for (const Curve2d& c : r.curves)
            if (c.entityId == id && c.part == part)
                return &c;
        return nullptr;
    };
    std::vector<const SketchEntity*> lines;
    for (const SketchEntity& e : sketch.entities())
    {
        if (const auto* rect = std::get_if<SketchRectangle>(&e.geometry))
        {
            const auto pts = rect->corners();
            for (int k = 0; k < 4; ++k)
            {
                const Curve2d* in = curveOf(e.id, (k + 3) % 4);
                const Curve2d* out = curveOf(e.id, k);
                if (in && out && in->end.Distance(pts[k]) <= kClosureTolerance
                    && out->start.Distance(pts[k]) <= kClosureTolerance)
                    corners.push_back(SketchCorner{pts[k], e.id, e.id, k});
            }
        }
        else if (std::holds_alternative<SketchLine>(e.geometry))
            lines.push_back(&e);
    }
    for (size_t i = 0; i < lines.size(); ++i)
        for (size_t j = i + 1; j < lines.size(); ++j)
        {
            const Curve2d* a = curveOf(lines[i]->id, 0);
            const Curve2d* b = curveOf(lines[j]->id, 0);
            for (const gp_Pnt2d& pa : {a->start, a->end})
                for (const gp_Pnt2d& pb : {b->start, b->end})
                    if (pa.Distance(pb) <= kClosureTolerance)
                    {
                        // Só cantos reais (lados não colineares).
                        const gp_Pnt2d qa = pa.Distance(a->start) < kClosureTolerance ? a->end : a->start;
                        const gp_Pnt2d qb = pb.Distance(b->start) < kClosureTolerance ? b->end : b->start;
                        gp_Vec2d u(pa, qa), v(pb, qb);
                        if (u.Magnitude() < kClosureTolerance || v.Magnitude() < kClosureTolerance)
                            continue;
                        const double ang = u.Angle(v) * 180.0 / kPi;
                        if (std::fabs(ang) >= kMinCornerAngle && std::fabs(ang) <= kMaxCornerAngle)
                            corners.push_back(SketchCorner{pa, lines[i]->id, lines[j]->id, -1});
                    }
        }
    return corners;
}

std::optional<SketchCorner> nearestCorner(const Sketch& sketch, const gp_Pnt2d& p, double tolerance)
{
    std::optional<SketchCorner> best;
    double bestDistance = tolerance;
    for (const SketchCorner& c : findCorners(sketch))
    {
        const double d = c.point.Distance(p);
        if (d <= bestDistance)
        {
            bestDistance = d;
            best = c;
        }
    }
    return best;
}

} // namespace cad::sketch
