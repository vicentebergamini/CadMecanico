// E5.7.4b.2 sem interface — núcleo constitutivo DEM-DYNA:
//  - constantes da rede (DEM_HIAGO_PRINCIPAL.m, l. 123-131) com os valores padrão, comparadas com as
//    formas fechadas exatas (delta = 81/28, alfa = 25/68 para ν = 0,36);
//  - reprodução, caractere por caractere no formato %g do MATLAB, das leis 1 e 2 de DEM_DYNA.txt
//    (curvas de carga e descarga, TYI e CYI), com o fator g calculado pelo mesmo algoritmo de
//    GF_field.m (classes de Weibull, CVgf = 0,8, inter = 20);
//  - identidades da lei: δr = EPR·g·LCR, ½·FP·δr = GRF·g²·CAF·LCO²·(E·A/ENR), U_el(pico)/U_d = 1/kr,
//    independência da energia em relação a ν, d_eq e Li;
//  - comprimento real Li, limite kr > 1 (Li < LCR) e recusas;
//  - lei ideal de Hillerborg (força nula na ruptura, dano secante, energias OPC e OAP);
//  - curva regularizada do MATLAB/APDL (força residual FP/100) e sua contribuição energética;
//  - tp = 3 como extensão isolada da referência (3·ENR/4).
#include "TestCheck.h"

#include "dem/BilinearLaw.h"
#include "dem/LatticeConstants.h"

#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

using namespace cad::dem;

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kInf = std::numeric_limits<double>::infinity();

bool approx(double value, double expected, double relative)
{
    return std::fabs(value - expected) <= relative * std::fabs(expected);
}

// Formatação %g do MATLAB (fprintf), igual à do printf de C para estes valores.
std::string g6(double v)
{
    char buf[64];
    std::snprintf(buf, sizeof buf, "%g", v);
    return buf;
}

std::string joined(const std::array<double, 5>& v)
{
    std::string s;
    for (std::size_t i = 0; i < v.size(); ++i)
        s += (i ? "," : "") + g6(v[i]);
    return s;
}

// Valores médios das classes do campo de Weibull de GF_field.m (linhas 38 a 67), xm(1..inter).
// Somente para o teste: o campo aleatório não faz parte desta etapa.
std::vector<double> weibullClasses(double cv, int inter)
{
    double gm = 35.0, err = 1.0, dg = 0.01;
    while (std::fabs(err) != 0.0)
    {
        if (err > 0)
            gm -= dg;
        if (err < 0)
        {
            gm += dg;
            dg /= 2.0;
        }
        const double de = cv * cv + 1.0 - std::tgamma(1.0 + 2.0 / gm) / std::pow(std::tgamma(1.0 + 1.0 / gm), 2.0);
        err = std::trunc(10000.0 * de); // fix()
    }
    const double bt = std::tgamma(1.0 + 1.0 / gm);
    std::vector<double> x(inter + 1, 0.0), xm(inter + 1, 0.0);
    for (int i = 1; i < inter; ++i)
    {
        x[i] = std::pow(std::pow(std::log(1.0 / (double(inter - i) / inter)), 1.0 / gm) / bt, 0.5);
        if (i == 1)
            xm[i] = (x[i] + std::pow(std::pow(std::log(1.0 / 0.999), 1.0 / gm) / bt, 0.5)) / 2.0;
        else
            xm[i] = (x[i] + x[i - 1]) / 2.0;
    }
    xm[inter] = (x[inter - 1] + std::pow(std::pow(std::log(1.0 / 0.001), 1.0 / gm) / bt, 0.5)) / 2.0;
    return xm;
}

const BarType kAllTypes[] = {BarType::EdgeOneCell,   BarType::EdgeTwoCells, BarType::EdgeThreeCells,
                             BarType::EdgeFourCells, BarType::CenterCenter, BarType::Diagonal};

// Comprimento real de cada tipo na rede regular.
double regularLength(BarType t, double lco)
{
    return t == BarType::Diagonal ? lco * std::sqrt(3.0) / 2.0 : lco;
}

