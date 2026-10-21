// Documento CAD paramétrico: parâmetros, sequência ordenada de operações, geometria
// resultante e esboços 2D. Núcleo sem dependência de Qt; a geometria é construída pelo
// Open CASCADE.
//
// Operações: a peça de exemplo (bloco base + furo passante, opcional), extrusões de esboços
// (E5.3) e operações booleanas (E5.4). Operações que produzem sólido são "corpos" (furo passante
// = peça, extrusão, booleana). Uma booleana referencia os corpos A e B pelos ids persistentes e os
// consome: eles continuam no histórico, mas deixam de ser exibidos e exportados. Cada corpo só
// pode ser consumido uma vez e só por uma operação posterior (a ordem da lista impede ciclos).
// O resultado do documento são os corpos ativos (não consumidos): o sólido único ou um
// TopoDS_Compound com os corpos lado a lado (sem união implícita).
// Fillet 3D e Chamfer 3D (E5.5B) também consomem o corpo de origem e referenciam as arestas por
// assinaturas geométricas persistentes (EdgeNaming), remapeadas a cada reconstrução; aresta não
// identificável com segurança invalida a operação (a alteração que a causou é recusada).
// Toda alteração recalcula as operações em ordem de dependência numa cópia; o documento só muda
// se tudo for válido.
#pragma once

#include "modeling/EdgeNaming.h"
#include "modeling/Features.h"
#include "sketch/Sketch.h"

#include <TopoDS_Shape.hxx>

#include <optional>
#include <string>
#include <vector>

namespace cad::document {

// Unidade de comprimento de todos os parâmetros do documento.
inline constexpr const char* kLengthUnit = "mm";

// Parâmetros da peça (bloco com furo passante vertical centrado), em mm.
struct PartParameters
{
    double length = 100.0;    // X
    double width = 60.0;      // Y
    double height = 20.0;     // Z
    double holeRadius = 10.0; // furo centrado em (length/2, width/2)

    bool operator==(const PartParameters& o) const
    {
        return length == o.length && width == o.width && height == o.height && holeRadius == o.holeRadius;
    }
    bool operator!=(const PartParameters& o) const { return !(*this == o); }
};

enum class OperationType
{
    Block,       // bloco base
    ThroughHole, // furo passante
    Extrude,     // extrusão de esboço (E5.3)
    Boolean,     // união, subtração ou interseção de dois corpos (E5.4)
    Fillet3D,    // arredondamento de arestas de um corpo (E5.5B)
    Chamfer3D    // chanfro simétrico de arestas de um corpo (E5.5B)
};

using cad::modeling::BooleanKind;

enum class OperationStatus
{
    Valid,
    Invalid
};

struct OperationParameter
{
    std::string key; // nome estável usado no arquivo (ex.: "length", "depth")
    double value = 0.0;
};

struct Operation
{
    std::string id; // identificador estável (persistido; não muda ao editar parâmetros)
    OperationType type = OperationType::Block;
    std::string name;
    std::vector<OperationParameter> parameters;
    OperationStatus status = OperationStatus::Valid;
    std::string statusMessage;

    // Extrusão: esboço de origem e sentido (false = normal do plano; true = oposto).
    std::string sketchId;
    bool reversed = false;

    // Booleana: tipo e corpos operandos (ids de operações anteriores). Na subtração A é o corpo
    // principal e B a ferramenta de corte.
    BooleanKind booleanKind = BooleanKind::Fuse;
    std::string operandA;
    std::string operandB;

    // Fillet 3D / Chamfer 3D: corpo de origem (consumido) e arestas (assinaturas persistentes,
    // ver EdgeNaming). Parâmetro "radius" ou "distance".
    std::string sourceId;
    std::vector<cad::modeling::EdgeSignature> edges;

    // Geometria resultante da operação (calculada; não persistida).
    TopoDS_Shape result;
    // Operação posterior que consome este corpo (calculado; vazio = corpo ativo).
    std::string consumedBy;

    // Valor do parâmetro 'key' (0 se ausente).
    double parameter(const std::string& key) const;
};

// Parâmetros de uma extrusão.
struct ExtrudeParameters
{
    std::string name;     // vazio = nome padrão ("Extrusão N")
    std::string sketchId; // esboço de origem
    double depth = 10.0;  // mm, > 0
    bool reversed = false;
};

// Parâmetros de uma operação booleana.
struct BooleanParameters
{
    std::string name; // vazio = nome padrão ("União N", "Subtração N", "Interseção N")
    BooleanKind kind = BooleanKind::Fuse;
    std::string operandA;
    std::string operandB;
};

// Parâmetros de Fillet 3D (value = raio) ou Chamfer 3D (value = distância).
struct FinishParameters
{
    std::string name;     // vazio = nome padrão ("Fillet 3D N", "Chamfer 3D N")
    std::string sourceId; // corpo de origem
    double value = 1.0;   // mm, > 0
    std::vector<cad::modeling::EdgeSignature> edges;
};

struct ValidationResult
{
    bool ok = true;
    std::string field;   // parâmetro que causou a falha (ex.: "holeRadius", "depth")
    std::string message; // mensagem para o usuário
};

class CadDocument
{
public:
    // Limites geométricos aceitos (mm).
    static constexpr double kMinDimension = 0.1;
    static constexpr double kMaxDimension = 10000.0;
    static constexpr double kMinHoleRadius = 0.05;
    static constexpr double kMinWall = 1.0; // parede mínima entre o furo e as faces laterais
    static constexpr double kMinExtrudeDepth = 0.01;
    static constexpr double kMaxExtrudeDepth = 10000.0;
    static constexpr size_t kMaxNameLength = 200;

