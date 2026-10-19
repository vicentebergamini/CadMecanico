#include "modeling/TestPart.h"

#include <BRepAlgoAPI_Cut.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <gp_Ax2.hxx>

namespace cad::modeling {

TopoDS_Shape buildTestPart(const TestPartDimensions& dims)
{
    // O cilindro ultrapassa 1 mm em cada face para garantir o furo passante.
    const gp_Pnt base(dims.length / 2.0, dims.width / 2.0, -1.0);

    TopoDS_Shape bloco = BRepPrimAPI_MakeBox(dims.length, dims.width, dims.height).Shape();
    TopoDS_Shape furo  = BRepPrimAPI_MakeCylinder(gp_Ax2(base, gp::DZ()), dims.holeRadius, dims.height + 2.0).Shape();
    return BRepAlgoAPI_Cut(bloco, furo).Shape();
}

} // namespace cad::modeling