// ------------------------------------------------------------------------------------------------
void testLatticeDefaults()
{
    std::printf("[1] constantes da rede (valores padrao, LCO = 1 mm)\n");
    const DemProperties p = matlabDefaults();
    CAD_CHECK(validateLattice(p, 1e-3).ok);
    const LatticeConstants c = computeLattice(p, 1e-3);

    // Formas fechadas exatas para ν = 0,36: delta = 3,24/1,12 = 81/28; alfa = (225/7)/(612/7) = 25/68.
    const double delta = 81.0 / 28.0;
    const double alpha = 25.0 / 68.0;
    CAD_CHECK(approx(c.caf, 0.1385, 1e-15));
    CAD_CHECK(approx(c.rfc, 10.0, 1e-15));
    CAD_CHECK(approx(c.poa, 0.8704, 1e-15));
    CAD_CHECK(approx(c.delta, delta, 1e-14));
    CAD_CHECK(approx(c.alpha, alpha, 1e-14));
    CAD_CHECK(approx(c.lcr, 2.0 * 0.1385 * 0.8704 * 0.01 / alpha, 1e-14)); // 6,55794176e-3 m
    CAD_CHECK(approx(c.lcr, 6.55794176e-3, 1e-12));
    CAD_CHECK(approx(c.epr, std::sqrt(2400.0 / (0.01 * 3.5e9 * 0.8704)), 1e-14));
    CAD_CHECK(approx(c.epr, 10.0 * std::sqrt(2400.0 / (3.5e9 * 0.8704)), 1e-14)); // forma do MATLAB, l. 128
    CAD_CHECK(approx(c.enr, 3.5e9 * 1e-6 * alpha, 1e-14));
    CAD_CHECK(approx(c.edg, (2.0 * delta / std::sqrt(3.0)) * c.enr, 1e-14));
    // kr da lei 1 de DEM_DYNA.txt: d(4)/d(3) = 4,16314e-05/6,34825e-06 (6 algarismos no arquivo).
    CAD_CHECK(approx(c.lcr / 1e-3, 4.16314e-05 / 6.34825e-06, 2e-6));

    std::printf("  POA = %.10g  delta = %.10g  alfa = %.10g\n", c.poa, c.delta, c.alpha);
    std::printf("  LCR = %.10g m  EPR = %.10g  ENR = %.10g N  EDG = %.10g N  (EDG/ENR = %.8g)\n", c.lcr, c.epr,
                c.enr, c.edg, c.edg / c.enr);
}

void testLatticeValidation()
{
    std::printf("[2] validacao das entradas da rede\n");
    const DemProperties p = matlabDefaults();
    for (double lco : {0.0, -1e-3, kNaN, kInf})
        CAD_CHECK(!validateLattice(p, lco).ok);
    DemProperties bad = p;
    bad.model.equivalentDefect = 0.0;
    CAD_CHECK(!validateLattice(bad, 1e-3).ok);
    bad = p;
    bad.material.poissonRatio = 0.5;
    CAD_CHECK(!validateLattice(bad, 1e-3).ok);
    // ν = 0 é aceito pela rede (como no MATLAB); a diagonal fica sem rigidez e a lei dela é recusada.
    DemProperties nu0 = p;
    nu0.material.poissonRatio = 0.0;
    CAD_CHECK(validateLattice(nu0, 1e-3).ok);
    const LatticeConstants c0 = computeLattice(nu0, 1e-3);
    CAD_CHECK(c0.edg == 0.0);
    CAD_CHECK(!makeBilinearLaw(c0, BarType::Diagonal, regularLength(BarType::Diagonal, 1e-3)).ok);
    CAD_CHECK(makeBilinearLaw(c0, BarType::EdgeFourCells, 1e-3).ok);
    DemProperties nuNeg = p;
    nuNeg.material.poissonRatio = -0.2;
    CAD_CHECK(validateLattice(nuNeg, 1e-3).ok);
    CAD_CHECK(!makeBilinearLaw(computeLattice(nuNeg, 1e-3), BarType::Diagonal, 0.8e-3).ok);
}

