#include "dem/BilinearLaw.h"

#include <cmath>
#include <cstddef>
#include <limits>

namespace cad::dem {

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

bool finitePositive(double v)
{
    return std::isfinite(v) && v > 0.0;
}

} // namespace

int matlabTp(BarType type)
{
    switch (type)
    {
    case BarType::EdgeOneCell: return 1;
    case BarType::EdgeTwoCells: return 2;
    case BarType::EdgeThreeCells: return 3;
    case BarType::EdgeFourCells: return 4;
    case BarType::CenterCenter: return 4;
    case BarType::Diagonal: return 0;
    }
    return -1;
}

bool isMatlabReference(BarType type)
{
    return type != BarType::EdgeThreeCells;
}

const char* barTypeLabel(BarType type)
{
    switch (type)
    {
    case BarType::EdgeOneCell: return "aresta com 1 cubo (tp = 1)";
    case BarType::EdgeTwoCells: return "aresta com 2 cubos (tp = 2)";
    case BarType::EdgeThreeCells: return "aresta com 3 cubos (tp = 3, extensão da referência)";
    case BarType::EdgeFourCells: return "aresta com 4 cubos (tp = 4)";
    case BarType::CenterCenter: return "centro a centro (tp = 4)";
    case BarType::Diagonal: return "diagonal centro-vértice (tp = 0)";
    }
    return "?";
}

double axialRigidity(const LatticeConstants& c, BarType type)
{
    // Mesmas expressões de lawnumbers.m (FP = (ENR/4)·EEP, (ENR/2)·EEP, ENR·EEP, EDG·EEP).
    switch (type)
    {
    case BarType::EdgeOneCell: return c.enr / 4.0;
    case BarType::EdgeTwoCells: return c.enr / 2.0;
    case BarType::EdgeThreeCells: return 3.0 * c.enr / 4.0; // extensão (tp = 3)
    case BarType::EdgeFourCells: return c.enr;
    case BarType::CenterCenter: return c.enr;
    case BarType::Diagonal: return c.edg;
    }
    return kNaN;
}

BilinearLawResult makeBilinearLaw(const LatticeConstants& c, BarType type, double li, double g)
{
    BilinearLawResult r;
    if (!finitePositive(li))
    {
        r.message = "O comprimento da barra Li deve ser finito e positivo.";
        return r;
    }
    if (!finitePositive(g))
    {
        r.message = "O fator de energia de fratura g (Gf_bond) deve ser finito e positivo.";
        return r;
    }
    const double ea = axialRigidity(c, type);
    if (!finitePositive(ea))
    {
        r.message = std::string("A rigidez axial E·A da barra (") + barTypeLabel(type)
                    + ") não é positiva; para as diagonais isso ocorre com coeficiente de Poisson <= 0.";
        return r;
    }

    BilinearLaw& w = r.law;
    w.type = type;
    w.length = li;
    w.gfFactor = g;
    w.axialRigidity = ea;
    w.kr = c.lcr / li;                         // lawnumbers.m l. 19
    w.strainPeak = c.epr * g;                  // EEP = EPR·Gfb
    w.forcePeak = ea * w.strainPeak;           // FP = (E·A)·EEP
    w.strainRupture = w.strainPeak * w.kr;     // EEU = EEP·kr
    w.stiffness = ea / li;                     // k0
    w.dispPeak = w.strainPeak * li;            // EEP·LI
    w.dispRupture = w.strainPeak * w.kr * li;  // EEP·kr·LI

    if (!(w.kr > 1.0))
    {
        r.message = "O comprimento da barra Li deve ser menor que o comprimento crítico LCR (kr = LCR/Li > 1); "
                    "com kr <= 1 o ramo descendente não existe (snap-back).";
        return r;
    }
    const double values[] = {w.kr, w.strainPeak, w.forcePeak, w.strainRupture, w.stiffness, w.dispPeak, w.dispRupture};
    for (double v : values)
        if (!finitePositive(v))
        {
            r.message = "As grandezas da lei bilinear não são representáveis numericamente (valores extremos).";
            return r;
        }
    if (!(w.dispRupture > w.dispPeak))
    {
        r.message = "O deslocamento de ruptura deve ser maior que o deslocamento crítico.";
        return r;
    }
    r.ok = true;
    return r;
}

// ---------------------------------------------------------------- lei ideal

