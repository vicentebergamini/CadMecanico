// Documento CAD paramétrico: parâmetros, sequência ordenada de operações e geometria
// resultante. Núcleo sem dependência de Qt; a geometria é construída pelo Open CASCADE.
#pragma once

#include <TopoDS_Shape.hxx>

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

    // Documento com a peça de referência (bloco 100x60x20 com furo R10).
    CadDocument();
    explicit CadDocument(std::string name, const PartParameters& parameters = PartParameters());

    // Valida as dimensões e as relações entre bloco e furo (sem construir geometria).
    static ValidationResult validate(const PartParameters& parameters);

    // Constrói um documento a partir de dados persistidos. Em caso de falha retorna false e
    // não altera 'out'. Os identificadores são preservados.
    static bool fromData(const std::string& name, const PartParameters& parameters, const std::string& blockId,
                         const std::string& holeId, CadDocument& out, std::string& error);

    const std::string& name() const { return m_name; }
    void setName(const std::string& name);

    const char* unit() const { return kLengthUnit; }

    const std::vector<Operation>& operations() const { return m_operations; }
    const Operation* findOperation(const std::string& id) const;

    PartParameters parameters() const;

    // Valida e recalcula. Se os parâmetros forem inválidos ou a construção falhar, o documento
    // (parâmetros, operações e geometria) permanece no último estado válido.
    ValidationResult setParameters(const PartParameters& parameters);

    // Geometria resultante do último recálculo válido.
    const TopoDS_Shape& shape() const { return m_shape; }

    bool isModified() const { return m_modified; }
    void setModified(bool modified) { m_modified = modified; }

    // Incrementado a cada recálculo bem-sucedido.
    int revision() const { return m_revision; }

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
};

// Identificador estável: letras, dígitos, '-' e '_' (1 a 64 caracteres).
bool isValidIdentifier(const std::string& id);

} // namespace cad::document
