// Classificação geométrica dos cubos candidatos em relação ao sólido (E5.7.2).
//
// Fundamento: seja Q o interior de um cubo (ou de um bloco de cubos), encolhido por δ em cada
// face. Se o contorno do sólido (suas faces) não atravessa Q, Q está inteiramente dentro ou
// inteiramente fora do material, e um único ponto interior decide. Se o contorno atravessa Q, o
// cubo contém material e vazio ao mesmo tempo (interseção volumétrica positiva, mas não contido).
//
// Teste de atravessamento (robusto, Open CASCADE), do mais barato ao mais caro:
//   1. faces cuja caixa envolvente exata não toca Q são descartadas;
//   2. faces cuja superfície de suporte ILIMITADA (plano, cilindro de eixo paralelo à grade,
//      esfera) não toca Q são descartadas (cálculo analítico exato);
//   3. (no orquestrador) pontos amostrados de Q — centro e cantos — com estados diferentes, ou um
//      ponto sobre a superfície, provam o atravessamento;
//   4. para as demais, BRepExtrema_DistShapeShape entre o SÓLIDO caixa Q e a face — distância nula
//      quando a face corta Q ou está dentro dele (solução interna).
// Isso detecta furos, cavidades internas e superfícies curvas que passam entre os pontos
// amostrados: testar só os oito vértices não é usado em nenhum critério.
//
// Critérios (por cubo):
//   1. totalmente contido:   contorno não atravessa Q  e  centro IN;
//   2. centro no material:   BRepClass3d_SolidClassifier do centro com tolerância explícita; ON
//                            (centro sobre a superfície, dentro da tolerância) NÃO é material;
//   3. interseção positiva:  contorno atravessa Q  ou  centro IN. Faces que apenas tocam o cubo
//                            (coincidentes com as faces do cubo, até δ) não contam.
//
// Tolerâncias: δ = max(1e-4·lco, 1e-6 mm) (encolhimento de Q; superfícies a menos de δ das faces
// do cubo são tratadas como contato) e tolerância de classificação de pontos = max(1e-7 mm,
// 1e-6·lco). Distância limite para "atravessa": δ/2.
//
// Eficiência: blocos de células são classificados de forma hierárquica (subdivisão binária do
// bloco de índices, como uma octree); blocos longe do contorno são decididos por um único ponto.
#pragma once

#include "discretization/Discretization.h"

#include <Bnd_Box.hxx>
#include <GeomAbs_SurfaceType.hxx>
#include <TopoDS_Face.hxx>
#include <gp_Ax3.hxx>
#include <TopoDS_Shape.hxx>
#include <TopoDS_Solid.hxx>

#include <memory>
#include <vector>

class BRepClass3d_SolidClassifier;

namespace cad::discretization {

enum class PointState
{
    In,
    On,
    Out
};

class CubeClassifier
{
public:
    // 'shape' deve conter ao menos um sólido; vários sólidos (corpo descontínuo) são tratados como
    // um único material (ponto IN em qualquer sólido).
    CubeClassifier(const TopoDS_Shape& shape, double lco);
    ~CubeClassifier();
    CubeClassifier(const CubeClassifier&) = delete;
    CubeClassifier& operator=(const CubeClassifier&) = delete;

    bool valid() const { return !m_solids.empty(); }
    double shrink() const { return m_delta; }
    double pointTolerance() const { return m_pointTol; }
    const Bnd_Box& box() const { return m_box; }
    std::size_t faceCount() const { return m_faces.size(); }

    // Classificação de um ponto (mm). Não é reentrante: usar um objeto por thread.
    PointState classify(const gp_XYZ& point) const;

    // Faces (índices), dentre 'candidates', que podem atravessar a caixa [pmin, pmax]: a caixa exata
    // da face toca a caixa e a superfície de suporte analítica (se houver) também.
    void facesNear(const gp_XYZ& pmin, const gp_XYZ& pmax, const std::vector<int>& candidates,
                   std::vector<int>& out) const;
    std::vector<int> allFaces() const;
    // true se alguma das faces atravessa a caixa aberta [pmin, pmax] (já encolhida). 'failures'
    // conta os testes sem resultado (tratados como atravessamento, conservador).
    bool boundaryCrosses(const gp_XYZ& pmin, const gp_XYZ& pmax, const std::vector<int>& faces,
                         long long* tests = nullptr, long long* failures = nullptr) const;

private:
    struct FaceEntry
    {
        TopoDS_Face face;
        Bnd_Box box;
        // Superfície de suporte analítica (exclusão exata e barata): plano, cilindro com eixo
        // paralelo a X/Y/Z ou esfera; demais tipos usam somente o teste de distância.
        GeomAbs_SurfaceType type = GeomAbs_OtherSurface;
        gp_Ax3 position;
        double radius = 0.0;
        int axis = -1; // cilindro: eixo da grade paralelo ao eixo do cilindro
    };
    // true se a superfície ILIMITADA de suporte da face não toca a caixa (então a face também não).
    static bool surfaceMissesBox(const FaceEntry& face, const gp_XYZ& pmin, const gp_XYZ& pmax);
    std::vector<TopoDS_Solid> m_solids;
    std::vector<FaceEntry> m_faces;
    Bnd_Box m_box;
    double m_delta = 1e-6;
    double m_pointTol = 1e-7;
    // Um classificador por sólido, criado no primeiro uso (carregar o sólido é a parte cara).
    mutable std::vector<std::unique_ptr<BRepClass3d_SolidClassifier>> m_classifiers;
};

} // namespace cad::discretization
