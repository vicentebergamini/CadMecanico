#include "dem/LawValidation.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <utility>

namespace cad::dem {

namespace {

bool finite(double v)
{
    return std::isfinite(v);
}

// Igualdade exata (bit a bit para valores finitos; NaN igual a NaN).
bool same(double a, double b)
{
    return a == b || (std::isnan(a) && std::isnan(b));
}

// Valor efetivamente escrito no texto APDL: "%g" do fprintf do MATLAB (6 algarismos significativos,
// mesmo formato de discretization::matlabG), relido. Só para valores finitos.
double written(double v)
{
    char buf[64];
    std::snprintf(buf, sizeof buf, "%g", v);
    return std::strtod(buf, nullptr);
}

std::string id(std::int64_t v)
{
    return std::to_string(v);
}

// Uso de cada lei pelas barras (índice = ID da lei − 1).
struct LawUse
{
    std::int64_t counted = 0;     // barras com mat_bonds = lei
    std::int64_t active = 0;      // dessas, não excluídas
    std::int64_t firstSeen = 0;   // primeira barra (ID) com mat_bonds = lei
    std::int64_t firstActive = 0; // primeira barra ativa (ID)
};

const char* curveName(int curve)
{
    return curve == 1 ? "carga" : "descarga";
}

} // namespace

std::string formatIssueValue(double v)
{
    if (std::isnan(v))
        return "NaN";
    if (std::isinf(v))
        return v > 0 ? "Inf" : "-Inf";
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.15g", v);
    if (std::strtod(buf, nullptr) != v)
        std::snprintf(buf, sizeof buf, "%.17g", v);
    return buf;
}

std::string describe(const LawIssue& i)
{
    std::string s;
    switch (i.severity())
    {
    case LawIssueSeverity::Info: s = "INFO"; break;
    case LawIssueSeverity::Warning: s = "AVISO"; break;
    case LawIssueSeverity::Error: s = "ERRO"; break;
    }
    s += i.blocksExport() ? ", bloqueia a exportação" : ", não bloqueia";
    s += " [";
    switch (i.category)
    {
    case LawIssueCategory::Numeric: s += "numérico"; break;
    case LawIssueCategory::Structural: s += "estrutural"; break;
    case LawIssueCategory::Constitutive: s += "constitutivo"; break;
    }
    switch (i.kind)
    {
    case LawIssueKind::ReferenceZero: s += ", ReferenceZero"; break;
    case LawIssueKind::InvalidExported: s += ", InvalidExported"; break;
    case LawIssueKind::InvalidEmitted: s += ", InvalidEmitted"; break;
    case LawIssueKind::Informative: s += ", Informative"; break;
    }
    switch (i.scope)
    {
    case LawIssueScope::Table: s += "; escopo: tabela]"; break;
    case LawIssueScope::Bond: s += "; escopo: barra]"; break;
    case LawIssueScope::Law: s += "; escopo: lei e bloco APDL]"; break;
    }
    const auto status = [](bool deleted) { return deleted ? "excluído" : "ativo"; };
    std::string where;
    const auto add = [&](const std::string& part) { where += (where.empty() ? " " : ", ") + part; };
    if (i.lawId > 0)
        add("lei " + id(i.lawId));
    if (i.tp >= 0)
        add("tp = " + id(i.tp));
    if (i.elementId > 0)
        add("elemento " + id(i.elementId) + " (" + status(i.elementDeleted)
            + (i.originElementId == i.elementId ? ", criou a lei)" : ")"));
    if (i.originElementId > 0 && i.originElementId != i.elementId)
        add(std::string(i.elementId > 0 ? "afetado pela lei criada pelo elemento " : "lei criada pelo elemento ")
            + id(i.originElementId) + " (" + status(i.originDeleted) + ")");
    s += where;
    if (i.lawId > 0)
        s += "; " + id(i.activeElements) + (i.activeElements == 1 ? " barra ativa na lei" : " barras ativas na lei");
    if (!i.parameter.empty())
        s += std::string(": ")
             + (i.scope == LawIssueScope::Law    ? "valor armazenado na lei "
                : i.scope == LawIssueScope::Bond ? "valor da barra "
                                                 : "")
             + i.parameter + " = " + formatIssueValue(i.value);
    s += ". " + i.reason;
    return s;
}

LawValidationReport validateLawTable(const MatlabLawTable& t, const std::vector<bool>& deleted)
{
    LawValidationReport r;
    r.bonds = t.bonds();
    r.laws = t.lawCount();

    const auto global = [&](std::string parameter, double value, std::string reason) {
        LawIssue i;
        i.kind = LawIssueKind::InvalidExported;
        i.category = LawIssueCategory::Structural;
        i.scope = LawIssueScope::Table;
        i.parameter = std::move(parameter);
        i.value = value;
        i.reason = std::move(reason);
        r.issues.push_back(std::move(i));
    };

    // ---------------------------------------------------------------- S1
    if (!t.ok)
    {
        global("", 0.0, "Tabela de leis inválida (" + t.message + "); nenhuma lei pode ser exportada.");
        return r;
    }
    const std::size_t n = t.bondLaw.size();
    if (!deleted.empty() && deleted.size() != n)
    {
        global("deleted", double(deleted.size()),
               "O vetor de barras excluídas tem " + id(std::int64_t(deleted.size())) + " entradas para "
                   + id(std::int64_t(n)) + " barras (mat_bonds); a associação elemento–lei é inconsistente.");
        return r;
    }
    if (t.bondKr.size() != n || t.bondEep.size() != n || t.bondFp.size() != n || t.bondEeu.size() != n)
    {
        global("mat_bonds", double(n),
               "Os vetores por barra (mat_bonds, kr, EEP, FP, EEU) têm tamanhos diferentes; a associação "
               "elemento–lei é inconsistente.");
        return r;
    }

    const auto isDeleted = [&](std::size_t b) { return !deleted.empty() && deleted[b]; };
    const std::size_t k = t.laws.size();
    const std::int32_t jj = t.curveOffset;
    std::vector<LawUse> use(k);
    std::vector<char> bondBlocked(n, 0), lawBlocked(k, 0);

    // ---------------------------------------------------------------- S3, S5 e N1 por barra
    for (std::size_t b = 0; b < n; ++b)
    {
        const std::int64_t element = std::int64_t(b) + 1;
        const bool del = isDeleted(b);
        r.deletedBonds += del ? 1 : 0;
        const std::int32_t m = t.bondLaw[b];
        if (m < 1 || std::size_t(m) > k)
        {
            LawIssue i;
            i.kind = del ? LawIssueKind::InvalidEmitted : LawIssueKind::InvalidExported;
            i.category = LawIssueCategory::Structural;
            i.scope = LawIssueScope::Bond;
            i.elementId = element;
            i.elementDeleted = del;
            i.parameter = "mat_bonds";
            i.value = double(m);
            i.reason = "O elemento referencia a lei " + id(m) + ", inexistente (leis 1.." + id(std::int64_t(k))
                       + "); EMODIF," + id(element) + ",MAT," + id(m) + " seria escrito"
                       + (del ? " mesmo com a barra excluída (EDELE vem depois)." : ".");
            r.issues.push_back(std::move(i));
            bondBlocked[b] = del ? 0 : 1;
            continue;
        }
        LawUse& u = use[std::size_t(m - 1)];
        ++u.counted;
        if (u.firstSeen == 0)
            u.firstSeen = element;
        if (!del)
        {
            ++u.active;
            if (u.firstActive == 0)
                u.firstActive = element;
        }
    }
    for (std::size_t b = 0; b < n; ++b)
    {
        const std::int32_t m = t.bondLaw[b];
        if (m < 1 || std::size_t(m) > k)
            continue;
        const MatlabLaw& w = t.laws[std::size_t(m - 1)];
        const std::int64_t element = std::int64_t(b) + 1;
        const bool del = isDeleted(b);
        const bool originValid = w.firstBond >= 1 && w.firstBond <= std::int64_t(n);
        const auto bond = [&](LawIssueKind kind, LawIssueCategory category, const char* parameter, double value,
                              std::string reason) {
            LawIssue i;
            i.kind = kind;
            i.category = category;
            i.scope = LawIssueScope::Bond;
            i.lawId = m;
            i.tp = w.tp;
            i.elementId = element;
            i.elementDeleted = del;
            if (originValid)
            {
                i.originElementId = w.firstBond;
                i.originDeleted = isDeleted(std::size_t(w.firstBond - 1));
            }
            i.activeElements = use[std::size_t(m - 1)].active;
            i.parameter = parameter;
            i.value = value;
            i.reason = std::move(reason);
            if (i.blocksExport())
                lawBlocked[std::size_t(m - 1)] = 1;
            r.issues.push_back(std::move(i));
        };
        const LawIssueKind bad = del ? LawIssueKind::InvalidEmitted : LawIssueKind::InvalidExported;
        const double bondValues[] = {t.bondEep[b], t.bondFp[b], t.bondEeu[b]};
        const double lawValues[] = {w.eep, w.fp, w.eeu};
        const char* names[] = {"EEP", "FP", "EEU"};
        for (int q = 0; q < 3; ++q)
            if (!same(bondValues[q], lawValues[q]))
                bond(bad, LawIssueCategory::Structural, names[q], bondValues[q],
                     "Valor da barra diferente do da lei (" + formatIssueValue(lawValues[q])
                         + "); lawnumbers.m copia literalmente os valores da lei para cada barra.");
        // kr geométrico da barra. Só o kr da barra que criou a lei é usado (armazenado na lei e
        // verificado no escopo lei); o das demais barras não entra na curva nem no texto APDL.
        // Política conservadora preservada: barra ativa → InvalidExported; excluída → aviso.
        if (!finite(t.bondKr[b]))
        {
            const bool creator = originValid && w.firstBond == element;
            std::string reason = "kr geométrico desta barra (LCR/Li) não finito.";
            if (creator)
                reason += " Esta barra criou a lei " + id(m) + ": o mesmo valor está armazenado na lei; a validade "
                          "da lei compartilhada e do bloco APDL emitido é decidida pelos diagnósticos da lei "
                          + id(m) + " (escopo lei).";
            else
                reason += " Esta barra reutiliza a lei " + id(m)
                          + (originValid ? ", criada pelo elemento " + id(w.firstBond) + " (kr armazenado na lei = "
                                               + formatIssueValue(w.kr) + ")"
                                         : std::string())
                          + "; o kr desta barra não é usado na curva nem escrito no APDL.";
            if (del)
                reason += " Aviso individual da barra excluída: não determina sozinho a validade da lei "
                          "compartilhada nem a do bloco APDL emitido.";
            else if (!creator)
                reason += " Bloqueio conservador da barra ativa (comprimento degenerado), embora a lei "
                          "compartilhada e o bloco APDL emitido não contenham este valor.";
            else
                reason += " Barra ativa: bloqueia.";
            bond(del ? LawIssueKind::Informative : LawIssueKind::InvalidExported, LawIssueCategory::Numeric, "kr",
                 t.bondKr[b], std::move(reason));
        }
    }

    // ---------------------------------------------------------------- por lei
    for (std::size_t j = 0; j < k; ++j)
    {
        const MatlabLaw& w = t.laws[j];
        const LawUse& u = use[j];
        const std::int32_t lawId = std::int32_t(j) + 1;
        const bool active = u.active > 0;
        if (!active)
            ++r.lawsOnlyDeleted;
        const bool firstValid = w.firstBond >= 1 && w.firstBond <= std::int64_t(n);
        const std::int64_t element = active ? u.firstActive : (firstValid ? w.firstBond : u.firstSeen);
        const LawIssueKind bad = active ? LawIssueKind::InvalidExported : LawIssueKind::InvalidEmitted;
        const std::string where = active ? " A lei está atribuída a " + id(u.active) + " barra(s) ativa(s)."
                                         : " A lei está atribuída somente a barras excluídas, mas seus comandos "
                                           "(*DIM, EDCURVE, MAT, TB, TBDAT) continuam no arquivo, como em "
                                           "lawnumbers.m.";
        const auto law = [&](LawIssueKind kind, LawIssueCategory category, std::string parameter, double value,
                             std::string reason) {
            LawIssue i;
            i.kind = kind;
            i.category = category;
            i.scope = LawIssueScope::Law;
            i.lawId = lawId;
            i.tp = w.tp;
            i.elementId = element;
            i.elementDeleted = element > 0 && isDeleted(std::size_t(element - 1));
            if (firstValid)
            {
                i.originElementId = w.firstBond;
                i.originDeleted = isDeleted(std::size_t(w.firstBond - 1));
            }
            i.activeElements = u.active;
            i.parameter = std::move(parameter);
            i.value = value;
            i.reason = std::move(reason);
            if (i.blocksExport())
                lawBlocked[j] = 1;
            r.issues.push_back(std::move(i));
        };
        const auto structural = [&](const char* parameter, double value, const std::string& reason) {
            law(bad, LawIssueCategory::Structural, parameter, value, reason + where);
        };

        // S2: identificadores.
        if (w.id != lawId)
            structural("id", w.id, "Lei na posição " + id(lawId) + " com ID " + id(w.id)
                                       + "; os IDs (MAT, TB,DISCRETE) devem ser 1..k na ordem de criação.");
        if (w.loadingCurveId != lawId)
            structural("curva de carga", w.loadingCurveId,
                       "O ID da curva de carga deve ser o ID da lei (" + id(lawId) + ").");
        if (w.unloadingCurveId != lawId + jj)
            structural("curva de descarga", w.unloadingCurveId,
                       "O ID da curva de descarga deve ser lei + jj = " + id(std::int64_t(lawId) + jj) + ".");
        // S4: primeira barra e associação.
        if (!firstValid)
            structural("lawBond", double(w.firstBond),
                       "Primeira barra da lei fora de 1.." + id(std::int64_t(n)) + ".");
        else if (t.bondLaw[std::size_t(w.firstBond - 1)] != lawId)
            structural("lawBond", double(w.firstBond), "A primeira barra registrada não está atribuída à lei.");
        else if (u.firstSeen != w.firstBond)
            structural("lawBond", double(w.firstBond),
                       "A primeira barra registrada não é a primeira ocorrência da lei (elemento "
                           + id(u.firstSeen) + ").");
        if (u.counted == 0)
            structural("barras", 0.0, "Lei sem nenhuma barra atribuída; lawnumbers.m só cria leis a partir de barras.");
        else if (w.assignedBonds != u.counted)
            structural("barras", double(w.assignedBonds),
                       "Número de barras registrado diferente das atribuições (" + id(u.counted) + ").");
        // S5: tipo e ramo.
        if (w.tp < 0 || w.tp > 4)
            structural("tp", w.tp, "tp fora de 0..4.");
        else if (w.hasMatlabBranch != (w.tp != 3))
            structural("tp", w.tp, "Indicador de ramo de lawnumbers.m incoerente com tp (só tp = 3 não tem ramo).");
        if (firstValid && !same(w.kr, t.bondKr[std::size_t(w.firstBond - 1)]))
            structural("kr", w.kr, "kr da lei diferente do kr da primeira barra ("
                                       + formatIssueValue(t.bondKr[std::size_t(w.firstBond - 1)]) + ").");
        if (w.tp == 3 && !(w.eep == 0.0 && w.fp == 0.0 && w.eeu == 0.0))
            structural("EEP/FP/EEU", w.fp,
                       "tp = 3 sem ramo em lawnumbers.m deveria ter EEP = FP = EEU = 0 (vetores iniciados com zeros).");

        // N1: valores da lei finitos.
        const double values[] = {w.eep, w.fp, w.eeu, w.kr, w.li};
        const char* names[] = {"EEP", "FP", "EEU", "kr", "Li"};
        // Origem: Li e kr armazenados na lei são os da barra que a criou (lawBond); o elemento afetado
        // pode ser outro, que reutiliza a lei sem que seu próprio Li/kr geométrico seja usado.
        std::string origin;
        if (firstValid)
        {
            origin = " Valor armazenado na lei, proveniente do elemento " + id(w.firstBond)
                     + (isDeleted(std::size_t(w.firstBond - 1)) ? " (excluído)" : " (ativo)") + ", que criou a lei";
            if (element > 0 && element != w.firstBond)
                origin += "; o elemento afetado " + id(element) + " reutiliza a lei e seu kr geométrico ("
                          + formatIssueValue(t.bondKr[std::size_t(element - 1)]) + ") não é usado";
            origin += ".";
        }
        for (int q = 0; q < 5; ++q)
            if (!finite(values[q]))
                law(bad, LawIssueCategory::Numeric, names[q], values[q],
                    std::string("Valor não finito; o texto APDL conteria \"") + (std::isnan(values[q]) ? "nan" : "inf")
                        + "\" (MATLAB: \"" + (std::isnan(values[q]) ? "NaN" : "Inf") + "\")." + origin + where);

        // N2, C3 e N3: pontos das curvas. O segundo ponto é o literal "0" escrito por lawnumbers.m.
        for (int c = 1; c <= 2; ++c)
        {
            const RegularizedCurve& curve = c == 1 ? w.loading : w.unloading;
            const std::string d = "d" + id(c), f = "f" + id(c);
            const std::string curveId = id(c == 1 ? std::int64_t(lawId) : std::int64_t(lawId) + jj);
            bool allFinite = true;
            for (int p = 0; p < 5; ++p)
            {
                if (p == 1)
                    continue;
                if (!finite(curve.displacement[std::size_t(p)]))
                {
                    allFinite = false;
                    law(bad, LawIssueCategory::Numeric, d + id(lawId) + "(" + id(p + 1) + ")",
                        curve.displacement[std::size_t(p)],
                        std::string("Abscissa não finita da curva de ") + curveName(c) + " (EDCURVE " + curveId + ")."
                            + where);
                }
                if (!finite(curve.force[std::size_t(p)]))
                    law(bad, LawIssueCategory::Numeric, f + id(lawId) + "(" + id(p + 1) + ")",
                        curve.force[std::size_t(p)],
                        std::string("Força não finita da curva de ") + curveName(c) + " (EDCURVE " + curveId + ")."
                            + where);
            }
            if (!allFinite)
                continue;
            const double x[5] = {curve.displacement[0], 0.0, curve.displacement[2], curve.displacement[3],
                                 curve.displacement[4]};
            int p = 0;
            while (p < 4 && x[p] < x[p + 1])
                ++p;
            if (p < 4)
            {
                law(bad, LawIssueCategory::Constitutive, d + id(lawId) + "(" + id(p + 2) + ")", x[p + 1],
                    std::string("Abscissas da curva de ") + curveName(c) + " (EDCURVE " + curveId
                        + ") não estritamente crescentes: " + d + id(lawId) + "(" + id(p + 1) + ") = "
                        + formatIssueValue(x[p]) + " ≥ " + d + id(lawId) + "(" + id(p + 2) + ") = "
                        + formatIssueValue(x[p + 1]) + " (kr = " + formatIssueValue(w.kr)
                        + "; a lei exige EEP > 0 e kr > 1, isto é, Li < LCR)." + where);
                continue;
            }
            double y[5];
            for (int q = 0; q < 5; ++q)
                y[q] = q == 1 ? 0.0 : written(x[q]);
            p = 0;
            while (p < 4 && y[p] < y[p + 1])
                ++p;
            if (p < 4)
                law(bad, LawIssueCategory::Numeric, d + id(lawId) + "(" + id(p + 2) + ")", x[p + 1],
                    std::string("Abscissas da curva de ") + curveName(c) + " (EDCURVE " + curveId
                        + ") crescentes em precisão dupla, mas não no texto escrito com %g (6 algarismos): "
                        + formatIssueValue(y[p]) + " ≥ " + formatIssueValue(y[p + 1]) + " (kr = "
                        + formatIssueValue(w.kr) + ")." + where);
        }

        // C1 e C2: valores constitutivos.
        if (w.tp == 3)
        {
            if (active)
                law(LawIssueKind::InvalidExported, LawIssueCategory::Constitutive, "FP", w.fp,
                    "tp = 3 (aresta com 3 cubos ativos): lawnumbers.m não tem ramo e deixa EEP = FP = EEU = 0, mola "
                    "sem resistência; a extensão 3·ENR/4 não é aplicada e nenhum valor é corrigido." + where);
            else
                law(LawIssueKind::ReferenceZero, LawIssueCategory::Constitutive, "FP", w.fp,
                    "tp = 3: EEP = FP = EEU = 0 da referência MATLAB, atribuída somente a barras excluídas; "
                    "informativo (a validade dos comandos escritos é verificada separadamente).");
        }
        else
        {
            for (int q = 0; q < 3; ++q)
                if (finite(values[q]) && !(values[q] > 0.0))
                    law(bad, LawIssueCategory::Constitutive, names[q], values[q],
                        std::string(names[q]) + " deve ser estritamente positivo (Gf = " + formatIssueValue(w.gf)
                            + "); nenhum valor é corrigido nem substituído." + where);
        }

        // I1: comprimentos diferentes na mesma lei.
        if (w.bondsWithOtherLength > 0)
        {
            std::int64_t other = 0;
            for (std::size_t b = 0; b < n && other == 0; ++b)
                if (t.bondLaw[b] == lawId && !same(t.bondKr[b], w.kr))
                    other = std::int64_t(b) + 1;
            LawIssue i;
            i.kind = LawIssueKind::Informative;
            i.category = LawIssueCategory::Constitutive;
            i.scope = LawIssueScope::Law;
            i.lawId = lawId;
            i.tp = w.tp;
            i.elementId = other;
            i.elementDeleted = other > 0 && isDeleted(std::size_t(other - 1));
            if (firstValid)
            {
                i.originElementId = w.firstBond;
                i.originDeleted = isDeleted(std::size_t(w.firstBond - 1));
            }
            i.activeElements = u.active;
            i.parameter = "max |Li − li|/li";
            i.value = w.maxRelativeLengthDeviation();
            i.reason = id(w.bondsWithOtherLength) + " de " + id(w.assignedBonds)
                       + " barra(s) da lei têm comprimento diferente do da primeira barra (elemento "
                       + id(w.firstBond) + ", Li = " + formatIssueValue(w.li) + " m; faixa "
                       + formatIssueValue(w.minLength) + " a " + formatIssueValue(w.maxLength)
                       + " m). A curva usa Li e kr da primeira barra, como lawnumbers.m; associação preservada.";
            r.issues.push_back(std::move(i));
        }
    }

    // S2: IDs de curva positivos e distintos (carga 1..k, descarga 1 + jj..k + jj).
    if (k > 0 && (jj != matlabCurveOffset(std::int64_t(k)) || jj <= 0))
        global("jj", jj, "Deslocamento das curvas de descarga diferente de round(k, −1) + 10 = "
                             + id(matlabCurveOffset(std::int64_t(k))) + "; IDs de curva podem colidir.");
    else
    {
        std::vector<char> seen(2 * k + std::size_t(jj > 0 ? jj : 0) + 1, 0);
        for (const MatlabLaw& w : t.laws)
            for (std::int32_t cid : {w.loadingCurveId, w.unloadingCurveId})
            {
                if (cid < 1 || std::size_t(cid) >= seen.size() || seen[std::size_t(cid)])
                {
                    global("curva", cid, "ID de curva repetido ou fora da faixa (lei " + id(w.id) + ").");
                    continue;
                }
                seen[std::size_t(cid)] = 1;
            }
    }

    for (std::size_t j = 0; j < k; ++j)
        if (lawBlocked[j])
            r.invalidLaws.push_back(std::int32_t(j) + 1);
    for (std::size_t b = 0; b < n; ++b)
    {
        const std::int32_t m = t.bondLaw[b];
        const bool lawBad = m >= 1 && std::size_t(m) <= k && lawBlocked[std::size_t(m - 1)];
        if (!isDeleted(b) && (bondBlocked[b] || lawBad))
            r.blockedElements.push_back(std::int64_t(b) + 1);
    }
    return r;
}

} // namespace cad::dem
