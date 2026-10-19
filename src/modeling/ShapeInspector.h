// Consultas topológicas e geométricas sobre shapes (sem dependência de Qt).
#pragma once

#include <Bnd_Box.hxx>
#include <TopAbs_ShapeEnum.hxx>
#include <TopoDS_Shape.hxx>

namespace cad::modeling {

struct ShapeSummary
{
    bool isNull  = true;
    bool isValid = false;
    int solids   = 0;
    int shells   = 0;
    int faces    = 0;
    int edges    = 0;           // arestas distintas
    int edgeOccurrences = 0;    // uma por wire em que a aresta aparece
    int vertices = 0;
    double volume = 0.0;
    double area   = 0.0;
    Bnd_Box boundingBox;
};

class ShapeInspector
{
public:
    static ShapeSummary inspect(const TopoDS_Shape& shape);

    static int countDistinct(const TopoDS_Shape& shape, TopAbs_ShapeEnum type);
    static int countOccurrences(const TopoDS_Shape& shape, TopAbs_ShapeEnum type);

    static double volume(const TopoDS_Shape& shape);
    static double area(const TopoDS_Shape& shape);

    // Caixa exata pela geometria, sem tolerâncias nem triangulação.
    static Bnd_Box boundingBox(const TopoDS_Shape& shape);

    static bool isValid(const TopoDS_Shape& shape);
};

} // namespace cad::modeling