    // Documento vazio: sem operações, sem geometria e sem esboços (E5.2.1).
    CadDocument();
    explicit CadDocument(std::string name);

    // Documento de exemplo/teste com a peça paramétrica (bloco com furo passante). Nunca é
    // criado automaticamente pela aplicação; parâmetros inválidos usam a peça de referência.
    static CadDocument withTestPart(std::string name = "Peça de teste",
                                    const PartParameters& parameters = PartParameters());

    // Valida as dimensões e as relações entre bloco e furo (sem construir geometria).
    static ValidationResult validate(const PartParameters& parameters);
    // Valida profundidade e nome (não verifica o esboço).
    static ValidationResult validateExtrude(const ExtrudeParameters& parameters);

    // Dados persistidos (leitura do .cadmec).
    struct Data
    {
        std::string name;
        std::optional<PartParameters> part; // bloco + furo (opcional)
        std::string blockId = "op-1";
        std::string holeId = "op-2";
        // Operações após a peça, em ordem: id, tipo, nome e campos de definição (extrusão:
        // sketchId/reversed/"depth"; booleana: booleanKind/operandA/operandB; acabamento 3D:
        // sourceId/edges/"radius" ou "distance").
        std::vector<Operation> features;
        std::vector<cad::sketch::Sketch> sketches;
        int nextSketchNumber = 1;
        int nextOperationNumber = 0; // 0 = calcular a partir dos ids
    };
    // Constrói e recalcula a partir de dados persistidos. Em falha retorna false e não altera
    // 'out'. Os identificadores são preservados.
    static bool fromData(const Data& data, CadDocument& out, std::string& error);
    // Compatibilidade (E5.1/E5.2): peça opcional e esboços, sem extrusões.
    static bool fromData(const std::string& name, const std::optional<PartParameters>& part,
                         const std::string& blockId, const std::string& holeId, CadDocument& out,
                         std::string& error, const std::vector<cad::sketch::Sketch>& sketches = {},
                         int nextSketchNumber = 1);

    const std::string& name() const { return m_name; }
    void setName(const std::string& name);

    const char* unit() const { return kLengthUnit; }

    const std::vector<Operation>& operations() const { return m_operations; }
    const Operation* findOperation(const std::string& id) const;
    // Próximo número de operação ("op-N"); nunca diminui.
    int nextOperationNumber() const { return m_nextOperationNumber; }

    // true se o documento contém a peça paramétrica (bloco base + furo passante).
    bool hasPart() const;
    // Número de sólidos no resultado do documento.
    int solidCount() const;

    // Parâmetros da peça (valores de referência se o documento não tiver peça).
    PartParameters parameters() const;

    // Valida e recalcula. Se os parâmetros forem inválidos ou a construção falhar, o documento
    // (parâmetros, operações e geometria) permanece no último estado válido. Recusado em
    // documento sem peça.
    ValidationResult setParameters(const PartParameters& parameters);

    // --- Extrusões (E5.3) -----------------------------------------------------
    // Sólido que a extrusão produziria, sem alterar o documento (pré-visualização).
    cad::modeling::FeatureResult previewExtrusion(const ExtrudeParameters& parameters) const;
    // Acrescenta a extrusão ao final da sequência e recalcula. Em falha nada muda.
    ValidationResult addExtrusion(const ExtrudeParameters& parameters, std::string* newId = nullptr);
    // Altera nome, profundidade, sentido (e esboço) de uma extrusão existente.
    ValidationResult updateExtrusion(const std::string& id, const ExtrudeParameters& parameters);
    ExtrudeParameters extrudeParameters(const std::string& id) const;
    // Extrusões que dependem do esboço.
    std::vector<std::string> dependentOperations(const std::string& sketchId) const;
    std::string defaultExtrusionName() const;

    // --- Corpos e operações booleanas (E5.4) -------------------------------------
    static bool producesBody(OperationType type);
    // Corpos ativos (não consumidos), na ordem do histórico.
    std::vector<std::string> activeBodies() const;
    // Operação que consome o corpo (vazio se ativo ou inexistente).
    std::string consumerOf(const std::string& bodyId) const;
    // Resultado que a booleana produziria, sem alterar o documento.
    cad::modeling::FeatureResult previewBoolean(const BooleanParameters& parameters) const;
    // Acrescenta a booleana (consome A e B) e recalcula. Em falha nada muda.
    ValidationResult addBoolean(const BooleanParameters& parameters, std::string* newId = nullptr);
    // Altera tipo, operandos e nome; recalcula a booleana e as operações seguintes.
    ValidationResult updateBoolean(const std::string& id, const BooleanParameters& parameters);
    BooleanParameters booleanParameters(const std::string& id) const;
    std::string defaultBooleanName(BooleanKind kind) const;

