// Valores de referência da peça de teste, conferidos na validação da primeira execução
// (relatorio_primeira_execucao.txt, seção 6).
#pragma once

#include "TestCheck.h"

#include "modeling/ShapeInspector.h"

#include <cstdio>

namespace cad::test {

constexpr double kPi = 3.14159265358979323846;
constexpr double kExpectedVolume = 100.0 * 60.0 * 20.0 - kPi * 10.0 * 10.0 * 20.0; // 113716.8147...

inline void printSummary(const cad::modeling::ShapeSummary& s)
{
    double xmin = 0, ymin = 0, zmin = 0, xmax = 0, ymax = 0, zmax = 0;
    if (!s.boundingBox.IsVoid())
        s.boundingBox.Get(xmin, ymin, zmin, xmax, ymax, zmax);

    std::printf("  Valido (BRepCheck): %s\n", s.isValid ? "sim" : "nao");
    std::printf("  Solidos: %d | Cascas: %d | Faces: %d\n", s.solids, s.shells, s.faces);
    std::printf("  Arestas distintas: %d | Ocorrencias de arestas: %d | Vertices: %d\n",
                s.edges, s.edgeOccurrences, s.vertices);
    std::printf("  Volume: %.4f (esperado %.4f; desvio relativo %.2e)\n",
                s.volume, kExpectedVolume, std::fabs(s.volume - kExpectedVolume) / kExpectedVolume);
    std::printf("  Area: %.4f\n", s.area);
    std::printf("  Caixa: [%.6f, %.6f, %.6f] - [%.6f, %.6f, %.6f]\n", xmin, ymin, zmin, xmax, ymax, zmax);
}

// Critérios do verificador STEP original: 1 sólido, 7 faces, 30 ocorrências de arestas,
// shape válido, volume analítico e caixa [0,0,0]-[100,60,20].
inline void checkTestPart(const cad::modeling::ShapeSummary& s)
{
    CAD_CHECK(!s.isNull);
    CAD_CHECK(s.isValid);
    CAD_CHECK(s.solids == 1);
    CAD_CHECK(s.faces == 7);
    CAD_CHECK(s.edgeOccurrences == 30);
    CAD_CHECK_NEAR(s.volume, kExpectedVolume, kExpectedVolume * 1e-9);

    CAD_CHECK(!s.boundingBox.IsVoid());
    if (s.boundingBox.IsVoid())
        return;
    double xmin, ymin, zmin, xmax, ymax, zmax;
    s.boundingBox.Get(xmin, ymin, zmin, xmax, ymax, zmax);
    const double tol = 1e-6;
    CAD_CHECK_NEAR(xmin, 0.0, tol);
    CAD_CHECK_NEAR(ymin, 0.0, tol);
    CAD_CHECK_NEAR(zmin, 0.0, tol);
    CAD_CHECK_NEAR(xmax, 100.0, tol);
    CAD_CHECK_NEAR(ymax, 60.0, tol);
    CAD_CHECK_NEAR(zmax, 20.0, tol);
}

} // namespace cad::test
