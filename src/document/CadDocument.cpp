#include "document/CadDocument.h"

#include "modeling/Features.h"
#include "modeling/ShapeInspector.h"
#include "sketch/SketchProfile.h"
#include "sketch/SketchShapes.h"

#include <BRepAdaptor_Curve.hxx>
#include <BRepBuilderAPI_MakeVertex.hxx>
#include <BRepExtrema_DistShapeShape.hxx>
#include <BRep_Builder.hxx>
#include <BRep_Tool.hxx>
#include <BRepBndLib.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <gp_Ax3.hxx>
#include <gp_Pln.hxx>
#include <Bnd_Box.hxx>
#include <TopoDS.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS_Compound.hxx>

#include <Standard_Failure.hxx>
#include <TopLoc_Location.hxx>
#include <TopoDS_Vertex.hxx>
#include <gp_Trsf.hxx>

#include <algorithm>
#include <functional>
#include <cmath>
#include <iterator>
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
    rebuild(doc.m_operations, doc.m_sketches, doc.m_shape, error, &doc.m_log);
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
    if (p.mode != ExtrudeMode::NewBody && p.targetId.empty())
        return fail("targetId", "Selecione o corpo de destino da extrusão");
    if (trim(p.name).size() > kMaxNameLength)
        return fail("name", "Nome longo demais");
    const EndCondition end = p.endCondition;
    // Condições que dependem do corpo de destino (extensão calculada a partir dele).
    const bool needsTarget = end == EndCondition::ThroughAll || end == EndCondition::ThroughAllBoth
                             || end == EndCondition::UpToNext;
    if (needsTarget && p.mode == ExtrudeMode::NewBody)
        return fail("endCondition", std::string("\"") + endConditionLabel(end)
                                        + "\" depende do corpo de destino: use adicionar ou remover material");
    if (end == EndCondition::UpToFace && p.limitBodyId.empty())
        return fail("limitFace", "Selecione a face limite de \"Até a face\"");
    const bool prismatic = end == EndCondition::Blind || end == EndCondition::MidPlane
                           || end == EndCondition::ThroughAll || end == EndCondition::ThroughAllBoth;
    if (p.cutOutside && p.mode != ExtrudeMode::Cut)
        return fail("cutOutside", "\"Cortar o lado de fora\" só se aplica à remoção de material (corte)");
    if (p.cutOutside && !prismatic)
        return fail("cutOutside", std::string("\"Cortar o lado de fora\" não se aplica a \"") + endConditionLabel(end) + "\"");
    auto checkDepth = [](double depth, const char* field, const char* label) -> ValidationResult {
        if (!std::isfinite(depth))
            return fail(field, std::string(label) + ": valor não numérico");
        if (depth < kMinExtrudeDepth)
            return fail(field, std::string(label) + " deve ser maior que zero (mínimo " + mm(kMinExtrudeDepth) + ")");
        if (depth > kMaxExtrudeDepth)
            return fail(field, std::string(label) + " deve ser no máximo " + mm(kMaxExtrudeDepth));
        return {};
    };
    if (end == EndCondition::Blind || end == EndCondition::MidPlane)
        if (const ValidationResult v = checkDepth(p.depth, "depth", "A profundidade"); !v.ok)
            return v;
    if (p.direction2)
    {
        if (end == EndCondition::MidPlane || end == EndCondition::ThroughAllBoth)
            return fail("direction2", std::string("A direção 2 não se aplica a \"") + endConditionLabel(end) + "\"");
        if (p.endCondition2 != EndCondition::Blind && p.endCondition2 != EndCondition::ThroughAll)
            return fail("direction2", "Direção 2: use profundidade definida ou atravessar tudo");
        if (p.endCondition2 == EndCondition::ThroughAll && p.mode == ExtrudeMode::NewBody)
            return fail("direction2", "Direção 2: \"Atravessar tudo\" depende do corpo de destino");
        if (p.endCondition2 == EndCondition::Blind)
            if (const ValidationResult v = checkDepth(p.depth2, "depth2", "A profundidade da direção 2"); !v.ok)
                return v;
    }
    return {};
}

const char* endConditionLabel(EndCondition condition)
{
    switch (condition)
    {
    case EndCondition::Blind:          return "Profundidade definida";
    case EndCondition::ThroughAll:     return "Atravessar tudo";
    case EndCondition::ThroughAllBoth: return "Atravessar tudo (dois sentidos)";
    case EndCondition::UpToNext:       return "Até a próxima face";
    case EndCondition::UpToFace:       return "Até a face";
    case EndCondition::MidPlane:       return "Plano médio";
    }
    return "";
}

bool CadDocument::fromData(const Data& data, CadDocument& out, std::string& error, bool allowInvalid)
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
    for (const Operation& f : data.features)
    {
        if (trim(f.name).empty())
        {
            error = "Operação " + f.id + ": nome vazio";
            return false;
        }
        if (trim(f.name).size() > kMaxNameLength)
        {
            error = "Operação " + f.id + ": nome longo demais";
            return false;
        }
        if (f.type == OperationType::Extrude)
        {
            const ExtrudeParameters p = extrudeParametersOf(f);
            const ValidationResult v = validateExtrude(p);
            if (!v.ok)
            {
                error = "Extrusão " + f.id + ": " + v.message;
                return false;
            }
            const bool sketchExists = std::any_of(data.sketches.begin(), data.sketches.end(),
                                                  [&](const cad::sketch::Sketch& s) { return s.id() == p.sketchId; });
            if (!sketchExists)
            {
                error = "Extrusão " + f.id + ": esboço de origem inexistente (" + p.sketchId + ")";
                return false;
            }
            ops.push_back(makeExtrusion(f.id, p));
        }
        else if (f.type == OperationType::Boolean)
        {
            ops.push_back(makeBoolean(f.id, BooleanParameters{f.name, f.booleanKind, f.operandA, f.operandB}));
        }
        else if (f.type == OperationType::Fillet3D || f.type == OperationType::Chamfer3D)
        {
            const char* key = f.type == OperationType::Fillet3D ? "radius" : "distance";
            const double value = f.parameter(key);
            if (!std::isfinite(value) || value <= 0.0 || value > kMaxDimension)
            {
                error = "Operação " + f.id + ": " + (f.type == OperationType::Fillet3D ? "raio" : "distância") + " inválido";
                return false;
            }
            if (f.edges.empty())
            {
                error = "Operação " + f.id + ": nenhuma aresta referenciada";
                return false;
            }
            FinishParameters fp{f.name, f.sourceId, value, f.edges};
            fp.chamferMode = f.chamferMode;
            fp.value2 = f.parameter("distance2");
            fp.angle = f.parameter("angle");
            fp.referenceNormals = f.referenceNormals;
            ops.push_back(makeFinish(f.id, f.type, fp));
        }
        else
        {
            error = "Operação " + f.id + ": tipo não permitido após a peça";
            return false;
        }
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
    std::vector<cad::sketch::Sketch> sketches = data.sketches;
    RebuildLog log;
    if (allowInvalid)
    {
        // Referências estruturais continuam obrigatórias; falhas geométricas ficam registradas
        // nas operações (estado e mensagem) sem descartar nem alterar nada.
        std::vector<Operation> check = ops;
        if (!checkReferences(check, error))
            return false;
        std::string rebuildError;
        rebuild(ops, sketches, shape, rebuildError, &log, true);
    }
    else if (!rebuild(ops, sketches, shape, error, &log))
        return false;

    CadDocument doc(data.name);
    doc.m_operations = std::move(ops);
    doc.m_log = std::move(log);
    doc.m_shape = shape;
    doc.m_revision = doc.m_operations.empty() ? 0 : 1;
    doc.m_sketches = std::move(sketches); // planes dos esboços sobre faces reconstruídos
    doc.m_nextSketchNumber = data.nextSketchNumber;
    doc.m_nextOperationNumber = nextOp;
    if (data.demProperties)
    {
        const cad::dem::PropertyValidation v = cad::dem::validate(*data.demProperties);
        if (!v.ok)
        {
            error = "Propriedades MED inválidas: " + v.message;
            return false;
        }
        doc.m_demProperties = data.demProperties;
    }
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
        RebuildLog log;
        if (!rebuildForCommit(ops, sketches, shape, rebuildError, log))
        {
            error = "A alteração do esboço invalida uma operação dependente: " + rebuildError;
            return false;
        }
        m_log = std::move(log);
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

cad::dem::PropertyValidation CadDocument::setDemProperties(const cad::dem::DemProperties& properties)
{
    const cad::dem::PropertyValidation v = cad::dem::validate(properties);
    if (!v.ok)
        return v;
    if (m_demProperties != properties)
    {
        m_demProperties = properties;
        m_modified = true;
    }
    return v;
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
    std::vector<cad::sketch::Sketch> sketches = m_sketches;
    RebuildLog log;
    if (!rebuildForCommit(ops, sketches, shape, error, log))
        return ValidationResult{false, "", error};
    m_log = std::move(log);
    commit(std::move(ops), std::move(sketches), shape);
    return {};
}

const char* extrudeModeLabel(ExtrudeMode mode)
{
    switch (mode)
    {
    case ExtrudeMode::NewBody: return "Novo corpo";
    case ExtrudeMode::Boss:    return "Saliência";
    case ExtrudeMode::Cut:     return "Corte";
    }
    return "";
}

std::string CadDocument::defaultExtrusionName(ExtrudeMode mode) const
{
    // Novo corpo mantém o nome da E5.3 ("Extrusão N"); saliência e corte numeram separadamente.
    int n = 1;
    for (const Operation& op : m_operations)
        if (op.type == OperationType::Extrude && op.extrudeMode == mode)
            ++n;
    switch (mode)
    {
    case ExtrudeMode::NewBody: break;
    case ExtrudeMode::Boss:    return "Extrusão de Saliência " + std::to_string(n);
    case ExtrudeMode::Cut:     return "Extrusão de Corte " + std::to_string(n);
    }
    return "Extrusão " + std::to_string(n);
}

bool CadDocument::extrusionInput(const ExtrudeParameters& p, const std::string& editingId, ExtrusionInput& input,
                                 std::string& error, bool checkConsumed) const
{
    input = ExtrusionInput();
    input.opId = editingId.empty() ? "op-" + std::to_string(m_nextOperationNumber) : editingId;
    if (p.mode != ExtrudeMode::NewBody)
    {
        const Operation* t = findOperation(p.targetId);
        if (!t || !producesBody(t->type))
        {
            error = "Corpo de destino inexistente: " + p.targetId;
            return false;
        }
        if (checkConsumed && !t->consumedBy.empty() && t->consumedBy != editingId)
        {
            error = "O corpo " + t->id + " já foi usado por " + t->consumedBy;
            return false;
        }
        input.target = t->result;
        input.targetNames = &t->faceNames;
    }
    if (p.endCondition == EndCondition::UpToFace)
    {
        input.limitFace = resolveFaceReference(p.limitBodyId, p.limitFace, &error);
        if (input.limitFace.IsNull())
            return false;
    }
    return true;
}

cad::modeling::FeatureResult CadDocument::previewExtrusion(const ExtrudeParameters& parameters,
                                                           const std::string& editingId) const
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
    ExtrusionInput input;
    if (!extrusionInput(parameters, editingId, input, r.message))
        return r;
    return computeExtrusion(parameters, *sketch, input, nullptr);
}

