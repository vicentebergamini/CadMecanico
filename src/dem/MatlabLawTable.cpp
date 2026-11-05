#include "dem/MatlabLawTable.h"

#include <cmath>
#include <cstddef>
#include <string>
#include <utility>

namespace cad::dem {

namespace {

bool finite(double v)
{
    return std::isfinite(v);
}

// E·A de lawnumbers.m para tp = 1, 2, 4 e 0, pelas MESMAS expressões (ENR/4, ENR/2, ENR, EDG) de
// axialRigidity(), cuja equivalência operação a operação com lawnumbers.m l. 43/50/56/62 está
// documentada em BilinearLaw.cpp. tp = 3 nunca chega aqui.
double matlabAxialRigidity(const LatticeConstants& c, int tp)
{
    switch (tp)
    {
    case 1: return axialRigidity(c, BarType::EdgeOneCell);   // ENR/4
    case 2: return axialRigidity(c, BarType::EdgeTwoCells);  // ENR/2
    case 4: return axialRigidity(c, BarType::EdgeFourCells); // ENR
    case 0: return axialRigidity(c, BarType::Diagonal);      // EDG
    default: return 0.0;
    }
}

MatlabLawTable fail(MatlabLawTable t, std::string message)
{
    t.ok = false;
    t.message = std::move(message);
    t.laws.clear();
    t.bondLaw.clear();
    t.bondKr.clear();
    t.bondEep.clear();
    t.bondFp.clear();
    t.bondEeu.clear();
    return t;
}

} // namespace

double MatlabLaw::maxRelativeLengthDeviation() const
{
    if (!(li > 0.0))
        return 0.0;
    const double a = std::fabs(minLength - li), b = std::fabs(maxLength - li);
    return (a > b ? a : b) / li;
}

std::vector<std::int32_t> MatlabLawTable::lawsWithMixedLengths() const
{
    std::vector<std::int32_t> ids;
    for (const MatlabLaw& w : laws)
        if (w.bondsWithOtherLength > 0)
            ids.push_back(w.id);
    return ids;
}

std::int32_t matlabCurveOffset(std::int64_t k)
{
    // round(k, −1) do MATLAB: múltiplo de 10 mais próximo, empate para longe de zero (k ≥ 0).
    return std::int32_t(((k + 5) / 10) * 10 + 10);
}

MatlabLawTable buildMatlabLawTable(const LatticeConstants& c, const std::vector<std::int8_t>& tp,
                                   const std::vector<double>& length, const std::vector<double>& gf)
{
    MatlabLawTable t;
    t.lco = c.lco;
    t.tolerance = c.lco * 1e-6; // LCO*1e-6 (l. 23)

    if (!(finite(c.lco) && c.lco > 0.0))
        return fail(t, "LCO deve ser finito e positivo.");
    const double constants[] = {c.lcr, c.epr, c.enr, c.edg};
    for (double v : constants)
        if (!finite(v))
            return fail(t, "As constantes da rede (LCR, EPR, ENR, EDG) devem ser finitas.");
    if (tp.size() != length.size() || tp.size() != gf.size())
        return fail(t, "Os vetores tp, Li e Gf devem ter um valor por barra (tamanhos diferentes).");

    const std::size_t n = tp.size();
    for (std::size_t b = 0; b < n; ++b)
    {
        if (tp[b] < 0 || tp[b] > 4)
            return fail(t, "Barra " + std::to_string(b + 1) + " com tp = " + std::to_string(int(tp[b]))
                               + " (malha_por_mascara.m só produz tp de 0 a 4).");
        if (!(finite(length[b]) && length[b] > 0.0))
            return fail(t, "Barra " + std::to_string(b + 1) + " com comprimento Li não finito ou não positivo.");
        if (!finite(gf[b]))
            return fail(t, "Barra " + std::to_string(b + 1) + " com Gf não finito.");
    }

    t.bondLaw.assign(n, 0);
    t.bondKr.assign(n, 0.0);
    t.bondEep.assign(n, 0.0); // EEP = zeros(nBonds,1) (l. 6)
    t.bondFp.assign(n, 0.0);  // FP  = zeros(nBonds,1) (l. 7)
    t.bondEeu.assign(n, 0.0); // EEU = zeros(nBonds,1) (l. 8)

    for (std::size_t b = 0; b < n; ++b)
    {
        t.bondKr[b] = c.lcr / length[b]; // kr(b) = LCR(1)/UNDEFORMEDLENGTH(b) (l. 19)

        // Leis repetidas: busca literal de trás para frente (l. 21-32).
        bool repeated = false;
        for (std::size_t i = b; i-- > 0;)
        {
            ++t.comparisons;
            if (std::fabs(gf[b] - gf[i]) < c.lco * 1e-6 && tp[b] == tp[i])
            {
                repeated = true;
                t.bondEep[b] = t.bondEep[i];
                t.bondFp[b] = t.bondFp[i];
                t.bondEeu[b] = t.bondEeu[i];
                t.bondLaw[b] = t.bondLaw[i];
                break;
            }
        }
        if (repeated)
            continue;

        // Lei nova (l. 34-64).
        MatlabLaw w;
        w.id = std::int32_t(t.laws.size()) + 1; // k = k + 1
        w.tp = tp[b];
        w.gf = gf[b];                           // Gfb(k) = Gf_bond(b)
        w.firstBond = std::int64_t(b) + 1;      // lawBond(k) = b
        w.li = length[b];
        w.kr = t.bondKr[b];
        w.hasMatlabBranch = w.tp != 3;
        t.bondLaw[b] = w.id;
        if (w.hasMatlabBranch)
        {
            t.bondEep[b] = c.epr * w.gf;                             // EEP(b) = EPR * Gfb(k)
            t.bondFp[b] = matlabAxialRigidity(c, w.tp) * t.bondEep[b]; // FP(b) = (E·A) * EEP(b)
            t.bondEeu[b] = t.bondEep[b] * t.bondKr[b];               // EEU(b) = EEP(b) * kr(b)
        }
        // tp = 3: nenhum ramo; EEP, FP e EEU permanecem 0.
        w.eep = t.bondEep[b];
        w.fp = t.bondFp[b];
        w.eeu = t.bondEeu[b];
        t.laws.push_back(w);
    }

    // jj e identificadores das curvas (l. 71-108).
    t.curveOffset = matlabCurveOffset(std::int64_t(t.laws.size()));
    for (MatlabLaw& w : t.laws)
    {
        // Curvas com EEP_, FP_, kr_ e LI_ da primeira barra (l. 75-80), mesmas operações de
        // matlabLoadingCurve/matlabUnloadingCurve (BilinearLaw.cpp).
        BilinearLaw source;
        source.length = w.li;
        source.kr = w.kr;
        source.strainPeak = w.eep;
        source.forcePeak = w.fp;
        w.loading = matlabLoadingCurve(source);
        w.unloading = matlabUnloadingCurve(source);
        w.loadingCurveId = w.id;
        w.unloadingCurveId = w.id + t.curveOffset;
        w.minLength = w.maxLength = w.li;
    }

    // Associação elemento–lei e informação de comprimentos (diagnóstico posterior).
    for (std::size_t b = 0; b < n; ++b)
    {
        MatlabLaw& w = t.laws[std::size_t(t.bondLaw[b] - 1)];
        ++w.assignedBonds;
        if (length[b] < w.minLength)
            w.minLength = length[b];
        if (length[b] > w.maxLength)
            w.maxLength = length[b];
        if (length[b] != w.li)
            ++w.bondsWithOtherLength;
    }

    t.ok = true;
    return t;
}

WeibullClasses weibullBins(double cv, int inter)
{
    WeibullClasses w;
    w.cv = cv;
    w.inter = inter;
    if (!(finite(cv) && cv > 0.0))
    {
        w.message = "O coeficiente de variação CVgf deve ser finito e positivo (CVgf = 0 usa Gf = 1).";
        return w;
    }
    if (inter < 2)
    {
        w.message = "O número de divisões inter deve ser pelo menos 2.";
        return w;
    }

    // GF_field.m l. 38-53.
    double gm = 35.0;
    double err = 1.0;
    double dg = 0.01;
    const double cv2 = cv * cv; // Cv^2
    long long iterations = 0;
    while (std::fabs(err) != 0.0)
    {
        if (++iterations > 100000000LL)
        {
            w.message = "A iteração do parâmetro de forma GM da Weibull não convergiu.";
            return w;
        }
        if (err > 0)
            gm = gm - dg;
        if (err < 0)
        {
            gm = gm + dg;
            dg = dg / 2.0;
        }
        const double de = cv2 + 1.0 - (std::tgamma(1.0 + 2.0 / gm)) / std::pow(std::tgamma(1.0 + 1.0 / gm), 2.0);
        err = std::trunc(10000.0 * de); // fix()
        if (!finite(err))
        {
            w.message = "A iteração do parâmetro de forma GM da Weibull produziu valores não finitos.";
            return w;
        }
    }
    const double bt = std::tgamma(1.0 + 1.0 / gm);

    // GF_field.m l. 57-67.
    std::vector<double> x(std::size_t(inter), 0.0), xm(std::size_t(inter), 0.0); // índice MATLAB i → i − 1
    for (int i = 1; i < inter; ++i)
    {
        x[std::size_t(i - 1)] =
            std::pow(std::pow(std::log(1.0 / (double(inter - i) / double(inter))), 1.0 / gm) / bt, 0.5);
        if (i == 1)
            xm[0] = (x[0] + std::pow(std::pow(std::log(1.0 / (0.999)), 1.0 / gm) / bt, 0.5)) / 2.0;
        else
            xm[std::size_t(i - 1)] = (x[std::size_t(i - 1)] + x[std::size_t(i - 2)]) / 2.0;
    }
    xm[std::size_t(inter - 1)] =
        (x[std::size_t(inter - 2)] + std::pow(std::pow(std::log(1.0 / (0.001)), 1.0 / gm) / bt, 0.5)) / 2.0;
    x.resize(std::size_t(inter - 1));

    w.gm = gm;
    w.bt = bt;
    w.x = std::move(x);
    w.xm = std::move(xm);
    w.ok = true;
    return w;
}

} // namespace cad::dem
