#include "sketch/SketchEntity.h"

#include <algorithm>
#include <cmath>
#include <sstream>

namespace cad::sketch {

namespace {

std::string mm(double v)
{
    std::ostringstream s;
    s.precision(2);
    s << std::fixed << v;
    std::string text = s.str();
    std::replace(text.begin(), text.end(), '.', ',');
    return text + " mm";
}

std::string deg(double v)
{
    std::string text = mm(v);
    return text.substr(0, text.size() - 3) + "°";
}

EntityValidation fail(const char* field, const std::string& message)
{
    return EntityValidation{false, field, message};
}

EntityValidation checkCoordinate(const char* field, const char* label, double v)
{
    if (!std::isfinite(v))
        return fail(field, std::string(label) + ": valor não numérico");
    if (std::fabs(v) > kMaxCoordinate)
        return fail(field, std::string(label) + " fora dos limites (±" + mm(kMaxCoordinate) + ")");
    return {};
}

EntityValidation checkSize(const char* field, const char* label, double v)
{
    if (!std::isfinite(v))
        return fail(field, std::string(label) + ": valor não numérico");
    if (v < kMinSize)
        return fail(field, std::string(label) + " deve ser maior que zero (mínimo " + mm(kMinSize) + ")");
    if (v > kMaxSize)
        return fail(field, std::string(label) + " deve ser no máximo " + mm(kMaxSize));
    return {};
}

template <typename... R>
EntityValidation firstFailure(const R&... results)
{
    for (const EntityValidation& r : {results...})
        if (!r.ok)
            return r;
    return {};
}

constexpr double kPi = 3.14159265358979323846;
constexpr double kDeg = kPi / 180.0;

EntityValidation checkAngle(const char* field, const char* label, double v)
{
    if (!std::isfinite(v))
        return fail(field, std::string(label) + ": valor não numérico");
    if (std::fabs(v) > kMaxAngle)
        return fail(field, std::string(label) + " fora dos limites (±720°)");
    return {};
}

// Referências de um modificador de canto.
EntityValidation checkCornerRefs(const std::string& a, const std::string& b, int corner, const char* what)
{
    if (a.empty() || b.empty())
        return fail("entityA", std::string(what) + " sem as entidades do canto");
    if (a == b && (corner < 0 || corner > 3))
        return fail("corner", std::string(what) + ": canto do retângulo inválido (esperado 0 a 3)");
    if (a != b && corner != -1)
        return fail("corner", std::string(what) + ": o índice de canto só se aplica a retângulos");
    return {};
}

// Ângulo normalizado em [0, 360).
double normalized(double deg)
{
    double a = std::fmod(deg, 360.0);
    if (a < 0.0)
        a += 360.0;
    return a;
}

} // namespace

double SketchArc::sweep() const
{
    const double ccw = normalized(endAngle - startAngle);
    if (ccw == 0.0)
        return 0.0;
    return clockwise ? 360.0 - ccw : ccw;
}

double SketchArc::signedSweepRadians() const
{
    return (clockwise ? -sweep() : sweep()) * kDeg;
}

double SketchArc::length() const
{
    return radius * sweep() * kDeg;
}

gp_Pnt2d SketchArc::pointAt(double t) const
{
    const double a = startAngle * kDeg + t * signedSweepRadians();
    return gp_Pnt2d(cx + radius * std::cos(a), cy + radius * std::sin(a));
}

SketchArc SketchArc::fromCenterStartEnd(const gp_Pnt2d& center, const gp_Pnt2d& start, const gp_Pnt2d& end,
                                        bool clockwise)
{
    SketchArc a;
    a.cx = center.X();
    a.cy = center.Y();
    a.radius = center.Distance(start);
    a.startAngle = normalized(std::atan2(start.Y() - center.Y(), start.X() - center.X()) / kDeg);
    a.endAngle = normalized(std::atan2(end.Y() - center.Y(), end.X() - center.X()) / kDeg);
    a.clockwise = clockwise;
    return a;
}

const char* chamferModeName(ChamferMode mode)
{
    switch (mode)
    {
    case ChamferMode::Equal:         return "equal";
    case ChamferMode::TwoDistances:  return "twoDistances";
    case ChamferMode::DistanceAngle: return "distanceAngle";
    }
    return "";
}

bool isCornerModifier(const SketchGeometry& geometry)
{
    return std::holds_alternative<SketchFillet>(geometry) || std::holds_alternative<SketchChamfer>(geometry);
}

double SketchLine::length() const
{
    return std::hypot(x2 - x1, y2 - y1);
}

std::array<gp_Pnt2d, 4> SketchRectangle::corners() const
{
    return {gp_Pnt2d(x, y), gp_Pnt2d(x + width, y), gp_Pnt2d(x + width, y + height), gp_Pnt2d(x, y + height)};
}

std::array<SketchLine, 4> SketchRectangle::segments() const
{
    const auto c = corners();
    std::array<SketchLine, 4> s;
    for (int i = 0; i < 4; ++i)
    {
        const gp_Pnt2d& a = c[i];
        const gp_Pnt2d& b = c[(i + 1) % 4];
        s[i] = SketchLine{a.X(), a.Y(), b.X(), b.Y()};
    }
    return s;
}

SketchRectangle SketchRectangle::fromCorners(double xa, double ya, double xb, double yb)
{
    return SketchRectangle{std::min(xa, xb), std::min(ya, yb), std::fabs(xb - xa), std::fabs(yb - ya)};
}

EntityType SketchEntity::type() const
{
    switch (geometry.index())
    {
    case 0: return EntityType::Line;
    case 1: return EntityType::Rectangle;
    case 2: return EntityType::Circle;
    case 3: return EntityType::Arc;
    case 4: return EntityType::Fillet;
    default: return EntityType::Chamfer;
    }
}

EntityValidation SketchEntity::validate(const SketchGeometry& geometry)
{
    if (const auto* l = std::get_if<SketchLine>(&geometry))
    {
        const EntityValidation r = firstFailure(checkCoordinate("x1", "X inicial", l->x1),
                                                checkCoordinate("y1", "Y inicial", l->y1),
                                                checkCoordinate("x2", "X final", l->x2),
                                                checkCoordinate("y2", "Y final", l->y2));
        if (!r.ok)
            return r;
        if (l->length() < kMinSize)
            return fail("length", "Linha com comprimento nulo (pontos inicial e final coincidentes)");
        return {};
    }
    if (const auto* r = std::get_if<SketchRectangle>(&geometry))
    {
        const EntityValidation v = firstFailure(checkCoordinate("x", "X do canto", r->x),
                                                checkCoordinate("y", "Y do canto", r->y),
                                                checkSize("width", "Largura", r->width),
                                                checkSize("height", "Altura", r->height));
        if (!v.ok)
            return v;
        if (std::fabs(r->x + r->width) > kMaxCoordinate || std::fabs(r->y + r->height) > kMaxCoordinate)
            return fail("width", "Retângulo ultrapassa os limites do esboço (±" + mm(kMaxCoordinate) + ")");
        return {};
    }
    if (const auto* c = std::get_if<SketchCircle>(&geometry))
        return firstFailure(checkCoordinate("cx", "X do centro", c->cx), checkCoordinate("cy", "Y do centro", c->cy),
                            checkSize("radius", "Raio", c->radius));
    if (const auto* a = std::get_if<SketchArc>(&geometry))
    {
        const EntityValidation v = firstFailure(checkCoordinate("cx", "X do centro", a->cx),
                                                checkCoordinate("cy", "Y do centro", a->cy),
                                                checkSize("radius", "Raio", a->radius),
                                                checkAngle("startAngle", "Ângulo inicial", a->startAngle),
                                                checkAngle("endAngle", "Ângulo final", a->endAngle));
        if (!v.ok)
            return v;
        const double s = a->sweep();
        if (s < kMinArcSweep || s > 360.0 - kMinArcSweep)
            return fail("endAngle", "Varredura angular inválida (" + deg(s)
                                        + "): o arco deve ter entre 0,1° e 359,9° — para a circunferência completa "
                                          "use Círculo");
        if (a->length() < kMinSize)
            return fail("radius", "Arco degenerado (comprimento menor que " + mm(kMinSize) + ")");
        return {};
    }
    if (const auto* f = std::get_if<SketchFillet>(&geometry))
        return firstFailure(checkCornerRefs(f->entityA, f->entityB, f->corner, "Fillet 2D"),
                            checkSize("radius", "Raio do fillet", f->radius));
    const auto& c = std::get<SketchChamfer>(geometry);
    const EntityValidation v = firstFailure(checkCornerRefs(c.entityA, c.entityB, c.corner, "Chamfer 2D"),
                                            checkSize("distance", c.mode == ChamferMode::Equal ? "Distância do chanfro"
                                                                                               : "Distância D1",
                                                      c.distance));
    if (!v.ok)
        return v;
    if (!std::isfinite(c.distance2) || !std::isfinite(c.angle))
        return fail(!std::isfinite(c.distance2) ? "distance2" : "angle", "Chanfro: valor não numérico");
    if (c.mode == ChamferMode::TwoDistances)
        return checkSize("distance2", "Distância D2", c.distance2);
    if (c.mode == ChamferMode::DistanceAngle && (c.angle < 0.1 || c.angle > 179.9))
        return fail("angle", "Ângulo do chanfro deve estar entre 0,1° e 179,9°");
    return {};
}

const char* typeName(EntityType type)
{
    switch (type)
    {
    case EntityType::Line:      return "line";
    case EntityType::Rectangle: return "rectangle";
    case EntityType::Circle:    return "circle";
    case EntityType::Arc:       return "arc";
    case EntityType::Fillet:    return "fillet";
    case EntityType::Chamfer:   return "chamfer";
    }
    return "";
}

std::optional<EntityType> typeFromName(const std::string& name)
{
    if (name == "line") return EntityType::Line;
    if (name == "rectangle") return EntityType::Rectangle;
    if (name == "circle") return EntityType::Circle;
    if (name == "arc") return EntityType::Arc;
    if (name == "fillet") return EntityType::Fillet;
    if (name == "chamfer") return EntityType::Chamfer;
    return std::nullopt;
}

std::vector<SketchDimension> dimensionsOf(const SketchEntity& e)
{
    auto d = [&e](const char* key, const char* label, double value, bool editable = true,
                  DimensionKind kind = DimensionKind::Length) {
        return SketchDimension{e.id, key, label, value, editable, kind};
    };
    if (const auto* l = std::get_if<SketchLine>(&e.geometry))
        return {d("x1", "X inicial", l->x1), d("y1", "Y inicial", l->y1), d("x2", "X final", l->x2),
                d("y2", "Y final", l->y2), d("length", "Comprimento", l->length(), false)};
    if (const auto* r = std::get_if<SketchRectangle>(&e.geometry))
        return {d("x", "X do canto", r->x), d("y", "Y do canto", r->y), d("width", "Largura", r->width),
                d("height", "Altura", r->height)};
    if (const auto* c = std::get_if<SketchCircle>(&e.geometry))
        return {d("cx", "X do centro", c->cx), d("cy", "Y do centro", c->cy), d("radius", "Raio", c->radius),
                d("diameter", "Diâmetro", c->diameter(), false)};
    if (const auto* a = std::get_if<SketchArc>(&e.geometry))
        return {d("cx", "X do centro", a->cx),
                d("cy", "Y do centro", a->cy),
                d("radius", "Raio", a->radius),
                d("startAngle", "Ângulo inicial", a->startAngle, true, DimensionKind::Angle),
                d("endAngle", "Ângulo final", a->endAngle, true, DimensionKind::Angle),
                d("clockwise", "Sentido horário", a->clockwise ? 1.0 : 0.0, true, DimensionKind::Flag),
                d("sweep", "Varredura", a->sweep(), false, DimensionKind::Angle),
                d("length", "Comprimento do arco", a->length(), false)};
    if (const auto* f = std::get_if<SketchFillet>(&e.geometry))
        return {d("radius", "Raio", f->radius)};
    const auto& c = std::get<SketchChamfer>(e.geometry);
    SketchDimension mode = d("mode", "Modo", double(int(c.mode)), true, DimensionKind::Choice);
    mode.choices = {"Distâncias iguais", "Duas distâncias", "Distância e ângulo"};
    return {mode,
            d("distance", c.mode == ChamferMode::Equal ? "Distância" : "Distância D1 (lado de referência)", c.distance),
            d("distance2", "Distância D2 (outro lado)", c.distance2),
            d("angle", "Ângulo (lado de referência → chanfro)", c.angle, true, DimensionKind::Angle),
            d("flip", "Referência no segundo lado", c.flip ? 1.0 : 0.0, true, DimensionKind::Flag)};
}

bool applyDimension(SketchGeometry& geometry, const std::string& key, double value)
{
    if (auto* l = std::get_if<SketchLine>(&geometry))
    {
        double* field = key == "x1" ? &l->x1 : key == "y1" ? &l->y1 : key == "x2" ? &l->x2 : key == "y2" ? &l->y2 : nullptr;
        if (!field)
            return false;
        *field = value;
        return true;
    }
    if (auto* r = std::get_if<SketchRectangle>(&geometry))
    {
        double* field = key == "x" ? &r->x : key == "y" ? &r->y : key == "width" ? &r->width
                        : key == "height" ? &r->height : nullptr;
        if (!field)
            return false;
        *field = value;
        return true;
    }
    if (auto* c = std::get_if<SketchCircle>(&geometry))
    {
        double* field = key == "cx" ? &c->cx : key == "cy" ? &c->cy : key == "radius" ? &c->radius : nullptr;
        if (!field)
            return false;
        *field = value;
        return true;
    }
    if (auto* a = std::get_if<SketchArc>(&geometry))
    {
        if (key == "clockwise")
        {
            a->clockwise = value != 0.0;
            return true;
        }
        double* field = key == "cx" ? &a->cx : key == "cy" ? &a->cy : key == "radius" ? &a->radius
                        : key == "startAngle" ? &a->startAngle : key == "endAngle" ? &a->endAngle : nullptr;
        if (!field)
            return false;
        *field = value;
        return true;
    }
    if (auto* f = std::get_if<SketchFillet>(&geometry))
    {
        if (key != "radius")
            return false;
        f->radius = value;
        return true;
    }
    auto& c = std::get<SketchChamfer>(geometry);
    if (key == "distance")
        c.distance = value;
    else if (key == "distance2")
        c.distance2 = value;
    else if (key == "angle")
        c.angle = value;
    else if (key == "flip")
        c.flip = value != 0.0;
    else if (key == "mode")
    {
        const int m = int(value);
        if (m < 0 || m > 2 || double(m) != value)
            return false;
        c.mode = ChamferMode(m);
    }
    else
        return false;
    return true;
}

} // namespace cad::sketch
