// Resultado da discretização volumétrica (E5.7.2): cubos aceitos, nós e numeração global.
//
// Armazenamento: somente os índices inteiros (as coordenadas são reconstruídas da grade).
//   - cubos aceitos: índices lineares das células em ordem crescente (Z → Y → X); ID_cube = posição + 1;
//   - mapa de células aceitas (1 bit por célula) para consultas de vizinhança;
//   - ID global de cada ponto da rede (0 = ponto não usado por nenhum cubo aceito) e a lista dos
//     pontos usados em ordem de ID.
//
// Numeração dos nós (regra obrigatória):
//   1. nós dos vértices: todos os pontos da rede usados por algum cubo aceito, numerados de 1 a Nv
//      na ordem Z crescente → Y crescente → X crescente (X varia mais rapidamente), sem lacunas.
//      Vértices compartilhados por cubos adjacentes têm um único ID (deduplicação pelo índice
//      inteiro da rede, nunca por comparação de coordenadas em ponto flutuante);
//   2. nós centrais: um por cubo aceito, IDs Nv + 1 .. Nv + Nc na mesma ordem Z → Y → X
//      (nó central do cubo c = Nv + ID_cube).
// A ordem não depende da ordem interna de retorno dos objetos do Open CASCADE: depende apenas da
// grade (origem, lco, orientação) e do conjunto de cubos aceitos.
//
// Convenção dos nove nós de um cubo com canto mínimo (x, y, z):
//   V1 (x, y, z)            V2 (x+lco, y, z)        V3 (x+lco, y+lco, z)    V4 (x, y+lco, z)
//   V5 (x, y, z+lco)        V6 (x+lco, y, z+lco)    V7 (x+lco, y+lco, z+lco) V8 (x, y+lco, z+lco)
//   V9 centro (x+lco/2, y+lco/2, z+lco/2)
//
// Geometria ajustada (E5.7.2.3, critério "projeção dos nós sobre o contorno"): a numeração e a
// conectividade continuam definidas pelos índices inteiros da grade cartesiana (antes da projeção);
// somente as coordenadas físicas de alguns nós mudam. Os nós deslocados têm a posição guardada
// explicitamente (mm); os demais continuam calculados pela grade (valores decimais exatos). A célula
// deformada é o hexaedro trilinear dos seus oito vértices e o nó central é a imagem do centro
// paramétrico (média dos oito vértices), verificado como interior à célula antes da aceitação.
#pragma once

