// Entidades geométricas 2D dos esboços (coordenadas locais do plano, em mm) e dimensões
// editáveis. Sem dependência de Qt. Não há solucionador de restrições: cada dimensão edita
// diretamente o parâmetro correspondente da entidade.
//
// E5.5: arco circular exato (centro, raio, ângulos inicial e final em graus no sistema local,
// sentido anti-horário ou horário) e os modificadores de canto Fillet 2D e Chamfer 2D. Um
// modificador referencia dois lados retos que se encontram num canto (duas linhas com uma
// extremidade comum, ou um canto de retângulo) e guarda só o raio/distância: os lados
// continuam com a geometria original no esboço e são recortados ao resolver o esboço
// (SketchResolver), de modo que o raio ou a distância podem ser editados depois.
#pragma once

#include <array>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include <gp_Pnt2d.hxx>

namespace cad::sketch {

inline constexpr double kMaxCoordinate = 10000.0; // |coordenada| máxima (mm)
inline constexpr double kMinSize = 0.01;          // comprimento, lado ou raio mínimo (mm)
inline constexpr double kMaxSize = 20000.0;
inline constexpr double kMinArcSweep = 0.1;     // varredura mínima do arco (graus)
inline constexpr double kMaxAngle = 720.0;      // |ângulo| máximo aceito (graus)

enum class EntityType
{
    Line,
    Rectangle,
    Circle,
    Arc,     // E5.5
    Fillet,  // E5.5: arredondamento de canto
    Chamfer  // E5.5: chanfro simétrico de canto
};

struct SketchLine
{
    double x1 = 0.0, y1 = 0.0, x2 = 0.0, y2 = 0.0;
    double length() const;
};

// Retângulo alinhado aos eixos locais: canto (x, y), largura e altura positivas.
struct SketchRectangle
{
    double x = 0.0, y = 0.0, width = 0.0, height = 0.0;

    // Cantos em sentido anti-horário a partir de (x, y).
    std::array<gp_Pnt2d, 4> corners() const;
    // Quatro segmentos formando o contorno fechado (fim de cada um = início do próximo).
    std::array<SketchLine, 4> segments() const;

    // Retângulo definido por dois cantos opostos quaisquer.
    static SketchRectangle fromCorners(double xa, double ya, double xb, double yb);
};

struct SketchCircle
{
    double cx = 0.0, cy = 0.0, radius = 0.0;
    double diameter() const { return 2.0 * radius; }
};

// Arco circular: centro, raio e ângulos (graus, medidos a partir do eixo horizontal local, no
// sentido anti-horário). O arco vai de startAngle a endAngle no sentido indicado: anti-horário
// (clockwise = false, padrão) ou horário. A varredura é sempre menor que 360° (circunferência
// completa = SketchCircle).
struct SketchArc
{
    double cx = 0.0, cy = 0.0, radius = 0.0;
    double startAngle = 0.0, endAngle = 90.0;
    bool clockwise = false;

    // Varredura no sentido de percurso, em graus, no intervalo [0, 360).
    double sweep() const;
    // Varredura com sinal em radianos (positiva = anti-horária).
    double signedSweepRadians() const;
    double length() const;
    gp_Pnt2d pointAt(double t) const; // t em [0, 1] ao longo do percurso
    gp_Pnt2d startPoint() const { return pointAt(0.0); }
    gp_Pnt2d endPoint() const { return pointAt(1.0); }

    // Centro -> ponto inicial (define o raio) -> ponto final (projetado na circunferência).
    static SketchArc fromCenterStartEnd(const gp_Pnt2d& center, const gp_Pnt2d& start, const gp_Pnt2d& end,
                                        bool clockwise);
};

// Modificador de canto. Duas linhas: entityA/entityB = ids das linhas e corner = -1 (o canto é
// a extremidade comum). Retângulo: entityA = entityB = id do retângulo e corner = índice do
// canto (0 a 3, na ordem de SketchRectangle::corners()).
struct SketchFillet
{
    std::string entityA, entityB;
    int corner = -1;
    double radius = 0.0;
};

// Modos do chanfro (E5.6). Lado de referência = primeiro lado do canto (linha entityA, ou o lado
// do retângulo que termina no canto), ou o segundo se 'flip' for verdadeiro.
//   Equal         : distância 'distance' nos dois lados (modo da E5.5);
//   TwoDistances  : D1 = 'distance' no lado de referência, D2 = 'distance2' no outro lado;
//   DistanceAngle : D1 = 'distance' no lado de referência e 'angle' = ângulo (graus) entre o lado
//                   de referência e o segmento do chanfro, medido no ponto de recorte, por dentro
//                   do canto. D2 = D1 · sen(α) / sen(θ + α), θ = ângulo interno do canto; exige
//                   0 < α e θ + α < 180°.
enum class ChamferMode
{
    Equal,
    TwoDistances,
    DistanceAngle
};

struct SketchChamfer
{
    std::string entityA, entityB;
    int corner = -1;
    double distance = 0.0; // D1, medida a partir do vértice original, ao longo do lado
    ChamferMode mode = ChamferMode::Equal;
    double distance2 = 0.0; // D2 (modo TwoDistances)
    double angle = 45.0;    // graus (modo DistanceAngle)
    bool flip = false;      // referência no segundo lado
};

const char* chamferModeName(ChamferMode mode); // "equal", "twoDistances", "distanceAngle"

using SketchGeometry =
    std::variant<SketchLine, SketchRectangle, SketchCircle, SketchArc, SketchFillet, SketchChamfer>;

// true para Fillet 2D e Chamfer 2D (dependem de outras entidades do esboço).
bool isCornerModifier(const SketchGeometry& geometry);

// Natureza da dimensão: comprimento/coordenada (mm), ângulo (graus) ou opção sim/não.
enum class DimensionKind
{
    Length,
    Angle,
    Flag,
    Choice // índice em 'choices'
};

// Dimensão (coordenada ou medida) de uma entidade. Valores calculados (comprimento da linha,
// diâmetro) são apenas informativos.
struct SketchDimension
{
    std::string entityId;
    std::string key;   // "x1", "width", "radius"...
    std::string label; // "X inicial", "Largura"...
    double value = 0.0;
    bool editable = true;
    DimensionKind kind = DimensionKind::Length;
    std::vector<std::string> choices; // DimensionKind::Choice
};

struct EntityValidation
{
    bool ok = true;
    std::string field;
    std::string message;
};

struct SketchEntity
{
    std::string id;       // estável dentro do esboço (ex.: "e-3"); nunca reutilizado
    std::string sketchId; // esboço ao qual pertence
    SketchGeometry geometry;

    EntityType type() const;
    bool isValid() const { return validate(geometry).ok; }

    static EntityValidation validate(const SketchGeometry& geometry);
};

const char* typeName(EntityType type); // "line", "rectangle", "circle", "arc", "fillet", "chamfer"
std::optional<EntityType> typeFromName(const std::string& name);

// Dimensões da entidade, na ordem de exibição.
std::vector<SketchDimension> dimensionsOf(const SketchEntity& entity);

// Geometria com a dimensão 'key' alterada (sem validar). Retorna false se a chave não for
// editável para o tipo da entidade.
bool applyDimension(SketchGeometry& geometry, const std::string& key, double value);

} // namespace cad::sketch
