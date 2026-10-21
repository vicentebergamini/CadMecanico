// Resolução do esboço (E5.5): converte as entidades em curvas 2D elementares (segmentos, arcos e
// circunferências, em coordenadas locais) e aplica os modificadores de canto Fillet 2D e
// Chamfer 2D, recortando os lados e inserindo o arco tangente ou o segmento do chanfro.
//
// Cada curva guarda o id da entidade de origem: os lados recortados continuam pertencendo à
// linha/retângulo; o arco do fillet e o segmento do chanfro pertencem ao modificador. A mesma
// resolução alimenta a exibição, a seleção e o construtor de perfis da extrusão (arcos exatos,
// convertidos em arestas circulares do OCCT).
//
// Fillet de raio r num canto de ângulo interno θ entre os lados de direções unitárias u e v
// (a partir do vértice P): distância de tangência t = r / tan(θ/2), pontos de tangência
// P + u·t e P + v·t, centro P + w·r / sin(θ/2) com w = (u + v)/|u + v|; o arco (varredura
// π − θ) é tangente aos dois lados. Chamfer de distância d: pontos P + u·d e P + v·d ligados
// por um segmento. Ambos exigem que sobre, em cada lado, pelo menos o comprimento mínimo de
// entidade (0,01 mm) além do recorte.
#pragma once

#include "sketch/Sketch.h"

#include <gp_Pnt2d.hxx>

#include <optional>
#include <string>
#include <vector>

namespace cad::sketch {

struct Curve2d
{
    enum class Kind
    {
        Segment,
        Arc,
        Circle
    };
    Kind kind = Kind::Segment;
    gp_Pnt2d start, end;       // segmento/arco, no sentido de percurso
    gp_Pnt2d center;           // arco/circunferência
    double radius = 0.0;
    double startAngle = 0.0;   // radianos
    double sweep = 0.0;        // radianos, com sinal (positivo = anti-horário)
    std::string entityId;      // entidade de origem
    int part = 0;              // lado do retângulo (0 a 3); 0 nos demais

    gp_Pnt2d pointAt(double t) const;
    double length() const;
};

struct ResolvedSketch
{
    bool ok = true;
    std::string message;
    std::string failedEntity; // modificador que não pôde ser aplicado
    std::vector<Curve2d> curves;
};

ResolvedSketch resolveSketch(const Sketch& sketch);

// Canto livre (sem modificador) entre dois lados retos: duas linhas com uma extremidade comum
// ou um canto de retângulo.
struct SketchCorner
{
    gp_Pnt2d point;
    std::string entityA, entityB;
    int corner = -1;
};

std::vector<SketchCorner> findCorners(const Sketch& sketch);
// Canto mais próximo de 'p' dentro da tolerância (mm).
std::optional<SketchCorner> nearestCorner(const Sketch& sketch, const gp_Pnt2d& p, double tolerance);

} // namespace cad::sketch
