// Representação Open CASCADE (arestas 3D no plano do esboço) das entidades 2D.
#pragma once

#include "sketch/Sketch.h"
#include "sketch/SketchResolver.h"

#include <TopoDS_Edge.hxx>
#include <TopoDS_Shape.hxx>

#include <string>
#include <utility>
#include <vector>

namespace cad::sketch {

// Linha -> aresta; retângulo -> wire fechado com 4 arestas; círculo -> aresta circular fechada;
// arco -> aresta circular exata (E5.5). Retorna shape nulo se a entidade for inválida ou for um
// modificador de canto (que depende das outras entidades: ver buildResolvedShapes).
TopoDS_Shape buildEntityShape(const SketchPlane& plane, const SketchEntity& entity);

// Aresta OCCT de uma curva resolvida: segmento reto ou arco/circunferência exatos (gp_Circ com
// eixo = normal do plano e direção X = eixo horizontal; ângulos locais = parâmetros do círculo).
TopoDS_Edge buildCurveEdge(const SketchPlane& plane, const Curve2d& curve);

// Forma de cada entidade após a resolução (lados recortados pelos fillets/chanfros; arco do
// fillet e segmento do chanfro como forma do modificador), na ordem das entidades.
std::vector<std::pair<std::string, TopoDS_Shape>> buildResolvedShapes(const Sketch& sketch);

// Composto com todas as entidades do esboço (resolvidas).
TopoDS_Shape buildSketchShape(const Sketch& sketch);

} // namespace cad::sketch