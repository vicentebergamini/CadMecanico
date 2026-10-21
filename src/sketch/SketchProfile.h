// Reconhecimento do perfil fechado de um esboço e construção da face plana OCCT (E5.3).
//
// Perfis suportados (exatamente UM contorno fechado por esboço):
//   - um retângulo;
//   - um círculo;
//   - segmentos de linha conectados formando um polígono simples;
//   - (E5.5) contornos de segmentos e arcos circulares, incluindo cantos com Fillet 2D e
//     Chamfer 2D (o esboço é resolvido por SketchResolver antes do reconhecimento). Arcos viram
//     arestas circulares exatas do OCCT (GC_MakeArcOfCircle pelos nós do contorno), nunca
//     polilinhas; a área do contorno é exata (polígono das cordas + segmentos circulares). A
//     verificação de autointerseção usa uma amostragem dos arcos (passo ≤ 2°), só para validar.
// Extremidades de linhas são consideradas coincidentes quando a distância entre elas é no
// máximo kClosureTolerance (mm). São rejeitados, com mensagem: esboço sem entidades, perfis
// abertos (extremidade livre), segmentos degenerados ou duplicados, nós com mais de dois
// segmentos (contorno ambíguo), autointerseções, área nula e múltiplos contornos.
// As entidades são 2D no plano do esboço, portanto a geometria é planar por construção; a face
// gerada é ainda verificada (BRepCheck_Analyzer e área).
#pragma once

#include "sketch/Sketch.h"

#include <TopoDS_Face.hxx>

#include <string>

namespace cad::sketch {

inline constexpr double kClosureTolerance = 1e-3; // mm
inline constexpr double kMinProfileArea = 1e-4;   // mm²

struct ProfileResult
{
    bool ok = false;
    std::string message;
    TopoDS_Face face;   // normal da face = normal do plano do esboço (contorno anti-horário)
    double area = 0.0;  // mm²
    int loops = 0;      // contornos fechados encontrados
};

ProfileResult buildProfile(const Sketch& sketch);

} // namespace cad::sketch