cad::modeling::FeatureResult CadDocument::extrusionTool(const ExtrudeParameters& parameters) const
{
    cad::modeling::FeatureResult r;
    const cad::sketch::Sketch* sketch = findSketch(parameters.sketchId);
    if (!sketch)
    {
        r.message = "Esboço de origem inexistente";
        return r;
    }
    ExtrusionInput input;
    // Somente a ferramenta (pré-visualização, inclusive na edição): o destino pode estar consumido.
    if (!extrusionInput(parameters, std::string(), input, r.message, false))
        return r;
    // Mesma ferramenta usada na operação (extensões e região externa dependem do corpo e da face).
    TopoDS_Shape tool;
    const cad::modeling::FeatureResult full = computeExtrusion(parameters, *sketch, input, &tool);
    r.ok = !tool.IsNull();
    r.shape = tool;
    if (!r.ok)
        r.message = full.message.empty() ? "Ferramenta da extrusão indisponível" : full.message;
    return r;
}

namespace {

// Rótulo de cada aresta do perfil: entidade do esboço de onde ela vem ("e-1", "e-2:3" = lado 3
// do retângulo e-2). Comparação geométrica com as formas resolvidas das entidades.
std::function<std::string(const TopoDS_Shape&)> profileEdgeLabels(const cad::sketch::Sketch& sketch)
{
    const auto shapes = cad::sketch::buildResolvedShapes(sketch);
    return [shapes](const TopoDS_Shape& edge) -> std::string {
        if (edge.IsNull() || edge.ShapeType() != TopAbs_EDGE)
            return {};
        try
        {
            BRepAdaptor_Curve c(TopoDS::Edge(edge));
            const TopoDS_Vertex mid =
                BRepBuilderAPI_MakeVertex(c.Value(0.5 * (c.FirstParameter() + c.LastParameter()))).Vertex();
            for (const auto& [id, shape] : shapes)
            {
                if (shape.IsNull())
                    continue;
                int count = 0;
                for (TopExp_Explorer ex(shape, TopAbs_EDGE); ex.More(); ex.Next())
                    ++count;
                int k = 0;
                for (TopExp_Explorer ex(shape, TopAbs_EDGE); ex.More(); ex.Next(), ++k)
                {
                    BRepExtrema_DistShapeShape d(mid, ex.Current());
                    if (d.IsDone() && d.Value() < 1e-5)
                        return count > 1 ? id + ":" + std::to_string(k) : id;
                }
            }
        }
        catch (const Standard_Failure&)
        {
        }
        return {};
    };
}

// Maior projeção dos cantos da caixa sobre 'd' a partir de 'o' (−∞ para caixa vazia).
double reach(const Bnd_Box& box, const gp_Pnt& o, const gp_Dir& d)
{
    if (box.IsVoid())
        return -1e300;
    double x0, y0, z0, x1, y1, z1;
    box.Get(x0, y0, z0, x1, y1, z1);
    double best = -1e300;
    for (double x : {x0, x1})
        for (double y : {y0, y1})
            for (double z : {z0, z1})
                best = std::max(best, gp_Vec(o, gp_Pnt(x, y, z)).Dot(gp_Vec(d)));
    return best;
}

// Menor e maior distância (ao longo de 'd', a partir de 'o') dos vértices de um sólido.
std::pair<double, double> extent(const TopoDS_Shape& s, const gp_Pnt& o, const gp_Dir& d)
{
    double lo = 1e300, hi = -1e300;
    for (TopExp_Explorer ex(s, TopAbs_VERTEX); ex.More(); ex.Next())
    {
        const double t = gp_Vec(o, BRep_Tool::Pnt(TopoDS::Vertex(ex.Current()))).Dot(gp_Vec(d));
        lo = std::min(lo, t);
        hi = std::max(hi, t);
    }
    return {lo, hi};
}

TopoDS_Shape compoundOf(const std::vector<TopoDS_Shape>& solids)
{
    if (solids.size() == 1)
        return solids.front();
    BRep_Builder builder;
    TopoDS_Compound compound;
    builder.MakeCompound(compound);
    for (const TopoDS_Shape& s : solids)
        builder.Add(compound, s);
    return compound;
}

} // namespace

