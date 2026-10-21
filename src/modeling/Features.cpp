#include "modeling/Features.h"

#include "modeling/ShapeInspector.h"

#include <BRepAlgoAPI_Common.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepFilletAPI_MakeChamfer.hxx>
#include <BRepFilletAPI_MakeFillet.hxx>
#include "modeling/EdgeNaming.h"
#include <BRep_Builder.hxx>
#include <TopoDS_Compound.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <Standard_Failure.hxx>
#include <gp_Ax2.hxx>

#include <cmath>
#include <vector>

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

FeatureResult extrudeFace(const TopoDS_Face& face, const gp_Vec& direction)
{
    FeatureResult r;
    if (face.IsNull())
    {
        r.message = "Perfil sem face";
        return r;
    }
    if (direction.Magnitude() <= 0.0)
    {
        r.message = "Profundidade de extrusão nula";
        return r;
    }
    try
    {
        BRepPrimAPI_MakePrism prism(face, direction, true /*copia a face*/, true);
        prism.Build();
        if (!prism.IsDone())
        {
            r.message = "Falha na extrusão (BRepPrimAPI_MakePrism)";
            return r;
        }
        r.shape = prism.Shape();
    }
    catch (const Standard_Failure& e)
    {
        r.message = std::string("Falha na extrusão: ") + e.what();
        return r;
    }

    // Um único sólido.
    int solids = 0;
    TopoDS_Shape solid;
    for (TopExp_Explorer ex(r.shape, TopAbs_SOLID); ex.More(); ex.Next())
    {
        ++solids;
        solid = ex.Current();
    }
    if (solids != 1)
    {
        r.message = "A extrusão não produziu exatamente um sólido";
        return r;
    }
    r.shape = solid;
    // Volume positivo: se o sólido estiver com orientação invertida, inverte-o.
    if (ShapeInspector::volume(r.shape) < 0.0)
        r.shape.Reverse();
    if (ShapeInspector::volume(r.shape) <= 0.0)
    {
        r.message = "Sólido extrudado com volume nulo";
        return r;
    }
    r.ok = ShapeInspector::isValid(r.shape);
    if (!r.ok)
        r.message = "Sólido extrudado inválido (BRepCheck)";
    return r;
}

int countSolids(const TopoDS_Shape& shape)
{
    int n = 0;
    for (TopExp_Explorer ex(shape, TopAbs_SOLID); ex.More(); ex.Next())
        ++n;
    return n;
}

namespace {

// Volume mínimo de um sólido resultante (mm³); abaixo disso o componente é tratado como resíduo
// numérico (ex.: contato tangente).
constexpr double kMinSolidVolume = 1e-6;

const char* kindName(BooleanKind kind)
{
    switch (kind)
    {
    case BooleanKind::Fuse:   return "união";
    case BooleanKind::Cut:    return "subtração";
    case BooleanKind::Common: return "interseção";
    }
    return "";
}

std::string checkOperand(const TopoDS_Shape& s, const char* label)
{
    if (s.IsNull())
        return std::string("Operando ") + label + " sem geometria";
    if (countSolids(s) == 0)
        return std::string("Operando ") + label + " não contém sólido";
    if (!ShapeInspector::isValid(s))
        return std::string("Operando ") + label + " com topologia inválida (BRepCheck)";
    return {};
}

} // namespace

FeatureResult booleanOperation(BooleanKind kind, const TopoDS_Shape& a, const TopoDS_Shape& b)
{
    FeatureResult r;
    for (const std::string& m : {checkOperand(a, "A"), checkOperand(b, "B")})
        if (!m.empty())
        {
            r.message = m;
            return r;
        }

    TopoDS_Shape raw;
    try
    {
        // Os construtores executam a operação; o estado é verificado antes de usar o resultado.
        auto take = [&raw](BRepAlgoAPI_BooleanOperation& op) {
            if (!op.IsDone() || op.HasErrors())
                return false;
            op.SimplifyResult(); // unifica faces/arestas coplanares criadas pela operação
            raw = op.Shape();
            return true;
        };
        bool done = false;
        switch (kind)
        {
        case BooleanKind::Fuse:
        {
            BRepAlgoAPI_Fuse op(a, b);
            done = take(op);
            break;
        }
        case BooleanKind::Cut:
        {
            BRepAlgoAPI_Cut op(a, b);
            done = take(op);
            break;
        }
        case BooleanKind::Common:
        {
            BRepAlgoAPI_Common op(a, b);
            done = take(op);
            break;
        }
        }
        if (!done)
        {
            r.message = std::string("Falha do algoritmo de ") + kindName(kind) + " (BRepAlgoAPI)";
            return r;
        }
    }
    catch (const Standard_Failure& e)
    {
        r.message = std::string("Falha na ") + kindName(kind) + ": " + e.what();
        return r;
    }
    // Sólidos com volume do resultado.
    std::vector<TopoDS_Shape> solids;
    for (TopExp_Explorer ex(raw, TopAbs_SOLID); ex.More(); ex.Next())
        if (std::fabs(ShapeInspector::volume(ex.Current())) > kMinSolidVolume)
            solids.push_back(ex.Current());
    if (solids.empty())
    {
        switch (kind)
        {
        case BooleanKind::Common:
            r.message = "Interseção vazia: os sólidos não têm volume em comum";
            break;
        case BooleanKind::Cut:
            r.message = "A subtração eliminou todo o corpo principal (resultado sem volume)";
            break;
        case BooleanKind::Fuse:
            r.message = "União sem volume";
            break;
        }
        return r;
    }
    if (solids.size() == 1)
        r.shape = solids.front();
    else
    {
        BRep_Builder builder;
        TopoDS_Compound compound;
        builder.MakeCompound(compound);
        for (const TopoDS_Shape& s : solids)
            builder.Add(compound, s);
        r.shape = compound;
    }
    if (!ShapeInspector::isValid(r.shape))
    {
        r.message = std::string("Resultado da ") + kindName(kind) + " com topologia inválida (BRepCheck)";
        return r;
    }
    if (ShapeInspector::volume(r.shape) <= kMinSolidVolume)
    {
        r.message = std::string("Resultado da ") + kindName(kind) + " sem volume";
        return r;
    }
    r.ok = true;
    return r;
}

