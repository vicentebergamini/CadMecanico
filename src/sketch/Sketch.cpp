#include "sketch/Sketch.h"

#include "sketch/SketchResolver.h"

#include <algorithm>
#include <set>

namespace cad::sketch {

namespace {

bool isIdentifier(const std::string& id)
{
    if (id.empty() || id.size() > 64)
        return false;
    return std::all_of(id.begin(), id.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
    });
}

} // namespace

int entityNumber(const std::string& entityId)
{
    if (entityId.size() < 3 || entityId.compare(0, 2, "e-") != 0)
        return 0;
    int n = 0;
    for (size_t i = 2; i < entityId.size(); ++i)
    {
        if (entityId[i] < '0' || entityId[i] > '9' || n > 100000000)
            return 0;
        n = n * 10 + (entityId[i] - '0');
    }
    return n;
}

Sketch::Sketch(std::string id, std::string name, PlaneType plane)
    : m_id(std::move(id))
    , m_name(std::move(name))
    , m_plane(plane)
{
}

Sketch::Sketch(std::string id, std::string name, const SketchPlane& plane)
    : m_id(std::move(id))
    , m_name(std::move(name))
    , m_plane(plane)
{
}

const SketchEntity* Sketch::findEntity(const std::string& entityId) const
{
    for (const SketchEntity& e : m_entities)
        if (e.id == entityId)
            return &e;
    return nullptr;
}

EntityValidation Sketch::checkResolvable() const
{
    const ResolvedSketch r = resolveSketch(*this);
    if (!r.ok)
        return EntityValidation{false, "", r.message};
    return {};
}

EntityValidation Sketch::addEntity(const SketchGeometry& geometry, std::string* newId)
{
    const EntityValidation v = SketchEntity::validate(geometry);
    if (!v.ok)
        return v;
    SketchEntity e;
    e.id = "e-" + std::to_string(m_nextEntityNumber);
    e.sketchId = m_id;
    e.geometry = geometry;
    m_entities.push_back(e);
    // Modificadores de canto (e alterações em geral) só entram se o esboço continuar resolúvel.
    const EntityValidation r = checkResolvable();
    if (!r.ok)
    {
        m_entities.pop_back();
        return r;
    }
    ++m_nextEntityNumber;
    if (newId)
        *newId = e.id;
    return {};
}

EntityValidation Sketch::updateEntity(const std::string& entityId, const SketchGeometry& geometry)
{
    auto it = std::find_if(m_entities.begin(), m_entities.end(), [&](const SketchEntity& e) { return e.id == entityId; });
    if (it == m_entities.end())
        return EntityValidation{false, "", "Entidade inexistente: " + entityId};
    if (it->geometry.index() != geometry.index())
        return EntityValidation{false, "", "O tipo da entidade não pode ser alterado"};
    const EntityValidation v = SketchEntity::validate(geometry);
    if (!v.ok)
        return v;
    const SketchGeometry previous = it->geometry;
    it->geometry = geometry;
    // Ex.: encurtar uma linha a ponto de não comportar o fillet do canto.
    const EntityValidation r = checkResolvable();
    if (!r.ok)
    {
        it->geometry = previous;
        return r;
    }
    return {};
}

EntityValidation Sketch::setDimension(const std::string& entityId, const std::string& key, double value)
{
    const SketchEntity* e = findEntity(entityId);
    if (!e)
        return EntityValidation{false, "", "Entidade inexistente: " + entityId};
    SketchGeometry g = e->geometry;
    if (!applyDimension(g, key, value))
        return EntityValidation{false, key, "Dimensão não editável: " + key};
    return updateEntity(entityId, g);
}

bool Sketch::removeEntity(const std::string& entityId)
{
    std::string error;
    return removeEntity(entityId, error);
}

bool Sketch::removeEntity(const std::string& entityId, std::string& error)
{
    const auto it = std::find_if(m_entities.begin(), m_entities.end(), [&](const SketchEntity& e) { return e.id == entityId; });
    if (it == m_entities.end())
    {
        error = "Entidade inexistente: " + entityId;
        return false;
    }
    for (const std::string& user : modifiersUsing(entityId))
    {
        error = "A entidade " + entityId + " é usada pelo modificador de canto " + user + "; exclua-o antes";
        return false;
    }
    m_entities.erase(it);
    return true;
}

std::vector<std::string> Sketch::modifiersUsing(const std::string& entityId) const
{
    std::vector<std::string> ids;
    for (const SketchEntity& e : m_entities)
    {
        if (const auto* f = std::get_if<SketchFillet>(&e.geometry))
        {
            if (f->entityA == entityId || f->entityB == entityId)
                ids.push_back(e.id);
        }
        else if (const auto* c = std::get_if<SketchChamfer>(&e.geometry))
        {
            if (c->entityA == entityId || c->entityB == entityId)
                ids.push_back(e.id);
        }
    }
    return ids;
}

std::vector<SketchDimension> Sketch::dimensions() const
{
    std::vector<SketchDimension> all;
    for (const SketchEntity& e : m_entities)
    {
        const auto d = dimensionsOf(e);
        all.insert(all.end(), d.begin(), d.end());
    }
    return all;
}

bool Sketch::fromData(const std::string& id, const std::string& name, PlaneType plane,
                      const std::vector<SketchEntity>& entities, int nextEntityNumber, Sketch& out, std::string& error)
{
    if (!isIdentifier(id))
    {
        error = "Identificador de esboço inválido";
        return false;
    }
    Sketch s(id, name, plane);
    std::set<std::string> ids;
    int maxNumber = 0;
    for (const SketchEntity& e : entities)
    {
        const int n = entityNumber(e.id);
        if (n <= 0)
        {
            error = "Identificador de entidade inválido: " + e.id;
            return false;
        }
        if (!ids.insert(e.id).second)
        {
            error = "Identificador de entidade duplicado: " + e.id;
            return false;
        }
        const EntityValidation v = SketchEntity::validate(e.geometry);
        if (!v.ok)
        {
            error = "Entidade " + e.id + ": " + v.message;
            return false;
        }
        maxNumber = std::max(maxNumber, n);
        SketchEntity copy = e;
        copy.sketchId = id;
        s.m_entities.push_back(copy);
    }
    if (nextEntityNumber <= maxNumber)
    {
        error = "Contador de entidades inconsistente";
        return false;
    }
    const EntityValidation resolvable = s.checkResolvable();
    if (!resolvable.ok)
    {
        error = resolvable.message;
        return false;
    }
    s.m_nextEntityNumber = nextEntityNumber;
    out = std::move(s);
    return true;
}

} // namespace cad::sketch
