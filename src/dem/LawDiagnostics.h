// Estruturas de diagnóstico das leis constitutivas antes da exportação APDL/DYNA (preparadas na
// E5.7.4b.3; preenchidas por validateLawTable, dem/LawValidation.h, E5.7.4b.7). Sem Qt.
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
//   InvalidExported  lei nula, inválida ou não finita atribuída a um elemento que será exportado
//                    (barra ativa): a exportação deve ser bloqueada e o diagnóstico apresentado.
// Acrescentados na E5.7.4b.7:
//   InvalidEmitted   lei inválida atribuída SOMENTE a barras excluídas, mas cujos comandos
//                    (*DIM, EDCURVE, MAT, TB, TBDAT, EMODIF) continuam no arquivo, como em
//                    lawnumbers.m (EDELE vem depois). O comando escrito é inválido: bloqueia.
//   Informative      aviso sem efeito na validade do arquivo (ex.: barras de comprimentos
//                    diferentes compartilhando a lei, preservado como no MATLAB).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace cad::dem {

enum class LawIssueKind
{
    ReferenceZero,
    InvalidExported,
    InvalidEmitted,
    Informative
};

// Natureza do problema.
enum class LawIssueCategory
{
    Numeric,      // valor não finito (Inf/NaN) ou não representável no texto (%g, 6 algarismos)
    Structural,   // IDs, numeração, associações, tamanhos dos vetores
    Constitutive  // lei fisicamente inválida (EEP/FP/EEU ≤ 0, tp = 3 nulo, abscissas não crescentes)
};

enum class LawIssueSeverity
{
    Info,
    Warning,
    Error
};

// O que o diagnóstico avalia. A validade de uma barra individual não decide sozinha a validade da
// lei compartilhada; a lei e o bloco APDL que a escreve são avaliados no escopo Law.
enum class LawIssueScope
{
    Table, // tabela e numeração global (tamanhos dos vetores, jj, IDs de curva)
    Bond,  // barra individual (associação mat_bonds, valores copiados, kr geométrico da barra)
    Law    // lei compartilhada: valores armazenados na lei e o bloco APDL dela (*DIM, EDCURVE, MAT, TB, TBDAT)
};

struct LawIssue
{
    LawIssueKind kind = LawIssueKind::InvalidExported;
    std::int64_t elementId = 0; // ID do elemento (EN) afetado; 0 = não se aplica
    int tp = -1;                // tipo da barra no MATLAB (0..4); −1 = não se aplica
    std::string parameter;      // ex.: "EEP", "FP", "EEU", "kr", "d1(4)", "mat_bonds"
    double value = 0.0;         // valor encontrado (pode ser Inf/NaN)
    std::string reason;         // motivo da rejeição ou do aviso
    // E5.7.4b.7 (membros com valor padrão: a inicialização agregada da E5.7.4b.3 continua válida).
    std::int32_t lawId = 0;     // ID da lei (MAT); 0 = não se aplica
    LawIssueCategory category = LawIssueCategory::Constitutive;
    std::int64_t activeElements = 0; // barras ativas (não excluídas) atribuídas à lei
    LawIssueScope scope = LawIssueScope::Law;
    bool elementDeleted = false;      // o elemento afetado (elementId) é uma barra excluída
    // Elemento que criou a lei (lawBond, primeira barra; lawnumbers.m): Li, kr, EEP, FP, EEU e as
    // curvas armazenados na lei vêm dele. 0 = não se aplica. Pode diferir de elementId (elemento afetado).
    std::int64_t originElementId = 0;
    bool originDeleted = false;       // o elemento de origem é uma barra excluída

    LawIssueSeverity severity() const
    {
        switch (kind)
        {
        case LawIssueKind::ReferenceZero: return LawIssueSeverity::Info;
        case LawIssueKind::Informative: return LawIssueSeverity::Warning;
        default: return LawIssueSeverity::Error;
        }
    }
    // Bloqueiam a exportação todas as leis e comandos inválidos que chegam ao arquivo.
    bool blocksExport() const
    {
        return kind == LawIssueKind::InvalidExported || kind == LawIssueKind::InvalidEmitted;
    }
};

struct LawValidationReport
{
    std::vector<LawIssue> issues;
    // Preenchidos por validateLawTable (E5.7.4b.7).
    std::int64_t bonds = 0;           // barras (elementos EN) da tabela
    std::int64_t deletedBonds = 0;    // barras excluídas
    std::int32_t laws = 0;            // leis da tabela
    std::int32_t lawsOnlyDeleted = 0; // leis atribuídas somente a barras excluídas
    std::vector<std::int32_t> invalidLaws;     // leis com ao menos um problema bloqueante (IDs crescentes)
    std::vector<std::int64_t> blockedElements; // barras ativas atribuídas a leis inválidas (IDs crescentes)

    std::int64_t count(LawIssueKind kind) const
    {
        std::int64_t n = 0;
        for (const LawIssue& i : issues)
            n += i.kind == kind ? 1 : 0;
        return n;
    }
    std::int64_t count(LawIssueCategory category) const
    {
        std::int64_t n = 0;
        for (const LawIssue& i : issues)
            n += i.category == category ? 1 : 0;
        return n;
    }
    // A exportação é bloqueada quando há ao menos um diagnóstico bloqueante.
    bool blocksExport() const
    {
        for (const LawIssue& i : issues)
            if (i.blocksExport())
                return true;
        return false;
    }
};

} // namespace cad::dem
