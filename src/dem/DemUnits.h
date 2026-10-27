// Unidades do módulo MED (E5.7.4a). A geometria CAD continua em milímetros (documento, esboços,
// discretização e exportações existentes); o modelo físico do Método dos Elementos Discretos usa o
// Sistema Internacional:
//   comprimento m, massa kg, tempo s, força N, densidade kg/m³, módulo de elasticidade Pa (N/m²),
//   energia J (N·m) e energia específica de fratura por área N/m (J/m²).
// A passagem mm → m acontece somente por estas funções, sobre cópias dos valores: as coordenadas
// CAD originais nunca são alteradas. As divisões por potências de 10 (em vez de multiplicar por
// 1e-3, 1e-6, 1e-9, que não são exatos em ponto flutuante) dão o resultado corretamente arredondado.
// Sem dependência de Qt.
#pragma once

namespace cad::dem::units {

inline constexpr const char* kCadLengthUnit = "mm"; // geometria CAD
inline constexpr const char* kDemLengthUnit = "m";  // modelo físico MED
inline constexpr const char* kUnitSystem = "SI";

inline constexpr double kMillimetersPerMeter = 1000.0;

// Comprimento.
constexpr double mmToM(double mm) { return mm / kMillimetersPerMeter; }
constexpr double mToMm(double m) { return m * kMillimetersPerMeter; }
// Área.
constexpr double mm2ToM2(double mm2) { return mm2 / (kMillimetersPerMeter * kMillimetersPerMeter); }
constexpr double m2ToMm2(double m2) { return m2 * (kMillimetersPerMeter * kMillimetersPerMeter); }
// Volume.
constexpr double mm3ToM3(double mm3) { return mm3 / (kMillimetersPerMeter * kMillimetersPerMeter * kMillimetersPerMeter); }
constexpr double m3ToMm3(double m3) { return m3 * (kMillimetersPerMeter * kMillimetersPerMeter * kMillimetersPerMeter); }

// Ponto do modelo físico (m), convertido a partir de coordenadas CAD (mm).
struct PointM
{
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
};

constexpr PointM pointMmToM(double xMm, double yMm, double zMm)
{
    return PointM{mmToM(xMm), mmToM(yMm), mmToM(zMm)};
}

} // namespace cad::dem::units
