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

} // namespace

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
    if (std::holds_alternative<SketchLine>(geometry))
        return EntityType::Line;
    if (std::holds_alternative<SketchRectangle>(geometry))
        return EntityType::Rectangle;
    return EntityType::Circle;
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
    const auto& c = std::get<SketchCircle>(geometry);
    return firstFailure(checkCoordinate("cx", "X do centro", c.cx), checkCoordinate("cy", "Y do centro", c.cy),
                        checkSize("radius", "Raio", c.radius));
}

const char* typeName(EntityType type)
{
    switch (type)
    {
    case EntityType::Line:      return "line";
    case EntityType::Rectangle: return "rectangle";
    case EntityType::Circle:    return "circle";
    }
    return "";
}

std::optional<EntityType> typeFromName(const std::string& name)
{
    if (name == "line") return EntityType::Line;
    if (name == "rectangle") return EntityType::Rectangle;
    if (name == "circle") return EntityType::Circle;
    return std::nullopt;
}

std::vector<SketchDimension> dimensionsOf(const SketchEntity& e)
{
    auto d = [&e](const char* key, const char* label, double value, bool editable = true) {
        return SketchDimension{e.id, key, label, value, editable};
    };
    if (const auto* l = std::get_if<SketchLine>(&e.geometry))
        return {d("x1", "X inicial", l->x1), d("y1", "Y inicial", l->y1), d("x2", "X final", l->x2),
                d("y2", "Y final", l->y2), d("length", "Comprimento", l->length(), false)};
    if (const auto* r = std::get_if<SketchRectangle>(&e.geometry))
        return {d("x", "X do canto", r->x), d("y", "Y do canto", r->y), d("width", "Largura", r->width),
                d("height", "Altura", r->height)};
    const auto& c = std::get<SketchCircle>(e.geometry);
    return {d("cx", "X do centro", c.cx), d("cy", "Y do centro", c.cy), d("radius", "Raio", c.radius),
            d("diameter", "Diâmetro", c.diameter(), false)};
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
    auto& c = std::get<SketchCircle>(geometry);
    double* field = key == "cx" ? &c.cx : key == "cy" ? &c.cy : key == "radius" ? &c.radius : nullptr;
    if (!field)
        return false;
    *field = value;
    return true;
}

} // namespace cad::sketch
