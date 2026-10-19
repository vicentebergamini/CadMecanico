// Peça de teste: bloco com furo passante.
#pragma once

#include <TopoDS_Shape.hxx>

namespace cad::modeling {

struct TestPartDimensions
{
    double length = 100.0; // X
    double width  = 60.0;  // Y
    double height = 20.0;  // Z
    double holeRadius = 10.0;
};

// Bloco length x width x height com furo cilíndrico vertical centrado.
TopoDS_Shape buildTestPart(const TestPartDimensions& dims = TestPartDimensions());

} // namespace cad::modeling
