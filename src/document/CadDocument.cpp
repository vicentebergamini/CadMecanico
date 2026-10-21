#include "document/CadDocument.h"

#include "modeling/Features.h"
#include "modeling/ShapeInspector.h"
#include "sketch/SketchProfile.h"

#include <BRep_Builder.hxx>
#include <BRepBndLib.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <gp_Ax3.hxx>
#include <gp_Pln.hxx>
#include <Bnd_Box.hxx>
#include <TopoDS.hxx>
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
    if (p.mode != ExtrudeMode::NewBody && p.targetId.empty())
        return fail("targetId", "Selecione o corpo de destino da extrusão");
    if (p.endCondition == EndCondition::ThroughAll && p.mode != ExtrudeMode::Cut)
        return fail("endCondition", "\"Atravessar tudo\" só se aplica à Extrusão de Corte");
    if (p.cutOutside && p.mode != ExtrudeMode::Cut)
        return fail("cutOutside", "\"Cortar o lado de fora\" só se aplica à remoção de material (corte)");
    if (p.endCondition == EndCondition::ThroughAll)
    {
        if (trim(p.name).size() > kMaxNameLength)
            return fail("name", "Nome longo demais");
        return {}; // a extensão é calculada a partir do corpo de destino
    }
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
    std::vector<SketchIssue> issues;
    if (allowInvalid)
    {
        // Referências estruturais continuam obrigatórias; falhas geométricas ficam registradas
        // nas operações (estado e mensagem) sem descartar nem alterar nada.
        std::vector<Operation> check = ops;
        if (!checkReferences(check, error))
            return false;
        std::string rebuildError;
        rebuild(ops, sketches, shape, rebuildError, &issues);
    }
    else if (!rebuild(ops, sketches, shape, error))
        return false;

    CadDocument doc(data.name);
    doc.m_operations = std::move(ops);
    doc.m_sketchIssues = std::move(issues);
    doc.m_shape = shape;
    doc.m_revision = doc.m_operations.empty() ? 0 : 1;
    doc.m_sketches = std::move(sketches); // planes dos esboços sobre faces reconstruídos
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
        std::vector<SketchIssue> issues;
        if (!rebuildForCommit(ops, sketches, shape, rebuildError, issues))
        {
            error = "A alteração do esboço invalida uma operação dependente: " + rebuildError;
            return false;
        }
        m_sketchIssues = std::move(issues);
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
    std::vector<cad::sketch::Sketch> sketches = m_sketches;
    std::vector<SketchIssue> issues;
    if (!rebuildForCommit(ops, sketches, shape, error, issues))
        return ValidationResult{false, "", error};
    m_sketchIssues = std::move(issues);
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
    TopoDS_Shape target;
    if (parameters.mode != ExtrudeMode::NewBody)
    {
        const Operation* t = findOperation(parameters.targetId);
        if (!t || !producesBody(t->type))
        {
            r.message = "Corpo de destino inexistente: " + parameters.targetId;
            return r;
        }
        if (!t->consumedBy.empty() && t->consumedBy != editingId)
        {
            r.message = "O corpo " + t->id + " já foi usado por " + t->consumedBy;
            return r;
        }
        target = t->result;
    }
    return computeExtrusion(parameters, *sketch, target, nullptr);
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
    const Operation* t = parameters.mode == ExtrudeMode::NewBody ? nullptr : findOperation(parameters.targetId);
    TopoDS_Shape tool;
    ExtrudeParameters p = parameters;
    p.mode = ExtrudeMode::NewBody; // só o prisma
    const bool through = parameters.endCondition == EndCondition::ThroughAll;
    if (through || (parameters.mode == ExtrudeMode::Cut && parameters.cutOutside))
    {
        // Mesma ferramenta usada no corte (comprimento e região externa dependem do corpo).
        computeExtrusion(parameters, *sketch, t ? t->result : TopoDS_Shape(), &tool);
        r.ok = !tool.IsNull();
        r.shape = tool;
        if (!r.ok)
            r.message = "Ferramenta de corte indisponível";
        return r;
    }
    if (parameters.mode == ExtrudeMode::Cut)
        p.reversed = !p.reversed; // sentido do corte: para dentro da face
    p.endCondition = EndCondition::Blind;
    return computeExtrusion(p, *sketch, TopoDS_Shape(), nullptr);
}

