// E5.7.4b.5 sem interface — tabela de leis constitutivas DEM-DYNA (transcrição de lawnumbers.m):
//  [1] classes de Weibull (GF_field.m l. 38-67) comparadas com o MATLAB R2023b (bit a bit);
//  [2] jj = round(k,-1) + 10 para k = 0..200 comparado com o MATLAB (limites de arredondamento);
//  [3] constantes da rede: computeLattice × MATLAB (DEM_HIAGO_PRINCIPAL.m l. 123-131);
//  [4] caso sintético executado em lawnumbers.m: tolerância não transitiva (vale a barra mais
//      próxima), fronteira exata |ΔGf| = LCO·1e-6 (desigualdade estrita), tp obrigatório, tp = 3
//      nulo, Gf = 0, barras de comprimentos diferentes na mesma lei; saídas por barra bit a bit;
//  [5] redes de malha_por_mascara.m + lawnumbers.m (caixa 4×4×2, seção em L com tp = 3, cubos que se
//      tocam por aresta/vértice), Gf = 1 e Gf sobre as classes xm: tp, mat_bonds, k, jj e valores
//      de cada lei comparados com o MATLAB;
//  [6] equivalência com DEM_DYNA.txt (Gf aleatório do MATLAB, seed 10): Gf de cada lei recuperado
//      de FP e ajustado à classe xm exata; k = 51, jj = 60, 490 associações EMODIF e os pontos das
//      curvas (%g) e TYI de todas as leis idênticos ao arquivo;
//  [7] entradas recusadas e invariantes.
// Referências MATLAB: tests/dados/matlab_e574b5_ref.txt, gerado por tests/dados/gerar_ref_e574b5.m
// (executa lawnumbers.m e malha_por_mascara.m originais, sem alterá-los).
#include "TestCheck.h"

#include "dem/BilinearLaw.h"
#include "dem/LatticeConstants.h"
#include "dem/MatlabLawTable.h"
#include "discretization/ApdlExporter.h"
#include "discretization/LatticeTopology.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <limits>
#include <map>
#include <sstream>
#include <string>
#include <vector>

using namespace cad::dem;
namespace disc = cad::discretization;

