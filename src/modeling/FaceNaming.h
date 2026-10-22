// Referência persistente de faces planas (E5.6) para esboços sobre faces.
//
// O índice do TopExp_Explorer muda a cada reconstrução e não serve como referência. A face de
// suporte é guardada pela operação de origem (corpo) e por uma assinatura geométrica:
//   - normal externa da face (orientação da face respeitada);
//   - posição do plano ao longo da normal (d = N · P);
//   - centroide e área.
//
// Remapeamento no resultado atual do corpo de origem (resolvePlanarFace):
//   1. candidatas = faces planas com a mesma normal (ângulo ≤ 1e-6 rad);
//   2. uma candidata no mesmo plano (|Δd| ≤ 1e-4 mm) → mesma face (exata); se houver várias no
//      mesmo plano, a de centroide mais próximo, com margem (ver 4);
//   3. senão, uma única candidata → face deslocada ao longo da normal (ex.: espessura alterada);
//   4. várias candidatas: aceita a de centroide mais próximo somente se a distância for menor que
//      metade da segunda; caso contrário a referência é ambígua;
//   5. nenhuma candidata (ou ambígua): o esboço dependente é invalidado com mensagem — nunca é
//      associado silenciosamente a outra face.
// Após cada reconstrução bem-sucedida a assinatura é atualizada para a face encontrada.
#pragma once

#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>
#include <gp_Dir.hxx>
#include <gp_Pln.hxx>
#include <gp_Pnt.hxx>

#include <optional>
#include <string>

namespace cad::modeling {

struct FaceSignature
{
    gp_Dir normal;      // normal externa
    double offset = 0;  // N · P (mm)
    gp_Pnt centroid;
    double area = 0.0;
    std::string name;   // nome topológico (E5.7.1, ver TopoNaming); vazio = só geometria
};

struct FaceMatch
{
    bool ok = false;
    bool exact = false;
    TopoDS_Face face;
    std::string message;
};

// Plano da face (normal externa) se ela for plana; vazio para faces curvas.
std::optional<gp_Pln> planarFacePlane(const TopoDS_Face& face);

// Assinatura de uma face plana (normal externa, plano, centroide e área).
FaceSignature describeFace(const TopoDS_Face& face);

FaceMatch resolvePlanarFace(const TopoDS_Shape& body, const FaceSignature& signature);

// true se 'face' é uma face de 'shape'.
bool containsFace(const TopoDS_Shape& shape, const TopoDS_Shape& face);
// A mesma face (IsSame) como ela aparece em 'shape', com a orientação do sólido (a face vinda da
// seleção do visualizador pode ter outra orientação). Nula se não pertencer a 'shape'.
TopoDS_Face faceInShape(const TopoDS_Shape& shape, const TopoDS_Shape& face);

} // namespace cad::modeling
