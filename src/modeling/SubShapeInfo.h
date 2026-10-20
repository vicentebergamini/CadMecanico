// Descrição de sub-shapes selecionados (face, aresta, vértice), sem dependência de Qt.
#pragma once

#include <TopoDS_Shape.hxx>
#include <gp_Pnt.hxx>

namespace cad::modeling {

enum class SubShapeKind
{
    None,
    Face,
    Edge,
    Vertex,
    Other
};

enum class GeometryKind
{
    None,
    // Superfícies
    Plane,
    Cylinder,
    Cone,
    Sphere,
    Torus,
    OtherSurface,
    // Curvas
    Line,
    Circle,
    Ellipse,
    OtherCurve,
    // Vértice
    Point
};

struct SubShapeInfo
{
    SubShapeKind kind = SubShapeKind::None;
    GeometryKind geometry = GeometryKind::None;
    int index = 0;       // posição (1..n) entre os sub-shapes do mesmo tipo na peça; 0 se não pertence
    int count = 0;       // total de sub-shapes desse tipo na peça
    double area = 0.0;   // faces
    double length = 0.0; // arestas
    double radius = 0.0; // cilindros, cones (raio de referência), esferas, círculos
    gp_Pnt point;        // vértices
};

// Descreve 'sub' em relação à peça 'part' (índice estável dado por TopExp::MapShapes).
SubShapeInfo describeSubShape(const TopoDS_Shape& part, const TopoDS_Shape& sub);

} // namespace cad::modeling
