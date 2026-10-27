// Discretização com projeção dos nós sobre o contorno (ajuste geométrico) (E5.7.2.3), sem Qt.
//
// Ponto de partida: a grade cartesiana regular e o conjunto de cubos do critério "centro do cubo no
// material" (os vértices de fronteira ficam dos dois lados da superfície, a menos de ~0,87·lco
// dela). A numeração dos nós e toda a conectividade continuam definidas pelos índices inteiros da
// grade, ANTES da projeção; o ajuste altera somente coordenadas.
//
// Etapas:
//   1. faces expostas: faces de cubos aceitos cujo vizinho (mesma direção) não é aceito; normal
//      externa = direção do vizinho vazio. Os vértices dessas faces são os únicos candidatos (nós
//      de contorno da discretização); nós internos não são projetados;
//   2. face CAD de destino de cada face exposta: entre as faces do sólido (Open CASCADE) próximas,
//      a mais próxima do centro da face exposta cuja normal EXTERNA no ponto mais próximo forma
//      ângulo agudo com a normal da face exposta (impede escolher o lado oposto de um furo, de uma
//      cavidade ou de uma parede fina) e a menos de lco;
//   3. destino de cada vértice de contorno, pela topologia CAD: as faces de destino das faces
//      expostas que o contêm são agrupadas em superfícies suaves (normais a menos de 25°). Um grupo:
//      ponto mais próximo sobre essas faces (projeção 3D sobre a superfície limitada — plano,
//      cilindro em qualquer orientação, esfera, B-spline...); dois grupos: ponto mais próximo sobre
//      as arestas CAD compartilhadas pelos dois grupos (aresta viva, ex.: borda de um furo); três ou
//      mais: vértice CAD comum. Sem destino coerente, deslocamento acima de lco ou deslocamento no
//      sentido errado (nó no material afastando-se do vazio ou vice-versa): o nó não é deslocado;
//   4. suavização: nós internos vizinhos (por aresta) de nós projetados recebem metade do
//      deslocamento médio desses vizinhos somente se isso melhora o pior jacobiano das suas células;
//      nós mais afastados permanecem nas posições cartesianas;
//   5. validação das células deformadas (hexaedro trilinear dos 8 vértices): jacobiano escalado
//      >= 0,2 nos 8 cantos e no centro (sem inversão, sem distorção excessiva), volume > 0, arestas
//      >= 0,05·lco, nó central (média dos vértices) estritamente interior às seis faces. Célula
//      inválida: o deslocamento dos vértices responsáveis (os cantos que falham ou, se a falha não
//      for de canto, todos os deslocados) é reduzido gradualmente (× 0,75; abaixo de 5 % é
//      cancelado) e tudo é reavaliado — a grade original é sempre válida, então o processo termina;
//   6. verificações globais: ligações (as mesmas do APDL) com extremidade deslocada que passam por
//      vazio mais fundo que suas extremidades (> 0,1·lco) e nós coincidentes (< 0,05·lco) também
//      provocam redução do deslocamento;
//   7. resultado: coordenadas ajustadas, centros recalculados, relatório de qualidade (comprimentos
//      das ligações, volumes das células deformadas por integração exata, desvios ao contorno).
//
// A projeção muda comprimentos e orientações das ligações; isso NÃO garante distribuição uniforme
// de rigidez (assunto da formulação do MED, fora desta etapa).
#pragma once

#include "discretization/Discretization.h"

#include <TopoDS_Shape.hxx>

#include <array>
#include <functional>
#include <memory>
#include <string>

namespace cad::discretization {

struct ProjectionOptions
{
    double maxDisplacement = 1.0;   // deslocamento admissível, em múltiplos de lco
    double minScaledJacobian = 0.2; // abaixo disso a célula é considerada excessivamente distorcida
    double minEdge = 0.05;          // aresta mínima da célula e distância mínima entre nós (× lco)
    double smoothAngleDeg = 25.0;   // faces com normais até este ângulo formam uma superfície suave
    double smoothing = 0.5;         // fração do deslocamento médio aplicada aos nós internos vizinhos
    double voidDepth = 0.1;         // profundidade admissível de uma ligação no vazio (× lco)
    double reduction = 0.75;        // fator aplicado ao deslocamento a cada redução
    double minFraction = 0.05;      // abaixo desta fração do deslocamento a projeção é cancelada
};

// Qualidade de um hexaedro trilinear com vértices na convenção V1..V8 (Discretization.h).
struct HexQuality
{
    double minScaledJacobian = 0.0;         // mínimo nos 8 cantos e no centro
    std::array<double, 8> cornerJacobian{}; // jacobiano escalado em cada canto (V1..V8)
    double centerJacobian = 0.0;
    double volume = 0.0;  // integração de Gauss 2×2×2 (exata para o hexaedro trilinear)
    double minEdge = 0.0; // menor das 12 arestas
    bool centerInside = false; // média dos vértices estritamente interior às seis faces
};
HexQuality evaluateHex(const std::array<gp_XYZ, 8>& v);

class BoundaryProjector
{
public:
    BoundaryProjector(const TopoDS_Shape& shape, double lco, const ProjectionOptions& options = {});
    ~BoundaryProjector();
    BoundaryProjector(const BoundaryProjector&) = delete;
    BoundaryProjector& operator=(const BoundaryProjector&) = delete;

    // Ajusta 'data' (grade do mesmo sólido e mesmo lco). false: cancelado ('keepGoing') ou não foi
    // possível obter uma discretização válida — 'message' explica e 'data' não é alterado.
    bool run(Discretization& data, std::string& message, const std::function<bool()>& keepGoing = {});

private:
    struct Impl;
    std::unique_ptr<Impl> m;
};

} // namespace cad::discretization
