// Exportação da geometria discretizada em comandos ANSYS APDL (/PREP7) (E5.7.2.2), sem Qt.
//
// Referência obrigatória: arquivo DEM_DYNA.txt fornecido pelo usuário (cubo 4 × 4 × 4 mm,
// lco = 1 mm, coordenadas em metros). Somente a parte geométrica é reproduzida: nós (N), tipo de
// elemento (ET,1,165) e elementos de ligação de dois nós (EN). Não são escritos propriedades
// físicas (R, MAT, TB, EDCURVE, EMODIF), elementos de massa (ET,2,166), carregamentos, condições de
// contorno nem comandos de solução.
//
// Estrutura do arquivo:
//   !seed == 10                         (preservado do exemplo, sem significado físico nesta etapa)
//   FINISH
//   /CLEAR                              (opcional; apaga o banco de dados corrente do ANSYS)
//   !Code generate on DD/MM/AAAA, HH:MM:SS
//   /TITLE, DEM-DYNA
//   /PREP7
//   ! comentários informativos (unidade das coordenadas, lco, contagens)
//   N,ID,X,Y,Z        um por nó, na numeração global da E5.7.2 (vértices e depois centros)
//   ET,1,165          COMBI165 (mola/amortecedor explícito de dois nós, I e J)
//   EN,ID,N1,N2       um por ligação, IDs contínuos a partir de 1
//
// Nós: os IDs e a ordem são exatamente os da discretização (iguais aos da exportação ASCII);
// nenhuma renumeração. As coordenadas usam a mesma formatação da exportação ASCII (menor
// representação decimal exata) na unidade escolhida (mm ou m); a geometria CAD não muda.
//
// Ordem dos elementos (verificada contra o arquivo de referência, elemento a elemento):
//   Grupo A — arestas entre vértices: primeiro todas as paralelas a X, depois a Y, depois a Z.
//     Dentro de cada direção, em ordem crescente do ID do primeiro nó (Z → Y → X); EN,id,a,b com
//     a < b (b = vizinho na direção positiva). Uma aresta só existe se pertencer a pelo menos um
//     cubo aceito (nunca atravessa um vazio) e é escrita uma única vez.
//   Grupos B e C — percorrendo os cubos na ordem dos IDs (= ordem dos nós centrais):
//     B: ligações do centro com os centros vizinhos por face nas direções +X, +Y, +Z (somente
//        cubos aceitos; cada par aparece uma única vez, sempre a partir do centro de menor ID);
//     C: as oito ligações centro-vértice, na ordem (dx, dy, dz) com dz variando mais rapidamente:
//        (0,0,0) (0,0,1) (0,1,0) (0,1,1) (1,0,0) (1,0,1) (1,1,0) (1,1,1); EN,id,centro,vértice.
//   Cubo de referência 4 × 4 × 4 (lco = 1): 300 + 144 + 512 = 956 elementos.
//
// A sequência é gerada sob demanda (ApdlLinkSequence), sem armazenar todas as ligações: a
// gravação e a verificação por releitura percorrem a mesma sequência.
//
// Gravação segura: arquivo temporário -> verificação completa por releitura -> substituição
// atômica (mesma política da exportação ASCII). Texto somente ASCII, fim de linha "\n".
#pragma once

#include "discretization/Discretization.h"

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>