    // --- Acabamentos 3D (E5.5B) ---------------------------------------------------
    // Assinaturas das arestas (do corpo ativo 'bodyId') para usar em FinishParameters.
    std::vector<cad::modeling::EdgeSignature> edgeSignatures(const std::string& bodyId,
                                                            const std::vector<TopoDS_Edge>& edges) const;
    // Corpo ativo que contém a aresta (vazio se nenhum).
    std::string bodyContaining(const TopoDS_Shape& edge) const;
    cad::modeling::FeatureResult previewFinish(OperationType type, const FinishParameters& parameters) const;
    ValidationResult addFinish(OperationType type, const FinishParameters& parameters, std::string* newId = nullptr);
    // Altera valor, arestas e nome (o tipo e o corpo de origem são mantidos).
    ValidationResult updateFinish(const std::string& id, const FinishParameters& parameters);
    FinishParameters finishParameters(const std::string& id) const;
    std::string defaultFinishName(OperationType type) const;
    // Arestas atuais do corpo de origem correspondentes às referências da operação.
    std::vector<TopoDS_Edge> resolvedFinishEdges(const std::string& id) const;

    // Geometria resultante do último recálculo válido (nula em documento sem sólidos).
    const TopoDS_Shape& shape() const { return m_shape; }

    bool isModified() const { return m_modified; }
    void setModified(bool modified) { m_modified = modified; }

    // Incrementado a cada recálculo bem-sucedido.
    int revision() const { return m_revision; }

    // --- Esboços --------------------------------------------------------------
    const std::vector<cad::sketch::Sketch>& sketches() const { return m_sketches; }
    const cad::sketch::Sketch* findSketch(const std::string& id) const;
    // Esboço vazio com novo id ("sk-N") e nome ("Esboço N"), ainda não incluído no documento.
    cad::sketch::Sketch createSketch(cad::sketch::PlaneType plane) const;
    // Inclui (id novo) ou substitui (id existente) o esboço e recalcula as extrusões
    // dependentes. Se alguma entidade for inválida ou alguma extrusão dependente falhar, o
    // documento não muda e 'error' explica o motivo.
    bool commitSketch(const cad::sketch::Sketch& sketch, std::string& error);
    // Remove o esboço; recusado se alguma extrusão depender dele.
    bool removeSketch(const std::string& id);
    bool removeSketch(const std::string& id, std::string& error);
    // Próximo número de esboço; nunca diminui (ids não reutilizados).
    int nextSketchNumber() const { return m_nextSketchNumber; }

private:
    static std::vector<Operation> makeOperations(const PartParameters& p, const std::string& blockId,
                                                 const std::string& holeId);
    static Operation makeExtrusion(const std::string& id, const ExtrudeParameters& parameters);
    static Operation makeBoolean(const std::string& id, const BooleanParameters& parameters);
    static Operation makeFinish(const std::string& id, OperationType type, const FinishParameters& parameters);
    // Calcula um acabamento a partir do sólido de origem e das assinaturas (atualizadas para as
    // arestas encontradas).
    static cad::modeling::FeatureResult computeFinish(OperationType type, const TopoDS_Shape& source, double value,
                                                      std::vector<cad::modeling::EdgeSignature>& edges);
    // Referências entre operações: operandos existentes, anteriores, de corpos, distintos e
    // consumidos uma única vez. Preenche consumedBy.
    static bool checkReferences(std::vector<Operation>& operations, std::string& error);
    ValidationResult applyOperations(std::vector<Operation> operations, const char* field);
    // Recalcula a sequência de operações (em ordem) com os esboços dados; preenche o estado e o
    // resultado de cada operação e o resultado combinado.
    static bool rebuild(std::vector<Operation>& operations, const std::vector<cad::sketch::Sketch>& sketches,
                        TopoDS_Shape& shape, std::string& error);
    // Aplica um novo estado (já recalculado) e marca o documento como modificado.
    void commit(std::vector<Operation> operations, TopoDS_Shape shape);

    std::string m_name;
    std::vector<Operation> m_operations;
    TopoDS_Shape m_shape;
    bool m_modified = false;
    int m_revision = 0;
    std::vector<cad::sketch::Sketch> m_sketches;
    int m_nextSketchNumber = 1;
    int m_nextOperationNumber = 1;
};

// Identificador estável: letras, dígitos, '-' e '_' (1 a 64 caracteres).
bool isValidIdentifier(const std::string& id);

// Número N de um identificador de esboço "sk-N" (0 se o formato for outro).
int sketchNumber(const std::string& sketchId);
// Número N de um identificador de operação "op-N" (0 se o formato for outro).
int operationNumber(const std::string& operationId);

// Nome da booleana para a interface ("União", "Subtração", "Interseção").
const char* booleanKindLabel(BooleanKind kind);

} // namespace cad::document