cad::modeling::FeatureResult CadDocument::computeExtrusion(const ExtrudeParameters& p, const cad::sketch::Sketch& sketch,
                                                           const ExtrusionInput& input, TopoDS_Shape* tool,
                                                           cad::modeling::FaceNames* names)
{
    using cad::modeling::FaceNames;
    using cad::modeling::FeatureResult;
    FeatureResult r;
    const cad::sketch::ProfileResult profile = cad::sketch::buildProfile(sketch);
    if (!profile.ok)
    {
        r.message = "perfil do " + sketch.name() + " inválido: " + profile.message;
        return r;
    }
    const TopoDS_Shape& target = input.target;
    if (p.mode != ExtrudeMode::NewBody && target.IsNull())
    {
        r.message = "corpo de destino sem geometria (" + p.targetId + ")";
        return r;
    }
    const std::string& op = input.opId;
    const bool cut = p.mode == ExtrudeMode::Cut;
    // Sentido: normal do plano local do esboço (no corte, para dentro da face); invertível.
    gp_Dir dir = sketch.plane().normal();
    if (cut)
        dir.Reverse();
    if (p.reversed)
        dir.Reverse();
    const gp_Pnt o = sketch.plane().origin();
    Bnd_Box targetBox;
    if (!target.IsNull())
        BRepBndLib::Add(target, targetBox);
    Bnd_Box limitBox;
    if (!input.limitFace.IsNull())
        BRepBndLib::Add(input.limitFace, limitBox);
    Bnd_Box scaleBox = targetBox;
    if (!limitBox.IsVoid())
        scaleBox.Add(limitBox);
    BRepBndLib::Add(profile.face, scaleBox);
    const double diagonal = std::sqrt(scaleBox.SquareExtent());
    const double margin = std::max(1.0, 0.01 * diagonal);
    const double tol = 1e-6 * std::max(1.0, diagonal);
    const char* sense = cut ? "do corte" : "da extrusão";

    // 1. Extensões ao longo de 'dir' (s0 < s1), a partir do plano do esboço.
    struct Segment
    {
        double s0, s1;
        bool second; // direção 2
    };
    std::vector<Segment> segments;
    const double farPos = reach(targetBox, o, dir);
    const double farNeg = reach(targetBox, o, dir.Reversed());
    switch (p.endCondition)
    {
    case EndCondition::Blind:
        segments.push_back({0.0, p.depth, false});
        break;
    case EndCondition::MidPlane:
        segments.push_back({-0.5 * p.depth, 0.5 * p.depth, false});
        break;
    case EndCondition::ThroughAll:
        if (farPos <= tol)
        {
            r.message = std::string("o corpo de destino não está no sentido ") + sense + " (inverta a direção)";
            return r;
        }
        segments.push_back({0.0, farPos + margin, false});
        break;
    case EndCondition::UpToNext:
        if (farPos <= tol)
        {
            r.message = std::string("nenhuma face do corpo de destino à frente no sentido ") + sense
                        + "; inverta a direção ou use profundidade definida";
            return r;
        }
        segments.push_back({0.0, farPos + margin, false});
        break;
    case EndCondition::ThroughAllBoth:
        if (farPos + farNeg <= tol)
        {
            r.message = "o corpo de destino não está em nenhum dos sentidos da extrusão";
            return r;
        }
        segments.push_back({-(std::max(farNeg, 0.0) + margin), std::max(farPos, 0.0) + margin, false});
        break;
    case EndCondition::UpToFace:
    {
        // Extensão até onde o plano da face (estendido) corta as retas que partem dos cantos do
        // perfil no sentido da extrusão.
        const auto plane = input.limitFace.IsNull() ? std::nullopt : cad::modeling::planarFacePlane(input.limitFace);
        if (!plane)
        {
            r.message = "a face limite precisa ser plana";
            return r;
        }
        const gp_Vec n(plane->Axis().Direction());
        const double cosine = gp_Vec(dir).Dot(n);
        if (std::fabs(cosine) < 1e-9)
        {
            r.message = std::string("a face limite é paralela ao sentido ") + sense;
            return r;
        }
        Bnd_Box profileBox;
        BRepBndLib::Add(profile.face, profileBox);
        double x0, y0, z0, x1, y1, z1;
        profileBox.Get(x0, y0, z0, x1, y1, z1);
        double far = -1e300;
        for (double x : {x0, x1})
            for (double y : {y0, y1})
                for (double z : {z0, z1})
                {
                    // Distância ao longo de 'dir' a partir do plano do esboço até o plano limite.
                    const gp_Pnt corner(x, y, z);
                    const double start = gp_Vec(o, corner).Dot(gp_Vec(dir));
                    far = std::max(far, start + gp_Vec(corner, plane->Location()).Dot(n) / cosine);
                }
        if (far <= tol)
        {
            r.message = std::string("a face limite não está no sentido ") + sense + " (inverta o sentido ou escolha outra face)";
            return r;
        }
        segments.push_back({0.0, far + margin, false});
        break;
    }
    }
    if (p.direction2)
    {
        if (p.endCondition2 == EndCondition::ThroughAll)
        {
            if (farNeg <= tol)
            {
                r.message = "direção 2: o corpo de destino não está no sentido oposto";
                return r;
            }
            segments.push_back({-(farNeg + margin), 0.0, true});
        }
        else
            segments.push_back({-p.depth2, 0.0, true});
    }

    // 2. Prisma de cada extensão, com os nomes das faces (base/topo/laterais por entidade).
    const auto labels = profileEdgeLabels(sketch);
    auto buildPrism = [&](const Segment& s, FaceNames& n) -> FeatureResult {
        TopoDS_Face base = profile.face;
        if (std::fabs(s.s0) > 1e-12)
        {
            gp_Trsf t;
            t.SetTranslation(gp_Vec(dir) * s.s0);
            base = TopoDS::Face(profile.face.Moved(TopLoc_Location(t)));
        }
        FeatureResult pr = cad::modeling::extrudeFace(base, gp_Vec(dir) * (s.s1 - s.s0));
        if (!pr.ok)
            return pr;
        n.add(pr.firstFace, op + (s.second ? ":topo2" : ":base"));
        n.add(pr.lastFace, op + (s.second ? ":base2" : ":topo"));
        cad::modeling::nameGenerated(n, pr, [&](const TopoDS_Shape& e) {
            // Aresta deslocada: mesma TShape da aresta original do perfil.
            TopoDS_Shape original = e;
            for (TopExp_Explorer ex(profile.face, TopAbs_EDGE); ex.More(); ex.Next())
                if (ex.Current().TShape() == e.TShape())
                    original = ex.Current();
            const std::string l = labels(original);
            return op + (s.second ? ":lado2:" : ":lado:") + (l.empty() ? std::string("?") : l);
        });
        return pr;
    };
    FaceNames toolNames;
    FeatureResult toolResult = buildPrism(segments[0], toolNames);
    if (!toolResult.ok)
        return toolResult;
    if (segments.size() > 1)
    {
        FaceNames secondNames;
        const FeatureResult second = buildPrism(segments[1], secondNames);
        if (!second.ok)
            return second;
        FeatureResult both = cad::modeling::booleanOperation(BooleanKind::Fuse, toolResult.shape, second.shape);
        if (!both.ok)
        {
            r.message = "falha ao unir as duas direções: " + both.message;
            return r;
        }
        toolNames = cad::modeling::propagateNames({&toolNames, &secondNames}, both);
        toolResult = both;
    }

    // 3. Condições que recortam a extensão longa: até a face e até a próxima.
    if (p.endCondition == EndCondition::UpToFace)
    {
        const auto plane = cad::modeling::planarFacePlane(input.limitFace);
        if (!plane)
        {
            r.message = "a face limite precisa ser plana";
            return r;
        }
        const gp_Vec n(plane->Axis().Direction());
        // Lado "além" da face: o oposto ao lado em que o perfil começa.
        Bnd_Box profileBox;
        BRepBndLib::Add(profile.face, profileBox);
        double x0, y0, z0, x1, y1, z1;
        profileBox.Get(x0, y0, z0, x1, y1, z1);
        const gp_Pnt center(0.5 * (x0 + x1), 0.5 * (y0 + y1), 0.5 * (z0 + z1));
        const double side = gp_Vec(plane->Location(), center).Dot(n);
        if (std::fabs(side) <= tol)
        {
            r.message = "o perfil está sobre o plano da face limite";
            return r;
        }
        // Semiespaço finito além do plano da face (laje grande), removido da extensão longa.
        const double big = 4.0 * (diagonal + segments[0].s1) + 10.0;
        // Plano centrado no ponto mais próximo do perfil (a laje cobre toda a extensão).
        gp_Pln limitPlane = *plane;
        limitPlane.SetLocation(center.Translated(-n * side));
        const TopoDS_Face planeFace = BRepBuilderAPI_MakeFace(limitPlane, -big, big, -big, big).Face();
        const FeatureResult beyond = cad::modeling::extrudeFace(planeFace, n * (side > 0.0 ? -big : big));
        if (!beyond.ok)
        {
            r.message = "falha ao construir o limite da face: " + beyond.message;
            return r;
        }
        FaceNames beyondNames;
        beyondNames.add(beyond.firstFace, op + ":limite");
        const FeatureResult trimmed = cad::modeling::booleanOperation(BooleanKind::Cut, toolResult.shape, beyond.shape);
        if (!trimmed.ok)
        {
            r.message = "falha ao limitar a extrusão pela face: " + trimmed.message;
            return r;
        }
        toolNames = cad::modeling::propagateNames({&toolNames, &beyondNames}, trimmed);
        toolResult = trimmed;
    }
    else if (p.endCondition == EndCondition::UpToNext)
    {
        if (cut)
        {
            // Corte: a primeira região de material encontrada a partir do plano do esboço.
            const FeatureResult common = cad::modeling::booleanOperation(BooleanKind::Common, target, toolResult.shape);
            if (!common.ok)
            {
                r.message = "não há material no sentido do corte (até a próxima face)";
                return r;
            }
            std::vector<std::pair<double, TopoDS_Shape>> parts;
            for (TopExp_Explorer ex(common.shape, TopAbs_SOLID); ex.More(); ex.Next())
                parts.emplace_back(extent(ex.Current(), o, dir).first, ex.Current());
            const double first = std::min_element(parts.begin(), parts.end(), [](const auto& a, const auto& b) {
                                     return a.first < b.first;
                                 })->first;
            std::vector<TopoDS_Shape> chosen;
            for (const auto& [start, solid] : parts)
                if (start <= first + 1e3 * tol)
                    chosen.push_back(solid);
            const FaceNames commonNames = cad::modeling::propagateNames({input.targetNames, &toolNames}, common);
            toolResult.shape = compoundOf(chosen);
            toolNames = commonNames;
        }
        else
        {
            // Saliência/novo corpo: do plano do esboço até encontrar o corpo de destino.
            const FeatureResult ahead = cad::modeling::booleanOperation(BooleanKind::Cut, toolResult.shape, target);
            if (!ahead.ok)
            {
                r.message = "a extrusão começa dentro do corpo de destino (até a próxima face)";
                return r;
            }
            TopoDS_Shape start;
            double startHi = 0.0;
            for (TopExp_Explorer ex(ahead.shape, TopAbs_SOLID); ex.More(); ex.Next())
            {
                const auto [lo, hi] = extent(ex.Current(), o, dir);
                if (lo <= 1e3 * tol && (start.IsNull() || hi < startHi))
                {
                    start = ex.Current();
                    startHi = hi;
                }
            }
            if (start.IsNull())
            {
                r.message = "a extrusão começa dentro do corpo de destino (até a próxima face)";
                return r;
            }
            if (startHi >= segments[0].s1 - 1e3 * tol)
            {
                r.message = "nenhuma face do corpo de destino à frente no sentido da extrusão; use profundidade definida";
                return r;
            }
            toolNames = cad::modeling::propagateNames({&toolNames, input.targetNames}, ahead);
            toolResult.shape = start;
        }
    }

    // 4. Corte do lado de fora: laje do plano do esboço com as mesmas extensões, menos o prisma.
    TopoDS_Shape cutter = toolResult.shape;
    FaceNames cutterNames = toolNames;
    if (cut && p.cutOutside)
    {
        const cad::sketch::SketchPlane& plane = sketch.plane();
        double umin = 1e300, umax = -1e300, vmin = 1e300, vmax = -1e300;
        double x0, y0, z0, x1, y1, z1;
        targetBox.Get(x0, y0, z0, x1, y1, z1);
        for (double x : {x0, x1})
            for (double y : {y0, y1})
                for (double z : {z0, z1})
                {
                    const gp_Vec v(o, gp_Pnt(x, y, z));
                    umin = std::min(umin, v.Dot(gp_Vec(plane.horizontalAxis())));
                    umax = std::max(umax, v.Dot(gp_Vec(plane.horizontalAxis())));
                    vmin = std::min(vmin, v.Dot(gp_Vec(plane.verticalAxis())));
                    vmax = std::max(vmax, v.Dot(gp_Vec(plane.verticalAxis())));
                }
        const gp_Pln slabPlane(gp_Ax3(o, plane.normal(), plane.horizontalAxis()));
        const TopoDS_Face slabFace =
            BRepBuilderAPI_MakeFace(slabPlane, umin - margin, umax + margin, vmin - margin, vmax + margin).Face();
        TopoDS_Shape slab;
        for (const Segment& s : segments)
        {
            gp_Trsf t;
            t.SetTranslation(gp_Vec(dir) * s.s0);
            const FeatureResult part =
                cad::modeling::extrudeFace(TopoDS::Face(slabFace.Moved(TopLoc_Location(t))), gp_Vec(dir) * (s.s1 - s.s0));
            if (!part.ok)
            {
                r.message = "falha ao construir a região externa ao perfil: " + part.message;
                return r;
            }
            if (slab.IsNull())
                slab = part.shape;
            else
            {
                const FeatureResult joined = cad::modeling::booleanOperation(BooleanKind::Fuse, slab, part.shape);
                if (!joined.ok)
                {
                    r.message = "falha ao construir a região externa ao perfil: " + joined.message;
                    return r;
                }
                slab = joined.shape;
            }
        }
        const FeatureResult outside = cad::modeling::booleanOperation(BooleanKind::Cut, slab, toolResult.shape);
        if (!outside.ok)
        {
            r.message = "falha ao construir a região externa ao perfil: " + outside.message;
            return r;
        }
        cutterNames = cad::modeling::propagateNames({&toolNames}, outside);
        cutter = outside.shape;
    }
    if (tool)
        *tool = cutter;

    // 5. Aplicação no corpo de destino.
    FaceNames resultNames;
    if (p.mode == ExtrudeMode::NewBody)
    {
        r.ok = true;
        r.shape = toolResult.shape;
        resultNames = toolNames;
    }
    else if (p.mode == ExtrudeMode::Boss)
    {
        r = cad::modeling::booleanOperation(BooleanKind::Fuse, target, toolResult.shape);
        if (r.ok && cad::modeling::countSolids(r.shape) > cad::modeling::countSolids(target))
        {
            r.ok = false;
            r.message = "o volume da saliência não toca o corpo de destino (" + p.targetId
                        + "); use Extrusão de Novo Corpo para um sólido independente";
        }
        else if (r.ok && cad::modeling::ShapeInspector::volume(r.shape)
                             <= cad::modeling::ShapeInspector::volume(target) + 1e-6)
        {
            r.ok = false;
            r.message = "a saliência não adiciona material ao corpo de destino (volume dentro do corpo); verifique o sentido";
        }
        if (r.ok)
            resultNames = cad::modeling::propagateNames({input.targetNames, &toolNames}, r);
    }
    else
    {
        r = cad::modeling::booleanOperation(BooleanKind::Cut, target, cutter);
        if (r.ok && std::fabs(cad::modeling::ShapeInspector::volume(r.shape) - cad::modeling::ShapeInspector::volume(target))
                        < 1e-6)
        {
            r.ok = false;
            r.message = p.cutOutside ? "não há material fora do perfil na extensão do corte (" + p.targetId
                                           + "); verifique o sentido e a profundidade"
                                     : "a ferramenta de corte não intersecta o corpo de destino (" + p.targetId
                                           + "); verifique o sentido";
        }
        if (r.ok)
            resultNames = cad::modeling::propagateNames({input.targetNames, &cutterNames}, r);
    }
    if (r.ok && names)
    {
        cad::modeling::completeNames(resultNames, r.shape, op + ":aux");
        *names = resultNames;
    }
    return r;
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
        p.name = defaultExtrusionName(p.mode);
    const std::string id = "op-" + std::to_string(m_nextOperationNumber);
    std::vector<Operation> ops = m_operations;
    ops.push_back(makeExtrusion(id, p));
    const ValidationResult applied = applyOperations(std::move(ops), "profile");
    if (!applied.ok)
        return applied;
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
        && updated.parameter("depth") == it->parameter("depth") && updated.extrudeMode == it->extrudeMode
        && updated.targetId == it->targetId && updated.endCondition == it->endCondition
        && updated.cutOutside == it->cutOutside && updated.direction2 == it->direction2
        && updated.endCondition2 == it->endCondition2 && updated.parameter("depth2") == it->parameter("depth2")
        && updated.limitBodyId == it->limitBodyId && updated.limitFace.name == it->limitFace.name
        && updated.limitFace.centroid.Distance(it->limitFace.centroid) < 1e-9)
        return {};
    *it = updated;
    return applyOperations(std::move(ops), "profile");
}