cad::modeling::FeatureResult CadDocument::computeExtrusion(const ExtrudeParameters& p, const cad::sketch::Sketch& sketch,
                                                           const TopoDS_Shape& target, TopoDS_Shape* tool)
{
    cad::modeling::FeatureResult r;
    const cad::sketch::ProfileResult profile = cad::sketch::buildProfile(sketch);
    if (!profile.ok)
    {
        r.message = "perfil do " + sketch.name() + " inválido: " + profile.message;
        return r;
    }
    if (p.mode != ExtrudeMode::NewBody && target.IsNull())
    {
        r.message = "corpo de destino sem geometria (" + p.targetId + ")";
        return r;
    }
    // Sentido: normal do plano local do esboço (no corte, para dentro da face); invertível.
    gp_Dir dir = sketch.plane().normal();
    if (p.mode == ExtrudeMode::Cut)
        dir.Reverse();
    if (p.reversed)
        dir.Reverse();
    double length = p.depth;
    if (p.mode == ExtrudeMode::Cut && p.endCondition == EndCondition::ThroughAll)
    {
        // Atravessar tudo: extensão até o canto mais distante da caixa do corpo na direção do
        // corte, medida a partir do plano do esboço, mais uma margem (1 mm ou 1% da diagonal).
        Bnd_Box box;
        BRepBndLib::Add(target, box);
        double x0, y0, z0, x1, y1, z1;
        box.Get(x0, y0, z0, x1, y1, z1);
        const gp_Pnt o = sketch.plane().origin();
        double farthest = -1e300;
        for (double x : {x0, x1})
            for (double y : {y0, y1})
                for (double z : {z0, z1})
                    farthest = std::max(farthest, gp_Vec(o, gp_Pnt(x, y, z)).Dot(gp_Vec(dir)));
        if (farthest <= 1e-6)
        {
            r.message = "o corpo de destino não está no sentido do corte (inverta a direção)";
            return r;
        }
        const double diagonal = std::sqrt(box.SquareExtent());
        length = farthest + std::max(1.0, 0.01 * diagonal);
    }
    const cad::modeling::FeatureResult prism = cad::modeling::extrudeFace(profile.face, gp_Vec(dir) * length);
    if (!prism.ok || p.mode == ExtrudeMode::NewBody)
    {
        if (tool && prism.ok)
            *tool = prism.shape;
        return prism;
    }
    TopoDS_Shape cutter = prism.shape;
    if (p.mode == ExtrudeMode::Cut && p.cutOutside)
    {
        // Cortar o lado de fora: laje do plano do esboço que cobre o corpo de destino (com folga),
        // varrida pela mesma extensão, menos o prisma do perfil.
        const cad::sketch::SketchPlane& plane = sketch.plane();
        Bnd_Box box;
        BRepBndLib::Add(target, box);
        double x0, y0, z0, x1, y1, z1;
        box.Get(x0, y0, z0, x1, y1, z1);
        double umin = 1e300, umax = -1e300, vmin = 1e300, vmax = -1e300;
        for (double x : {x0, x1})
            for (double y : {y0, y1})
                for (double z : {z0, z1})
                {
                    const gp_Vec v(plane.origin(), gp_Pnt(x, y, z));
                    umin = std::min(umin, v.Dot(gp_Vec(plane.horizontalAxis())));
                    umax = std::max(umax, v.Dot(gp_Vec(plane.horizontalAxis())));
                    vmin = std::min(vmin, v.Dot(gp_Vec(plane.verticalAxis())));
                    vmax = std::max(vmax, v.Dot(gp_Vec(plane.verticalAxis())));
                }
        const double margin = std::max(1.0, 0.01 * std::sqrt(box.SquareExtent()));
        const gp_Pln slabPlane(gp_Ax3(plane.origin(), plane.normal(), plane.horizontalAxis()));
        const TopoDS_Face slabFace =
            BRepBuilderAPI_MakeFace(slabPlane, umin - margin, umax + margin, vmin - margin, vmax + margin).Face();
        const cad::modeling::FeatureResult slab = cad::modeling::extrudeFace(slabFace, gp_Vec(dir) * length);
        if (!slab.ok)
        {
            r.message = "falha ao construir a região externa ao perfil: " + slab.message;
            return r;
        }
        const cad::modeling::FeatureResult outside = cad::modeling::booleanOperation(BooleanKind::Cut, slab.shape, prism.shape);
        if (!outside.ok)
        {
            r.message = "falha ao construir a região externa ao perfil: " + outside.message;
            return r;
        }
        cutter = outside.shape;
    }
    if (tool)
        *tool = cutter;
    if (p.mode == ExtrudeMode::Boss)
    {
        r = cad::modeling::booleanOperation(BooleanKind::Fuse, target, prism.shape);
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
        return r;
    }
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
        && updated.cutOutside == it->cutOutside)
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
        if (producesBody(op.type))
            bodies.insert(op.id);
    }
    return true;
}