void checkLawAgainstFile(const LatticeConstants& c, double g, const char* f, const char* d1, const char* d2,
                         const char* tyi, const char* cyi)
{
    const BilinearLawResult r = makeBilinearLaw(c, BarType::EdgeOneCell, 1e-3, g);
    CAD_CHECK(r.ok);
    const RegularizedCurve load = matlabLoadingCurve(r.law);
    const RegularizedCurve unload = matlabUnloadingCurve(r.law);
    const std::string sf = joined(load.force), sd1 = joined(load.displacement);
    const std::string sf2 = joined(unload.force), sd2 = joined(unload.displacement);
    std::printf("  f = %s\n  d1 = %s\n  d2 = %s\n  TYI = %s  CYI = %s\n", sf.c_str(), sd1.c_str(), sd2.c_str(),
                g6(r.law.forcePeak).c_str(), g6(-r.law.forcePeak).c_str());
    CAD_CHECK(sf == f);
    CAD_CHECK(sd1 == d1);
    CAD_CHECK(sf2 == f);
    CAD_CHECK(sd2 == d2);
    CAD_CHECK(g6(r.law.forcePeak) == tyi);
    CAD_CHECK(g6(-r.law.forcePeak) == cyi);
}

void testDemDynaLaws()
{
    std::printf("[3] leis 1 e 2 de DEM_DYNA.txt (formato %%g do MATLAB)\n");
    const LatticeConstants c = computeLattice(matlabDefaults(), 1e-3);
    const std::vector<double> xm = weibullClasses(0.8, 20);
    std::printf("  classes de Gf_bond: xm(7) = %.14g  xm(11) = %.14g\n", xm[7], xm[11]);
    CAD_CHECK(approx(xm[7], 0.71522330621373, 1e-12));

    // Lei 1: barra 1 = aresta X de (0,0,0) a (0.001,0,0), 1 cubo ativo (tp = 1), Li = 0,001 m.
    // DEM_DYNA.txt, linhas 1045-1046 (carga), 1050-1051 (descarga), 1058-1059 (TYI, CYI).
    checkLawAgainstFile(c, xm[7], "-2.04218,0,2.04218,0.0204218,0.0204218",
                        "-6.34825e-06,0,6.34825e-06,4.16314e-05,8.32629e-05",
                        "-6.34825e-06,0,6.34825e-06,4.16314e-05,0.00416314", "2.04218", "-2.04218");
    // Lei 2 (linhas 1065-1066, 1070-1071, 1078-1079): mesma barra tp = 1, classe 11.
    checkLawAgainstFile(c, xm[11], "-2.63355,0,2.63355,0.0263355,0.0263355",
                        "-8.18657e-06,0,8.18657e-06,5.36871e-05,0.000107374",
                        "-8.18657e-06,0,8.18657e-06,5.36871e-05,0.00536871", "2.63355", "-2.63355");
}

void testIdentities()
{
    std::printf("[4] identidades da lei por tipo de barra (g = 1 e g = 0,8)\n");
    const LatticeConstants c = computeLattice(matlabDefaults(), 1e-3);
    for (double g : {1.0, 0.8})
        for (BarType t : kAllTypes)
        {
            const double li = regularLength(t, c.lco);
            const BilinearLawResult r = makeBilinearLaw(c, t, li, g);
            CAD_CHECK(r.ok);
            const BilinearLaw& w = r.law;
            const double ea = axialRigidity(c, t);
            CAD_CHECK(w.forcePeak == ea * w.strainPeak);
            CAD_CHECK(approx(w.strainPeak, c.epr * g, 1e-15));
            CAD_CHECK(approx(w.kr, c.lcr / li, 1e-15));
            CAD_CHECK(approx(w.stiffness, ea / li, 1e-15));
            CAD_CHECK(approx(w.dispRupture, c.epr * g * c.lcr, 1e-13));
            const double expectedEnergy = 2400.0 * g * g * c.caf * c.lco * c.lco * (ea / c.enr);
            CAD_CHECK(approx(idealFractureEnergy(w), expectedEnergy, 1e-12));
            CAD_CHECK(approx(idealWork(w, w.dispPeak) / idealFractureEnergy(w), 1.0 / w.kr, 1e-12));
            if (g == 1.0)
                std::printf("  %-48s Li = %.6g m  kr = %.6g  k0 = %.6g N/m  FP = %.6g N  dp = %.6g m  dr = %.6g m  "
                            "Gf_barra = %.6g J  U_el(A) = %.6g J\n",
                            barTypeLabel(t), li, w.kr, w.stiffness, w.forcePeak, w.dispPeak, w.dispRupture,
                            idealFractureEnergy(w), idealWork(w, w.dispPeak));
        }
}

