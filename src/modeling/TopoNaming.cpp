#include "modeling/TopoNaming.h"

#include <BRepGProp.hxx>
#include <BRep_Tool.hxx>
#include <GProp_GProps.hxx>
#include <NCollection_IndexedDataMap.hxx>
#include <NCollection_IndexedMap.hxx>
#include <NCollection_List.hxx>
#include <Standard_Failure.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_ShapeMapHasher.hxx>
#include <TopoDS.hxx>

#include <algorithm>
#include <cmath>

namespace cad::modeling {

namespace {

using ShapeSet = NCollection_IndexedMap<TopoDS_Shape, TopTools_ShapeMapHasher>;
using EdgeFaceMap = NCollection_IndexedDataMap<TopoDS_Shape, NCollection_List<TopoDS_Shape>, TopTools_ShapeMapHasher>;

// Desempate entre partes com o mesmo nome: a mais próxima precisa estar a menos da metade da
// distância da segunda.
constexpr double kMargin = 0.5;

gp_Pnt centroidOf(const TopoDS_Shape& s, bool surface)
{
    GProp_GProps props;
    if (surface)
        BRepGProp::SurfaceProperties(s, props);
    else
        BRepGProp::LinearProperties(s, props);
    return props.CentreOfMass();
}

} // namespace

void FaceNames::add(const TopoDS_Shape& face, const std::string& name)
{
    for (auto& item : m_items)
        if (item.first.IsSame(face))
        {
            item.second = name;
            return;
        }
    m_items.emplace_back(face, name);
}

std::string FaceNames::nameOf(const TopoDS_Shape& face) const
{
    for (const auto& item : m_items)
        if (item.first.IsSame(face))
            return item.second;
    return {};
}

std::vector<TopoDS_Face> FaceNames::facesNamed(const TopoDS_Shape& shape, const std::string& name) const
{
    std::vector<TopoDS_Face> faces;
    if (shape.IsNull() || name.empty())
        return faces;
    ShapeSet seen;
    for (TopExp_Explorer ex(shape, TopAbs_FACE); ex.More(); ex.Next())
    {
        const int before = seen.Extent();
        if (seen.Add(ex.Current()) > before && nameOf(ex.Current()) == name)
            faces.push_back(TopoDS::Face(ex.Current()));
    }
    return faces;
}

FaceNames nameBoxFaces(const TopoDS_Shape& box, const std::string& prefix)
{
    FaceNames names;
    for (TopExp_Explorer ex(box, TopAbs_FACE); ex.More(); ex.Next())
    {
        const auto plane = planarFacePlane(TopoDS::Face(ex.Current()));
        if (!plane)
            continue;
        const gp_Dir n = plane->Axis().Direction();
        const char* role = n.X() > 0.5 ? "xmax" : n.X() < -0.5 ? "xmin" : n.Y() > 0.5 ? "ymax"
                           : n.Y() < -0.5 ? "ymin" : n.Z() > 0.5 ? "zmax" : "zmin";
        names.add(ex.Current(), prefix + ":" + role);
    }
    return names;
}

FaceNames propagateNames(const std::vector<const FaceNames*>& inputs, const FeatureResult& result)
{
    FaceNames out;
    ShapeSet resultFaces;
    TopExp::MapShapes(result.shape, TopAbs_FACE, resultFaces);
    const BRepTools_History* history = result.history.IsNull() ? nullptr : result.history.get();
    for (const FaceNames* names : inputs)
    {
        if (!names)
            continue;
        for (const auto& [face, name] : names->items())
        {
            if (history)
            {
                if (history->IsRemoved(face))
                    continue;
                const auto& modified = history->Modified(face);
                if (!modified.IsEmpty())
                {
                    for (const TopoDS_Shape& m : modified)
                        if (m.ShapeType() == TopAbs_FACE && resultFaces.Contains(m))
                            out.add(m, name);
                    continue;
                }
            }
            if (resultFaces.Contains(face))
                out.add(face, name);
        }
    }
    return out;
}

void nameGenerated(FaceNames& names, const FeatureResult& result,
                   const std::function<std::string(const TopoDS_Shape& edge)>& labelFor)
{
    ShapeSet resultFaces;
    TopExp::MapShapes(result.shape, TopAbs_FACE, resultFaces);
    for (const auto& [edge, face] : result.generated)
    {
        // A face gerada pode ter sido modificada depois (ex.: simplificação da booleana).
        std::vector<TopoDS_Shape> images;
        if (resultFaces.Contains(face))
            images.push_back(face);
        else if (!result.history.IsNull())
            for (const TopoDS_Shape& m : result.history->Modified(face))
                if (resultFaces.Contains(m))
                    images.push_back(m);
        const std::string label = labelFor(edge);
        if (label.empty())
            continue;
        for (const TopoDS_Shape& f : images)
            if (names.nameOf(f).empty())
                names.add(f, label);
    }
}

void completeNames(FaceNames& names, const TopoDS_Shape& result, const std::string& prefix)
{
    ShapeSet seen;
    int k = 0;
    for (TopExp_Explorer ex(result, TopAbs_FACE); ex.More(); ex.Next())
    {
        const int before = seen.Extent();
        if (seen.Add(ex.Current()) > before && names.nameOf(ex.Current()).empty())
            names.add(ex.Current(), prefix + ":" + std::to_string(k++));
    }
}

std::string edgeName(const TopoDS_Shape& solid, const FaceNames& names, const TopoDS_Edge& edge)
{
    if (solid.IsNull() || edge.IsNull())
        return {};
    EdgeFaceMap map;
    TopExp::MapShapesAndAncestors(solid, TopAbs_EDGE, TopAbs_FACE, map);
    const NCollection_List<TopoDS_Shape>* adjacent = map.Seek(edge);
    if (!adjacent)
        return {};
    std::vector<std::string> parts;
    for (const TopoDS_Shape& f : *adjacent)
    {
        const std::string n = names.nameOf(f);
        if (n.empty())
            return {};
        if (std::find(parts.begin(), parts.end(), n) == parts.end())
            parts.push_back(n);
    }
    std::sort(parts.begin(), parts.end());
    std::string joined;
    for (const std::string& p : parts)
        joined += (joined.empty() ? "" : "|") + p;
    return joined;
}

NamedFaceMatch resolveNamedFace(const TopoDS_Shape& body, const FaceNames& names, const FaceSignature& signature)
{
    NamedFaceMatch r;
    if (signature.name.empty())
    {
        // Referência sem nome (documento anterior à E5.7.1): somente geometria.
        const FaceMatch geometric = resolvePlanarFace(body, signature);
        r.ok = geometric.ok;
        r.face = geometric.face;
        r.message = geometric.message;
        return r;
    }
    const std::vector<TopoDS_Face> candidates = names.facesNamed(body, signature.name);
    if (candidates.size() == 1)
    {
        r.ok = r.byName = true;
        r.face = candidates.front();
        return r;
    }
    if (candidates.size() > 1)
    {
        // Face dividida por uma operação posterior: a parte de centroide mais próximo, com margem.
        std::vector<std::pair<double, TopoDS_Face>> d;
        for (const TopoDS_Face& f : candidates)
            d.emplace_back(centroidOf(f, true).Distance(signature.centroid), f);
        std::sort(d.begin(), d.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        if (d[0].first < kMargin * d[1].first)
        {
            r.ok = r.byName = true;
            r.face = d[0].second;
            return r;
        }
        r.message = "face \"" + signature.name + "\" ambígua: " + std::to_string(candidates.size())
                    + " partes com o mesmo nome topológico, sem correspondência segura";
        return r;
    }
    // Nome ausente no corpo: só uma correspondência geométrica exata (mesmo plano) e única.
    const FaceMatch geometric = resolvePlanarFace(body, signature);
    if (geometric.ok && geometric.exact)
    {
        r.ok = true;
        r.face = geometric.face;
        return r;
    }
    r.message = "face não remapeável: \"" + signature.name + "\" não existe mais no corpo (removida "
                "ou substituída por uma alteração anterior) e não há face equivalente no mesmo plano";
    return r;
}

NamedEdgeMatch resolveNamedEdge(const TopoDS_Shape& body, const FaceNames& names, const EdgeSignature& signature)
{
    NamedEdgeMatch r;
    if (signature.name.empty())
    {
        const EdgeMatch geometric = resolveEdge(body, signature);
        r.ok = geometric.ok;
        r.edge = geometric.edge;
        r.message = geometric.message;
        return r;
    }
    std::vector<std::pair<double, TopoDS_Edge>> candidates;
    try
    {
        EdgeFaceMap map;
        TopExp::MapShapesAndAncestors(body, TopAbs_EDGE, TopAbs_FACE, map);
        for (int i = 1; i <= map.Extent(); ++i)
        {
            const TopoDS_Edge e = TopoDS::Edge(map.FindKey(i));
            if (BRep_Tool::Degenerated(e) || edgeName(body, names, e) != signature.name)
                continue;
            candidates.emplace_back(centroidOf(e, false).Distance(signature.mid), e);
        }
    }
    catch (const Standard_Failure& e)
    {
        r.message = std::string("falha ao remapear a aresta: ") + e.what();
        return r;
    }
    std::sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    if (candidates.size() == 1 || (candidates.size() > 1 && candidates[0].first < kMargin * candidates[1].first))
    {
        r.ok = r.byName = true;
        r.edge = candidates.front().second;
        return r;
    }
    if (candidates.size() > 1)
    {
        r.message = "aresta \"" + signature.name + "\" ambígua: " + std::to_string(candidates.size())
                    + " arestas entre as mesmas faces, sem correspondência segura";
        return r;
    }
    const EdgeMatch geometric = resolveEdge(body, signature);
    if (geometric.ok && geometric.exact)
    {
        r.ok = true;
        r.edge = geometric.edge;
        return r;
    }
    r.message = "aresta não remapeável: \"" + signature.name + "\" não existe mais no corpo (faces adjacentes "
                "removidas ou substituídas) e não há aresta equivalente na mesma posição";
    return r;
}

} // namespace cad::modeling
