#include "modeling/Features.h"

#include "modeling/ShapeInspector.h"

#include <BRepAlgoAPI_Cut.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <Standard_Failure.hxx>
#include <gp_Ax2.hxx>

namespace cad::modeling {

FeatureResult makeBlock(double length, double width, double height)
{
    FeatureResult r;
    if (!(length > 0.0 && width > 0.0 && height > 0.0))
    {
        r.message = "Dimensões do bloco devem ser positivas";
        return r;
    }
    try
    {
        BRepPrimAPI_MakeBox box(length, width, height);
        box.Build();
        if (!box.IsDone())
        {
            r.message = "Falha ao construir o bloco";
            return r;
        }
        r.shape = box.Shape();
    }
    catch (const Standard_Failure& e)
    {
        r.message = std::string("Falha ao construir o bloco: ") + e.what();
        return r;
    }
    r.ok = ShapeInspector::isValid(r.shape);
    if (!r.ok)
        r.message = "Bloco resultante inválido";
    return r;
}

FeatureResult cutThroughHole(const TopoDS_Shape& base, double centerX, double centerY, double radius)
{
    FeatureResult r;
    if (base.IsNull())
    {
        r.message = "Furo sem sólido base";
        return r;
    }
    if (!(radius > 0.0))
    {
        r.message = "Raio do furo deve ser positivo";
        return r;
    }

    const Bnd_Box box = ShapeInspector::boundingBox(base);
    if (box.IsVoid())
    {
        r.message = "Sólido base vazio";
        return r;
    }
    double xmin, ymin, zmin, xmax, ymax, zmax;
    box.Get(xmin, ymin, zmin, xmax, ymax, zmax);

    try
    {
        const gp_Pnt bottom(centerX, centerY, zmin - 1.0);
        BRepPrimAPI_MakeCylinder cylinder(gp_Ax2(bottom, gp::DZ()), radius, (zmax - zmin) + 2.0);
        BRepAlgoAPI_Cut cut(base, cylinder.Shape());
        if (!cut.IsDone() || cut.HasErrors())
        {
            r.message = "Falha na operação booleana do furo";
            return r;
        }
        r.shape = cut.Shape();
    }
    catch (const Standard_Failure& e)
    {
        r.message = std::string("Falha ao construir o furo: ") + e.what();
        return r;
    }
    r.ok = ShapeInspector::isValid(r.shape);
    if (!r.ok)
        r.message = "Sólido resultante do furo inválido";
    return r;
}

} // namespace cad::modeling