void testEnergyInvariance()
{
    std::printf("[5] energia de fratura independente de nu e d_eq (POA e RFC se cancelam)\n");
    for (double nu : {0.1, 0.25, 0.36, 0.45})
        for (double deq : {0.002, 0.01, 0.05})
        {
            DemProperties p = matlabDefaults();
            p.material.poissonRatio = nu;
            p.model.equivalentDefect = deq;
            CAD_CHECK(validateLattice(p, 1e-3).ok);
            const LatticeConstants c = computeLattice(p, 1e-3);
            const BilinearLawResult r = makeBilinearLaw(c, BarType::EdgeFourCells, 1e-3);
            if (c.lcr <= 1e-3)
            {
                CAD_CHECK(!r.ok); // Li >= LCR
                continue;
            }
            CAD_CHECK(r.ok);
            CAD_CHECK(approx(idealFractureEnergy(r.law), 2400.0 * 0.1385 * 1e-6, 1e-12));
        }
}

void testRealLength()
{
    std::printf("[6] comprimento real Li (geometria projetada) e limite kr > 1\n");
    const LatticeConstants c = computeLattice(matlabDefaults(), 1e-3);
    const BilinearLaw ref = makeBilinearLaw(c, BarType::EdgeFourCells, 1e-3).law;
    for (double li : {0.05e-3, 0.6e-3, 1.4e-3, 3.0e-3})
    {
        const BilinearLawResult r = makeBilinearLaw(c, BarType::EdgeFourCells, li);
        CAD_CHECK(r.ok);
        const BilinearLaw& w = r.law;
        CAD_CHECK(w.strainPeak == ref.strainPeak);
        CAD_CHECK(w.forcePeak == ref.forcePeak);
        CAD_CHECK(approx(w.dispRupture, ref.dispRupture, 1e-13));
        CAD_CHECK(approx(w.stiffness, ref.stiffness * (1e-3 / li), 1e-13));
        CAD_CHECK(approx(w.dispPeak, ref.dispPeak * (li / 1e-3), 1e-13));
        CAD_CHECK(approx(idealFractureEnergy(w), idealFractureEnergy(ref), 1e-12));
        std::printf("  Li = %.4g mm: kr = %.6g  k0 = %.6g N/m  dp = %.6g m  dr = %.6g m\n", li * 1e3, w.kr,
                    w.stiffness, w.dispPeak, w.dispRupture);
    }
    CAD_CHECK(!makeBilinearLaw(c, BarType::EdgeFourCells, c.lcr).ok);
    CAD_CHECK(!makeBilinearLaw(c, BarType::EdgeFourCells, 1.01 * c.lcr).ok);
    CAD_CHECK(makeBilinearLaw(c, BarType::EdgeFourCells, 0.99 * c.lcr).ok);
    // LCR não depende de LCO: com os valores padrão, LCO = 10 mm já não admite a lei (Li = 10 mm > 6,56 mm).
    const LatticeConstants c10 = computeLattice(matlabDefaults(), 10e-3);
    CAD_CHECK(c10.lcr == c.lcr);
    CAD_CHECK(!makeBilinearLaw(c10, BarType::EdgeFourCells, 10e-3).ok);
    std::printf("  LCR = %.6g m (independente de LCO); LCO = 10 mm recusado\n", c.lcr);

    for (double li : {0.0, -1e-3, kNaN, kInf})
        CAD_CHECK(!makeBilinearLaw(c, BarType::EdgeFourCells, li).ok);
    for (double g : {0.0, -1.0, kNaN, kInf})
        CAD_CHECK(!makeBilinearLaw(c, BarType::EdgeFourCells, 1e-3, g).ok);
}

