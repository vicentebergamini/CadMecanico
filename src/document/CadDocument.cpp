#include "document/CadDocument.h"

#include "modeling/Features.h"
#include "sketch/SketchProfile.h"

#include <BRep_Builder.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS_Compound.hxx>

#include <algorithm>
#include <cmath>
#include <set>
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

int numberAfter(const std::string& id, const char* prefix)
{
    const size_t p = std::char_traits<char>::length(prefix);
    if (id.size() <= p || id.compare(0, p, prefix) != 0)
        return 0;
    int n = 0;
    for (size_t i = p; i < id.size(); ++i)
    {
        if (id[i] < '0' || id[i] > '9' || n > 100000000)
            return 0;
        n = n * 10 + (id[i] - '0');
    }
    return n;
}

std::string trim(const std::string& s)
{
    const size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos)
        return {};
    const size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
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

int sketchNumber(const std::string& sketchId)
{
    return numberAfter(sketchId, "sk-");
}

int operationNumber(const std::string& operationId)
{
    return numberAfter(operationId, "op-");
}

CadDocument::CadDocument()
    : CadDocument("Sem título")
{
}

CadDocument::CadDocument(std::string name)
    : m_name(std::move(name))
{
}

CadDocument CadDocument::withTestPart(std::string name, const PartParameters& parameters)
{
    CadDocument doc(std::move(name));
    PartParameters p = parameters;
    if (!validate(p).ok)
        p = PartParameters(); // nunca deixa o exemplo sem geometria válida
    doc.m_operations = makeOperations(p, kDefaultBlockId, kDefaultHoleId);
    std::string error;
    rebuild(doc.m_operations, doc.m_sketches, doc.m_shape, error);
    doc.m_nextOperationNumber = 3;
    ++doc.m_revision;
    return doc;
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

ValidationResult CadDocument::validateExtrude(const ExtrudeParameters& p)
{
    if (!std::isfinite(p.depth))
        return fail("depth", "Profundidade: valor não numérico");
    if (p.depth < kMinExtrudeDepth)
        return fail("depth", "A profundidade deve ser maior que zero (mínimo " + mm(kMinExtrudeDepth) + ")");
    if (p.depth > kMaxExtrudeDepth)
        return fail("depth", "A profundidade deve ser no máximo " + mm(kMaxExtrudeDepth));
    if (trim(p.name).size() > kMaxNameLength)
        return fail("name", "Nome longo demais");
    return {};
}

bool CadDocument::fromData(const Data& data, CadDocument& out, std::string& error)
{
    // Esboços: ids "sk-N" únicos e contador coerente (entidades já validadas em Sketch::fromData).
    int maxSketch = 0;
    for (size_t i = 0; i < data.sketches.size(); ++i)
    {
        const int n = sketchNumber(data.sketches[i].id());
        if (n <= 0)
        {
            error = "Identificador de esboço inválido: " + data.sketches[i].id();
            return false;
        }
        for (size_t j = 0; j < i; ++j)
            if (data.sketches[j].id() == data.sketches[i].id())
            {
                error = "Identificador de esboço duplicado: " + data.sketches[i].id();
                return false;
            }
        maxSketch = std::max(maxSketch, n);
    }
    if (data.nextSketchNumber <= maxSketch)
    {
        error = "Contador de esboços inconsistente";
        return false;
    }

    std::vector<Operation> ops;
    if (data.part)
    {
        const ValidationResult v = validate(*data.part);
        if (!v.ok)
        {
            error = v.message;
            return false;
        }
        ops = makeOperations(*data.part, data.blockId, data.holeId);
    }
    for (const ExtrudeData& e : data.extrusions)
    {
        const ValidationResult v = validateExtrude(e.parameters);
        if (!v.ok)
        {
            error = "Extrusão " + e.id + ": " + v.message;
            return false;
        }
        if (trim(e.parameters.name).empty())
        {
            error = "Extrusão " + e.id + ": nome vazio";
            return false;
        }
        const bool sketchExists = std::any_of(data.sketches.begin(), data.sketches.end(),
                                              [&](const cad::sketch::Sketch& s) { return s.id() == e.parameters.sketchId; });
        if (!sketchExists)
        {
            error = "Extrusão " + e.id + ": esboço de origem inexistente (" + e.parameters.sketchId + ")";
            return false;
        }
        ops.push_back(makeExtrusion(e.id, e.parameters));
    }

    // Identificadores de operação válidos e únicos; contador coerente.
    std::set<std::string> ids;
    int maxOp = 0;
    for (const Operation& op : ops)
    {
        if (!isValidIdentifier(op.id))
        {
            error = "Identificador de operação inválido";
            return false;
        }
        if (!ids.insert(op.id).second)
        {
            error = "Identificadores de operação duplicados";
            return false;
        }
        maxOp = std::max(maxOp, operationNumber(op.id));
    }
    int nextOp = data.nextOperationNumber;
    if (nextOp == 0)
        nextOp = maxOp + 1;
    else if (nextOp <= maxOp)
    {
        error = "Contador de operações inconsistente";
        return false;
    }

    TopoDS_Shape shape;
    if (!rebuild(ops, data.sketches, shape, error))
        return false;

    CadDocument doc(data.name);
    doc.m_operations = std::move(ops);
    doc.m_shape = shape;
    doc.m_revision = doc.m_operations.empty() ? 0 : 1;
    doc.m_sketches = data.sketches;
    doc.m_nextSketchNumber = data.nextSketchNumber;
    doc.m_nextOperationNumber = nextOp;
    doc.m_modified = false;
    out = std::move(doc);
    return true;
}

bool CadDocument::fromData(const std::string& name, const std::optional<PartParameters>& part,
                           const std::string& blockId, const std::string& holeId, CadDocument& out,
                           std::string& error, const std::vector<cad::sketch::Sketch>& sketches,
                           int nextSketchNumber)
{
    Data data;
    data.name = name;
    data.part = part;
    data.blockId = blockId;
    data.holeId = holeId;
    data.sketches = sketches;
    data.nextSketchNumber = nextSketchNumber;
    return fromData(data, out, error);
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

    std::vector<cad::sketch::Sketch> sketches = m_sketches;
    bool replaced = false;
    for (cad::sketch::Sketch& existing : sketches)
        if (existing.id() == sketch.id())
        {
            existing = sketch;
            replaced = true;
        }
    if (!replaced)
        sketches.push_back(sketch);

    // Recalcula as extrusões dependentes com o esboço alterado (tudo ou nada).
    const bool hasDependents = !dependentOperations(sketch.id()).empty();
    if (hasDependents)
    {
        std::vector<Operation> ops = m_operations;
        TopoDS_Shape shape;
        std::string rebuildError;
        if (!rebuild(ops, sketches, shape, rebuildError))
        {
            error = "A alteração do esboço invalida a extrusão dependente: " + rebuildError;
            return false;
        }
        m_sketches = std::move(sketches);
        commit(std::move(ops), shape);
    }
    else
    {
        m_sketches = std::move(sketches);
        m_modified = true;
    }
    if (!replaced)
        m_nextSketchNumber = std::max(m_nextSketchNumber, n + 1);
    return true;
}

bool CadDocument::removeSketch(const std::string& id)
{
    std::string error;
    return removeSketch(id, error);
}

bool CadDocument::removeSketch(const std::string& id, std::string& error)
{
    const auto it = std::find_if(m_sketches.begin(), m_sketches.end(),
                                 [&](const cad::sketch::Sketch& s) { return s.id() == id; });
    if (it == m_sketches.end())
    {
        error = "Esboço inexistente: " + id;
        return false;
    }
    const auto deps = dependentOperations(id);
    if (!deps.empty())
    {
        error = "O esboço é usado pela operação " + deps.front() + " e não pode ser removido";
        return false;
    }
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

bool CadDocument::hasPart() const
{
    return std::any_of(m_operations.begin(), m_operations.end(),
                       [](const Operation& op) { return op.type == OperationType::Block; });
}

int CadDocument::solidCount() const
{
    int n = 0;
    for (TopExp_Explorer ex(m_shape, TopAbs_SOLID); ex.More(); ex.Next())
        ++n;
    return n;
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
    if (!hasPart())
        return ValidationResult{false, "", "O documento não possui peça paramétrica"};
    const ValidationResult v = validate(parameters);
    if (!v.ok)
        return v;
    if (parameters == this->parameters())
        return {};

    // Recalcula em uma cópia: o documento só muda se tudo der certo.
    std::vector<Operation> ops = m_operations;
    for (Operation& op : ops)
    {
        if (op.type == OperationType::Block)
            op.parameters = {{"length", parameters.length}, {"width", parameters.width}, {"height", parameters.height}};
        else if (op.type == OperationType::ThroughHole)
            op.parameters = {{"radius", parameters.holeRadius}};
    }
    TopoDS_Shape shape;
    std::string error;
    if (!rebuild(ops, m_sketches, shape, error))
        return ValidationResult{false, "", error};
    commit(std::move(ops), shape);
    return {};
}

std::string CadDocument::defaultExtrusionName() const
{
    int n = 1;
    for (const Operation& op : m_operations)
        if (op.type == OperationType::Extrude)
            ++n;
    return "Extrusão " + std::to_string(n);
}

cad::modeling::FeatureResult CadDocument::previewExtrusion(const ExtrudeParameters& parameters) const
{
    cad::modeling::FeatureResult r;
    const ValidationResult v = validateExtrude(parameters);
    if (!v.ok)
    {
        r.message = v.message;
        return r;
    }
    const cad::sketch::Sketch* sketch = findSketch(parameters.sketchId);
    if (!sketch)
    {
        r.message = "Esboço de origem inexistente: " + parameters.sketchId;
        return r;
    }
    const cad::sketch::ProfileResult profile = cad::sketch::buildProfile(*sketch);
    if (!profile.ok)
    {
        r.message = profile.message;
        return r;
    }
    const gp_Vec direction = gp_Vec(sketch->plane().normal()) * (parameters.reversed ? -parameters.depth : parameters.depth);
    return cad::modeling::extrudeFace(profile.face, direction);
}

ValidationResult CadDocument::addExtrusion(const ExtrudeParameters& parameters, std::string* newId)
{
    ValidationResult v = validateExtrude(parameters);
    if (!v.ok)
        return v;
    if (!findSketch(parameters.sketchId))
        return ValidationResult{false, "sketchId", "Esboço de origem inexistente: " + parameters.sketchId};

    ExtrudeParameters p = parameters;
    if (trim(p.name).empty())
        p.name = defaultExtrusionName();
    const std::string id = "op-" + std::to_string(m_nextOperationNumber);
    std::vector<Operation> ops = m_operations;
    ops.push_back(makeExtrusion(id, p));
    TopoDS_Shape shape;
    std::string error;
    if (!rebuild(ops, m_sketches, shape, error))
        return ValidationResult{false, "profile", error};
    commit(std::move(ops), shape);
    ++m_nextOperationNumber;
    if (newId)
        *newId = id;
    return {};
}

ValidationResult CadDocument::updateExtrusion(const std::string& id, const ExtrudeParameters& parameters)
{
    const ValidationResult v = validateExtrude(parameters);
    if (!v.ok)
        return v;
    if (!findSketch(parameters.sketchId))
        return ValidationResult{false, "sketchId", "Esboço de origem inexistente: " + parameters.sketchId};
    std::vector<Operation> ops = m_operations;
    auto it = std::find_if(ops.begin(), ops.end(), [&](const Operation& op) { return op.id == id; });
    if (it == ops.end() || it->type != OperationType::Extrude)
        return ValidationResult{false, "", "Extrusão inexistente: " + id};
    ExtrudeParameters p = parameters;
    if (trim(p.name).empty())
        p.name = it->name;
    const Operation updated = makeExtrusion(id, p);
    if (updated.name == it->name && updated.sketchId == it->sketchId && updated.reversed == it->reversed
        && updated.parameter("depth") == it->parameter("depth"))
        return {};
    *it = updated;
    TopoDS_Shape shape;
    std::string error;
    if (!rebuild(ops, m_sketches, shape, error))
        return ValidationResult{false, "profile", error};
    commit(std::move(ops), shape);
    return {};
}

ExtrudeParameters CadDocument::extrudeParameters(const std::string& id) const
{
    ExtrudeParameters p;
    if (const Operation* op = findOperation(id); op && op->type == OperationType::Extrude)
    {
        p.name = op->name;
        p.sketchId = op->sketchId;
        p.depth = op->parameter("depth");
        p.reversed = op->reversed;
    }
    return p;
}

std::vector<std::string> CadDocument::dependentOperations(const std::string& sketchId) const
{
    std::vector<std::string> ids;
    for (const Operation& op : m_operations)
        if (op.type == OperationType::Extrude && op.sketchId == sketchId)
            ids.push_back(op.id);
    return ids;
}

void CadDocument::commit(std::vector<Operation> operations, TopoDS_Shape shape)
{
    m_operations = std::move(operations);
    m_shape = shape;
    m_modified = true;
    ++m_revision;
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

Operation CadDocument::makeExtrusion(const std::string& id, const ExtrudeParameters& parameters)
{
    Operation op;
    op.id = id;
    op.type = OperationType::Extrude;
    op.name = trim(parameters.name);
    op.sketchId = parameters.sketchId;
    op.reversed = parameters.reversed;
    op.parameters = {{"depth", parameters.depth}};
    return op;
}

bool CadDocument::rebuild(std::vector<Operation>& operations, const std::vector<cad::sketch::Sketch>& sketches,
                          TopoDS_Shape& shape, std::string& error)
{
    TopoDS_Shape part; // bloco (+ furo)
    std::vector<TopoDS_Shape> solids;
    double length = 0.0, width = 0.0;
    bool ok = true;
    for (Operation& op : operations)
    {
        cad::modeling::FeatureResult r;
        op.result.Nullify();
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
            if (r.ok)
                part = r.shape;
            break;
        case OperationType::ThroughHole:
            // Relação geométrica: furo centrado na face superior do bloco.
            r = cad::modeling::cutThroughHole(part, length / 2.0, width / 2.0, op.parameter("radius"));
            if (r.ok)
                part = r.shape;
            break;
        case OperationType::Extrude:
        {
            const auto it = std::find_if(sketches.begin(), sketches.end(),
                                         [&](const cad::sketch::Sketch& s) { return s.id() == op.sketchId; });
            if (it == sketches.end())
            {
                r.message = "esboço de origem inexistente (" + op.sketchId + ")";
                break;
            }
            const cad::sketch::ProfileResult profile = cad::sketch::buildProfile(*it);
            if (!profile.ok)
            {
                r.message = "perfil do " + it->name() + " inválido: " + profile.message;
                break;
            }
            const double depth = op.parameter("depth");
            const gp_Vec direction = gp_Vec(it->plane().normal()) * (op.reversed ? -depth : depth);
            r = cad::modeling::extrudeFace(profile.face, direction);
            if (r.ok)
                solids.push_back(r.shape);
            break;
        }
        }
        op.status = r.ok ? OperationStatus::Valid : OperationStatus::Invalid;
        op.statusMessage = r.message;
        if (r.ok)
            op.result = r.shape;
        else
        {
            ok = false;
            error = op.name + ": " + r.message;
        }
    }
    if (!ok)
        return false;

    if (!part.IsNull())
        solids.insert(solids.begin(), part);
    if (solids.empty())
        shape.Nullify();
    else if (solids.size() == 1)
        shape = solids.front();
    else
    {
        // Sólidos independentes lado a lado, sem união booleana.
        BRep_Builder builder;
        TopoDS_Compound compound;
        builder.MakeCompound(compound);
        for (const TopoDS_Shape& s : solids)
            builder.Add(compound, s);
        shape = compound;
    }
    return true;
}

} // namespace cad::document
