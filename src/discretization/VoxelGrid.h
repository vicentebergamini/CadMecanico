// Grade estruturada da discretização volumétrica (E5.7.2), sem dependência de Qt.
//
// Sistema de referência: eixos X, Y e Z globais do CAD (orientação fixa nesta etapa). A origem da
// grade é o canto mínimo da caixa envolvente exata do sólido, arredondado a 1 nm (1e-6 mm) para
// eliminar ruído numérico da caixa; ela NÃO é deslocada para (0, 0, 0).
//
// Índices inteiros: célula (cubo) (i, j, k) com 0 <= i < nx, 0 <= j < ny, 0 <= k < nz; ponto da
// rede (vértice) (i, j, k) com 0 <= i <= nx etc. Índice linear com X variando mais rapidamente,
// depois Y e por último Z — a ordem crescente do índice linear é a ordem Z → Y → X.
//
// Coordenadas sempre calculadas por "origem + índice inteiro × lco" (nunca por somas sucessivas).
// Quando origem e lco são múltiplos exatos de 1 nm (caso de valores decimais digitados pelo
// usuário), o cálculo é feito em inteiros de meio-nanômetro e dividido uma única vez pelo fator da
// unidade: o resultado é o double mais próximo do valor decimal exato (ex.: 0,3 mm e não
// 0,30000000000000004; 20 mm = 0,02 m).
#pragma once

#include <gp_XYZ.hxx>

#include <cmath>
#include <cstdint>

namespace cad::discretization {

// Unidade das coordenadas exportadas. A geometria e lco ficam sempre em mm.
enum class LengthUnit
{
    Millimeter,
    Meter
};

const char* unitSymbol(LengthUnit unit); // "mm", "m"
const char* unitName(LengthUnit unit);   // "milímetros", "metros"

struct VoxelGrid
{
    static constexpr double kNanometersPerMm = 1.0e6;

    gp_XYZ origin;     // mm (canto mínimo, arredondado a 1 nm)
    double lco = 1.0;  // mm
    int nx = 0, ny = 0, nz = 0;
    // Representação exata em nm (válida se 'exact').
    std::int64_t originNm[3] = {0, 0, 0};
    std::int64_t lcoNm = 0;
    bool exact = false;

    // Grade com origem (mm) arredondada a 1 nm e lco dado; nx/ny/nz preenchidos por quem cria.
    static VoxelGrid make(const gp_XYZ& minCorner, double lco);

    std::int64_t cellCount() const { return std::int64_t(nx) * ny * nz; }
    std::int64_t latticeCount() const { return std::int64_t(nx + 1) * (ny + 1) * (nz + 1); }

    std::int64_t cellIndex(int i, int j, int k) const { return (std::int64_t(k) * ny + j) * nx + i; }
    std::int64_t latticeIndex(int i, int j, int k) const
    {
        return (std::int64_t(k) * (ny + 1) + j) * (nx + 1) + i;
    }
    void cellIjk(std::int64_t cell, int& i, int& j, int& k) const
    {
        i = int(cell % nx);
        j = int((cell / nx) % ny);
        k = int(cell / (std::int64_t(nx) * ny));
    }
    void latticeIjk(std::int64_t point, int& i, int& j, int& k) const
    {
        i = int(point % (nx + 1));
        j = int((point / (nx + 1)) % (ny + 1));
        k = int(point / (std::int64_t(nx + 1) * (ny + 1)));
    }

    // Coordenada no eixo 'axis' (0 = X, 1 = Y, 2 = Z) do índice em meias células: h = 2·i para
    // pontos da rede e h = 2·i + 1 para centros de células. Resultado na unidade pedida.
    double coordinate(int axis, std::int64_t halfIndex, LengthUnit unit = LengthUnit::Millimeter) const
    {
        if (exact)
        {
            // 2·origem + h·lco em nm (inteiro exato), uma única divisão.
            const std::int64_t twice = 2 * originNm[axis] + halfIndex * lcoNm;
            const double divisor = unit == LengthUnit::Meter ? 2.0e9 : 2.0e6;
            return double(twice) / divisor;
        }
        const double mm = origin.Coord(axis + 1) + 0.5 * double(halfIndex) * lco;
        return unit == LengthUnit::Meter ? mm / 1000.0 : mm;
    }
    gp_XYZ vertex(int i, int j, int k, LengthUnit unit = LengthUnit::Millimeter) const
    {
        return gp_XYZ(coordinate(0, 2 * std::int64_t(i), unit), coordinate(1, 2 * std::int64_t(j), unit),
                      coordinate(2, 2 * std::int64_t(k), unit));
    }
    gp_XYZ center(int i, int j, int k, LengthUnit unit = LengthUnit::Millimeter) const
    {
        return gp_XYZ(coordinate(0, 2 * std::int64_t(i) + 1, unit), coordinate(1, 2 * std::int64_t(j) + 1, unit),
                      coordinate(2, 2 * std::int64_t(k) + 1, unit));
    }
};

} // namespace cad::discretization
