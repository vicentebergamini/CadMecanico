// Exportação ASCII da nuvem de nós da discretização (E5.7.2), sem dependência de Qt.
//
// Conteúdo: exclusivamente os dados nodais — cabeçalho "ID_node Coord_X Coord_Y Coord_Z" e uma
// linha por nó, na ordem dos IDs globais (vértices e depois centros). Sem comandos APDL, sem o
// prefixo "n" e sem conectividade.
//
// Formato: UTF-8 (somente ASCII), campos separados por um espaço, ponto decimal, IDs inteiros,
// fim de linha "\n". Coordenadas em notação decimal fixa com a MENOR representação que relê
// exatamente o mesmo double (std::to_chars): nenhuma casa é truncada e não há dígitos espúrios
// (20 mm em metros = "0.02"; 0,5 mm = "0.5"). Zero negativo é escrito como "0". Leitura direta em
// MATLAB (readmatrix) e Python (numpy.loadtxt(..., skiprows=1)).
//
// Unidade: milímetros (unidade do CAD) ou metros (mm / 1000, calculado a partir dos índices
// inteiros da grade). A unidade não altera IDs nem ordem; a geometria CAD não é alterada.
//
// Gravação segura: arquivo temporário no mesmo diretório -> verificação completa (releitura) ->
// substituição atômica. Em falha o destino não é criado nem alterado. Arquivo existente só é
// substituído com 'overwrite' (a confirmação do usuário é feita pela interface).
#pragma once

#include "discretization/Discretization.h"

#include <filesystem>
#include <string>

namespace cad::discretization {

inline constexpr const char* kNodeFileHeader = "ID_node Coord_X Coord_Y Coord_Z";

struct NodeExportResult
{
    bool ok = false;
    std::string message;
    std::int64_t lines = 0; // linhas de dados (sem o cabeçalho)
    std::uintmax_t bytes = 0;
    double seconds = 0.0;
};

struct NodeFileCheck
{
    bool ok = false;
    std::string message;
    std::int64_t lines = 0;
    std::int64_t vertexLines = 0;
    std::int64_t centerLines = 0;
};

class NodeAsciiExporter
{
public:
    // Texto de uma coordenada (menor representação exata em notação fixa).
    static std::string formatCoordinate(double value);
    // Nome sugerido: "<peça>_nodes.txt".
    static std::string suggestedFileName(const std::string& partName);

    NodeExportResult write(const Discretization& data, const std::filesystem::path& file, LengthUnit unit,
                           bool overwrite = false) const;

    // Relê o arquivo e confere cabeçalho, número de linhas, IDs (1..N, sem duplicatas nem
    // lacunas), coordenadas finitas e IGUAIS às da discretização na unidade dada, e a ordenação
    // Z → Y → X dentro do bloco de vértices e do bloco de centros.
    static NodeFileCheck verify(const std::filesystem::path& file, const Discretization& data, LengthUnit unit);
};

} // namespace cad::discretization
