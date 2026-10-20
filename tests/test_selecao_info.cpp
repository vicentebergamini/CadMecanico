// Descrição das entidades selecionáveis da peça de teste (sem Qt, sem OpenGL).
#include "TestCheck.h"

#include "modeling/SubShapeInfo.h"
#include "modeling/TestPart.h"

#include <BRepPrimAPI_MakeBox.hxx>
#include <NCollection_IndexedMap.hxx>
#include <TopExp.hxx>
#include <TopTools_ShapeMapHasher.hxx>

#include <cstdio>

using namespace cad::modeling;

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kTol = 1e-6;

NCollection_IndexedMap<TopoDS_Shape, TopTools_ShapeMapHasher> subShapes(const TopoDS_Shape& s, TopAbs_ShapeEnum t)
{
    NCollection_IndexedMap<TopoDS_Shape, TopTools_ShapeMapHasher> map;
    TopExp::MapShapes(s, t, map);
    return map;
}

} // namespace

int main()
{
    const TopoDS_Shape part = buildTestPart();

    // Faces: 6 planas (topo e base com furo) + 1 cilíndrica (furo R10).
    {
        const auto faces = subShapes(part, TopAbs_FACE);
        int planes = 0, cylinders = 0;
        double total = 0.0, holeArea = 0.0;
        int pierced = 0;
        for (int i = 1; i <= faces.Extent(); ++i)
        {
            const SubShapeInfo info = describeSubShape(part, faces(i));
            CAD_CHECK(info.kind == SubShapeKind::Face);
            CAD_CHECK(info.index == i);
            CAD_CHECK(info.count == 7);
            total += info.area;
            if (info.geometry == GeometryKind::Plane)
            {
                ++planes;
                if (std::fabs(info.area - (6000.0 - kPi * 100.0)) < kTol)
                    ++pierced;
            }
            else if (info.geometry == GeometryKind::Cylinder)
            {
                ++cylinders;
                holeArea = info.area;
                CAD_CHECK_NEAR(info.radius, 10.0, kTol);
            }
        }
        std::printf("Faces: %d planas, %d cilindricas, area total %.4f\n", planes, cylinders, total);
        CAD_CHECK(planes == 6);
        CAD_CHECK(cylinders == 1);
        CAD_CHECK(pierced == 2);
        CAD_CHECK_NEAR(holeArea, 2.0 * kPi * 10.0 * 20.0, 1e-6);
        CAD_CHECK_NEAR(total, 2.0 * (6000.0 - kPi * 100.0) + 2.0 * 2000.0 + 2.0 * 1200.0 + 2.0 * kPi * 200.0, 1e-6);
    }

    // Arestas: 12 retas do bloco + costura do furo (reta, 20 mm) + 2 círculos R10.
    {
        const auto edges = subShapes(part, TopAbs_EDGE);
        int lines = 0, circles = 0, len100 = 0, len60 = 0, len20 = 0;
        for (int i = 1; i <= edges.Extent(); ++i)
        {
            const SubShapeInfo info = describeSubShape(part, edges(i));
            CAD_CHECK(info.kind == SubShapeKind::Edge);
            CAD_CHECK(info.index == i && info.count == 15);
            if (info.geometry == GeometryKind::Line)
            {
                ++lines;
                len100 += std::fabs(info.length - 100.0) < kTol;
                len60 += std::fabs(info.length - 60.0) < kTol;
                len20 += std::fabs(info.length - 20.0) < kTol;
            }
            else if (info.geometry == GeometryKind::Circle)
            {
                ++circles;
                CAD_CHECK_NEAR(info.radius, 10.0, kTol);
                CAD_CHECK_NEAR(info.length, 2.0 * kPi * 10.0, 1e-6);
            }
        }
        std::printf("Arestas: %d retas (%d x100, %d x60, %d x20), %d circulos\n", lines, len100, len60, len20, circles);
        CAD_CHECK(lines == 13);
        CAD_CHECK(circles == 2);
        CAD_CHECK(len100 == 4 && len60 == 4 && len20 == 5);
    }

    // Vértices: 8 cantos do bloco + 2 nas bordas do furo.
    {
        const auto vertices = subShapes(part, TopAbs_VERTEX);
        int corners = 0;
        for (int i = 1; i <= vertices.Extent(); ++i)
        {
            const SubShapeInfo info = describeSubShape(part, vertices(i));
            CAD_CHECK(info.kind == SubShapeKind::Vertex && info.geometry == GeometryKind::Point);
            CAD_CHECK(info.index == i && info.count == 10);
            const gp_Pnt& p = info.point;
            const bool cornerX = std::fabs(p.X()) < kTol || std::fabs(p.X() - 100.0) < kTol;
            const bool cornerY = std::fabs(p.Y()) < kTol || std::fabs(p.Y() - 60.0) < kTol;
            const bool cornerZ = std::fabs(p.Z()) < kTol || std::fabs(p.Z() - 20.0) < kTol;
            corners += cornerX && cornerY && cornerZ;
        }
        std::printf("Vertices: %d cantos de %d\n", corners, vertices.Extent());
        CAD_CHECK(corners == 8);
    }

    // Entidade que não pertence à peça: índice 0. Shape nulo: sem tipo.
    const TopoDS_Shape other = BRepPrimAPI_MakeBox(1.0, 1.0, 1.0).Shape();
    const auto foreignFace = subShapes(other, TopAbs_FACE)(1);
    const SubShapeInfo foreign = describeSubShape(part, foreignFace);
    CAD_CHECK(foreign.kind == SubShapeKind::Face && foreign.index == 0 && foreign.count == 7);
    CAD_CHECK(describeSubShape(part, TopoDS_Shape()).kind == SubShapeKind::None);

    return cad::test::finish("test_selecao_info");
}
