// Discretização volumétrica de um sólido em cubos elementares de aresta lco (E5.7.2).
//
// Etapas (separadas): grade (VoxelGrid, a partir da caixa envolvente exata) -> classificação
// geométrica dos cubos (CubeClassifier, hierárquica, ordem determinística) -> armazenamento dos
// cubos aceitos, deduplicação dos nós, ordenação Z → Y → X e IDs (Discretization::build).
//
// A forma recebida não é modificada. Recomenda-se passar uma CÓPIA (BRepBuilderAPI_Copy) quando o
// cálculo roda fora da thread da interface: a cópia não compartilha a triangulação de exibição.
//
// Tamanho: a grade é estimada antes de qualquer alocação (aritmética de 64 bits); grades acima de
// kMaxCells células candidatas são recusadas com mensagem.
#pragma once

#include "discretization/Discretization.h"

#include <TopoDS_Shape.hxx>

#include <functional>
#include <memory>
#include <string>

namespace cad::discretization {

struct GridEstimate
{
    bool ok = false;
    std::string message;
    VoxelGrid grid;
    gp_XYZ boxMin, boxMax; // caixa envolvente exata do sólido (mm)
    std::int64_t cells = 0;
    std::int64_t latticePoints = 0;
    std::size_t estimatedBytes = 0; // pico aproximado de memória da discretização
    int solids = 0;
};

struct DiscretizeResult
{
    bool ok = false;
    bool cancelled = false;
    std::string message;
    std::shared_ptr<Discretization> data;
};

// Progresso de 0 a 1; retornar false cancela o cálculo (chamado na thread do cálculo).
using ProgressCallback = std::function<bool(double fraction)>;

class VoxelDiscretizer
{
public:
    static constexpr std::int64_t kMaxCells = 20'000'000;

    static GridEstimate estimate(const TopoDS_Shape& shape, double lco);
    static DiscretizeResult run(const TopoDS_Shape& shape, const Parameters& parameters,
                                const ProgressCallback& progress = {});
};

} // namespace cad::discretization
