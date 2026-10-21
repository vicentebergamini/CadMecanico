// Referência persistente de arestas (E5.5B) para Fillet 3D e Chamfer 3D.
//
// Índices do TopExp_Explorer mudam a cada reconstrução e não servem como referência. Cada aresta
// selecionada é guardada por uma assinatura geométrica:
//   - tipo da curva (reta, circunferência, outra);
//   - direção (reta) ou eixo (circunferência), sem sinal;
//   - raio (circunferência) e comprimento;
//   - ponto médio (parâmetro médio da curva);
//   - normais das faces adjacentes no ponto médio (orientação da face respeitada).
//
// Remapeamento após reconstrução do sólido de origem (resolveEdge):
//   1. candidatas = arestas com o mesmo tipo de curva, direção paralela e o mesmo conjunto de
//      normais das faces adjacentes (o "papel" da aresta: ex. aresta superior frontal de um bloco);
//   2. se uma candidata tem o mesmo ponto médio (≤ 1e-4 mm), é a mesma aresta (exata);
//   3. senão, uma única candidata é aceita como a aresta remapeada (ex. após mudar a altura);
//   4. várias candidatas: aceita a de ponto médio mais próximo somente se a distância for menor
//      que metade da distância da segunda; caso contrário a referência é ambígua;
//   5. nenhuma candidata (ou ambígua): a operação dependente é invalidada com mensagem — nunca se
//      aplica o acabamento a uma aresta escolhida sem segurança.
// Após cada reconstrução bem-sucedida a assinatura é atualizada para a aresta encontrada.
#pragma once

#include <TopoDS_Edge.hxx>
#include <TopoDS_Shape.hxx>
#include <gp_Dir.hxx>
#include <gp_Pnt.hxx>

#include <string>
#include <vector>

namespace cad::modeling {

struct EdgeSignature
{
    enum class Curve
    {
        Line,
        Circle,
        Other
    };
    Curve curve = Curve::Line;
    gp_Pnt mid;
    gp_Dir direction;   // reta: direção; circunferência: eixo (sinal normalizado)
    double length = 0.0;
    double radius = 0.0;
    std::vector<gp_Dir> normals; // normais das faces adjacentes no ponto médio (1 ou 2)
};

struct EdgeMatch
{
    bool ok = false;
    bool exact = false;
    TopoDS_Edge edge;
    std::string message;
};

// Assinatura da aresta 'edge' pertencente a 'solid'.
EdgeSignature describeEdge(const TopoDS_Shape& solid, const TopoDS_Edge& edge);

// Localiza em 'solid' a aresta correspondente à assinatura (ver regras no início do arquivo).
EdgeMatch resolveEdge(const TopoDS_Shape& solid, const EdgeSignature& signature);

// true se 'edge' é uma aresta de 'shape'.
bool containsEdge(const TopoDS_Shape& shape, const TopoDS_Shape& edge);

const char* curveName(EdgeSignature::Curve curve); // "line", "circle", "other"

} // namespace cad::modeling
