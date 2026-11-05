// Classificação das barras (tp) e massas nodais da rede DEM-DYNA (E5.7.4b.3), sem Qt.
//
// Transcrição de malha_por_mascara.m (referência MATLAB, somente leitura), reutilizando a
// discretização existente (Discretization) e a sequência de ligações do exportador
// (ApdlLinkSequence): nenhuma malha, numeração ou ordem de elementos é recalculada aqui.
//
// Correspondência de índices: o vértice (i, j, k) da grade C++ (base 0) é o vértice vId(i+1, j+1, k+1)
// do MATLAB (base 1); a célula (i, j, k) C++ é a célula (i+1, j+1, k+1) do MATLAB. O vértice (i, j, k)
// é o canto mínimo da célula (i, j, k).
//
// Regras exatas do MATLAB:
//   cVert (l. 31-32): número de células ativas entre as oito que tocam o vértice:
//       células (i-1..i, j-1..j, k-1..k) (base 0). Um vértice só existe com cVert > 0 (l. 34).
//   tp das arestas entre vértices, n4 = número de células ativas em volta da aresta:
//       aresta X de (i,j,k) a (i+1,j,k): células (i, j-1..j, k-1..k)          (l. 95)
//       aresta Y de (i,j,k) a (i,j+1,k): células (i-1..i, j, k-1..k)          (l. 113)
//       aresta Z de (i,j,k) a (i,j,k+1): células (i-1..i, j-1..j, k)          (l. 130)
//     a aresta só existe com n4 > 0 e tp = n4 ∈ {1, 2, 3, 4} (l. 96-99, 114-117, 131-134);
//   ligação centro-centro (cubos ativos vizinhos por face): tp = 4 (l. 151-171);
//   diagonal centro-vértice (8 por cubo ativo): tp = 0 (l. 175-183).
// tp = 3 ocorre em arestas de cantos reentrantes (seções não convexas). Ele é apenas CLASSIFICADO
// aqui; lawnumbers.m não tem regra de lei para tp = 3 (ver dem/LawDiagnostics.h).
//
// Massas (l. 193-233), com LCO em metros e ROH em kg/m³:
//   mss = LCO³·ROH/2                     massa de referência de um cubo         (l. 195)
//   vértice: mass = mss·cVert/8          fração dos oito cubos em volta         (l. 207)
//   centro:  mass = mss                                                       (l. 223)
// Soma: Nc·mss (centros) + Σ cVert·mss/8 = Nc·mss + 8·Nc·mss/8 = ROH·LCO³·Nc. As massas usam
// somente LCO e cVert, como no MATLAB, também quando a geometria foi projetada (E5.7.2.3): nesse
// caso a massa total continua ROH·LCO³·Nc e não o produto ROH·(volume das células deformadas).
//
// Comandos APDL das massas (MATLAB, l. 197-233), na ordem dos IDs dos nós (vértices e depois
// centros, a mesma ordem dos laços k → j → i do MATLAB):
//   (linha vazia)
//   !************************NODAL DENSITY***********
//   ! Define o Elemento de massa MASS166
//   ET,2,166
//   R,<nBonds+n>,<massa %g>   TYPE,2   REAL,<nBonds+n>   E,<n>        (um bloco por nó n)
// O comando E sem número cria o próximo elemento: o elemento de massa do nó n recebe o ID
// nBonds + n (todas as ligações já existem, com IDs 1..nBonds).
#pragma once

#include "discretization/Discretization.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace cad::discretization {

struct LatticeTopology
{
    bool ok = false;
    std::string message;
    // cVert por vértice: índice = ID do nó − 1 (somente os Nv vértices).
    std::vector<std::uint8_t> vertexCubeCount;
    // tp por ligação: índice = ID do elemento − 1, na ordem de ApdlLinkSequence.
    std::vector<std::int8_t> linkTp;
    // Quantidade de ligações por tp (0..4) e por grupo.
    std::array<std::int64_t, 5> countByTp{};
    std::int64_t edgeLinks = 0, centerCenterLinks = 0, diagonalLinks = 0;
    std::int64_t links() const { return std::int64_t(linkTp.size()); }
};

// Classifica vértices e ligações de uma discretização não vazia.
LatticeTopology classifyLattice(const Discretization& data);

struct NodalMasses
{
    bool ok = false;
    std::string message;
    double lcoMeters = 0.0;
    double density = 0.0;       // ROH [kg/m³]
    double cubeMass = 0.0;      // mss = LCO³·ROH/2 [kg]
    std::vector<double> mass;   // por nó: índice = ID − 1 (vértices e depois centros) [kg]
    double total = 0.0;         // TOTMASS, acumulada na ordem dos nós (como no MATLAB) [kg]
};

// Massas nodais. 'density' em kg/m³ (finita e positiva); LCO vem da grade (mm → m).
NodalMasses computeNodalMasses(const Discretization& data, const LatticeTopology& topology, double density);

// Um bloco R/TYPE/REAL/E do MATLAB.
struct MassCommand
{
    std::int64_t nodeId = 0;    // n
    std::int64_t realId = 0;    // nBonds + n  (R e REAL)
    std::int64_t elementId = 0; // nBonds + n  (ID implícito criado por E,n)
    double mass = 0.0;          // [kg]
};

std::vector<MassCommand> massCommands(const NodalMasses& masses, std::int64_t bondCount);

// Texto APDL das massas exatamente como malha_por_mascara.m o escreve (formato %g, "\n"). Ainda não
// é usado pelo exportador principal.
std::string matlabMassBlock(const NodalMasses& masses, std::int64_t bondCount);

// Formatação numérica "%g" do fprintf do MATLAB (6 algarismos significativos).
std::string matlabG(double value);

} // namespace cad::discretization