void testIdealLaw()
{
    std::printf("[7] lei ideal de Hillerborg (forca nula na ruptura, dano secante)\n");
    const LatticeConstants c = computeLattice(matlabDefaults(), 1e-3);
    const BilinearLaw w = makeBilinearLaw(c, BarType::EdgeFourCells, 1e-3).law;
    const double dp = w.dispPeak, dr = w.dispRupture, fp = w.forcePeak;

    CAD_CHECK(idealForce(w, 0.0) == 0.0);
    CAD_CHECK(idealForce(w, dp) == fp);
    CAD_CHECK(idealForce(w, dr) == 0.0);
    CAD_CHECK(idealForce(w, 2.0 * dr) == 0.0);
    CAD_CHECK(std::isnan(idealForce(w, -1e-12)));
    CAD_CHECK(std::isnan(idealForce(w, kNaN)));
    CAD_CHECK(approx(idealForce(w, 0.5 * dp), w.stiffness * 0.5 * dp, 1e-13));
    CAD_CHECK(approx(idealForce(w, 0.5 * (dp + dr)), 0.5 * fp, 1e-13));

    CAD_CHECK(idealDamage(w, 0.0) == 0.0);
    CAD_CHECK(idealDamage(w, dp) == 0.0);
    CAD_CHECK(idealDamage(w, dr) == 1.0);
    CAD_CHECK(idealDamage(w, 3.0 * dr) == 1.0);
    double last = 0.0, lastForce = fp;
    bool monotonic = true, decreasing = true;
    for (int i = 1; i <= 200; ++i)
    {
        const double d = dp + (dr - dp) * i / 200.0;
        const double dmg = idealDamage(w, d);
        monotonic = monotonic && dmg >= last && dmg <= 1.0;
        decreasing = decreasing && idealForce(w, d) <= lastForce;
        last = dmg;
        lastForce = idealForce(w, d);
    }
    CAD_CHECK(monotonic);
    CAD_CHECK(decreasing);

    // Energias: OPC + OAP = área sob a envoltória; trapézios finos conferem a integral.
    for (double d : {0.3 * dp, dp, 0.5 * (dp + dr), 0.9 * dr, dr, 1.5 * dr})
    {
        CAD_CHECK(approx(idealElasticEnergy(w, d) + idealDissipatedEnergy(w, d), idealWork(w, d), 1e-12));
        const int n = 20000;
        double sum = 0.0;
        for (int i = 0; i < n; ++i)
            sum += 0.5 * (idealForce(w, d * i / n) + idealForce(w, d * (i + 1) / n)) * (d / n);
        CAD_CHECK(approx(idealWork(w, d), sum, 1e-6));
    }
    CAD_CHECK(idealDissipatedEnergy(w, dp) == 0.0);
    CAD_CHECK(idealElasticEnergy(w, dr) == 0.0);
    CAD_CHECK(approx(idealDissipatedEnergy(w, dr), idealFractureEnergy(w), 1e-12));
    CAD_CHECK(approx(idealDissipatedEnergy(w, 5.0 * dr), idealFractureEnergy(w), 1e-12));
    const double dm = 0.5 * (dp + dr);
    std::printf("  dmax = (dp+dr)/2: D = %.6g  U_el(OPC) = %.6g J  U_d(OAP) = %.6g J  Gf_barra = %.6g J\n",
                idealDamage(w, dm), idealElasticEnergy(w, dm), idealDissipatedEnergy(w, dm), idealFractureEnergy(w));
}