#include "discretization/VoxelGrid.h"

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace cad::discretization {

// Critério de inclusão dos cubos candidatos.
enum class InclusionCriterion
{
    FullyInside,         // 1. cubo totalmente contido no sólido
    CenterInside,        // 2. centro do cubo localizado no material
    PositiveIntersection, // 3. interseção volumétrica positiva com o sólido
    BoundaryProjection    // 4. projeção dos nós sobre o contorno (ajuste geométrico, E5.7.2.3)
};

const char* criterionLabel(InclusionCriterion criterion);
const char* criterionKey(InclusionCriterion criterion); // "totalmente_contido", ...

struct Parameters
{
    double lco = 1.0; // mm, > 0
    InclusionCriterion criterion = InclusionCriterion::FullyInside;
};

struct CubeInfo
{
    std::int64_t id = 0; // ID_cube (1..Nc)
    int i = 0, j = 0, k = 0;
    gp_XYZ center;                          // mm
    std::array<std::int64_t, 8> vertexIds{}; // V1..V8 (convenção acima)
    std::int64_t centerId = 0;              // V9
    double lco = 0.0;
};

struct NodeInfo
{
    std::int64_t id = 0;
    gp_XYZ position; // na unidade pedida
    bool center = false;
    int i = 0, j = 0, k = 0; // índices do ponto da rede (vértice) ou da célula (centro)
};

// Estatísticas do processamento.
struct DiscretizationStats
{
    double solidVolume = 0.0;           // mm³ (B-Rep, BRepGProp)
    double classificationSeconds = 0.0; // classificação geométrica dos cubos
    double numberingSeconds = 0.0;      // deduplicação, ordenação e IDs
    double totalSeconds = 0.0;
    std::int64_t uniformRegions = 0;    // regiões classificadas por um único ponto
    std::int64_t boundaryTests = 0;     // testes precisos de interseção com o contorno
    std::int64_t boundaryCubes = 0;     // cubos atravessados pelo contorno
    std::int64_t pointClassifications = 0;
    std::int64_t extremaFailures = 0;   // testes de distância sem resultado (tratados como interseção)
    double pointSeconds = 0.0;          // tempo gasto na classificação de pontos
    double boundarySeconds = 0.0;       // tempo gasto nos testes de distância
};

// Resultado do ajuste geométrico (critério 4, E5.7.2.3). Comprimentos em mm, volumes em mm³.
struct ProjectionReport
{
    bool applied = false;
    // Seleção dos nós de contorno.
    std::int64_t boundaryFaces = 0;        // faces de cubos expostas (cubo aceito | vizinho vazio)
    std::int64_t facesWithTarget = 0;      // faces expostas associadas a uma face CAD
    std::int64_t boundaryVertices = 0;     // vértices das faces expostas
    std::int64_t targetedVertices = 0;     // vértices com destino (face, aresta ou vértice CAD)
    std::int64_t featureEdgeTargets = 0;   // destinos sobre arestas vivas (interseção de faces)
    std::int64_t cornerTargets = 0;        // destinos sobre vértices CAD
    std::int64_t unresolvedVertices = 0;   // sem destino topológico coerente (não deslocados)
    std::int64_t inadmissibleVertices = 0; // deslocamento acima do admissível (não deslocados)
    // Resultado.
    std::int64_t projectedNodes = 0;    // nós de contorno projetados integralmente (deslocamento > 0)
    std::int64_t partialNodes = 0;      // projeção reduzida para manter as células válidas
    std::int64_t cancelledNodes = 0;    // projeção cancelada (permanecem na posição cartesiana)
    std::int64_t smoothedNodes = 0;     // nós internos reposicionados suavemente
    std::int64_t movedNodes = 0;        // nós (vértices e centros) com coordenadas alteradas
    std::int64_t adjustedCells = 0;     // células deformadas (algum vértice deslocado)
    std::int64_t rejectedCells = 0;     // células cuja deformação foi recusada ou reduzida
    std::int64_t invalidCells = 0;      // células inválidas no resultado final (deve ser 0)
    std::int64_t voidCrossingLinks = 0; // ligações que atravessariam vazios (corrigidas)
    std::int64_t coincidentNodes = 0;   // pares de nós coincidentes (corrigidos)
    int iterations = 0;                 // iterações de redução do deslocamento
    double maxDisplacement = 0.0;
    // Distância dos nós de contorno à geometria CAD de destino (face, aresta ou vértice).
    double deviationBeforeMax = 0.0, deviationBeforeMean = 0.0;
    double deviationAfterMax = 0.0, deviationAfterMean = 0.0;
    // Qualidade geométrica.
    double minScaledJacobian = 1.0; // mínimo do jacobiano escalado nos cantos e no centro
    double minCellVolume = 0.0, maxCellVolume = 0.0;
    double discretizedVolume = 0.0;      // soma dos volumes das células deformadas (integração exata)
    double minLink = 0.0, maxLink = 0.0; // comprimentos das ligações (todas as do APDL)
    double seconds = 0.0;
};

class Discretization
{
public:
    Discretization() = default;

    const VoxelGrid& grid() const { return m_grid; }
    const Parameters& parameters() const { return m_parameters; }
    const DiscretizationStats& stats() const { return m_stats; }
    DiscretizationStats& stats() { return m_stats; }

    bool empty() const { return m_cubes.empty(); }
    std::int64_t cubeCount() const { return std::int64_t(m_cubes.size()); }
    std::int64_t vertexNodeCount() const { return std::int64_t(m_vertexPoints.size()); }
    std::int64_t centerNodeCount() const { return cubeCount(); }
    std::int64_t totalNodeCount() const { return vertexNodeCount() + centerNodeCount(); }
    double cubeVolume() const { return double(cubeCount()) * m_grid.lco * m_grid.lco * m_grid.lco; }
    // Volume representado pelas células: cubeVolume() na grade cartesiana; soma dos volumes das
    // células deformadas quando a projeção foi aplicada.
    double discretizedVolume() const { return adjusted() ? m_projection.discretizedVolume : cubeVolume(); }

    // Geometria ajustada (E5.7.2.3).
    bool adjusted() const { return m_projection.applied; }
    const ProjectionReport& projection() const { return m_projection; }
    // Posição (mm) do vértice no ponto (i, j, k) da rede e do centro da célula (i, j, k), já com o
    // ajuste geométrico (iguais à grade quando não há ajuste).
    gp_XYZ vertexPosition(int i, int j, int k) const;
    gp_XYZ centerPosition(int i, int j, int k) const;
    // Instala as posições ajustadas (mm), por ID de nó: 'vertices' (Nv) e 'centers' (Nc); 'moved'
    // (Nv + Nc) indica os nós cuja posição difere da grade. Tamanhos inconsistentes: false.
    bool setAdjustedGeometry(std::vector<gp_XYZ> vertices, std::vector<gp_XYZ> centers,
                             std::vector<std::uint8_t> moved, const ProjectionReport& report);

    // Índices lineares das células aceitas (ordem dos IDs) e pontos da rede usados (ordem dos IDs).
    const std::vector<std::int64_t>& cubeCells() const { return m_cubes; }
    const std::vector<std::int64_t>& vertexPoints() const { return m_vertexPoints; }
    bool cellAccepted(int i, int j, int k) const;

    // Dados completos do cubo de índice 'index' (0-based; ID = index + 1).
    CubeInfo cube(std::int64_t index) const;
    // Nó de ID 'id' (1..totalNodeCount()).
    NodeInfo node(std::int64_t id, LengthUnit unit = LengthUnit::Millimeter) const;
    // ID global do vértice no ponto (i, j, k) da rede (0 se não usado).
    std::int64_t vertexId(int i, int j, int k) const;
    // ID_cube da célula (i, j, k) (0 se a célula não foi aceita). Busca binária nos cubos aceitos.
    std::int64_t cubeId(int i, int j, int k) const;

    // Memória aproximada das estruturas (bytes).
    std::size_t memoryBytes() const;

    // Constrói a numeração a partir do mapa de células aceitas (1 byte por célula, ordem do
    // índice linear). 'keepGoing' é consultado periodicamente (false = cancelar; retorna false).
    bool build(const VoxelGrid& grid, const Parameters& parameters, const std::vector<std::uint8_t>& accepted,
               const std::function<bool()>& keepGoing = {});

private:
    VoxelGrid m_grid;
    Parameters m_parameters;
    DiscretizationStats m_stats;
    std::vector<std::int64_t> m_cubes;
    std::vector<bool> m_accepted;
    std::vector<std::uint32_t> m_latticeId; // ID do vértice por ponto da rede (0 = não usado)
    std::vector<std::int64_t> m_vertexPoints;
    // Geometria ajustada (vazios sem ajuste).
    ProjectionReport m_projection;
    std::vector<gp_XYZ> m_vertexPos;
    std::vector<gp_XYZ> m_centerPos;
    std::vector<std::uint8_t> m_moved;
};

} // namespace cad::discretization
