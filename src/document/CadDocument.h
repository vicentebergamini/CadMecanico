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
//
// Propriedades MED (E5.7.4a): cada documento guarda opcionalmente as propriedades físicas do
// modelo MED (SI, ver dem/DemProperties.h). Elas não participam da geometria nem do recálculo.
#pragma once

#include "dem/DemProperties.h"
#include "modeling/EdgeNaming.h"
#include "modeling/Features.h"
#include "modeling/TopoNaming.h"
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

// Modo da extrusão (E5.6): novo corpo independente (comportamento da E5.3), saliência (volume
// unido ao corpo de destino) ou corte (volume subtraído do corpo de destino).
enum class ExtrudeMode
{
    NewBody,
    Boss,
    Cut
};

// Condição final da extrusão (E5.6; ampliada na E5.7.1 — P5). Extensões medidas a partir do
// plano do esboço ao longo do sentido da extrusão:
//   Blind          profundidade definida [0, d];
//   MidPlane       plano médio, simétrico ao plano do esboço [−d/2, d/2];
//   ThroughAll     atravessar todo o corpo de destino no sentido escolhido;
//   ThroughAllBoth atravessar todo o corpo de destino nos dois sentidos;
//   UpToNext       até a próxima face do corpo de destino: o corte remove a primeira região de
//                  material encontrada; a saliência cresce até encontrar o corpo;
//   UpToFace       até a face plana selecionada (plano da face estendido), em qualquer corpo.
// Comprimentos dependentes da geometria são calculados a cada reconstrução (nunca fixos).
enum class EndCondition
{
    Blind,
    ThroughAll,
    ThroughAllBoth,
    UpToNext,
    UpToFace,
    MidPlane
};

// Modo do Chamfer 3D (E5.6): distâncias iguais (E5.5), duas distâncias (D1 na face de
// referência, D2 na outra) ou distância e ângulo (D1 na face de referência; ângulo entre a face
// de referência e a face do chanfro).
enum class Chamfer3DMode
{
    Equal,
    TwoDistances,
    DistanceAngle
};

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

    // Extrusão: esboço de origem e sentido (false = normal do plano; true = oposto; no corte o
    // sentido padrão é para dentro da face, oposto à normal), modo, corpo de destino (saliência
    // e corte) e condição final.
    std::string sketchId;
    bool reversed = false;
    ExtrudeMode extrudeMode = ExtrudeMode::NewBody;
    std::string targetId;
    EndCondition endCondition = EndCondition::Blind;
    // Corte: remover o lado de fora do perfil (E5.7, ver ExtrudeParameters::cutOutside).
    bool cutOutside = false;
    // P5 (E5.7.1): direção 2 (parâmetro "depth2") e face limite de "até a face".
    bool direction2 = false;
    EndCondition endCondition2 = EndCondition::Blind;
    std::string limitBodyId;
    cad::modeling::FaceSignature limitFace;

    // Booleana: tipo e corpos operandos (ids de operações anteriores). Na subtração A é o corpo
    // principal e B a ferramenta de corte.
    BooleanKind booleanKind = BooleanKind::Fuse;
    std::string operandA;
    std::string operandB;

    // Fillet 3D / Chamfer 3D: corpo de origem (consumido) e arestas (assinaturas persistentes,
    // ver EdgeNaming). Parâmetro "radius" ou "distance".
    std::string sourceId;
    std::vector<cad::modeling::EdgeSignature> edges;
    // Chamfer 3D avançado: modo e normal da face de referência de cada aresta (parâmetros
    // "distance", "distance2" e "angle").
    Chamfer3DMode chamferMode = Chamfer3DMode::Equal;
    std::vector<gp_Dir> referenceNormals;

    // Geometria resultante da operação (calculada; não persistida).
    TopoDS_Shape result;
    // Operação posterior que consome este corpo (calculado; vazio = corpo ativo).
    std::string consumedBy;
    // Operação inválida da qual esta depende (calculado; vazio = a falha, se houver, é própria).
    std::string blockedBy;
    // Nomes topológicos das faces do resultado (calculado; ver TopoNaming) e avisos de
    // referências recuperadas só pela geometria (calculado).
    cad::modeling::FaceNames faceNames;
    std::vector<std::string> notes;

    // Valor do parâmetro 'key' (0 se ausente).
    double parameter(const std::string& key) const;
};

