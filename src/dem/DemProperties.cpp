#include "dem/DemProperties.h"

#include <cmath>
#include <cstddef>
#include <locale>
#include <sstream>

namespace cad::dem {

DemProperties matlabDefaults()
{
    DemProperties p;
    p.material.density = 1240.0;
    p.material.poissonRatio = 0.36;
    p.material.youngModulus = 3.5e9;
    p.material.fractureEnergy = 2400.0;
    p.model.equivalentDefect = 0.01; // RFC = 1/√0.01 = 10
    return p;
}

DerivedParameters derive(const DemProperties& p)
{
    DerivedParameters d;
    d.rfc = 1.0 / std::sqrt(p.model.equivalentDefect);
    d.poa = 1.0 - p.material.poissonRatio * p.material.poissonRatio;
    d.epr = std::sqrt(p.material.fractureEnergy / (p.model.equivalentDefect * p.material.youngModulus * d.poa));
    return d;
}

namespace {

struct Field
{
    const char* key;
    const char* label;
    double value;
};

// Primeiro campo não finito (NaN ou infinito), ou nenhum.
template <std::size_t N>
PropertyValidation checkFinite(const Field (&fields)[N])
{
    for (const Field& f : fields)
        if (!std::isfinite(f.value))
            return {false, f.key, std::string(f.label) + ": valor inválido (NaN ou infinito)."};
    return {};
}

} // namespace

PropertyValidation validateMaterial(const MaterialProperties& m)
{
    const Field fields[] = {
        {kFieldDensity, "Densidade ρ", m.density},
        {kFieldPoissonRatio, "Coeficiente de Poisson ν", m.poissonRatio},
        {kFieldYoungModulus, "Módulo de elasticidade E", m.youngModulus},
        {kFieldFractureEnergy, "Energia específica de fratura GRF", m.fractureEnergy},
    };
    if (const PropertyValidation r = checkFinite(fields); !r.ok)
        return r;

    if (!(m.density > 0.0))
        return {false, kFieldDensity, "A densidade ρ deve ser positiva."};
    if (!(m.poissonRatio > -1.0 && m.poissonRatio < 0.5))
        return {false, kFieldPoissonRatio,
                "O coeficiente de Poisson ν deve estar no intervalo aberto (−1; 0,5), "
                "como em DEM_HIAGO_PRINCIPAL.m."};
    if (!(m.youngModulus > 0.0))
        return {false, kFieldYoungModulus, "O módulo de elasticidade E deve ser positivo."};
    if (!(m.fractureEnergy > 0.0))
        return {false, kFieldFractureEnergy,
                "A energia específica de fratura GRF deve ser positiva: em EPR = √(GRF/(d_eq·E·(1−ν²))) "
                "um valor negativo não tem raiz real e o valor zero anula a deformação de pico."};
    return {};
}

PropertyValidation validate(const DemProperties& p)
{
    // Todos os campos finitos primeiro (mesma ordem de mensagens de antes), depois as regras do
    // material e por fim as de d_eq.
    const Field fields[] = {
        {kFieldDensity, "Densidade ρ", p.material.density},
        {kFieldPoissonRatio, "Coeficiente de Poisson ν", p.material.poissonRatio},
        {kFieldYoungModulus, "Módulo de elasticidade E", p.material.youngModulus},
        {kFieldFractureEnergy, "Energia específica de fratura GRF", p.material.fractureEnergy},
        {kFieldEquivalentDefect, "Defeito intrínseco equivalente d_eq", p.model.equivalentDefect},
    };
    if (const PropertyValidation r = checkFinite(fields); !r.ok)
        return r;
    if (const PropertyValidation r = validateMaterial(p.material); !r.ok)
        return r;
    if (!(p.model.equivalentDefect > 0.0))
        return {false, kFieldEquivalentDefect,
                "O defeito intrínseco equivalente d_eq deve ser positivo: RFC = 1/√d_eq e "
                "EPR = √(GRF/(d_eq·E·(1−ν²))) não existem com d_eq ≤ 0."};
    // POA = 1 − ν² > 0 já está garantido pelo intervalo de ν; resta o estouro (ou a anulação) numérico
    // em valores extremos.
    const DerivedParameters d = derive(p);
    if (!(std::isfinite(d.rfc) && std::isfinite(d.epr) && d.epr > 0.0))
        return {false, kFieldEquivalentDefect,
                "Com este defeito intrínseco equivalente d_eq, RFC = 1/√d_eq ou EPR = √(GRF/(d_eq·E·(1−ν²))) "
                "não é representável numericamente (valores extremos)."};
    return {};
}

bool parseNumber(const std::string& text, double& value)
{
    std::string s = text;
    const auto first = s.find_first_not_of(" \t");
    if (first == std::string::npos)
        return false;
    s = s.substr(first, s.find_last_not_of(" \t") - first + 1);
    // Um único separador decimal: ponto ou vírgula (nunca os dois, que indicariam milhar).
    const bool hasComma = s.find(',') != std::string::npos;
    const bool hasPoint = s.find('.') != std::string::npos;
    if (hasComma && hasPoint)
        return false;
    if (hasComma)
    {
        if (s.find(',') != s.rfind(','))
            return false;
        s[s.find(',')] = '.';
    }
    // Somente dígitos, sinal, ponto e expoente: recusa "nan", "inf", hexadecimal etc.
    if (s.find_first_not_of("0123456789+-.eE") != std::string::npos)
        return false;
    std::istringstream in(s);
    in.imbue(std::locale::classic());
    double v = 0.0;
    in >> v;
    if (in.fail() || !in.eof() || !std::isfinite(v))
        return false;
    value = v;
    return true;
}

} // namespace cad::dem