namespace {

template <typename Builder, typename AddFn>
FeatureResult finishEdges(const TopoDS_Shape& solid, const std::vector<TopoDS_Edge>& edges, double value,
                          const char* what, const char* valueLabel, AddFn add)
{
    FeatureResult r;
    if (!std::isfinite(value))
    {
        r.message = std::string(valueLabel) + ": valor não numérico";
        return r;
    }
    if (value <= 0.0)
    {
        r.message = std::string(valueLabel) + " deve ser maior que zero";
        return r;
    }
    if (solid.IsNull() || countSolids(solid) == 0)
    {
        r.message = std::string(what) + " sem sólido de origem";
        return r;
    }
    if (edges.empty())
    {
        r.message = std::string(what) + ": selecione pelo menos uma aresta";
        return r;
    }
    for (const TopoDS_Edge& e : edges)
        if (!containsEdge(solid, e))
        {
            r.message = std::string(what) + ": aresta não pertence ao sólido de origem";
            return r;
        }
    const double before = ShapeInspector::volume(solid);
    try
    {
        Builder builder(solid);
        for (const TopoDS_Edge& e : edges)
            add(builder, e);
        builder.Build();
        if (!builder.IsDone())
        {
            r.message = std::string(valueLabel) + " inviável para as arestas selecionadas (o " + what
                        + " não pôde ser construído)";
            return r;
        }
        r.shape = builder.Shape();
    }
    catch (const Standard_Failure& e)
    {
        r.message = std::string(valueLabel) + " inviável para as arestas selecionadas: " + e.what();
        return r;
    }
    if (countSolids(r.shape) != countSolids(solid))
    {
        r.message = std::string(what) + " alterou o número de sólidos (geometria inviável)";
        return r;
    }
    if (!ShapeInspector::isValid(r.shape))
    {
        r.message = std::string(what) + " produziu topologia inválida (BRepCheck): " + valueLabel + " inviável";
        return r;
    }
    const double after = ShapeInspector::volume(r.shape);
    if (after <= 0.0 || after >= before)
    {
        r.message = std::string(what) + " sem remoção de material (geometria inviável)";
        return r;
    }
    r.ok = true;
    return r;
}

} // namespace

FeatureResult filletEdges(const TopoDS_Shape& solid, const std::vector<TopoDS_Edge>& edges, double radius)
{
    return finishEdges<BRepFilletAPI_MakeFillet>(solid, edges, radius, "fillet 3D", "Raio",
                                                 [radius](BRepFilletAPI_MakeFillet& b, const TopoDS_Edge& e) { b.Add(radius, e); });
}

FeatureResult chamferEdgesAdvanced(const TopoDS_Shape& solid, const std::vector<TopoDS_Edge>& edges,
                                   const std::vector<TopoDS_Face>& faces, bool distanceAngle, double d1, double d2OrAngle)
{
    FeatureResult r;
    if (faces.size() != edges.size())
    {
        r.message = "chanfro 3D: uma face de referência por aresta";
        return r;
    }
    if (!std::isfinite(d2OrAngle) || d2OrAngle <= 0.0 || (distanceAngle && d2OrAngle >= 90.0))
    {
        r.message = distanceAngle ? "Ângulo do chanfro deve estar entre 0° e 90° (exclusive)"
                                  : "Distância D2 deve ser maior que zero";
        return r;
    }
    size_t index = 0;
    const double angle = d2OrAngle * 3.14159265358979323846 / 180.0;
    return finishEdges<BRepFilletAPI_MakeChamfer>(
        solid, edges, d1, "chanfro 3D", distanceAngle ? "Combinação distância/ângulo" : "Combinação de distâncias",
        [&](BRepFilletAPI_MakeChamfer& b, const TopoDS_Edge& e) {
            const TopoDS_Face& f = faces[index++];
            if (distanceAngle)
                b.AddDA(d1, angle, e, f);
            else
                b.Add(d1, d2OrAngle, e, f);
        });
}

FeatureResult chamferEdges(const TopoDS_Shape& solid, const std::vector<TopoDS_Edge>& edges, double distance)
{
    return finishEdges<BRepFilletAPI_MakeChamfer>(solid, edges, distance, "chanfro 3D", "Distância",
                                                  [distance](BRepFilletAPI_MakeChamfer& b, const TopoDS_Edge& e) {
                                                      b.Add(distance, e);
                                                  });
}

} // namespace cad::modeling