ExtrudeParameters CadDocument::extrudeParameters(const std::string& id) const
{
    ExtrudeParameters p;
    if (const Operation* op = findOperation(id); op && op->type == OperationType::Extrude)
        p = extrudeParametersOf(*op);
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

void CadDocument::commit(std::vector<Operation> operations, std::vector<cad::sketch::Sketch> sketches, TopoDS_Shape shape)
{
    m_sketches = std::move(sketches);
    commit(std::move(operations), shape);
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
    op.extrudeMode = parameters.mode;
    op.targetId = parameters.mode == ExtrudeMode::NewBody ? std::string() : parameters.targetId;
    op.endCondition = parameters.endCondition;
    op.cutOutside = parameters.mode == ExtrudeMode::Cut && parameters.cutOutside;
    op.parameters = {{"depth", parameters.depth}};
    op.direction2 = parameters.direction2;
    if (parameters.direction2)
    {
        op.endCondition2 = parameters.endCondition2;
        op.parameters.push_back({"depth2", parameters.depth2});
    }
    if (parameters.endCondition == EndCondition::UpToFace)
    {
        op.limitBodyId = parameters.limitBodyId;
        op.limitFace = parameters.limitFace;
    }
    return op;
}

ExtrudeParameters CadDocument::extrudeParametersOf(const Operation& op)
{
    ExtrudeParameters p;
    p.name = op.name;
    p.sketchId = op.sketchId;
    p.depth = op.parameter("depth");
    p.reversed = op.reversed;
    p.mode = op.extrudeMode;
    p.targetId = op.targetId;
    p.endCondition = op.endCondition;
    p.cutOutside = op.cutOutside;
    p.direction2 = op.direction2;
    p.endCondition2 = op.endCondition2;
    if (op.direction2)
        p.depth2 = op.parameter("depth2");
    p.limitBodyId = op.limitBodyId;
    p.limitFace = op.limitFace;
    return p;
}

bool CadDocument::producesBody(OperationType type)
{
    return type == OperationType::ThroughHole || type == OperationType::Extrude || type == OperationType::Boolean
           || type == OperationType::Fillet3D || type == OperationType::Chamfer3D;
}

bool CadDocument::checkReferences(std::vector<Operation>& operations, std::string& error)
{
    std::set<std::string> bodies; // corpos definidos antes da operação corrente
    std::set<std::string> consumed;
    for (Operation& op : operations)
        op.consumedBy.clear();
    auto findOp = [&operations](const std::string& id) -> Operation* {
        for (Operation& op : operations)
            if (op.id == id)
                return &op;
        return nullptr;
    };
    for (Operation& op : operations)
    {
        const bool finish = op.type == OperationType::Fillet3D || op.type == OperationType::Chamfer3D
                            || (op.type == OperationType::Extrude && op.extrudeMode != ExtrudeMode::NewBody);
        if (op.type == OperationType::Boolean || finish)
        {
            const std::string label = op.name + " (" + op.id + ")";
            if (op.type == OperationType::Extrude && op.targetId.empty())
            {
                error = label + ": selecione o corpo de destino";
                return false;
            }
            if (op.type != OperationType::Extrude && finish && op.sourceId.empty())
            {
                error = label + ": selecione o corpo de origem";
                return false;
            }
            if (!finish && (op.operandA.empty() || op.operandB.empty()))
            {
                error = label + ": selecione os corpos A e B";
                return false;
            }
            if (!finish && op.operandA == op.operandB)
            {
                error = label + ": os corpos A e B devem ser diferentes";
                return false;
            }
            std::vector<const std::string*> refs;
            if (op.type == OperationType::Extrude)
                refs = {&op.targetId};
            else if (finish)
                refs = {&op.sourceId};
            else
                refs = {&op.operandA, &op.operandB};
            for (const std::string* operand : refs)
            {
                const char* which = op.type == OperationType::Extrude ? "de destino"
                                    : finish                          ? "de origem"
                                    : operand == &op.operandA         ? "A"
                                                                      : "B";
                if (*operand == op.id)
                {
                    error = label + ": a operação não pode usar o próprio resultado (dependência circular)";
                    return false;
                }
                if (!bodies.count(*operand))
                {
                    const Operation* target = findOp(*operand);
                    if (!target)
                        error = label + ": corpo " + which + " inexistente (" + *operand + ")";
                    else if (!producesBody(target->type))
                        error = label + ": a operação " + *operand + " não produz um corpo sólido";
                    else
                        error = label + ": o corpo " + which + " (" + *operand
                                + ") é posterior à operação (dependência circular não permitida)";
                    return false;
                }
                if (consumed.count(*operand))
                {
                    error = label + ": o corpo " + *operand + " já é usado por " + findOp(*operand)->consumedBy;
                    return false;
                }
                consumed.insert(*operand);
                findOp(*operand)->consumedBy = op.id;
            }
        }
        // Até a face (E5.7.1): a face limite pertence a um corpo anterior (não é consumido).
        if (op.type == OperationType::Extrude && op.endCondition == EndCondition::UpToFace)
        {
            const std::string label = op.name + " (" + op.id + ")";
            if (op.limitBodyId.empty())
            {
                error = label + ": selecione a face limite";
                return false;
            }
            if (!bodies.count(op.limitBodyId))
            {
                error = label + ": o corpo da face limite (" + op.limitBodyId
                        + ") não existe antes da operação (dependência circular não permitida)";
                return false;
            }
        }
        if (producesBody(op.type))
            bodies.insert(op.id);
    }
    return true;
}

bool CadDocument::resolveSupport(cad::sketch::Sketch& sketch, const TopoDS_Shape& body,
                                 const cad::modeling::FaceNames* names, std::string& error, std::string* note)
{
    if (!sketch.support())
        return true;
    cad::sketch::SketchSupport support = *sketch.support();
    static const cad::modeling::FaceNames kNoNames;
    const cad::modeling::FaceNames& faceNames = names ? *names : kNoNames;
    const cad::modeling::NamedFaceMatch m = cad::modeling::resolveNamedFace(body, faceNames, support.face);
    if (!m.ok)
    {
        error = sketch.name() + ": " + m.message;
        return false;
    }
    const auto plane = cad::modeling::planarFacePlane(m.face);
    if (!plane)
    {
        error = sketch.name() + ": face de suporte deixou de ser plana";
        return false;
    }
    if (note && !m.byName && !support.face.name.empty())
        *note = sketch.name() + ": face de suporte \"" + support.face.name
                + "\" recuperada pela geometria (mesmo plano), não pelo nome topológico";
    sketch.setPlane(cad::sketch::SketchPlane::forPlane(*plane));
    // A referência acompanha a geometria; o nome é o atual (documento antigo recebe o nome aqui).
    const std::string name = faceNames.nameOf(m.face);
    support.face = cad::modeling::describeFace(m.face);
    support.face.name = name;
    sketch.setSupport(support);
    return true;
}

bool CadDocument::rebuild(std::vector<Operation>& operations, std::vector<cad::sketch::Sketch>& sketches,
                          TopoDS_Shape& shape, std::string& error, RebuildLog* log, bool tolerant)
{
    if (!checkReferences(operations, error))
        return false;

    TopoDS_Shape part; // bloco (+ furo)
    const cad::modeling::FaceNames* partNames = nullptr;
    double length = 0.0, width = 0.0;
    size_t current = 0;
    // Somente resultados (e nomes de faces) de operações anteriores à corrente.
    auto opBefore = [&operations, &current](const std::string& id) -> const Operation* {
        for (size_t i = 0; i < current && i < operations.size(); ++i)
            if (operations[i].id == id)
                return &operations[i];
        return nullptr;
    };
    auto resultOf = [&](const std::string& id) {
        const Operation* o = opBefore(id);
        return o ? o->result : TopoDS_Shape();
    };
    auto namesOf = [&](const std::string& id) -> const cad::modeling::FaceNames* {
        const Operation* o = opBefore(id);
        return o ? &o->faceNames : nullptr;
    };
    std::set<std::string> resolvedSketches;
    auto resolveSketchSupport = [&](cad::sketch::Sketch& s, std::string& message) {
        if (!s.support() || resolvedSketches.count(s.id()))
            return true;
        const TopoDS_Shape body = resultOf(s.support()->bodyId);
        if (body.IsNull())
        {
            message = s.name() + ": corpo de suporte " + s.support()->bodyId
                      + " inexistente ou posterior à operação que usa o esboço";
            return false;
        }
        std::string note;
        if (!resolveSupport(s, body, namesOf(s.support()->bodyId), message, &note))
            return false;
        if (!note.empty() && log)
            log->sketchNotes.push_back(SketchIssue{s.id(), note});
        resolvedSketches.insert(s.id());
        return true;
    };
    // Uma falha não interrompe as operações independentes: só as que dependem (corpo de destino,
    // de origem, operandos, corpo de suporte do esboço, corpo da face limite ou o bloco do furo)
    // ficam bloqueadas.
    std::string firstError;
    std::set<std::string> invalid;
    std::string blockId;
    auto nameOf = [&operations](const std::string& id) {
        for (const Operation& o : operations)
            if (o.id == id)
                return o.name + " (" + id + ")";
        return id;
    };
    auto dependencies = [&](const Operation& op) {
        std::vector<std::string> deps;
        switch (op.type)
        {
        case OperationType::Block: break;
        case OperationType::ThroughHole: deps.push_back(blockId); break;
        case OperationType::Extrude:
            if (op.extrudeMode != ExtrudeMode::NewBody)
                deps.push_back(op.targetId);
            if (op.endCondition == EndCondition::UpToFace)
                deps.push_back(op.limitBodyId);
            for (const cad::sketch::Sketch& s : sketches)
                if (s.id() == op.sketchId && s.support())
                    deps.push_back(s.support()->bodyId);
            break;
        case OperationType::Boolean:
            deps.push_back(op.operandA);
            deps.push_back(op.operandB);
            break;
        case OperationType::Fillet3D:
        case OperationType::Chamfer3D: deps.push_back(op.sourceId); break;
        }
        return deps;
    };
    for (Operation& op : operations)
    {
        cad::modeling::FeatureResult r;
        cad::modeling::FaceNames names;
        op.result.Nullify();
        op.faceNames = cad::modeling::FaceNames();
        op.notes.clear();
        op.blockedBy.clear();
        if (op.type == OperationType::Block)
            blockId = op.id;
        for (const std::string& dep : dependencies(op))
            if (invalid.count(dep))
            {
                op.blockedBy = dep;
                break;
            }
        if (!op.blockedBy.empty())
        {
            op.status = OperationStatus::Invalid;
            op.statusMessage = "depende de operação inválida: " + nameOf(op.blockedBy);
            invalid.insert(op.id);
            ++current;
            continue;
        }
        switch (op.type)
        {
        case OperationType::Block:
            length = op.parameter("length");
            width = op.parameter("width");
            r = cad::modeling::makeBlock(length, width, op.parameter("height"));
            if (r.ok)
            {
                part = r.shape;
                names = cad::modeling::nameBoxFaces(r.shape, op.id);
            }
            break;
        case OperationType::ThroughHole:
            // Relação geométrica: furo centrado na face superior do bloco.
            r = cad::modeling::cutThroughHole(part, length / 2.0, width / 2.0, op.parameter("radius"));
            if (r.ok)
            {
                part = r.shape;
                cad::modeling::FaceNames tool;
                for (TopExp_Explorer ex(r.tool, TopAbs_FACE); ex.More(); ex.Next())
                    if (!cad::modeling::planarFacePlane(TopoDS::Face(ex.Current())))
                        tool.add(ex.Current(), op.id + ":furo");
                names = cad::modeling::propagateNames({partNames, &tool}, r);
            }
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
            // Esboço sobre face: plano recalculado a partir da face no corpo de suporte.
            if (!resolveSketchSupport(*it, r.message))
                break;
            const ExtrudeParameters p = extrudeParametersOf(op);
            ExtrusionInput input;
            input.opId = op.id;
            if (op.extrudeMode != ExtrudeMode::NewBody)
            {
                input.target = resultOf(op.targetId);
                input.targetNames = namesOf(op.targetId);
            }
            if (op.endCondition == EndCondition::UpToFace)
            {
                // Face limite pelo nome topológico (geometria como desempate/legado).
                const TopoDS_Shape body = resultOf(op.limitBodyId);
                static const cad::modeling::FaceNames kNoNames;
                const cad::modeling::FaceNames* limitNames = namesOf(op.limitBodyId);
                const cad::modeling::NamedFaceMatch m =
                    cad::modeling::resolveNamedFace(body, limitNames ? *limitNames : kNoNames, op.limitFace);
                if (!m.ok)
                {
                    r.message = "face limite — " + m.message;
                    break;
                }
                if (!m.byName && !op.limitFace.name.empty())
                    op.notes.push_back("face limite \"" + op.limitFace.name + "\" recuperada pela geometria");
                input.limitFace = m.face;
                const std::string name = limitNames ? limitNames->nameOf(m.face) : std::string();
                op.limitFace = cad::modeling::describeFace(m.face);
                op.limitFace.name = name;
            }
            r = computeExtrusion(p, *it, input, nullptr, &names);
            break;
        }
        case OperationType::Boolean:
            r = cad::modeling::booleanOperation(op.booleanKind, resultOf(op.operandA), resultOf(op.operandB));
            if (r.ok)
            {
                names = cad::modeling::propagateNames({namesOf(op.operandA), namesOf(op.operandB)}, r);
                cad::modeling::completeNames(names, r.shape, op.id + ":aux");
            }
            break;
        case OperationType::Fillet3D:
        case OperationType::Chamfer3D:
            r = computeFinish(op, resultOf(op.sourceId), namesOf(op.sourceId), &names);
            break;
        }
        ++current;
        op.status = r.ok ? OperationStatus::Valid : OperationStatus::Invalid;
        op.statusMessage = r.message;
        if (r.ok)
        {
            op.result = r.shape;
            op.faceNames = std::move(names);
            if (op.type == OperationType::Block || op.type == OperationType::ThroughHole)
                partNames = &op.faceNames;
        }
        else
        {
            invalid.insert(op.id);
            if (firstError.empty())
                firstError = op.name + ": " + r.message;
        }
    }
    // Esboços sobre faces não usados por extrusões também acompanham o corpo de suporte.
    current = operations.size();
    for (cad::sketch::Sketch& s : sketches)
    {
        std::string message;
        if (s.support() && invalid.count(s.support()->bodyId))
            message = s.name() + ": corpo de suporte inválido (" + nameOf(s.support()->bodyId) + ")";
        else if (resolveSketchSupport(s, message))
            continue;
        if (tolerant && log)
            log->sketchIssues.push_back(SketchIssue{s.id(), message});
        else if (firstError.empty())
            firstError = message;
    }

    // Resultado do documento: corpos ativos (não consumidos), sem união implícita. Corpo válido
    // consumido por uma operação inválida continua exibido (último estado válido da cadeia).
    std::vector<TopoDS_Shape> solids;
    for (const Operation& op : operations)
    {
        if (!producesBody(op.type) || op.result.IsNull())
            continue;
        if (op.consumedBy.empty() || invalid.count(op.consumedBy))
            solids.push_back(op.result);
    }
    if (solids.empty())
        shape.Nullify();
    else if (solids.size() == 1)
        shape = solids.front();
    else
    {
        BRep_Builder builder;
        TopoDS_Compound compound;
        builder.MakeCompound(compound);
        for (const TopoDS_Shape& s : solids)
            builder.Add(compound, s);
        shape = compound;
    }
    if (!firstError.empty())
    {
        error = firstError;
        return false;
    }
    return true;
}

