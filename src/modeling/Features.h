// Operações geométricas elementares usadas pelo documento paramétrico (sem dependência de Qt).
#pragma once

#include <TopoDS_Shape.hxx>

#include <string>

namespace cad::modeling {

struct FeatureResult
{
    bool ok = false;
    std::string message;
    TopoDS_Shape shape;
};

// Bloco alinhado aos eixos, com canto na origem: [0,length] x [0,width] x [0,height].
FeatureResult makeBlock(double length, double width, double height);

// Furo cilíndrico vertical (eixo Z) passante através de 'base', centrado em (centerX, centerY).
// O cilindro ultrapassa 1 mm acima e abaixo da caixa da base para garantir o furo passante.
FeatureResult cutThroughHole(const TopoDS_Shape& base, double centerX, double centerY, double radius);

} // namespace cad::modeling