namespace {

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

double relDiff(double a, double b)
{
    if (a == b)
        return 0.0;
    const double m = std::fabs(a) > std::fabs(b) ? std::fabs(a) : std::fabs(b);
    return std::fabs(a - b) / m;
}

// ------------------------------------------------------------------ referência MATLAB (arquivo)
struct RefLaw
{
    int id = 0, tp = 0;
    long long firstBond = 0;
    double gf = 0, li = 0, kr = 0, eep = 0, fp = 0, eeu = 0;
};
struct RefBond
{
    double kr = 0, eep = 0, fp = 0, eeu = 0;
};
struct RefCase
{
    long long bonds = 0;
    int k = 0, jj = 0;
    std::vector<int> tp, mat;
    std::vector<RefLaw> laws;
    std::vector<RefBond> perBond;
};
struct RefWeibull
{
    double cv = 0;
    int inter = 0;
    double gm = 0, bt = 0;
    std::vector<double> x, xm;
};
struct Reference
{
    bool ok = false;
    double lcr = 0, epr = 0, enr = 0, edg = 0, tol = 0;
    std::vector<int> jj;
    std::vector<RefWeibull> weibull;
    std::map<std::string, RefCase> cases;
};

Reference readReference(const char* path)
{
    Reference r;
    std::ifstream in(path);
    if (!in)
        return r;
    std::string line;
    RefCase* current = nullptr;
    while (std::getline(in, line))
    {
        std::istringstream s(line);
        std::string tag;
        s >> tag;
        if (tag == "CONST")
            s >> r.lcr >> r.epr >> r.enr >> r.edg >> r.tol;
        else if (tag == "JJ")
        {
            int v;
            while (s >> v)
                r.jj.push_back(v);
        }
        else if (tag == "WEIBULL")
        {
            RefWeibull w;
            std::string word;
            s >> w.cv >> w.inter >> w.gm >> w.bt >> word; // "X"
            for (int i = 0; i < w.inter - 1; ++i)
            {
                double v;
                s >> v;
                w.x.push_back(v);
            }
            s >> word; // "XM"
            for (int i = 0; i < w.inter; ++i)
            {
                double v;
                s >> v;
                w.xm.push_back(v);
            }
            r.weibull.push_back(w);
        }
        else if (tag == "CASE")
        {
            std::string name;
            RefCase c;
            s >> name >> c.bonds >> c.k >> c.jj;
            current = &(r.cases[name] = c);
        }
        else if (tag == "TP" && current)
        {
            int v;
            while (s >> v)
                current->tp.push_back(v);
        }
        else if (tag == "MAT" && current)
        {
            int v;
            while (s >> v)
                current->mat.push_back(v);
        }
        else if (tag == "LAW" && current)
        {
            RefLaw w;
            s >> w.id >> w.tp >> w.firstBond >> w.gf >> w.li >> w.kr >> w.eep >> w.fp >> w.eeu;
            current->laws.push_back(w);
        }
        else if (tag == "BOND" && current)
        {
            long long b;
            RefBond v;
            s >> b >> v.kr >> v.eep >> v.fp >> v.eeu;
            current->perBond.push_back(v);
        }
    }
    r.ok = !r.jj.empty() && !r.weibull.empty() && !r.cases.empty();
    return r;
}

// Constantes exatamente iguais às do MATLAB (para comparações bit a bit do algoritmo da tabela).
LatticeConstants matlabExactConstants(const Reference& r)
{
    LatticeConstants c = computeLattice(matlabDefaults(), 1e-3);
    c.lcr = r.lcr;
    c.epr = r.epr;
    c.enr = r.enr;
    c.edg = r.edg;
    return c;
}

// ------------------------------------------------------------------ redes (como test_e574b3)
struct Mask
{
    int nx = 0, ny = 0, nz = 0;
    std::vector<std::uint8_t> on; // X mais rápido, depois Y, depois Z (mesma ordem de ativo(i,j,k))
};

Mask makeMask(int nx, int ny, int nz, const std::function<bool(int, int, int)>& active)
{
    Mask m{nx, ny, nz, std::vector<std::uint8_t>(std::size_t(nx) * ny * nz, 0)};
    for (int k = 0; k < nz; ++k)
        for (int j = 0; j < ny; ++j)
            for (int i = 0; i < nx; ++i)
                m.on[std::size_t((std::int64_t(k) * ny + j) * nx + i)] = active(i, j, k) ? 1 : 0;
    return m;
}

disc::Discretization build(const Mask& m, double lcoMm)
{
    disc::VoxelGrid g = disc::VoxelGrid::make(gp_XYZ(0.0, 0.0, 0.0), lcoMm);
    g.nx = m.nx;
    g.ny = m.ny;
    g.nz = m.nz;
    disc::Parameters p;
    p.lco = lcoMm;
    p.criterion = disc::InclusionCriterion::CenterInside;
    disc::Discretization d;
    CAD_CHECK(d.build(g, p, m.on));
    return d;
}

// Máscaras de gerar_ref_e574b5.m (índices MATLAB base 1 → C++ base 0).
Mask maskByName(const std::string& name)
{
    if (name == "caixa")
        return makeMask(4, 4, 2, [](int, int, int) { return true; });
    if (name == "ele") // ele(3:4,3:4,:) = false
        return makeMask(4, 4, 2, [](int i, int j, int) { return !(i >= 2 && j >= 2); });
    // cubos(1,1,1), cubos(2,2,1), cubos(2,2,2), cubos(3,1,2)
    return makeMask(3, 3, 3, [](int i, int j, int k) {
        return (i == 0 && j == 0 && k == 0) || (i == 1 && j == 1 && k == 0) || (i == 1 && j == 1 && k == 1)
               || (i == 2 && j == 0 && k == 1);
    });
}

// ------------------------------------------------------------------ [1] Weibull
void testWeibull(const Reference& ref)
{
    std::printf("[1] classes de Weibull (GF_field.m l. 38-67) x MATLAB\n");
    for (const RefWeibull& rw : ref.weibull)
    {
        const WeibullClasses w = weibullBins(rw.cv, rw.inter);
        CAD_CHECK(w.ok);
        if (!w.ok)
            continue;
        CAD_CHECK(w.x.size() == rw.x.size() && w.xm.size() == rw.xm.size());
        int equal = 0, total = 0;
        double worst = 0.0;
        const auto cmp = [&](double a, double b) {
            ++total;
            equal += a == b ? 1 : 0;
            const double d = relDiff(a, b);
            worst = d > worst ? d : worst;
        };
        cmp(w.gm, rw.gm);
        cmp(w.bt, rw.bt);
        for (std::size_t i = 0; i < rw.x.size() && i < w.x.size(); ++i)
            cmp(w.x[i], rw.x[i]);
        for (std::size_t i = 0; i < rw.xm.size() && i < w.xm.size(); ++i)
            cmp(w.xm[i], rw.xm[i]);
        CAD_CHECK(w.gm == rw.gm); // iteração de GM idêntica
        CAD_CHECK(worst <= 4.0 * std::numeric_limits<double>::epsilon());
        std::printf("  CV = %.2g inter = %2d: GM = %.17g; %d/%d valores identicos bit a bit; maior desvio relativo %.3g\n",
                    rw.cv, rw.inter, w.gm, equal, total, worst);
    }
    CAD_CHECK(!weibullBins(0.0, 20).ok);
    CAD_CHECK(!weibullBins(-0.1, 20).ok);
    CAD_CHECK(!weibullBins(std::numeric_limits<double>::quiet_NaN(), 20).ok);
    CAD_CHECK(!weibullBins(0.8, 1).ok);
}

// ------------------------------------------------------------------ [2] jj
void testCurveOffset(const Reference& ref)
{
    std::printf("[2] jj = round(k,-1) + 10, k = 0..200, x MATLAB\n");
    CAD_CHECK(ref.jj.size() == 201);
    int equal = 0;
    for (std::size_t k = 0; k < ref.jj.size(); ++k)
        equal += matlabCurveOffset(std::int64_t(k)) == ref.jj[k] ? 1 : 0;
    CAD_CHECK(equal == int(ref.jj.size()));
    // Limites de arredondamento (empate para longe de zero).
    CAD_CHECK(matlabCurveOffset(0) == 10);
    CAD_CHECK(matlabCurveOffset(4) == 10);
    CAD_CHECK(matlabCurveOffset(5) == 20);
    CAD_CHECK(matlabCurveOffset(14) == 20);
    CAD_CHECK(matlabCurveOffset(15) == 30);
    CAD_CHECK(matlabCurveOffset(44) == 50);
    CAD_CHECK(matlabCurveOffset(45) == 60);
    CAD_CHECK(matlabCurveOffset(51) == 60);
    CAD_CHECK(matlabCurveOffset(95) == 110);
    // A curva de descarga j + jj nunca colide com uma curva de carga 1..k.
    for (std::int64_t k = 0; k <= 1000; ++k)
        CAD_CHECK(matlabCurveOffset(k) > k);
    std::printf("  %d/201 iguais ao MATLAB (k = 4 -> 10, 5 -> 20, 45 -> 60, 51 -> 60, 95 -> 110)\n", equal);
}

// ------------------------------------------------------------------ [3] constantes
void testConstants(const Reference& ref)
{
    std::printf("[3] constantes da rede: computeLattice x MATLAB\n");
    const LatticeConstants c = computeLattice(matlabDefaults(), 1e-3);
    struct Item
    {
        const char* name;
        double cpp, matlab;
    } items[] = {{"LCR", c.lcr, ref.lcr}, {"EPR", c.epr, ref.epr}, {"ENR", c.enr, ref.enr}, {"EDG", c.edg, ref.edg},
                 {"LCO*1e-6", c.lco * 1e-6, ref.tol}};
    for (const Item& it : items)
    {
        CAD_CHECK(relDiff(it.cpp, it.matlab) <= 4.0 * std::numeric_limits<double>::epsilon());
        std::printf("  %-8s C++ %.17g  MATLAB %.17g  %s (desvio relativo %.3g)\n", it.name, it.cpp, it.matlab,
                    it.cpp == it.matlab ? "identico" : "difere", relDiff(it.cpp, it.matlab));
    }
}

// Compara a tabela C++ com um caso MATLAB (bitExact: todos os valores das leis idênticos bit a bit).
void compareCase(const char* name, const MatlabLawTable& t, const RefCase& rc, bool bitExact)
{
    CAD_CHECK(t.ok);
    CAD_CHECK(t.bonds() == rc.bonds);
    CAD_CHECK(t.lawCount() == rc.k);
    CAD_CHECK(t.curveOffset == rc.jj);
    bool matOk = std::int64_t(rc.mat.size()) == t.bonds();
    for (std::size_t b = 0; matOk && b < rc.mat.size(); ++b)
        matOk = t.bondLaw[b] == rc.mat[b];
    CAD_CHECK(matOk);
    int lawsExact = 0;
    double worst = 0.0;
    bool idsOk = std::int32_t(rc.laws.size()) == t.lawCount();
    for (std::size_t j = 0; idsOk && j < rc.laws.size(); ++j)
    {
        const RefLaw& r = rc.laws[j];
        const MatlabLaw& w = t.laws[j];
        // Gf: bit a bit quando vem do MATLAB; com as classes xm do C++ (weibullBins) a diferença é de
        // poucos ulp (std::tgamma × gamma do MATLAB) e entra no desvio relativo.
        idsOk = w.id == r.id && w.tp == r.tp && w.firstBond == r.firstBond && (!bitExact || w.gf == r.gf);
        const double v[][2] = {{w.gf, r.gf}, {w.li, r.li}, {w.kr, r.kr}, {w.eep, r.eep}, {w.fp, r.fp}, {w.eeu, r.eeu}};
        bool exact = true;
        for (const auto& p : v)
        {
            exact = exact && p[0] == p[1];
            const double d = relDiff(p[0], p[1]);
            worst = d > worst ? d : worst;
        }
        lawsExact += exact ? 1 : 0;
    }
    CAD_CHECK(idsOk);
    if (bitExact)
        CAD_CHECK(lawsExact == rc.k);
    else
        CAD_CHECK(worst <= 8.0 * std::numeric_limits<double>::epsilon());
    std::printf("  %-10s barras = %4lld  leis = %3d  jj = %3d  mat_bonds %s  leis bit a bit: %d/%d  maior desvio %.3g\n",
                name, (long long)t.bonds(), t.lawCount(), t.curveOffset, matOk ? "identico" : "DIFERENTE", lawsExact,
                rc.k, worst);
}

// ------------------------------------------------------------------ [4] caso sintético
void testSynthetic(const Reference& ref)
{
    std::printf("[4] caso sintetico S1 executado em lawnumbers.m (tolerancia nao transitiva, tp = 3, Gf = 0, Li)\n");
    const auto it = ref.cases.find("S1");
    CAD_CHECK(it != ref.cases.end());
    if (it == ref.cases.end())
        return;
    const RefCase& rc = it->second;

    // Entradas idênticas às de gerar_ref_e574b5.m.
    const double lco = 1e-3, tol = lco * 1e-6, dg = lco * std::sqrt(3.0) / 2.0;
    const std::vector<double> gf = {1, 1 + 1.5e-9, 1 + 0.8e-9, 0, tol, 1, 1, 1, 1, 0.7, 0.7, 1, 1};
    const std::vector<std::int8_t> tp = {4, 4, 4, 4, 4, 3, 3, 0, 1, 2, 2, 4, 4};
    const std::vector<double> li = {lco, lco, lco, lco, lco, lco, lco, dg, lco, lco, 1.2 * lco, 1.2 * lco, lco};

    const LatticeConstants c = matlabExactConstants(ref);
    const MatlabLawTable t = buildMatlabLawTable(c, tp, li, gf);
    compareCase("S1", t, rc, true);
    CAD_CHECK(t.tolerance == ref.tol);

    // Saídas por barra (EEP, FP, EEU, kr) bit a bit.
    bool bondsOk = std::int64_t(rc.perBond.size()) == t.bonds();
    for (std::size_t b = 0; bondsOk && b < rc.perBond.size(); ++b)
        bondsOk = t.bondKr[b] == rc.perBond[b].kr && t.bondEep[b] == rc.perBond[b].eep
                  && t.bondFp[b] == rc.perBond[b].fp && t.bondEeu[b] == rc.perBond[b].eeu;
    CAD_CHECK(bondsOk);

    // Regras explícitas.
    CAD_CHECK(std::fabs(gf[2] - gf[0]) < tol);    // a barra 3 também estaria na tolerância da barra 1 (lei 1)...
    CAD_CHECK(t.bondLaw[2] == 2);                  // ...mas a mais próxima (barra 2) decide: lei 2
    CAD_CHECK(t.bondEep[2] == t.bondEep[1]);       // EEP copiado da barra 2 (Gf = 1 + 1,5e-9)
    CAD_CHECK(t.bondLaw[11] == 2);                 // barra 12 (Gf = 1) casa com a barra 3 antes da 1
    CAD_CHECK(t.bondLaw[4] != t.bondLaw[3]);       // |ΔGf| = LCO·1e-6 exato: desigualdade estrita, lei nova
    CAD_CHECK(std::fabs(gf[4] - gf[3]) == tol);
    CAD_CHECK(t.bondLaw[5] != t.bondLaw[0]);       // tp obrigatório: tp = 3 com Gf = 1 não reutiliza a lei 1
    const MatlabLaw& l3 = t.laws[std::size_t(t.bondLaw[5] - 1)];
    CAD_CHECK(l3.tp == 3 && !l3.hasMatlabBranch && l3.eep == 0.0 && l3.fp == 0.0 && l3.eeu == 0.0);
    CAD_CHECK(l3.assignedBonds == 2 && t.bondLaw[6] == l3.id);
    CAD_CHECK(std::signbit(l3.loading.force[0]) && l3.loading.force[0] == 0.0); // −FP_ = −0 (MATLAB %g: "-0")
    CAD_CHECK(l3.loading.displacement[4] == 0.0 && l3.unloading.displacement[4] == 0.0);
    const MatlabLaw& gf0 = t.laws[std::size_t(t.bondLaw[3] - 1)]; // Gf = 0, tp = 4: lei nula prevista
    CAD_CHECK(gf0.hasMatlabBranch && gf0.eep == 0.0 && gf0.fp == 0.0);
    // Comprimentos diferentes na mesma lei: associação preservada, curva com Li da primeira barra.
    const MatlabLaw& l2 = t.laws[1];
    CAD_CHECK(l2.firstBond == 2 && l2.li == lco && l2.kr == c.lcr / lco);
    CAD_CHECK(l2.assignedBonds == 4 && l2.bondsWithOtherLength == 1 && l2.maxLength == 1.2 * lco);
    CAD_CHECK(t.bondKr[11] == c.lcr / (1.2 * lco) && t.bondEeu[11] == l2.eeu); // kr próprio, EEU da lei
    const std::vector<std::int32_t> mixed = t.lawsWithMixedLengths();
    CAD_CHECK(mixed.size() == 2 && mixed[0] == 2 && mixed[1] == 8);
    CAD_CHECK(std::fabs(l2.maxRelativeLengthDeviation() - 0.2) < 1e-12);
    std::printf("  mat_bonds = ");
    for (std::int32_t v : t.bondLaw)
        std::printf("%d ", v);
    std::printf("(MATLAB identico)\n  leis com comprimentos diferentes: 2 e 8; lei tp = 3 (id %d) nula; curva f = %s\n",
                l3.id, joined(l3.loading.force).c_str());
}

// ------------------------------------------------------------------ [5] redes
void testLattices(const Reference& ref)
{
    std::printf("[5] redes de malha_por_mascara.m + lawnumbers.m x C++ (classifyLattice + linkLengths)\n");
    const WeibullClasses w = weibullBins(0.8, 20);
    CAD_CHECK(w.ok);
    const LatticeConstants exact = matlabExactConstants(ref);
    const LatticeConstants product = computeLattice(matlabDefaults(), 1e-3);
    for (const char* mask : {"caixa", "ele", "cubos"})
    {
        const disc::Discretization d = build(maskByName(mask), 1.0);
        const disc::LatticeTopology topo = disc::classifyLattice(d);
        CAD_CHECK(topo.ok);
        const std::vector<double> len = disc::linkLengths(d, disc::LengthUnit::Meter);
        const std::int64_t n = topo.links();
        for (const char* variant : {"_g1", "_xm"})
        {
            const std::string name = std::string(mask) + variant;
            const auto it = ref.cases.find(name);
            CAD_CHECK(it != ref.cases.end());
            if (it == ref.cases.end())
                continue;
            const RefCase& rc = it->second;
            bool tpOk = std::int64_t(rc.tp.size()) == n;
            for (std::int64_t b = 0; tpOk && b < n; ++b)
                tpOk = rc.tp[std::size_t(b)] == topo.linkTp[std::size_t(b)];
            CAD_CHECK(tpOk);
            // Gf = xm20(mod(7*b,20)+1): com as classes do MATLAB (*) e com weibullBins (sem *).
            std::vector<double> gfMatlab(std::size_t(n), 1.0), gf(std::size_t(n), 1.0);
            if (std::string(variant) == "_xm")
                for (std::int64_t b = 1; b <= n; ++b)
                {
                    gfMatlab[std::size_t(b - 1)] = ref.weibull[0].xm[std::size_t((7 * b) % 20)];
                    gf[std::size_t(b - 1)] = w.xm[std::size_t((7 * b) % 20)];
                }
            // Entradas do MATLAB (constantes e Gf) com tp e Li do C++: resultado bit a bit.
            compareCase((name + "*").c_str(), buildMatlabLawTable(exact, topo.linkTp, len, gfMatlab), rc, true);
            // Fluxo do produto: computeLattice e weibullBins.
            compareCase(name.c_str(), buildMatlabLawTable(product, topo.linkTp, len, gf), rc, false);
        }
    }
    std::printf("  (* = constantes e classes xm do MATLAB; sem * = computeLattice e weibullBins)\n");

    // Seção em L com Gf = 1: a lei tp = 3 é a única nula e as demais não são afetadas.
    const disc::Discretization d = build(maskByName("ele"), 1.0);
    const disc::LatticeTopology topo = disc::classifyLattice(d);
    const MatlabLawTable t = buildMatlabLawTable(product, topo.linkTp, disc::linkLengths(d, disc::LengthUnit::Meter),
                                                 std::vector<double>(std::size_t(topo.links()), 1.0));
    int nullLaws = 0;
    for (const MatlabLaw& l : t.laws)
    {
        nullLaws += l.fp == 0.0 ? 1 : 0;
        CAD_CHECK((l.tp == 3) == (l.fp == 0.0));
        CAD_CHECK((l.tp == 3) != l.hasMatlabBranch);
    }
    CAD_CHECK(nullLaws == 1 && topo.countByTp[3] == t.laws[3].assignedBonds);
    std::printf("  secao em L, Gf = 1: %d leis; lei 4 (tp = 3) nula atribuida a %lld barras: d = %s\n", t.lawCount(),
                (long long)t.laws[3].assignedBonds, joined(t.laws[3].loading.displacement).c_str());
}

// ------------------------------------------------------------------ [6] DEM_DYNA.txt
struct FileLaw
{
    std::string f1, d1, f2, d2, tyi, cyi;
    int loadId = 0, unloadId = 0;
};

void testDemDyna(const char* path)
{
    std::printf("[6] equivalencia com DEM_DYNA.txt (caixa 4 x 4 x 2, Gf aleatorio do MATLAB, seed 10)\n");
    std::ifstream in(path);
    CAD_CHECK(bool(in));
    if (!in)
    {
        std::printf("  arquivo de referencia ausente: %s\n", path);
        return;
    }
    std::map<int, FileLaw> laws;
    std::vector<int> emodif; // índice = elemento − 1
    int currentMat = 0;
    std::string line;
    while (std::getline(in, line))
    {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        const auto assign = [&](const char* prefix, std::string FileLaw::*field) {
            const std::size_t p = std::string(prefix).size();
            if (line.compare(0, p, prefix) != 0)
                return false;
            const std::size_t paren = line.find("(1)= ");
            if (paren == std::string::npos)
                return false;
            laws[std::atoi(line.substr(p, paren - p).c_str())].*field = line.substr(paren + 5);
            return true;
        };
        if (assign("f1", &FileLaw::f1) || assign("d1", &FileLaw::d1) || assign("f2", &FileLaw::f2)
            || assign("d2", &FileLaw::d2))
            continue;
        if (line.rfind("EDCURVE,ADD,", 0) == 0)
        {
            const int id = std::atoi(line.c_str() + 12);
            const std::size_t comma = line.find(',', 12);
            const std::string curve = line.substr(comma + 1, 2);
            const int j = std::atoi(line.c_str() + comma + 3);
            (curve == "d1" ? laws[j].loadId : laws[j].unloadId) = id;
        }
        else if (line.rfind("MAT,", 0) == 0)
            currentMat = std::atoi(line.c_str() + 4);
        else if (line.rfind("TBDAT,4,", 0) == 0)
            laws[currentMat].tyi = line.substr(8, line.find(' ') - 8);
        else if (line.rfind("TBDAT,5,", 0) == 0)
            laws[currentMat].cyi = line.substr(8, line.find(' ') - 8);
        else if (line.rfind("EMODIF,", 0) == 0)
        {
            char* end = nullptr;
            const double b = std::strtod(line.c_str() + 7, &end);
            const std::size_t mat = line.find(",MAT,");
            const double m = std::strtod(line.c_str() + mat + 5, nullptr);
            if (std::size_t(b) > emodif.size())
                emodif.resize(std::size_t(b), 0);
            emodif[std::size_t(b) - 1] = int(m);
        }
    }
    CAD_CHECK(laws.size() == 51 && emodif.size() == 490);

    const disc::Discretization d = build(maskByName("caixa"), 1.0);
    const disc::LatticeTopology topo = disc::classifyLattice(d);
    CAD_CHECK(topo.ok && topo.links() == 490);
    const std::vector<double> len = disc::linkLengths(d, disc::LengthUnit::Meter);
    const LatticeConstants c = computeLattice(matlabDefaults(), 1e-3);
    const WeibullClasses w = weibullBins(0.8, 20);

    // Gf por barra: Gf ≈ FP/(E·A·EPR) da lei da barra no arquivo (6 algarismos), ajustado à classe xm.
    std::vector<double> gf(490, 0.0);
    double worstSnap = 0.0;
    for (std::size_t b = 0; b < 490; ++b)
    {
        const int tp = topo.linkTp[b];
        const double ea = tp == 1 ? axialRigidity(c, BarType::EdgeOneCell)
                        : tp == 2 ? axialRigidity(c, BarType::EdgeTwoCells)
                        : tp == 4 ? axialRigidity(c, BarType::EdgeFourCells)
                                  : axialRigidity(c, BarType::Diagonal);
        const double approx = std::strtod(laws[emodif[b]].tyi.c_str(), nullptr) / (ea * c.epr);
        std::size_t best = 0;
        for (std::size_t i = 1; i < w.xm.size(); ++i)
            if (std::fabs(w.xm[i] - approx) < std::fabs(w.xm[best] - approx))
                best = i;
        const double dev = std::fabs(w.xm[best] - approx) / w.xm[best];
        worstSnap = dev > worstSnap ? dev : worstSnap;
        gf[b] = w.xm[best];
    }
    CAD_CHECK(worstSnap < 1e-5);

    const MatlabLawTable t = buildMatlabLawTable(c, topo.linkTp, len, gf);
    CAD_CHECK(t.ok && t.lawCount() == 51 && t.curveOffset == 60);
    int assocEqual = 0;
    for (std::size_t b = 0; b < 490 && b < t.bondLaw.size(); ++b)
        assocEqual += t.bondLaw[b] == emodif[b] ? 1 : 0;
    CAD_CHECK(assocEqual == 490);
    int curvesEqual = 0, idsEqual = 0, yieldEqual = 0;
    std::string firstDiff;
    for (const MatlabLaw& l : t.laws)
    {
        const FileLaw& f = laws[l.id];
        const bool same = joined(l.loading.force) == f.f1 && joined(l.loading.displacement) == f.d1
                          && joined(l.unloading.force) == f.f2 && joined(l.unloading.displacement) == f.d2;
        curvesEqual += same ? 1 : 0;
        if (!same && firstDiff.empty())
            firstDiff = "lei " + std::to_string(l.id) + ": C++ d1 = " + joined(l.loading.displacement)
                        + " | arquivo " + f.d1;
        idsEqual += (l.loadingCurveId == f.loadId && l.unloadingCurveId == f.unloadId) ? 1 : 0;
        yieldEqual += (g6(l.fp) == f.tyi && g6(-l.fp) == f.cyi) ? 1 : 0;
    }
    CAD_CHECK(curvesEqual == 51);
    CAD_CHECK(idsEqual == 51);
    CAD_CHECK(yieldEqual == 51);
    std::printf("  Gf ajustado as classes xm (maior desvio do valor de 6 algarismos: %.2g)\n", worstSnap);
    std::printf("  leis = %d  jj = %d  EMODIF iguais: %d/490  curvas (%%g) iguais: %d/51  IDs EDCURVE: %d/51  TYI/CYI: %d/51\n",
                t.lawCount(), t.curveOffset, assocEqual, curvesEqual, idsEqual, yieldEqual);
    if (!firstDiff.empty())
        std::printf("  primeira diferenca: %s\n", firstDiff.c_str());
    int byTp[5] = {0, 0, 0, 0, 0};
    for (const MatlabLaw& l : t.laws)
        ++byTp[l.tp];
    std::printf("  leis por tp: tp0 = %d  tp1 = %d  tp2 = %d  tp3 = %d  tp4 = %d; leis com comprimentos diferentes: %zu\n",
                byTp[0], byTp[1], byTp[2], byTp[3], byTp[4], t.lawsWithMixedLengths().size());
}

// ------------------------------------------------------------------ [7] recusas e invariantes
void testRefusalsAndInvariants()
{
    std::printf("[7] entradas recusadas e invariantes\n");
    const LatticeConstants c = computeLattice(matlabDefaults(), 1e-3);
    const double nan = std::numeric_limits<double>::quiet_NaN();
    CAD_CHECK(!buildMatlabLawTable(c, {4, 4}, {1e-3}, {1, 1}).ok);              // tamanhos
    CAD_CHECK(!buildMatlabLawTable(c, {5}, {1e-3}, {1}).ok);                    // tp fora de 0..4
    CAD_CHECK(!buildMatlabLawTable(c, {-1}, {1e-3}, {1}).ok);
    CAD_CHECK(!buildMatlabLawTable(c, {4}, {0.0}, {1}).ok);                     // Li
    CAD_CHECK(!buildMatlabLawTable(c, {4}, {nan}, {1}).ok);
    CAD_CHECK(!buildMatlabLawTable(c, {4}, {1e-3}, {nan}).ok);                  // Gf
    LatticeConstants bad = c;
    bad.lco = 0.0;
    CAD_CHECK(!buildMatlabLawTable(bad, {4}, {1e-3}, {1}).ok);
    // Sem barras: k = 0 e jj = round(0,-1) + 10 = 10.
    const MatlabLawTable empty = buildMatlabLawTable(c, {}, {}, {});
    CAD_CHECK(empty.ok && empty.lawCount() == 0 && empty.curveOffset == 10);
    // kr ≤ 1 (Li ≥ LCR) e Gf negativo são preservados (validação física é etapa posterior).
    const MatlabLawTable longBar = buildMatlabLawTable(c, {4, 1}, {2.0 * c.lcr, 1e-3}, {1.0, -0.5});
    CAD_CHECK(longBar.ok && longBar.laws[0].kr == 0.5 && longBar.laws[1].fp < 0.0);

    // Rede grande: invariantes e custo da busca literal.
    const disc::Discretization d = build(makeMask(30, 20, 10, [](int i, int j, int k) { return (i + 2 * j + 3 * k) % 7 != 0; }), 1.0);
    const disc::LatticeTopology topo = disc::classifyLattice(d);
    const WeibullClasses w = weibullBins(0.8, 20);
    std::vector<double> gf(std::size_t(topo.links()));
    std::uint32_t s = 12345u;
    for (double& v : gf)
    {
        s = s * 1664525u + 1013904223u;
        v = w.xm[(s >> 16) % 20];
    }
    const MatlabLawTable t = buildMatlabLawTable(c, topo.linkTp, disc::linkLengths(d, disc::LengthUnit::Meter), gf);
    CAD_CHECK(t.ok);
    std::int64_t assigned = 0;
    bool firstOk = true, keyOk = true, idsOk = true;
    for (const MatlabLaw& l : t.laws)
    {
        assigned += l.assignedBonds;
        firstOk = firstOk && t.bondLaw[std::size_t(l.firstBond - 1)] == l.id;
        idsOk = idsOk && l.loadingCurveId == l.id && l.unloadingCurveId == l.id + t.curveOffset;
    }
    // Numeração em ordem de primeira ocorrência: a lei de cada barra nunca excede 1 + maior lei anterior.
    std::int32_t maxSoFar = 0;
    for (std::size_t b = 0; b < t.bondLaw.size(); ++b)
    {
        keyOk = keyOk && t.bondLaw[b] <= maxSoFar + 1;
        const MatlabLaw& l = t.laws[std::size_t(t.bondLaw[b] - 1)];
        keyOk = keyOk && l.tp == topo.linkTp[b] && std::fabs(l.gf - gf[b]) < t.tolerance;
        maxSoFar = t.bondLaw[b] > maxSoFar ? t.bondLaw[b] : maxSoFar;
    }
    CAD_CHECK(assigned == t.bonds() && firstOk && keyOk && idsOk);
    CAD_CHECK(t.lawCount() <= 100); // no máximo 20 classes × 5 tipos
    std::printf("  rede 30 x 20 x 10 vazada: %lld barras, %d leis, jj = %d, %lld comparacoes (%.1f por barra)\n",
                (long long)t.bonds(), t.lawCount(), t.curveOffset, (long long)t.comparisons,
                double(t.comparisons) / double(t.bonds()));
}

} // namespace

int main()
{
    const Reference ref = readReference(CAD_E574B5_REFERENCE);
    CAD_CHECK(ref.ok);
    if (!ref.ok)
    {
        std::printf("referencia MATLAB ausente: %s\n", CAD_E574B5_REFERENCE);
        return cad::test::finish("test_e574b5");
    }
    testWeibull(ref);
    testCurveOffset(ref);
    testConstants(ref);
    testSynthetic(ref);
    testLattices(ref);
    testDemDyna(CAD_MATLAB_REFERENCE_DIR "/DEM_DYNA.txt");
    testRefusalsAndInvariants();
    return cad::test::finish("test_e574b5");
}
