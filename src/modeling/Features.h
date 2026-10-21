// Operações geométricas elementares usadas pelo documento paramétrico (sem dependência de Qt).
#pragma once

#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>
#include <gp_Vec.hxx>

#include <string>
#include <vector>

namespace cad::modeling {

struct FeatureResult
{
    bool ok = false;
    std::string message;
    TopoDS_Shape shape;
};

// Bloco alinhado aos eixos, com canto na origem: [0,length] x [0,width] x [0,height].
FeatureResult makeBlock(double length, double width, double height);

// Furo cilíndrico vertical (eixo Z) passante através de 'base', centrado em (centerX, centerY).
// O cilindro ultrapassa 1 mm acima e abaixo da caixa da base para garantir o furo passante.
FeatureResult cutThroughHole(const TopoDS_Shape& base, double centerX, double centerY, double radius);

// Extrusão (prisma) de uma face plana segundo o vetor 'direction' (módulo = profundidade).
// Garante um sólido único, válido (BRepCheck) e com volume positivo (orientação corrigida se
// necessário).
FeatureResult extrudeFace(const TopoDS_Face& face, const gp_Vec& direction);

// Operações booleanas entre sólidos (E5.4).
enum class BooleanKind
{
    Fuse,   // união A ∪ B
    Cut,    // subtração A − B (A = corpo principal, B = ferramenta de corte)
    Common  // interseção A ∩ B
};

// Executa a operação booleana com validação dos operandos (não nulos, com sólido, BRepCheck) e do
// resultado (algoritmo concluído sem erros, BRepCheck, volume positivo). O resultado é o sólido
// único ou, se houver componentes desconectados, um TopoDS_Compound com os sólidos (nunca é
// informado como sólido único). Resultado sem volume (ex.: interseção de sólidos disjuntos ou
// tangentes, subtração que elimina todo o corpo) é recusado com mensagem explícita.
FeatureResult booleanOperation(BooleanKind kind, const TopoDS_Shape& a, const TopoDS_Shape& b);

// Número de sólidos (TopAbs_SOLID) contidos na forma.
int countSolids(const TopoDS_Shape& shape);

// Acabamentos 3D (E5.5B). Arredondamento de raio constante (BRepFilletAPI_MakeFillet) ou chanfro
// simétrico de distância d medida em cada face (BRepFilletAPI_MakeChamfer::Add(d, aresta)) nas
// arestas dadas de 'solid'. Recusa valor não positivo/não finito, arestas que não pertencem ao
// sólido e valores inviáveis (algoritmo não concluído, resultado inválido ou sem volume).
FeatureResult filletEdges(const TopoDS_Shape& solid, const std::vector<TopoDS_Edge>& edges, double radius);
FeatureResult chamferEdges(const TopoDS_Shape& solid, const std::vector<TopoDS_Edge>& edges, double distance);
// Chanfro assimétrico (E5.6): para cada aresta, 'faces[i]' é a face de referência (adjacente à
// aresta). Duas distâncias: D1 na face de referência e D2 na outra (Add(D1, D2, E, F)).
// Distância e ângulo: D1 na face de referência e ângulo (graus) entre a face de referência e a
// face do chanfro (AddDA(D1, ângulo, E, F)); exige 0 < ângulo < 90°.
FeatureResult chamferEdgesAdvanced(const TopoDS_Shape& solid, const std::vector<TopoDS_Edge>& edges,
                                   const std::vector<TopoDS_Face>& faces, bool distanceAngle, double d1,
                                   double d2OrAngle);

} // namespace cad::modeling