double idealForce(const BilinearLaw& w, double d)
{
    if (!(std::isfinite(d) && d >= 0.0))
        return kNaN;
    if (d <= w.dispPeak)
        return w.forcePeak * (d / w.dispPeak);
    if (d < w.dispRupture)
        return w.forcePeak * (w.dispRupture - d) / (w.dispRupture - w.dispPeak);
    return 0.0;
}

double idealWork(const BilinearLaw& w, double d)
{
    if (!(std::isfinite(d) && d >= 0.0))
        return kNaN;
    if (d <= w.dispPeak)
        return 0.5 * idealForce(w, d) * d;
    const double elasticPart = 0.5 * w.forcePeak * w.dispPeak;
    const double end = d < w.dispRupture ? d : w.dispRupture;
    return elasticPart + 0.5 * (w.forcePeak + idealForce(w, end)) * (end - w.dispPeak);
}

double idealDamage(const BilinearLaw& w, double dmax)
{
    if (!(std::isfinite(dmax) && dmax >= 0.0))
        return kNaN;
    if (dmax <= w.dispPeak)
        return 0.0;
    if (dmax >= w.dispRupture)
        return 1.0;
    // Rigidez secante F/δmax relativa à inicial FP/δp.
    return 1.0 - (idealForce(w, dmax) * w.dispPeak) / (w.forcePeak * dmax);
}

double idealElasticEnergy(const BilinearLaw& w, double dmax)
{
    if (!(std::isfinite(dmax) && dmax >= 0.0))
        return kNaN;
    return 0.5 * idealForce(w, dmax) * dmax;
}

double idealDissipatedEnergy(const BilinearLaw& w, double dmax)
{
    if (!(std::isfinite(dmax) && dmax >= 0.0))
        return kNaN;
    if (dmax <= w.dispPeak)
        return 0.0;
    return idealWork(w, dmax) - idealElasticEnergy(w, dmax);
}

double idealFractureEnergy(const BilinearLaw& w)
{
    return 0.5 * w.forcePeak * w.dispRupture;
}

// ---------------------------------------------------------------- curva regularizada

RegularizedCurve matlabLoadingCurve(const BilinearLaw& w)
{
    const double eep = w.strainPeak, fp = w.forcePeak, kr = w.kr, li = w.length;
    RegularizedCurve c;
    c.force = {-fp, 0.0, fp, fp / 100.0, fp / 100.0};
    c.displacement = {-eep * li, 0.0, eep * li, eep * kr * li, 2.0 * (eep * kr * li)};
    return c;
}

RegularizedCurve matlabUnloadingCurve(const BilinearLaw& w)
{
    const double eep = w.strainPeak, fp = w.forcePeak, kr = w.kr, li = w.length;
    RegularizedCurve c;
    c.force = {-fp, 0.0, fp, fp / 100.0, fp / 100.0};
    c.displacement = {-li * eep, 0.0, li * eep, li * kr * eep, 100.0 * li * kr * eep};
    return c;
}

double curveForce(const RegularizedCurve& c, double d)
{
    const auto& x = c.displacement;
    const auto& f = c.force;
    if (!(std::isfinite(d) && d >= x.front() && d <= x.back()))
        return kNaN;
    for (std::size_t i = 0; i + 1 < x.size(); ++i)
        if (d <= x[i + 1])
        {
            if (d == x[i + 1])
                return f[i + 1];
            const double t = (d - x[i]) / (x[i + 1] - x[i]);
            return f[i] + t * (f[i + 1] - f[i]);
        }
    return kNaN;
}

double curveWork(const RegularizedCurve& c, double d)
{
    const auto& x = c.displacement;
    const auto& f = c.force;
    if (!(std::isfinite(d) && d >= 0.0 && d <= x.back()) || x[1] != 0.0)
        return kNaN;
    double work = 0.0;
    for (std::size_t i = 1; i + 1 < x.size() && x[i] < d; ++i)
    {
        const double end = d < x[i + 1] ? d : x[i + 1];
        const double fend = end == x[i + 1] ? f[i + 1] : curveForce(c, end);
        work += 0.5 * (f[i] + fend) * (end - x[i]);
    }
    return work;
}

double regularizedExcessToRupture(const BilinearLaw& w)
{
    return 0.5 * (w.forcePeak / 100.0) * (w.dispRupture - w.dispPeak);
}

double regularizedPlateauWork(const BilinearLaw& w)
{
    return (w.forcePeak / 100.0) * w.dispRupture;
}

} // namespace cad::dem