namespace cad::discretization {

inline constexpr int kApdlLinkElementType = 165; // COMBI165 (ANSYS LS-DYNA)

enum class LinkGroup : std::uint8_t
{
    VertexEdge,   // A: aresta entre vértices adjacentes
    CenterCenter, // B: centros de cubos que compartilham uma face
    CenterVertex  // C: centro do cubo e um de seus oito vértices
};

const char* linkGroupLabel(LinkGroup group);

struct LinkElement
{
    std::int64_t id = 0; // ID do elemento (1..N, contínuo)
    std::int64_t n1 = 0;
    std::int64_t n2 = 0;
    LinkGroup group = LinkGroup::VertexEdge;
    int axis = -1; // direção (0 = X, 1 = Y, 2 = Z) nos grupos A e B; -1 no grupo C
};

struct LinkCounts
{
    std::int64_t vertexEdges = 0;
    std::int64_t centerCenter = 0;
    std::int64_t centerVertex = 0;
    std::int64_t total() const { return vertexEdges + centerCenter + centerVertex; }
};

// Sequência determinística das ligações (ordem descrita acima).
class ApdlLinkSequence
{
public:
    explicit ApdlLinkSequence(const Discretization& data) : m_data(data) {}
    // Próxima ligação; false ao final.
    bool next(LinkElement& out);

    static LinkCounts count(const Discretization& data);

private:
    const Discretization& m_data;
    int m_stage = 0;          // 0, 1, 2: arestas X, Y, Z; 3: centros; 4: fim
    std::int64_t m_index = 0; // índice do vértice (estágios 0..2) ou do cubo (estágio 3)
    int m_step = 0;           // no cubo: 0..2 vizinhos +X/+Y/+Z, 3..10 vértices
    std::int64_t m_nextId = 1;
};

struct ApdlOptions
{
    LengthUnit unit = LengthUnit::Meter; // o exemplo usa metros
    bool includeClear = true;            // /CLEAR (apaga o banco de dados corrente do ANSYS)
    std::string timestamp;               // "DD/MM/AAAA, HH:MM:SS"; vazio = momento da exportação
    std::string partName;                // somente para o comentário informativo
};

struct ApdlExportResult
{
    bool ok = false;
    std::string message;
    std::int64_t nodes = 0;
    LinkCounts links;
    std::uintmax_t bytes = 0;
    double seconds = 0.0;
};

struct ApdlFileCheck
{
    bool ok = false;
    std::string message;
    std::int64_t nodes = 0;
    std::int64_t elements = 0;
};

// Resultado da comparação geométrica com um arquivo APDL de referência.
struct ApdlComparison
{
    bool ok = false;
    std::string summary; // relatório em texto (várias linhas)
    std::int64_t nodesGenerated = 0, nodesReference = 0;
    std::int64_t elementsGenerated = 0, elementsReference = 0;
    bool elementTypeMatches = false;
    // Primeira divergência (vazio se não houver).
    std::string divergenceKind; // "N", "EN", "ET", "contagem"
    std::int64_t divergenceId = 0;
    std::string expected, obtained, cause;
    // Comandos do arquivo de referência fora do escopo geométrico (ignorados), por nome.
    std::map<std::string, std::int64_t> ignoredReferenceCommands;
};

class ApdlExporter
{
public:
    // Nome sugerido: "<peça>_PREP7.inp".
    static std::string suggestedFileName(const std::string& partName);
    // Data e hora locais atuais no formato "DD/MM/AAAA, HH:MM:SS".
    static std::string currentTimestamp();

    ApdlExportResult write(const Discretization& data, const std::filesystem::path& file, const ApdlOptions& options,
                           bool overwrite = false) const;

    // Relê o arquivo e confere: cabeçalho (FINISH, /CLEAR conforme a opção, /TITLE, /PREP7), nós
    // 1..N com coordenadas iguais às da discretização na unidade dada, ET,1,165 e cada EN igual à
    // sequência ApdlLinkSequence; ausência de comandos de solução.
    static ApdlFileCheck verify(const std::filesystem::path& file, const Discretization& data,
                                const ApdlOptions& options);

    // Compara a parte geométrica (N, ET,1 e EN) de dois arquivos APDL. Ignora maiúsculas e
    // minúsculas dos comandos, formatação numérica equivalente, data/hora e comentários; os demais
    // comandos da referência (propriedades físicas, massas, materiais...) são contados e
    // relatados como fora do escopo. Nenhum dos arquivos é modificado.
    static ApdlComparison compare(const std::filesystem::path& generated, const std::filesystem::path& reference);
};

} // namespace cad::discretization
