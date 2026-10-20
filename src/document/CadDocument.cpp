#include "document/CadDocument.h"

#include "modeling/Features.h"

#include <algorithm>
#include <cmath>
#include <sstream>

namespace cad::document {

namespace {

constexpr const char* kDefaultBlockId = "op-1";
constexpr const char* kDefaultHoleId = "op-2";

std::string mm(double v)
{
    std::ostringstream s;
    s.precision(2);
    s << std::fixed << v;
    std::string text = s.str();
    std::replace(text.begin(), text.end(), '.', ','); // decimal no padrão brasileiro
    return text + " mm";
}

ValidationResult fail(const char* field, const std::string& message)
{
    return ValidationResult{false, field, message};
}

ValidationResult checkDimension(const char* field, const char* label, double v)
{
    if (!std::isfinite(v))
        return fail(field, std::string(label) + ": valor não numérico");
    if (v < CadDocument::kMinDimension)
        return fail(field, std::string(label) + " deve ser no mínimo " + mm(CadDocument::kMinDimension));
    if (v > CadDocument::kMaxDimension)
        return fail(field, std::string(label) + " deve ser no máximo " + mm(CadDocument::kMaxDimension));
    return {};
}

} // namespace

double Operation::parameter(const std::string& key) const
{
    for (const OperationParameter& p : parameters)
        if (p.key == key)
            return p.value;
    return 0.0;
}

bool isValidIdentifier(const std::string& id)
{
    if (id.empty() || id.size() > 64)
        return false;
    return std::all_of(id.begin(), id.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
    });
}

CadDocument::CadDocument()
    : CadDocument("Peça de teste")
{
}

CadDocument::CadDocument(std::string name, const PartParameters& parameters)
    : m_name(std::move(name))
{
    PartParameters p = parameters;
    if (!validate(p).ok)
        p = PartParameters(); // construtor nunca deixa o documento sem geometria válida
    m_operations = makeOperations(p, kDefaultBlockId, kDefaultHoleId);
    std::string error;
    rebuild(m_operations, m_shape, error);
    ++m_revision;
}

ValidationResult CadDocument::validate(const PartParameters& p)
{
    for (const ValidationResult& r : {checkDimension("length", "Comprimento", p.length),
                                      checkDimension("width", "Largura", p.width),
                                      checkDimension("height", "Altura", p.height)})
        if (!r.ok)
            return r;

    if (!std::isfinite(p.holeRadius))
        return fail("holeRadius", "Raio do furo: valor não numérico");
    if (p.holeRadius < kMinHoleRadius)
        return fail("holeRadius", "Raio do furo deve ser no mínimo " + mm(kMinHoleRadius));

    // O furo é centrado no bloco: precisa caber com parede mínima em X e em Y.
    const double maxRadius = std::min(p.length, p.width) / 2.0 - kMinWall;
    if (p.holeRadius > maxRadius)
    {
        if (maxRadius < kMinHoleRadius)
            return fail("holeRadius", "Bloco estreito demais para um furo (menor lado deve exceder "
                                          + mm(2.0 * (kMinWall + kMinHoleRadius)) + ")");
        return fail("holeRadius", "Raio do furo deve ser no máximo " + mm(maxRadius)
                                      + " (metade do menor lado do bloco menos parede de " + mm(kMinWall) + ")");
    }
    return {};
}

int sketchNumber(const std::string& sketchId)
{
    if (sketchId.size() < 4 || sketchId.compare(0, 3, "sk-") != 0)
        return 0;
    int n = 0;
    for (size_t i = 3; i < sketchId.size(); ++i)
    {
        if (sketchId[i] < '0' || sketchId[i] > '9' || n > 100000000)
            return 0;
        n = n * 10 + (sketchId[i] - '0');
    }
    return n;
}

bool CadDocument::fromData(const std::string& name, const PartParameters& parameters, const std::string& blockId,
                           const std::string& holeId, CadDocument& out, std::string& error,
                           const std::vector<cad::sketch::Sketch>& sketches, int nextSketchNumber)
{
    // Esboços: ids "sk-N" únicos e contador coerente (entidades já validadas em Sketch::fromData).
    int maxSketch = 0;
    for (size_t i = 0; i < sketches.size(); ++i)
    {
        const int n = sketchNumber(sketches[i].id());
        if (n <= 0)
        {
            error = "Identificador de esboço inválido: " + sketches[i].id();
            return false;
        }
        for (size_t j = 0; j < i; ++j)
            if (sketches[j].id() == sketches[i].id())
            {
                error = "Identificador de esboço duplicado: " + sketches[i].id();
                return false;
            }
        maxSketch = std::max(maxSketch, n);
    }
    if (nextSketchNumber <= maxSketch)
    {
        error = "Contador de esboços inconsistente";
        return false;
    }

    if (!isValidIdentifier(blockId) || !isValidIdentifier(holeId))
    {
        error = "Identificador de operação inválido";
        return false;
    }
    if (blockId == holeId)
    {
        error = "Identificadores de operação duplicados";
        return false;
    }
    const ValidationResult v = validate(parameters);
    if (!v.ok)
    {
        error = v.message;
        return false;
    }

    CadDocument doc;
    doc.m_name = name;
    std::vector<Operation> ops = makeOperations(parameters, blockId, holeId);
    TopoDS_Shape shape;
    if (!rebuild(ops, shape, error))
        return false;
    doc.m_operations = std::move(ops);
    doc.m_shape = shape;
    doc.m_sketches = sketches;
    doc.m_nextSketchNumber = nextSketchNumber;
    doc.m_modified = false;
    out = std::move(doc);
    return true;
}

