// Estruturas de diagnóstico das leis constitutivas antes da exportação APDL/DYNA (preparadas na
// E5.7.4b.3; a validação que as preenche será implementada junto com a tabela de leis). Sem Qt.
//
// Decisão técnica (E5.7.4b.3): as leis são calculadas exatamente como em lawnumbers.m, inclusive
// os valores nulos. Em particular, barras tp = 3 (arestas com 3 cubos ativos) não têm ramo em
// lawnumbers.m: EEP, FP e EEU permanecem 0 e a lei escrita teria curvas com abscissas nulas. Nenhum
// parâmetro é alterado para "corrigir" uma lei; a extensão E·A = 3·ENR/4 (dem/BilinearLaw.h) NÃO é
// usada na geração das leis APDL/DYNA.
//
// Distinção obrigatória:
//   ReferenceZero    valor intermediário nulo previsto pela referência MATLAB, sem efeito no
//                    arquivo exportado (informativo);
//   InvalidExported  lei nula, inválida ou não finita atribuída a um elemento que será exportado:
//                    a exportação deve ser bloqueada e o diagnóstico apresentado.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace cad::dem {

enum class LawIssueKind
{
    ReferenceZero,
    InvalidExported
};

struct LawIssue
{
    LawIssueKind kind = LawIssueKind::InvalidExported;
    std::int64_t elementId = 0; // ID do elemento (EN) afetado
    int tp = -1;                // tipo da barra no MATLAB (0..4)
    std::string parameter;      // ex.: "EEP", "FP", "EEU", "kr"
    double value = 0.0;         // valor encontrado
    std::string reason;         // motivo da rejeição
};

struct LawValidationReport
{
    std::vector<LawIssue> issues;

    std::int64_t count(LawIssueKind kind) const
    {
        std::int64_t n = 0;
        for (const LawIssue& i : issues)
            n += i.kind == kind ? 1 : 0;
        return n;
    }
    // A exportação é bloqueada quando há ao menos uma lei inválida em elemento exportável.
    bool blocksExport() const { return count(LawIssueKind::InvalidExported) > 0; }
};

} // namespace cad::dem
