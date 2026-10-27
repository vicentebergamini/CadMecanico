#include "dem/DemProperties.h"

#include <cmath>
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
    p.model.failureFactor = 10.0;
    return p;
}

PropertyValidation validate(const DemProperties& p)
{
    struct Field
    {
        const char* key;
        const char* label;
        double value;
    };
    const Field fields[] = {
        {kFieldDensity, "Densidade ρ", p.material.density},
        {kFieldPoissonRatio, "Coeficiente de Poisson ν", p.material.poissonRatio},
        {kFieldYoungModulus, "Módulo de elasticidade E", p.material.youngModulus},
        {kFieldFractureEnergy, "Energia específica de fratura GRF", p.material.fractureEnergy},
        {kFieldFailureFactor, "Fator de falha RFC", p.model.failureFactor},
    };
    for (const Field& f : fields)
        if (!std::isfinite(f.value))
            return {false, f.key, std::string(f.label) + ": valor inválido (NaN ou infinito)."};

    if (!(p.material.density > 0.0))
        return {false, kFieldDensity, "A densidade ρ deve ser positiva."};
    if (!(p.material.poissonRatio > -1.0 && p.material.poissonRatio < 0.5))
        return {false, kFieldPoissonRatio,
                "O coeficiente de Poisson ν deve estar no intervalo aberto (−1; 0,5), "
                "como em DEM_HIAGO_PRINCIPAL.m."};
    if (!(p.material.youngModulus > 0.0))
        return {false, kFieldYoungModulus, "O módulo de elasticidade E deve ser positivo."};
    if (!(p.material.fractureEnergy > 0.0))
        return {false, kFieldFractureEnergy,
                "A energia específica de fratura GRF deve ser positiva: em EPR = RFC·√(GRF/(E·(1−ν²))) "
                "um valor negativo não tem raiz real e o valor zero anula a deformação de pico."};
    if (!(p.model.failureFactor > 0.0))
        return {false, kFieldFailureFactor,
                "O fator de falha RFC deve ser positivo: LCR = 2·CAF·(1−ν²)/(α·RFC²) não existe com "
                "RFC = 0 e RFC negativo torna negativa a deformação de pico EPR."};
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
