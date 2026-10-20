// Documento CAD paramétrico: parâmetros, sequência ordenada de operações, geometria
// resultante e esboços 2D. Núcleo sem dependência de Qt; a geometria é construída pelo
// Open CASCADE. Os esboços (E5.2) não alteram o sólido.
#pragma once

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
    ThroughHole  // furo passante
};

enum class OperationStatus
{
    Valid,
    Invalid
};

struct OperationParameter
{
    std::string key; // nome estável usado no arquivo (ex.: "length")
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

    // Valor do parâmetro 'key' (0 se ausente).
    double parameter(const std::string& key) const;
};

struct ValidationResult
{
    bool ok = true;
    std::string field;   // parâmetro que causou a falha (ex.: "holeRadius")
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

    // Documento vazio: sem operações, sem geometria e sem esboços (E5.2.1).
    CadDocument();
    explicit CadDocument(std::string name);

    // Documento de exemplo/teste com a peça paramétrica (bloco com furo passante). Nunca é
    // criado automaticamente pela aplicação; parâmetros inválidos usam a peça de referência.
    static CadDocument withTestPart(std::string name = "Peça de teste",
                                    const PartParameters& parameters = PartParameters());

    // Valida as dimensões e as relações entre bloco e furo (sem construir geometria).
    static ValidationResult validate(const PartParameters& parameters);

    // Constrói um documento a partir de dados persistidos. Em caso de falha retorna false e
    // não altera 'out'. Os identificadores são preservados.
    // 'part' vazio = documento sem operações (ids ignorados).
    static bool fromData(const std::string& name, const std::optional<PartParameters>& part,
                         const std::string& blockId, const std::string& holeId, CadDocument& out,
                         std::string& error, const std::vector<cad::sketch::Sketch>& sketches = {},
                         int nextSketchNumber = 1);

    const std::string& name() const { return m_name; }
    void setName(const std::string& name);

    const char* unit() const { return kLengthUnit; }

    const std::vector<Operation>& operations() const { return m_operations; }
    const Operation* findOperation(const std::string& id) const;

    // true se o documento contém a peça paramétrica (bloco base + furo passante).
    bool hasPart() const { return !m_operations.empty(); }

    // Parâmetros da peça (valores de referência se o documento não tiver peça).
    PartParameters parameters() const;

    // Valida e recalcula. Se os parâmetros forem inválidos ou a construção falhar, o documento
    // (parâmetros, operações e geometria) permanece no último estado válido. Recusado em
    // documento sem peça.
    ValidationResult setParameters(const PartParameters& parameters);

    // Geometria resultante do último recálculo válido (nula em documento sem operações).
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
    // Inclui (id novo) ou substitui (id existente) o esboço. Valida as entidades; em falha o
    // documento não muda.
    bool commitSketch(const cad::sketch::Sketch& sketch, std::string& error);
    bool removeSketch(const std::string& id);
    // Próximo número de esboço; nunca diminui (ids não reutilizados).
    int nextSketchNumber() const { return m_nextSketchNumber; }

private:
    static std::vector<Operation> makeOperations(const PartParameters& p, const std::string& blockId,
                                                 const std::string& holeId);
    // Recalcula a sequência de operações; preenche o estado de cada operação.
    static bool rebuild(std::vector<Operation>& operations, TopoDS_Shape& shape, std::string& error);

    std::string m_name;
    std::vector<Operation> m_operations;
    TopoDS_Shape m_shape;
    bool m_modified = false;
    int m_revision = 0;
    std::vector<cad::sketch::Sketch> m_sketches;
    int m_nextSketchNumber = 1;
};

// Número N de um identificador de esboço "sk-N" (0 se o formato for outro).
int sketchNumber(const std::string& sketchId);

// Identificador estável: letras, dígitos, '-' e '_' (1 a 64 caracteres).
bool isValidIdentifier(const std::string& id);

} // namespace cad::document