const cad::sketch::Sketch* CadDocument::findSketch(const std::string& id) const
{
    for (const cad::sketch::Sketch& s : m_sketches)
        if (s.id() == id)
            return &s;
    return nullptr;
}

cad::sketch::Sketch CadDocument::createSketch(cad::sketch::PlaneType plane) const
{
    const std::string n = std::to_string(m_nextSketchNumber);
    return cad::sketch::Sketch("sk-" + n, "Esboço " + n, plane);
}

bool CadDocument::commitSketch(const cad::sketch::Sketch& sketch, std::string& error)
{
    const int n = sketchNumber(sketch.id());
    if (n <= 0)
    {
        error = "Identificador de esboço inválido: " + sketch.id();
        return false;
    }
    for (const cad::sketch::SketchEntity& e : sketch.entities())
    {
        const auto v = cad::sketch::SketchEntity::validate(e.geometry);
        if (!v.ok)
        {
            error = "Entidade " + e.id + ": " + v.message;
            return false;
        }
    }
    for (cad::sketch::Sketch& existing : m_sketches)
        if (existing.id() == sketch.id())
        {
            existing = sketch;
            m_modified = true;
            return true;
        }
    m_sketches.push_back(sketch);
    m_nextSketchNumber = std::max(m_nextSketchNumber, n + 1);
    m_modified = true;
    return true;
}

bool CadDocument::removeSketch(const std::string& id)
{
    const auto it = std::find_if(m_sketches.begin(), m_sketches.end(),
                                 [&](const cad::sketch::Sketch& s) { return s.id() == id; });
    if (it == m_sketches.end())
        return false;
    m_sketches.erase(it);
    m_modified = true;
    return true;
}

void CadDocument::setName(const std::string& name)
{
    if (name == m_name)
        return;
    m_name = name;
    m_modified = true;
}

const Operation* CadDocument::findOperation(const std::string& id) const
{
    for (const Operation& op : m_operations)
        if (op.id == id)
            return &op;
    return nullptr;
}

PartParameters CadDocument::parameters() const
{
    PartParameters p;
    for (const Operation& op : m_operations)
    {
        if (op.type == OperationType::Block)
        {
            p.length = op.parameter("length");
            p.width = op.parameter("width");
            p.height = op.parameter("height");
        }
        else if (op.type == OperationType::ThroughHole)
        {
            p.holeRadius = op.parameter("radius");
        }
    }
    return p;
}

ValidationResult CadDocument::setParameters(const PartParameters& parameters)
{
    const ValidationResult v = validate(parameters);
    if (!v.ok)
        return v;
    if (parameters == this->parameters())
        return {};

    // Recalcula em uma cópia: o documento só muda se tudo der certo.
    std::vector<Operation> ops = makeOperations(parameters, m_operations.at(0).id, m_operations.at(1).id);
    TopoDS_Shape shape;
    std::string error;
    if (!rebuild(ops, shape, error))
        return ValidationResult{false, "", error};

    m_operations = std::move(ops);
    m_shape = shape;
    m_modified = true;
    ++m_revision;
    return {};
}

std::vector<Operation> CadDocument::makeOperations(const PartParameters& p, const std::string& blockId,
                                                   const std::string& holeId)
{
    Operation block;
    block.id = blockId;
    block.type = OperationType::Block;
    block.name = "Bloco base";
    block.parameters = {{"length", p.length}, {"width", p.width}, {"height", p.height}};

    Operation hole;
    hole.id = holeId;
    hole.type = OperationType::ThroughHole;
    hole.name = "Furo passante";
    hole.parameters = {{"radius", p.holeRadius}};

    return {block, hole};
}

bool CadDocument::rebuild(std::vector<Operation>& operations, TopoDS_Shape& shape, std::string& error)
{
    TopoDS_Shape current;
    double length = 0.0, width = 0.0;
    bool ok = true;
    for (Operation& op : operations)
    {
        cad::modeling::FeatureResult r;
        if (!ok)
        {
            op.status = OperationStatus::Invalid;
            op.statusMessage = "Operação anterior inválida";
            continue;
        }
        switch (op.type)
        {
        case OperationType::Block:
            length = op.parameter("length");
            width = op.parameter("width");
            r = cad::modeling::makeBlock(length, width, op.parameter("height"));
            break;
        case OperationType::ThroughHole:
            // Relação geométrica: furo centrado na face superior do bloco.
            r = cad::modeling::cutThroughHole(current, length / 2.0, width / 2.0, op.parameter("radius"));
            break;
        }
        op.status = r.ok ? OperationStatus::Valid : OperationStatus::Invalid;
        op.statusMessage = r.message;
        if (r.ok)
        {
            current = r.shape;
        }
        else
        {
            ok = false;
            error = op.name + ": " + r.message;
        }
    }
    if (ok)
        shape = current;
    return ok;
}

} // namespace cad::document