// Parâmetros de uma extrusão.
struct ExtrudeParameters
{
    std::string name;     // vazio = nome padrão ("Extrusão N" / "Extrusão de Saliência N" / ...)
    std::string sketchId; // esboço de origem
    double depth = 10.0;  // mm, > 0 (ignorada em "atravessar tudo")
    bool reversed = false;
    ExtrudeMode mode = ExtrudeMode::NewBody;
    std::string targetId; // corpo de destino (saliência e corte)
    EndCondition endCondition = EndCondition::Blind;
    // Somente corte (E5.7, "Cortar o lado de fora"): remove do corpo de destino o material FORA
    // do perfil dentro da extensão da extrusão (mesmo sentido e condição final); o material
    // dentro do perfil e o material além da extensão são preservados. Ferramenta = laje do plano
    // do esboço cobrindo o corpo, varrida pela extensão, menos o prisma do perfil. Somente com as
    // condições prismáticas (profundidade, plano médio, atravessar tudo, dois sentidos).
    bool cutOutside = false;
    // Direção 2 (E5.7.1): extensão adicional no sentido oposto ao da direção 1, com condição
    // própria (profundidade definida ou atravessar tudo). Não combina com plano médio nem com
    // atravessar tudo nos dois sentidos.
    bool direction2 = false;
    EndCondition endCondition2 = EndCondition::Blind;
    double depth2 = 10.0;
    // Até a face (E5.7.1): corpo e referência persistente (assinatura + nome) da face limite.
    std::string limitBodyId;
    cad::modeling::FaceSignature limitFace;
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
    double value = 1.0;   // mm, > 0 (raio, distância ou D1)
    std::vector<cad::modeling::EdgeSignature> edges;
    // Chamfer 3D avançado (E5.6).
    Chamfer3DMode chamferMode = Chamfer3DMode::Equal;
    double value2 = 1.0;  // D2 (mm)
    double angle = 45.0;  // graus
    std::vector<gp_Dir> referenceNormals; // uma por aresta (vazio = escolha padrão)
};

struct ValidationResult
{
    bool ok = true;
    std::string field;   // parâmetro que causou a falha (ex.: "holeRadius", "depth")
    std::string message; // mensagem para o usuário
};

// Esboço sobre face cuja face de suporte não pôde ser recuperada (documento aberto com erros), ou
// aviso de referência recuperada só pela geometria (E5.7.1).
struct SketchIssue
{
    std::string sketchId;
    std::string message;
};