bool CadDocument::hasInvalidOperations() const
{
    return std::any_of(m_operations.begin(), m_operations.end(),
                       [](const Operation& op) { return op.status == OperationStatus::Invalid; });
}

bool CadDocument::rebuildForCommit(std::vector<Operation>& operations, std::vector<cad::sketch::Sketch>& sketches,
                                   TopoDS_Shape& shape, std::string& error, RebuildLog& log) const
{
    const bool tolerant = hasInvalidOperations() || !m_log.sketchIssues.empty();
    // Referências estruturais (operandos inexistentes, ciclos) nunca são aceitas.
    std::vector<Operation> check = operations;
    if (!checkReferences(check, error))
        return false;
    log = RebuildLog();
    if (rebuild(operations, sketches, shape, error, &log, tolerant))
        return true;
    if (!tolerant)
        return false;
    // Documento já com erros: aceito se não surgir nenhuma falha nova (a correção pode ser
    // feita uma operação de cada vez, sem descartar nem alterar as outras).
    for (const Operation& op : operations)
    {
        const Operation* before = findOperation(op.id);
        if (op.status == OperationStatus::Invalid && (!before || before->status != OperationStatus::Invalid))
        {
            error = op.name + ": " + op.statusMessage;
            return false;
        }
    }
    for (const SketchIssue& issue : log.sketchIssues)
        if (std::none_of(m_log.sketchIssues.begin(), m_log.sketchIssues.end(),
                         [&](const SketchIssue& old) { return old.sketchId == issue.sketchId; }))
        {
            error = issue.message;
            return false;
        }
    error.clear();
    return true;
}

