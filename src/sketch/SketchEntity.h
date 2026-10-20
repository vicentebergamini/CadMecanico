// Entidades geométricas 2D dos esboços (coordenadas locais do plano, em mm) e dimensões
// editáveis. Sem dependência de Qt. Não há solucionador de restrições: cada dimensão edita
// diretamente o parâmetro correspondente da entidade.
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

enum class EntityType
{
    Line,
    Rectangle,
    Circle
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

using SketchGeometry = std::variant<SketchLine, SketchRectangle, SketchCircle>;

// Dimensão (coordenada ou medida) de uma entidade. Valores calculados (comprimento da linha,
// diâmetro) são apenas informativos.
struct SketchDimension
{
    std::string entityId;
    std::string key;   // "x1", "width", "radius"...
    std::string label; // "X inicial", "Largura"...
    double value = 0.0;
    bool editable = true;
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

const char* typeName(EntityType type);                 // "line", "rectangle", "circle"
std::optional<EntityType> typeFromName(const std::string& name);

// Dimensões da entidade, na ordem de exibição.
std::vector<SketchDimension> dimensionsOf(const SketchEntity& entity);

// Geometria com a dimensão 'key' alterada (sem validar). Retorna false se a chave não for
// editável para o tipo da entidade.
bool applyDimension(SketchGeometry& geometry, const std::string& key, double value);

} // namespace cad::sketch