bool CadDocument::resolveSupport(cad::sketch::Sketch& sketch, const TopoDS_Shape& body, std::string& error)
{
    if (!sketch.support())
        return true;
    cad::sketch::SketchSupport support = *sketch.support();
    const cad::modeling::FaceMatch m = cad::modeling::resolvePlanarFace(body, support.face);
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
    sketch.setPlane(cad::sketch::SketchPlane::forPlane(*plane));
    support.face = cad::modeling::describeFace(m.face); // referência acompanha a geometria
    sketch.setSupport(support);
    return true;
}

bool CadDocument::rebuild(std::vector<Operation>& operations, std::vector<cad::sketch::Sketch>& sketches,
                          TopoDS_Shape& shape, std::string& error, std::vector<SketchIssue>* issues)
{
    if (!checkReferences(operations, error))
        return false;

    TopoDS_Shape part; // bloco (+ furo)
    double length = 0.0, width = 0.0;
    size_t current = 0;
    // Somente resultados de operações anteriores à corrente (ordem de dependência).
    auto resultOf = [&operations, &current](const std::string& id) {
        for (size_t i = 0; i < current && i < operations.size(); ++i)
            if (operations[i].id == id)
                return operations[i].result;
        return TopoDS_Shape();
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
        if (!resolveSupport(s, body, message))
            return false;
        resolvedSketches.insert(s.id());
        return true;
    };
    // Uma falha não interrompe as operações independentes: só as que dependem (corpo de destino,
    // de origem, operandos, corpo de suporte do esboço ou o bloco do furo) ficam bloqueadas.
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
        op.result.Nullify();
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
            // Esboço sobre face: plano recalculado a partir da face no corpo de suporte.
            if (!resolveSketchSupport(*it, r.message))
                break;
            const ExtrudeParameters p = extrudeParametersOf(op);
            r = computeExtrusion(p, *it, op.extrudeMode == ExtrudeMode::NewBody ? TopoDS_Shape() : resultOf(op.targetId),
                                 nullptr);
            break;
        }
        case OperationType::Boolean:
            r = cad::modeling::booleanOperation(op.booleanKind, resultOf(op.operandA), resultOf(op.operandB));
            break;
        case OperationType::Fillet3D:
        case OperationType::Chamfer3D:
            r = computeFinish(op, resultOf(op.sourceId));
            break;
        }
        ++current;
        op.status = r.ok ? OperationStatus::Valid : OperationStatus::Invalid;
        op.statusMessage = r.message;
        if (r.ok)
            op.result = r.shape;
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
        if (issues)
            issues->push_back(SketchIssue{s.id(), message});
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
                                   TopoDS_Shape& shape, std::string& error, std::vector<SketchIssue>& issues) const
{
    const bool tolerant = hasInvalidOperations() || !m_sketchIssues.empty();
    // Referências estruturais (operandos inexistentes, ciclos) nunca são aceitas.
    std::vector<Operation> check = operations;
    if (!checkReferences(check, error))
        return false;
    issues.clear();
    if (rebuild(operations, sketches, shape, error, tolerant ? &issues : nullptr))
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
    for (const SketchIssue& issue : issues)
        if (std::none_of(m_sketchIssues.begin(), m_sketchIssues.end(),
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

cad::modeling::FeatureResult CadDocument::computeFinish(Operation& op, const TopoDS_Shape& source)
{
    cad::modeling::FeatureResult r;
    std::vector<TopoDS_Edge> found;
    for (size_t i = 0; i < op.edges.size(); ++i)
    {
        const cad::modeling::EdgeMatch m = cad::modeling::resolveEdge(source, op.edges[i]);
        if (!m.ok)
        {
            r.message = "aresta " + std::to_string(i + 1) + " — " + m.message;
            return r;
        }
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
        // Referências acompanham a geometria do corpo de origem.
        for (size_t i = 0; i < op.edges.size(); ++i)
            op.edges[i] = cad::modeling::describeEdge(source, found[i]);
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
    s.setSupport(cad::sketch::SketchSupport{bodyId, cad::modeling::describeFace(f)});
    return s;
}

TopoDS_Face CadDocument::supportFace(const std::string& sketchId) const
{
    const cad::sketch::Sketch* s = findSketch(sketchId);
    const Operation* body = s && s->support() ? findOperation(s->support()->bodyId) : nullptr;
    if (!body)
        return TopoDS_Face();
    const cad::modeling::FaceMatch m = cad::modeling::resolvePlanarFace(body->result, s->support()->face);
    return m.ok ? m.face : TopoDS_Face();
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
        signatures.push_back(cad::modeling::describeEdge(body->result, e));
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
    return computeFinish(op, source->result);
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
        const cad::modeling::EdgeMatch m = cad::modeling::resolveEdge(source->result, s);
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
    std::vector<SketchIssue> issues;
    if (!rebuildForCommit(operations, sketches, shape, error, issues))
        return ValidationResult{false, field, error};
    m_sketchIssues = std::move(issues);
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
