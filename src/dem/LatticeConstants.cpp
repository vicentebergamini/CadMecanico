#include "dem/LatticeConstants.h"

#include <cmath>

namespace cad::dem {

LatticeConstants computeLattice(const DemProperties& p, double lco)
{
    const DerivedParameters d = derive(p);
    const double nu = p.material.poissonRatio;
    const double e = p.material.youngModulus;

    LatticeConstants c;
    c.lco = lco;
    c.caf = kCafBase * ModelParameters::corrcaf();                     // l. 123
    c.rfc = d.rfc;
    c.poa = d.poa;                                                     // l. 124
    c.delta = 9.0 * nu / (4.0 - 8.0 * nu);                             // l. 125
    c.alpha = (9.0 + 8.0 * c.delta) / (18.0 + 24.0 * c.delta);         // l. 126
    c.lcr = 2.0 * c.caf * c.poa / (c.alpha * c.rfc * c.rfc);           // l. 127
    c.epr = d.epr;                                                     // l. 128 (E5.7.4a.1)
    c.enr = e * lco * lco * c.alpha;                                   // l. 129
    const double half = lco * std::sqrt(3.0) / 2.0;
    c.enrd = e * half * half * c.alpha;                                // l. 130
    c.edg = 2.0 * c.delta * (4.0 / 3.0) * (c.enrd / std::sqrt(3.0));   // l. 131
    return c;
}

PropertyValidation validateLattice(const DemProperties& p, double lco)
{
    if (const PropertyValidation r = validate(p); !r.ok)
        return r;
    if (!(std::isfinite(lco) && lco > 0.0))
        return {false, "lco", "A aresta do módulo LCO deve ser finita e positiva."};
    const LatticeConstants c = computeLattice(p, lco);
    const double values[] = {c.caf, c.rfc, c.poa, c.alpha, c.lcr, c.epr, c.enr, c.enrd};
    for (double v : values)
        if (!(std::isfinite(v) && v > 0.0))
            return {false, "lco", "As constantes da rede não são representáveis numericamente (valores extremos)."};
    // ν = 0 anula delta e, portanto, a rigidez das diagonais (EDG = 0): o MATLAB aceita esse caso.
    // ν < 0 torna delta e EDG negativos; o MATLAB também aceita (−1 < ν < 0,5), mas a diagonal deixa
    // de ter rigidez positiva e a lei bilinear dela não existe (makeBilinearLaw recusa).
    if (!std::isfinite(c.delta) || !std::isfinite(c.edg))
        return {false, "lco", "As constantes da rede não são representáveis numericamente (valores extremos)."};
    return {};
}

} // namespace cad::dem