// Registro de uma reconstrução: esboços sem suporte (documento com erros) e avisos de suporte
// recuperado pela geometria em vez do nome topológico.
struct RebuildLog
{
    std::vector<SketchIssue> sketchIssues;
    std::vector<SketchIssue> sketchNotes;
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
        // Propriedades MED (E5.7.4a); ausentes = não definidas (documentos antigos).
        std::optional<cad::dem::DemProperties> demProperties;
    };
    // Constrói e recalcula a partir de dados persistidos. Em falha retorna false e não altera
    // 'out'. Os identificadores são preservados. Com 'allowInvalid' (E5.7, abertura com erros),
    // falhas de reconstrução não impedem a abertura: as operações ficam marcadas como inválidas,
    // com mensagem, e nada é descartado nem alterado; dados estruturalmente inválidos continuam
    // recusados.
    static bool fromData(const Data& data, CadDocument& out, std::string& error, bool allowInvalid = false);
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
    // 'editingId': extrusão em edição (o corpo de destino consumido por ela continua disponível).
    cad::modeling::FeatureResult previewExtrusion(const ExtrudeParameters& parameters,
                                                  const std::string& editingId = std::string()) const;
    // Acrescenta a extrusão ao final da sequência e recalcula. Em falha nada muda.
    ValidationResult addExtrusion(const ExtrudeParameters& parameters, std::string* newId = nullptr);
    // Altera nome, profundidade, sentido (e esboço) de uma extrusão existente.
    ValidationResult updateExtrusion(const std::string& id, const ExtrudeParameters& parameters);
    ExtrudeParameters extrudeParameters(const std::string& id) const;
    // Extrusões que dependem do esboço.
    std::vector<std::string> dependentOperations(const std::string& sketchId) const;
    std::string defaultExtrusionName(ExtrudeMode mode = ExtrudeMode::NewBody) const;
    // Ferramenta da extrusão (prisma do perfil) com os parâmetros dados — pré-visualização da
    // região adicionada/removida.
    cad::modeling::FeatureResult extrusionTool(const ExtrudeParameters& parameters) const;

    // --- Esboços sobre faces (E5.6) ------------------------------------------------
    // Novo esboço (não incluído no documento) sobre a face plana 'face' do corpo 'bodyId'. Vazio
    // com 'error' se a face não for plana ou não pertencer ao corpo.
    std::optional<cad::sketch::Sketch> createFaceSketch(const std::string& bodyId, const TopoDS_Shape& face,
                                                        std::string& error) const;
    // Corpo (qualquer, ativo de preferência) que contém a face.
    std::string bodyContainingFace(const TopoDS_Shape& face) const;
    // Face atual de suporte de um esboço sobre face (nula se não houver).
    TopoDS_Face supportFace(const std::string& sketchId) const;

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
    // Normais das faces de referência padrão das arestas (ver Chamfer3DMode); 'flip' escolhe a
    // outra face adjacente.
    std::vector<gp_Dir> referenceNormals(const std::string& bodyId, const std::vector<TopoDS_Edge>& edges,
                                         bool flip) const;

    // Geometria resultante do último recálculo válido (nula em documento sem sólidos). Em
    // documento com operações inválidas: os corpos válidos no estado anterior à primeira falha
    // de cada cadeia.
    const TopoDS_Shape& shape() const { return m_shape; }

    // --- Diagnóstico (E5.7) -----------------------------------------------------------
    // true se alguma operação está inválida (documento aberto com erros ou ainda não corrigido).
    bool hasInvalidOperations() const;
    // Esboços sobre face sem face de suporte recuperável (somente em documento com erros).
    const std::vector<SketchIssue>& sketchIssues() const { return m_log.sketchIssues; }
    // Esboços cuja face de suporte foi recuperada só pela geometria (avisos, E5.7.1).
    const std::vector<SketchIssue>& sketchNotes() const { return m_log.sketchNotes; }

    // --- Referências topológicas (E5.7.1) -----------------------------------------------
    // Nome topológico atual de uma face/aresta do corpo (vazio se o corpo não existir).
    std::string faceName(const std::string& bodyId, const TopoDS_Shape& face) const;
    std::string edgeName(const std::string& bodyId, const TopoDS_Edge& edge) const;
    // Referência persistente (assinatura + nome) de uma face plana de um corpo, para "até a face".
    std::optional<cad::modeling::FaceSignature> faceReference(const std::string& bodyId, const TopoDS_Shape& face) const;
    // Face atual correspondente à referência (nula se não houver correspondência segura).
    TopoDS_Face resolveFaceReference(const std::string& bodyId, const cad::modeling::FaceSignature& face,
                                     std::string* error = nullptr) const;
    // Face limite escolhida na peça exibida durante a edição de uma operação: a peça exibida já
    // contém o resultado da própria operação (ou de posteriores). Devolve o corpo mais recente
    // ANTERIOR à operação que contém a mesma face (mesmo nome e mesmo plano), com a referência
    // refeita nesse corpo; nulo se a face só existe a partir da operação editada.
    std::optional<std::pair<std::string, cad::modeling::FaceSignature>>
    limitFaceBefore(const std::string& operationId, const std::string& bodyId,
                    const cad::modeling::FaceSignature& face) const;

    // --- Propriedades físicas do modelo MED (E5.7.4a) ----------------------------------------
    // Vazio = não definidas (documento novo ou anterior à E5.7.4a).
    const std::optional<cad::dem::DemProperties>& demProperties() const { return m_demProperties; }
    // Valida (cad::dem::validate) e grava no documento; em falha nada muda. Marca o documento como
    // modificado só se os valores mudarem. A geometria não é recalculada.
    cad::dem::PropertyValidation setDemProperties(const cad::dem::DemProperties& properties);

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
    static ExtrudeParameters extrudeParametersOf(const Operation& op);
    static Operation makeBoolean(const std::string& id, const BooleanParameters& parameters);
    static Operation makeFinish(const std::string& id, OperationType type, const FinishParameters& parameters);
    static ValidationResult validateChamferMode(OperationType type, const FinishParameters& parameters);
    // Calcula um acabamento a partir do sólido de origem e das referências (atualizadas para as
    // arestas encontradas); 'names' recebe os nomes das faces do resultado.
    static cad::modeling::FeatureResult computeFinish(Operation& op, const TopoDS_Shape& source,
                                                      const cad::modeling::FaceNames* sourceNames,
                                                      cad::modeling::FaceNames* names = nullptr);
    // Entradas da extrusão: corpo de destino (saliência/corte) com os nomes das faces, face limite
    // ("até a face") já resolvida e id da operação (prefixo dos nomes novos).
    struct ExtrusionInput
    {
        TopoDS_Shape target;
        const cad::modeling::FaceNames* targetNames = nullptr;
        TopoDS_Face limitFace;
        std::string opId = "extrusao";
    };
    // Extrusão de qualquer modo e condição final a partir do esboço (plano já resolvido); 'tool'
    // recebe o volume adicionado/removido e 'names' os nomes das faces do resultado.
    static cad::modeling::FeatureResult computeExtrusion(const ExtrudeParameters& p, const cad::sketch::Sketch& sketch,
                                                         const ExtrusionInput& input, TopoDS_Shape* tool,
                                                         cad::modeling::FaceNames* names = nullptr);
    // Entradas da extrusão a partir do documento atual (pré-visualização).
    bool extrusionInput(const ExtrudeParameters& p, const std::string& editingId, ExtrusionInput& input,
                        std::string& error, bool checkConsumed = true) const;
    // Atualiza o plano de um esboço sobre face a partir do corpo de origem (pelo nome topológico,
    // com a geometria como desempate/legado). 'note' recebe o aviso de recuperação geométrica.
    static bool resolveSupport(cad::sketch::Sketch& sketch, const TopoDS_Shape& body,
                               const cad::modeling::FaceNames* names, std::string& error, std::string* note = nullptr);
    // Referências entre operações: operandos existentes, anteriores, de corpos, distintos e
    // consumidos uma única vez. Preenche consumedBy.
    static bool checkReferences(std::vector<Operation>& operations, std::string& error);
    ValidationResult applyOperations(std::vector<Operation> operations, const char* field);
    // Recalcula a sequência de operações (em ordem) com os esboços dados; preenche o estado e o
    // resultado de cada operação e o resultado combinado.
    // Os planos dos esboços sobre faces são atualizados em 'sketches' (cópia de trabalho).
    // Uma falha não interrompe o cálculo das operações independentes; as dependentes ficam
    // inválidas com 'blockedBy'. Retorna false se houver alguma falha ('error' = a primeira);
    // 'shape' recebe sempre os corpos válidos (estado anterior à falha de cada cadeia). Com
    // 'tolerant', esboços não usados cuja face de suporte se perdeu são registrados em 'log' em
    // vez de causar falha; avisos de recuperação geométrica vão para 'log' (se dado).
    static bool rebuild(std::vector<Operation>& operations, std::vector<cad::sketch::Sketch>& sketches,
                        TopoDS_Shape& shape, std::string& error, RebuildLog* log = nullptr, bool tolerant = false);
    // Recalcula e decide se o novo estado pode ser aplicado: válido, ou — em documento que já
    // tem operações inválidas — sem novas operações inválidas (permite corrigir uma de cada vez).
    bool rebuildForCommit(std::vector<Operation>& operations, std::vector<cad::sketch::Sketch>& sketches,
                          TopoDS_Shape& shape, std::string& error, RebuildLog& log) const;
    // Aplica um novo estado (já recalculado) e marca o documento como modificado.
    void commit(std::vector<Operation> operations, TopoDS_Shape shape);
    void commit(std::vector<Operation> operations, std::vector<cad::sketch::Sketch> sketches, TopoDS_Shape shape);

    std::string m_name;
    std::vector<Operation> m_operations;
    TopoDS_Shape m_shape;
    bool m_modified = false;
    int m_revision = 0;
    std::vector<cad::sketch::Sketch> m_sketches;
    RebuildLog m_log;
    int m_nextSketchNumber = 1;
    int m_nextOperationNumber = 1;
    std::optional<cad::dem::DemProperties> m_demProperties;
};

// Identificador estável: letras, dígitos, '-' e '_' (1 a 64 caracteres).
bool isValidIdentifier(const std::string& id);

// Número N de um identificador de esboço "sk-N" (0 se o formato for outro).
int sketchNumber(const std::string& sketchId);
// Número N de um identificador de operação "op-N" (0 se o formato for outro).
int operationNumber(const std::string& operationId);

// Nome da booleana para a interface ("União", "Subtração", "Interseção").
const char* booleanKindLabel(BooleanKind kind);
// Nome do modo de extrusão ("Novo corpo", "Saliência", "Corte").
const char* extrudeModeLabel(ExtrudeMode mode);
// Nome da condição final para a interface ("Profundidade definida", "Plano médio", ...).
const char* endConditionLabel(EndCondition condition);

} // namespace cad::document
