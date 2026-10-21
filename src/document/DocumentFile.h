// Formato nativo .cadmec: JSON versionado com os parâmetros e a sequência de operações.
// Usa o QtCore (QJson*, QSaveFile), já dependência do projeto; o núcleo CadDocument não usa Qt.
//
// Esquema versão 1:
// {
//   "format": "cadmec",
//   "schemaVersion": 1,
//   "generator": "CadMecanico",
//   "document": {
//     "name": "Peça de teste",
//     "unit": "mm",
//     "operations": [
//       { "id": "op-1", "type": "block", "name": "Bloco base",
//         "parameters": { "length": 100, "width": 60, "height": 20 } },
//       { "id": "op-2", "type": "throughHole", "name": "Furo passante",
//         "parameters": { "radius": 10 } }
//     ]
//   }
// }
//
// Esquema versão 2 (E5.2): igual à versão 1, com dois campos novos em "document" (os campos da
// versão 1 mantêm o mesmo significado):
//     "nextSketchNumber": 3,
//     "sketches": [
//       { "id": "sk-1", "name": "Esboço 1", "plane": "XY", "nextEntityNumber": 4,
//         "entities": [
//           { "id": "e-1", "type": "line",      "x1": 0, "y1": 0, "x2": 50, "y2": 0 },
//           { "id": "e-2", "type": "rectangle", "x": 10, "y": 10, "width": 40, "height": 20 },
//           { "id": "e-3", "type": "circle",    "cx": 50, "cy": 30, "radius": 8 } ] } ]
// Coordenadas locais do plano, em mm.
//
// Esquema versão 3 (E5.2.1): mesmos campos da versão 2; "operations" pode ser vazia (documento
// sem sólido, por exemplo só com esboços). Nas versões 1 e 2 a lista continua exigindo
// exatamente bloco base + furo passante.
//
// Esquema versão 4 (E5.3): "operations" = par opcional bloco + furo no início, seguido de
// extrusões; novo campo "nextOperationNumber" em "document". Extrusão:
//     { "id": "op-3", "type": "extrude", "name": "Extrusão 1", "sketchId": "sk-1",
//       "reversed": false, "parameters": { "depth": 20 } }
// A profundidade é em mm; o sentido é a normal do plano do esboço (oposto se "reversed").
// A geometria é sempre reconstruída a partir desses dados (nenhuma malha ou objeto gráfico é
// salvo). Toda extrusão precisa referenciar um esboço existente no arquivo.
//
// Esquema versão 5 (E5.4): além das extrusões, "operations" aceita operações booleanas, que
// referenciam corpos anteriores pelos ids (a ordem do array é a ordem de reconstrução):
//     { "id": "op-3", "type": "boolean", "name": "Subtração 1", "operation": "cut",
//       "operandA": "op-1", "operandB": "op-2" }
// "operation" = "fuse" (A ∪ B), "cut" (A − B) ou "common" (A ∩ B). Os operandos são consumidos
// (permanecem no histórico, mas não são exibidos nem exportados); cada corpo só pode ser
// consumido uma vez e só por uma operação posterior.
//
// Arquivos das versões 1 a 4 continuam sendo lidos sem alteração; a gravação usa sempre a
// versão atual.
#pragma once

#include "document/CadDocument.h"

#include <QByteArray>
#include <QString>

namespace cad::document {

struct FileResult
{
    bool ok = false;
    QString message;
};

class DocumentFile
{
public:
    static constexpr int kSchemaVersion = 5;
    static constexpr qint64 kMaxFileSize = 4 * 1024 * 1024;
    static constexpr const char* kFormat = "cadmec";
    static constexpr const char* kExtension = "cadmec";

    static QByteArray toJson(const CadDocument& document);

    // Valida estrutura, tipos e limites e reconstrói a geometria. Em caso de falha 'out' não é
    // alterado.
    static FileResult fromJson(const QByteArray& json, CadDocument& out);

    // Grava em arquivo temporário e substitui o destino só ao final (QSaveFile).
    static FileResult save(const CadDocument& document, const QString& path);

    // Lê e valida; em caso de falha 'out' (o documento ativo) não é alterado.
    static FileResult load(const QString& path, CadDocument& out);
};

} // namespace cad::document
