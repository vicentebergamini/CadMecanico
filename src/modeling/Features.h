// Operações geométricas elementares usadas pelo documento paramétrico (sem dependência de Qt).
#pragma once

#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>
#include <gp_Vec.hxx>

#include <string>

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

} // namespace cad::modeling