void testRegularizedCurve()
{
    std::printf("[8] curva regularizada MATLAB/APDL (residual FP/100)\n");
    const LatticeConstants c = computeLattice(matlabDefaults(), 1e-3);
    for (BarType t : {BarType::EdgeFourCells, BarType::Diagonal})
    {
        const BilinearLaw w = makeBilinearLaw(c, t, regularLength(t, c.lco)).law;
        const RegularizedCurve load = matlabLoadingCurve(w);
        const RegularizedCurve unload = matlabUnloadingCurve(w);
        const double dp = w.dispPeak, dr = w.dispRupture, fp = w.forcePeak;

        CAD_CHECK(load.displacement[2] == dp && load.displacement[3] == dr);
        CAD_CHECK(load.displacement[4] == 2.0 * dr);
        CAD_CHECK(approx(unload.displacement[4], kMatlabUnloadingTailFactor * dr, 1e-14));
        CAD_CHECK(load.force[3] == fp / 100.0 && load.force[4] == fp / 100.0);
        for (std::size_t i = 0; i + 1 < 5; ++i)
            CAD_CHECK(load.displacement[i] < load.displacement[i + 1] && unload.displacement[i] < unload.displacement[i + 1]);

        // Igual à lei ideal até δp; difere no ramo descendente; residual não nulo depois de δr.
        CAD_CHECK(approx(curveForce(load, 0.5 * dp), idealForce(w, 0.5 * dp), 1e-13));
        CAD_CHECK(curveForce(load, dp) == fp);
        CAD_CHECK(curveForce(load, dr) == fp / 100.0);
        CAD_CHECK(idealForce(w, dr) == 0.0);
        CAD_CHECK(curveForce(load, 1.5 * dr) == fp / 100.0);
        CAD_CHECK(approx(curveForce(load, -0.5 * dp), -0.5 * fp, 1e-13));
        CAD_CHECK(std::isnan(curveForce(load, 2.0 * dr * 1.0001))); // além do último ponto: não assumido
        CAD_CHECK(std::isnan(curveForce(load, -1.0001 * dp)));

        // Contribuição energética da força residual.
        const double excess = curveWork(load, dr) - idealWork(w, dr);
        CAD_CHECK(approx(excess, regularizedExcessToRupture(w), 1e-10));
        CAD_CHECK(approx(curveWork(load, 2.0 * dr) - curveWork(load, dr), regularizedPlateauWork(w), 1e-10));
        CAD_CHECK(approx(regularizedExcessToRupture(w) / idealFractureEnergy(w), (1.0 - 1.0 / w.kr) / 100.0, 1e-12));
        CAD_CHECK(approx(regularizedPlateauWork(w) / idealFractureEnergy(w), 2.0 / 100.0, 1e-12));
        CAD_CHECK(approx(curveWork(unload, dr), curveWork(load, dr), 1e-12));
        std::printf("  %-36s Gf_ideal = %.6g J  excesso ate dr = %.6g J (%.4g %%)  patamar dr..2dr = %.6g J (%.4g %%)\n",
                    barTypeLabel(t), idealFractureEnergy(w), excess, 100.0 * excess / idealFractureEnergy(w),
                    regularizedPlateauWork(w), 100.0 * regularizedPlateauWork(w) / idealFractureEnergy(w));
    }
}

void testThreeCellExtension()
{
    std::printf("[9] tp = 3: extensao isolada da referencia (3*ENR/4)\n");
    const LatticeConstants c = computeLattice(matlabDefaults(), 1e-3);
    for (BarType t : kAllTypes)
        CAD_CHECK(isMatlabReference(t) == (t != BarType::EdgeThreeCells));
    CAD_CHECK(matlabTp(BarType::EdgeOneCell) == 1 && matlabTp(BarType::EdgeTwoCells) == 2);
    CAD_CHECK(matlabTp(BarType::EdgeThreeCells) == 3 && matlabTp(BarType::EdgeFourCells) == 4);
    CAD_CHECK(matlabTp(BarType::CenterCenter) == 4 && matlabTp(BarType::Diagonal) == 0);
    CAD_CHECK(axialRigidity(c, BarType::EdgeOneCell) == c.enr / 4.0);
    CAD_CHECK(axialRigidity(c, BarType::EdgeTwoCells) == c.enr / 2.0);
    CAD_CHECK(axialRigidity(c, BarType::EdgeThreeCells) == 3.0 * c.enr / 4.0);
    CAD_CHECK(axialRigidity(c, BarType::EdgeFourCells) == c.enr);
    CAD_CHECK(axialRigidity(c, BarType::CenterCenter) == c.enr);
    CAD_CHECK(axialRigidity(c, BarType::Diagonal) == c.edg);
    const BilinearLaw w3 = makeBilinearLaw(c, BarType::EdgeThreeCells, 1e-3).law;
    const BilinearLaw w4 = makeBilinearLaw(c, BarType::EdgeFourCells, 1e-3).law;
    CAD_CHECK(approx(w3.forcePeak, 0.75 * w4.forcePeak, 1e-15));
    CAD_CHECK(w3.dispPeak == w4.dispPeak && w3.dispRupture == w4.dispRupture);
    CAD_CHECK(approx(idealFractureEnergy(w3), 0.75 * idealFractureEnergy(w4), 1e-14));
    std::printf("  FP(tp3) = %.6g N = 0,75 x FP(tp4) = %.6g N; nao reproduzivel pelo MATLAB (lawnumbers.m deixa FP = 0)\n",
                w3.forcePeak, w4.forcePeak);
}

} // namespace

int main()
{
    testLatticeDefaults();
    testLatticeValidation();
    testDemDynaLaws();
    testIdentities();
    testEnergyInvariance();
    testRealLength();
    testIdealLaw();
    testRegularizedCurve();
    testThreeCellExtension();
    return cad::test::finish("test_e574b");
}