// --- Corpos e booleanas -------------------------------------------------------

const char* booleanKindLabel(BooleanKind kind)
{
    switch (kind)
    {
    case BooleanKind::Fuse:   return "União";
    case BooleanKind::Cut:    return "Subtração";
    case BooleanKind::Common: return "Interseção";
    }
    return "";
}

std::vector<std::string> CadDocument::activeBodies() const
{
    std::vector<std::string> ids;
    for (const Operation& op : m_operations)
        if (producesBody(op.type) && op.consumedBy.empty())
            ids.push_back(op.id);
    return ids;
}

std::string CadDocument::consumerOf(const std::string& bodyId) const
{
    const Operation* op = findOperation(bodyId);
    return op ? op->consumedBy : std::string();
}

std::string CadDocument::defaultBooleanName(BooleanKind kind) const
{
    int n = 1;
    for (const Operation& op : m_operations)
        if (op.type == OperationType::Boolean && op.booleanKind == kind)
            ++n;
    return std::string(booleanKindLabel(kind)) + " " + std::to_string(n);
}

Operation CadDocument::makeBoolean(const std::string& id, const BooleanParameters& parameters)
{
    Operation op;
    op.id = id;
    op.type = OperationType::Boolean;
    op.name = trim(parameters.name);
    op.booleanKind = parameters.kind;
    op.operandA = parameters.operandA;
    op.operandB = parameters.operandB;
    return op;
}

BooleanParameters CadDocument::booleanParameters(const std::string& id) const
{
    BooleanParameters p;
    if (const Operation* op = findOperation(id); op && op->type == OperationType::Boolean)
    {
        p.name = op->name;
        p.kind = op->booleanKind;
        p.operandA = op->operandA;
        p.operandB = op->operandB;
    }
    return p;
}

cad::modeling::FeatureResult CadDocument::previewBoolean(const BooleanParameters& parameters) const
{
    cad::modeling::FeatureResult r;
    const Operation* a = findOperation(parameters.operandA);
    const Operation* b = findOperation(parameters.operandB);
    if (!a || !b)
    {
        r.message = "Selecione os corpos A e B";
        return r;
    }
    if (a == b)
    {
        r.message = "Os corpos A e B devem ser diferentes";
        return r;
    }
    for (const Operation* op : {a, b})
        if (!producesBody(op->type) || !op->consumedBy.empty())
        {
            r.message = "O corpo " + op->id + " não está disponível (já consumido por " + op->consumedBy + ")";
            return r;
        }
    return cad::modeling::booleanOperation(parameters.kind, a->result, b->result);
}

// --- Acabamentos 3D -----------------------------------------------------------

