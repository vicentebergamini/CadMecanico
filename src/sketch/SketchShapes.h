// Representação Open CASCADE (arestas 3D no plano do esboço) das entidades 2D.
#pragma once

#include "sketch/Sketch.h"

#include <TopoDS_Shape.hxx>

namespace cad::sketch {

// Linha -> aresta; retângulo -> wire fechado com 4 arestas; círculo -> aresta circular fechada.
// Retorna shape nulo se a entidade for inválida.
TopoDS_Shape buildEntityShape(const SketchPlane& plane, const SketchEntity& entity);

// Composto com todas as entidades do esboço.
TopoDS_Shape buildSketchShape(const Sketch& sketch);

} // namespace cad::sketch
