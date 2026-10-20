// Planos de referência dos esboços e conversão entre coordenadas locais 2D e globais 3D.
//
// Convenção (Z vertical, como no visualizador). Um ponto local (u, v) do esboço corresponde a
//     P = origem + u * eixoHorizontal + v * eixoVertical
// e a normal é eixoHorizontal x eixoVertical (sistema dextrógiro). Olhando o plano de frente
// (câmera na direção -normal, "para cima" = eixo vertical), u cresce para a direita e v para cima,
// sem espelhamento:
//
//   Plano | Origem  | Horizontal (u) | Vertical (v) | Normal | Vista correspondente
//   XY    | (0,0,0) | +X             | +Y           | +Z     | Superior
//   XZ    | (0,0,0) | +X             | +Z           | -Y     | Frente
//   YZ    | (0,0,0) | +Y             | +Z           | +X     | Direita
#pragma once

#include <gp_Ax3.hxx>
#include <gp_Dir.hxx>
#include <gp_Pnt.hxx>
#include <gp_Pnt2d.hxx>

#include <optional>
#include <string>

namespace cad::sketch {

enum class PlaneType
{
    XY,
    XZ,
    YZ
};

class SketchPlane
{
public:
    explicit SketchPlane(PlaneType type = PlaneType::XY);

    PlaneType type() const { return m_type; }
    const gp_Pnt& origin() const { return m_origin; }
    const gp_Dir& horizontalAxis() const { return m_xAxis; }
    const gp_Dir& verticalAxis() const { return m_yAxis; }
    const gp_Dir& normal() const { return m_normal; }

    // Sistema de coordenadas OCCT do plano (origem, normal como eixo Z local, eixo X = horizontal).
    gp_Ax3 ax3() const;

    gp_Pnt toGlobal(double u, double v) const;
    gp_Pnt toGlobal(const gp_Pnt2d& p) const { return toGlobal(p.X(), p.Y()); }
    // Projeção ortogonal de um ponto 3D nas coordenadas locais do plano.
    gp_Pnt2d toLocal(const gp_Pnt& p) const;
    // Distância (com sinal, ao longo da normal) do ponto ao plano.
    double distance(const gp_Pnt& p) const;

    // Nome curto ("XY", "XZ", "YZ") usado no arquivo e na interface.
    static const char* name(PlaneType type);
    static std::optional<PlaneType> fromName(const std::string& name);

private:
    PlaneType m_type;
    gp_Pnt m_origin;
    gp_Dir m_xAxis;
    gp_Dir m_yAxis;
    gp_Dir m_normal;
};

} // namespace cad::sketch