cad::modeling::FeatureResult CadDocument::computeFinish(Operation& op, const TopoDS_Shape& source,
                                                        const cad::modeling::FaceNames* sourceNames,
                                                        cad::modeling::FaceNames* names)
{
    cad::modeling::FeatureResult r;
    static const cad::modeling::FaceNames kNoNames;
    const cad::modeling::FaceNames& srcNames = sourceNames ? *sourceNames : kNoNames;
    std::vector<TopoDS_Edge> found;
    for (size_t i = 0; i < op.edges.size(); ++i)
    {
        // Pelo nome topológico (faces adjacentes); geometria como desempate/legado.
        const cad::modeling::NamedEdgeMatch m = cad::modeling::resolveNamedEdge(source, srcNames, op.edges[i]);
        if (!m.ok)
        {
            r.message = "aresta " + std::to_string(i + 1) + " — " + m.message;
            return r;
        }
        if (!m.byName && !op.edges[i].name.empty())
            op.notes.push_back("aresta " + std::to_string(i + 1) + " (\"" + op.edges[i].name
                               + "\") recuperada pela geometria, não pelo nome topológico");
        for (const TopoDS_Edge& e : found)
            if (e.IsSame(m.edge))
            {
                r.message = "aresta " + std::to_string(i + 1) + " repetida na seleção";
                return r;
            }
        found.push_back(m.edge);
    }
    const bool advanced = op.type == OperationType::Chamfer3D && op.chamferMode != Chamfer3DMode::Equal;
    std::vector<TopoDS_Face> faces;
    if (advanced)
    {
        // Face de referência de cada aresta: a face adjacente cuja normal (no ponto médio)
        // coincide com a referência guardada; sem referência, a escolha padrão.
        for (size_t i = 0; i < found.size(); ++i)
        {
            const auto adjacent = cad::modeling::adjacentFaces(source, found[i]);
            if (adjacent.size() != 2)
            {
                r.message = "aresta " + std::to_string(i + 1) + " sem duas faces adjacentes";
                return r;
            }
            const TopoDS_Face* chosen = &adjacent[0].first;
            if (i < op.referenceNormals.size())
            {
                chosen = nullptr;
                for (const auto& [face, normal] : adjacent)
                    if (normal.Angle(op.referenceNormals[i]) < 1e-3)
                        chosen = &face;
                if (!chosen)
                {
                    r.message = "aresta " + std::to_string(i + 1) + " — face de referência não identificada";
                    return r;
                }
            }
            faces.push_back(*chosen);
        }
    }
    if (op.type == OperationType::Fillet3D)
        r = cad::modeling::filletEdges(source, found, op.parameter("radius"));
    else if (!advanced)
        r = cad::modeling::chamferEdges(source, found, op.parameter("distance"));
    else
        r = cad::modeling::chamferEdgesAdvanced(source, found, faces, op.chamferMode == Chamfer3DMode::DistanceAngle,
                                                op.parameter("distance"),
                                                op.chamferMode == Chamfer3DMode::DistanceAngle ? op.parameter("angle")
                                                                                               : op.parameter("distance2"));
    if (r.ok)
    {
        // Referências acompanham a geometria do corpo de origem (com o nome atual da aresta).
        for (size_t i = 0; i < op.edges.size(); ++i)
        {
            op.edges[i] = cad::modeling::describeEdge(source, found[i]);
            op.edges[i].name = cad::modeling::edgeName(source, srcNames, found[i]);
        }
        if (names)
        {
            // Faces do resultado: as do corpo de origem pelo histórico; as novas por aresta.
            *names = cad::modeling::propagateNames({sourceNames}, r);
            cad::modeling::nameGenerated(*names, r, [&](const TopoDS_Shape& e) {
                for (size_t i = 0; i < found.size(); ++i)
                    if (found[i].IsSame(e))
                        return op.id + ":acab:" + std::to_string(i);
                return std::string();
            });
            cad::modeling::completeNames(*names, r.shape, op.id + ":aux");
        }
        if (advanced)
        {
            op.referenceNormals.clear();
            for (size_t i = 0; i < faces.size(); ++i)
                for (const auto& [face, normal] : cad::modeling::adjacentFaces(source, found[i]))
                    if (face.IsSame(faces[i]))
                        op.referenceNormals.push_back(normal);
        }
    }
    return r;
}

std::vector<gp_Dir> CadDocument::referenceNormals(const std::string& bodyId, const std::vector<TopoDS_Edge>& edges,
                                                  bool flip) const
{
    std::vector<gp_Dir> normals;
    const Operation* body = findOperation(bodyId);
    if (!body)
        return normals;
    for (const TopoDS_Edge& e : edges)
    {
        const auto adjacent = cad::modeling::adjacentFaces(body->result, e);
        if (adjacent.size() == 2)
            normals.push_back(adjacent[flip ? 1 : 0].second);
    }
    return normals;
}

// --- Esboços sobre faces --------------------------------------------------------

std::string CadDocument::bodyContainingFace(const TopoDS_Shape& face) const
{
    // Corpo ativo de preferência (o que está visível); senão, o mais recente que a contém.
    std::string any;
    for (const Operation& op : m_operations)
        if (producesBody(op.type) && cad::modeling::containsFace(op.result, face))
        {
            if (op.consumedBy.empty())
                return op.id;
            any = op.id;
        }
    return any;
}

std::optional<cad::sketch::Sketch> CadDocument::createFaceSketch(const std::string& bodyId, const TopoDS_Shape& face,
                                                                 std::string& error) const
{
    const Operation* body = findOperation(bodyId);
    if (!body || !producesBody(body->type))
    {
        error = "Corpo de suporte inexistente: " + bodyId;
        return std::nullopt;
    }
    if (face.IsNull() || face.ShapeType() != TopAbs_FACE || !cad::modeling::containsFace(body->result, face))
    {
        error = "Selecione uma face do corpo " + body->name;
        return std::nullopt;
    }
    // Face com a orientação do sólido (a normal externa depende dela).
    const TopoDS_Face f = cad::modeling::faceInShape(body->result, face);
    const auto plane = cad::modeling::planarFacePlane(f);
    if (!plane)
    {
        error = "A face selecionada não é plana: esboços só podem ser criados sobre faces planas nesta etapa";
        return std::nullopt;
    }
    const std::string n = std::to_string(m_nextSketchNumber);
    cad::sketch::Sketch s("sk-" + n, "Esboço " + n, cad::sketch::SketchPlane::forPlane(*plane));
    // Referência persistente: assinatura geométrica + nome topológico da face (E5.7.1).
    cad::modeling::FaceSignature signature = cad::modeling::describeFace(f);
    signature.name = body->faceNames.nameOf(f);
    s.setSupport(cad::sketch::SketchSupport{bodyId, signature});
    return s;
}

TopoDS_Face CadDocument::supportFace(const std::string& sketchId) const
{
    const cad::sketch::Sketch* s = findSketch(sketchId);
    return s && s->support() ? resolveFaceReference(s->support()->bodyId, s->support()->face) : TopoDS_Face();
}

std::string CadDocument::faceName(const std::string& bodyId, const TopoDS_Shape& face) const
{
    const Operation* body = findOperation(bodyId);
    return body ? body->faceNames.nameOf(face) : std::string();
}

std::string CadDocument::edgeName(const std::string& bodyId, const TopoDS_Edge& edge) const
{
    const Operation* body = findOperation(bodyId);
    return body ? cad::modeling::edgeName(body->result, body->faceNames, edge) : std::string();
}

std::optional<cad::modeling::FaceSignature> CadDocument::faceReference(const std::string& bodyId,
                                                                       const TopoDS_Shape& face) const
{
    const Operation* body = findOperation(bodyId);
    if (!body || face.IsNull() || face.ShapeType() != TopAbs_FACE)
        return std::nullopt;
    const TopoDS_Face f = cad::modeling::faceInShape(body->result, face);
    if (f.IsNull() || !cad::modeling::planarFacePlane(f))
        return std::nullopt;
    cad::modeling::FaceSignature signature = cad::modeling::describeFace(f);
    signature.name = body->faceNames.nameOf(f);
    return signature;
}

TopoDS_Face CadDocument::resolveFaceReference(const std::string& bodyId, const cad::modeling::FaceSignature& face,
                                              std::string* error) const
{
    const Operation* body = findOperation(bodyId);
    if (!body || body->result.IsNull())
    {
        if (error)
            *error = "corpo da face inexistente ou sem geometria (" + bodyId + ")";
        return TopoDS_Face();
    }
    const cad::modeling::NamedFaceMatch m = cad::modeling::resolveNamedFace(body->result, body->faceNames, face);
    if (!m.ok && error)
        *error = m.message;
    return m.ok ? m.face : TopoDS_Face();
}

std::optional<std::pair<std::string, cad::modeling::FaceSignature>>
CadDocument::limitFaceBefore(const std::string& operationId, const std::string& bodyId,
                             const cad::modeling::FaceSignature& face) const
{
    const auto edited = std::find_if(m_operations.begin(), m_operations.end(),
                                     [&](const Operation& op) { return op.id == operationId; });
    if (edited == m_operations.end())
        return std::make_pair(bodyId, face);
    for (auto it = m_operations.begin(); it != edited; ++it)
        if (it->id == bodyId)
            return std::make_pair(bodyId, face); // o corpo já é anterior à operação
    if (face.name.empty())
        return std::nullopt;
    for (auto it = std::make_reverse_iterator(edited); it != m_operations.rend(); ++it)
    {
        if (!producesBody(it->type) || it->result.IsNull())
            continue;
        const auto m = cad::modeling::resolveNamedFace(it->result, it->faceNames, face);
        if (!m.ok || !m.byName)
            continue;
        const auto reference = faceReference(it->id, m.face);
        if (reference && reference->normal.Angle(face.normal) < 1e-6 && std::fabs(reference->offset - face.offset) < 1e-6)
            return std::make_pair(it->id, *reference);
    }
    return std::nullopt;
}
ValidationResult CadDocument::validateChamferMode(OperationType type, const FinishParameters& p)
{
    if (type != OperationType::Chamfer3D || p.chamferMode == Chamfer3DMode::Equal)
        return {};
    if (p.chamferMode == Chamfer3DMode::TwoDistances && (!std::isfinite(p.value2) || p.value2 <= 0.0))
        return ValidationResult{false, "value2", "Distância D2 deve ser maior que zero"};
    if (p.chamferMode == Chamfer3DMode::DistanceAngle && (!std::isfinite(p.angle) || p.angle <= 0.0 || p.angle >= 90.0))
        return ValidationResult{false, "angle", "Ângulo do chanfro deve estar entre 0° e 90° (exclusive)"};
    if (!p.referenceNormals.empty() && p.referenceNormals.size() != p.edges.size())
        return ValidationResult{false, "edges", "Uma face de referência por aresta"};
    return {};
}

std::string CadDocument::defaultFinishName(OperationType type) const
{
    int n = 1;
    for (const Operation& op : m_operations)
        if (op.type == type)
            ++n;
    return std::string(type == OperationType::Fillet3D ? "Fillet 3D " : "Chamfer 3D ") + std::to_string(n);
}

Operation CadDocument::makeFinish(const std::string& id, OperationType type, const FinishParameters& parameters)
{
    Operation op;
    op.id = id;
    op.type = type;
    op.name = trim(parameters.name);
    op.sourceId = parameters.sourceId;
    op.edges = parameters.edges;
    op.parameters = {{type == OperationType::Fillet3D ? "radius" : "distance", parameters.value}};
    if (type == OperationType::Chamfer3D)
    {
        op.chamferMode = parameters.chamferMode;
        if (parameters.chamferMode == Chamfer3DMode::TwoDistances)
            op.parameters.push_back({"distance2", parameters.value2});
        else if (parameters.chamferMode == Chamfer3DMode::DistanceAngle)
            op.parameters.push_back({"angle", parameters.angle});
        if (parameters.chamferMode != Chamfer3DMode::Equal)
            op.referenceNormals = parameters.referenceNormals;
    }
    return op;
}

std::vector<cad::modeling::EdgeSignature> CadDocument::edgeSignatures(const std::string& bodyId,
                                                                      const std::vector<TopoDS_Edge>& edges) const
{
    std::vector<cad::modeling::EdgeSignature> signatures;
    const Operation* body = findOperation(bodyId);
    if (!body)
        return signatures;
    for (const TopoDS_Edge& e : edges)
    {
        // Assinatura geométrica + nome topológico (faces adjacentes nomeadas, E5.7.1).
        cad::modeling::EdgeSignature s = cad::modeling::describeEdge(body->result, e);
        s.name = cad::modeling::edgeName(body->result, body->faceNames, e);
        signatures.push_back(s);
    }
    return signatures;
}

std::string CadDocument::bodyContaining(const TopoDS_Shape& edge) const
{
    for (const Operation& op : m_operations)
        if (producesBody(op.type) && op.consumedBy.empty() && cad::modeling::containsEdge(op.result, edge))
            return op.id;
    return {};
}

cad::modeling::FeatureResult CadDocument::previewFinish(OperationType type, const FinishParameters& parameters) const
{
    cad::modeling::FeatureResult r;
    const Operation* source = findOperation(parameters.sourceId);
    if (!source || !producesBody(source->type))
    {
        r.message = "Corpo de origem inexistente: " + parameters.sourceId;
        return r;
    }
    Operation op = makeFinish("preview", type, parameters);
    return computeFinish(op, source->result, &source->faceNames);
}

ValidationResult CadDocument::addFinish(OperationType type, const FinishParameters& parameters, std::string* newId)
{
    if (type != OperationType::Fillet3D && type != OperationType::Chamfer3D)
        return ValidationResult{false, "", "Tipo de acabamento inválido"};
    if (trim(parameters.name).size() > kMaxNameLength)
        return ValidationResult{false, "name", "Nome longo demais"};
    if (!std::isfinite(parameters.value) || parameters.value > kMaxDimension)
        return ValidationResult{false, "value", type == OperationType::Fillet3D ? "Raio inválido" : "Distância inválida"};
    if (parameters.edges.empty())
        return ValidationResult{false, "edges", "Selecione pelo menos uma aresta"};
    if (const ValidationResult v = validateChamferMode(type, parameters); !v.ok)
        return v;
    FinishParameters p = parameters;
    if (trim(p.name).empty())
        p.name = defaultFinishName(type);
    const std::string id = "op-" + std::to_string(m_nextOperationNumber);
    std::vector<Operation> ops = m_operations;
    ops.push_back(makeFinish(id, type, p));
    const ValidationResult v = applyOperations(std::move(ops), "value");
    if (!v.ok)
        return v;
    ++m_nextOperationNumber;
    if (newId)
        *newId = id;
    return {};
}

ValidationResult CadDocument::updateFinish(const std::string& id, const FinishParameters& parameters)
{
    std::vector<Operation> ops = m_operations;
    auto it = std::find_if(ops.begin(), ops.end(), [&](const Operation& op) { return op.id == id; });
    if (it == ops.end() || (it->type != OperationType::Fillet3D && it->type != OperationType::Chamfer3D))
        return ValidationResult{false, "", "Acabamento inexistente: " + id};
    if (trim(parameters.name).size() > kMaxNameLength)
        return ValidationResult{false, "name", "Nome longo demais"};
    if (!std::isfinite(parameters.value) || parameters.value > kMaxDimension)
        return ValidationResult{false, "value", "Valor inválido"};
    if (parameters.edges.empty())
        return ValidationResult{false, "edges", "Selecione pelo menos uma aresta"};
    if (const ValidationResult v = validateChamferMode(it->type, parameters); !v.ok)
        return v;
    FinishParameters p = parameters;
    p.sourceId = it->sourceId;
    if (trim(p.name).empty())
        p.name = it->name;
    *it = makeFinish(id, it->type, p);
    return applyOperations(std::move(ops), "value");
}

FinishParameters CadDocument::finishParameters(const std::string& id) const
{
    FinishParameters p;
    if (const Operation* op = findOperation(id);
        op && (op->type == OperationType::Fillet3D || op->type == OperationType::Chamfer3D))
    {
        p.name = op->name;
        p.sourceId = op->sourceId;
        p.value = op->parameter(op->type == OperationType::Fillet3D ? "radius" : "distance");
        p.edges = op->edges;
        p.chamferMode = op->chamferMode;
        if (op->chamferMode == Chamfer3DMode::TwoDistances)
            p.value2 = op->parameter("distance2");
        if (op->chamferMode == Chamfer3DMode::DistanceAngle)
            p.angle = op->parameter("angle");
        p.referenceNormals = op->referenceNormals;
    }
    return p;
}

std::vector<TopoDS_Edge> CadDocument::resolvedFinishEdges(const std::string& id) const
{
    std::vector<TopoDS_Edge> edges;
    const Operation* op = findOperation(id);
    const Operation* source = op ? findOperation(op->sourceId) : nullptr;
    if (!source)
        return edges;
    for (const auto& s : op->edges)
    {
        const cad::modeling::NamedEdgeMatch m = cad::modeling::resolveNamedEdge(source->result, source->faceNames, s);
        if (m.ok)
            edges.push_back(m.edge);
    }
    return edges;
}

ValidationResult CadDocument::applyOperations(std::vector<Operation> operations, const char* field)
{
    TopoDS_Shape shape;
    std::string error;
    std::vector<cad::sketch::Sketch> sketches = m_sketches;
    RebuildLog log;
    if (!rebuildForCommit(operations, sketches, shape, error, log))
        return ValidationResult{false, field, error};
    m_log = std::move(log);
    commit(std::move(operations), std::move(sketches), shape);
    return {};
}

ValidationResult CadDocument::addBoolean(const BooleanParameters& parameters, std::string* newId)
{
    if (trim(parameters.name).size() > kMaxNameLength)
        return ValidationResult{false, "name", "Nome longo demais"};
    BooleanParameters p = parameters;
    if (trim(p.name).empty())
        p.name = defaultBooleanName(p.kind);
    const std::string id = "op-" + std::to_string(m_nextOperationNumber);
    std::vector<Operation> ops = m_operations;
    ops.push_back(makeBoolean(id, p));
    const ValidationResult v = applyOperations(std::move(ops), "operands");
    if (!v.ok)
        return v;
    ++m_nextOperationNumber;
    if (newId)
        *newId = id;
    return {};
}

ValidationResult CadDocument::updateBoolean(const std::string& id, const BooleanParameters& parameters)
{
    if (trim(parameters.name).size() > kMaxNameLength)
        return ValidationResult{false, "name", "Nome longo demais"};
    std::vector<Operation> ops = m_operations;
    auto it = std::find_if(ops.begin(), ops.end(), [&](const Operation& op) { return op.id == id; });
    if (it == ops.end() || it->type != OperationType::Boolean)
        return ValidationResult{false, "", "Operação booleana inexistente: " + id};
    BooleanParameters p = parameters;
    if (trim(p.name).empty())
        p.name = it->name;
    const Operation updated = makeBoolean(id, p);
    if (updated.name == it->name && updated.booleanKind == it->booleanKind && updated.operandA == it->operandA
        && updated.operandB == it->operandB)
        return {};
    *it = updated;
    return applyOperations(std::move(ops), "operands");
}
} // namespace cad::document
