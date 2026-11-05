// E5.7.4b.6 sem interface — texto APDL/DYNA das leis constitutivas e da associação elemento–material
// (transcrição de lawnumbers.m l. 71-117):
//  [1] 15 casos executados no lawnumbers.m original (MATLAB R2023b): uma lei, várias leis, barras
//      compartilhando uma lei, tp = 3 nulo, Gf = 0, comprimentos diferentes na mesma lei, caso S1
//      (tolerância não transitiva), limites de jj (k = 4, 5, 14, 15, 45, 95), valores pequenos e
//      grandes em notação científica, zero negativo: texto C++ idêntico byte a byte ao do MATLAB;
//  [2] DEM_DYNA.txt: seções das leis e do EMODIF extraídas do arquivo e comparadas byte a byte;
//      51 leis, 490 associações, IDs das curvas e contagem dos comandos;
//  [3] ordem dos comandos (*DIM, f/d, EDCURVE, MAT, TB, TBDAT, EMODIF);
//  [4] numeração: ordem de primeira ocorrência, associação relida do texto, IDs sem colisão;
//  [5] formatadores %g/%f e tabela inválida.
// Referência: tests/dados/matlab_e574b6_ref.txt, gerado por tests/dados/gerar_ref_e574b6.m.
#include "TestCheck.h"

#include "dem/LatticeConstants.h"
#include "dem/MatlabLawTable.h"
#include "discretization/ApdlExporter.h"
#include "discretization/LatticeTopology.h"
#include "discretization/MatlabLawApdl.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using namespace cad::discretization;
namespace dem = cad::dem;

namespace {

std::string readAll(const char* path)
{
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

std::vector<std::string> lines(const std::string& text)
{
    std::vector<std::string> out;
    std::size_t p = 0;
    while (p < text.size())
    {
        const std::size_t e = text.find('\n', p);
        if (e == std::string::npos)
        {
            out.push_back(text.substr(p));
            break;
        }
        out.push_back(text.substr(p, e - p));
        p = e + 1;
    }
    return out;
}

// Primeira linha diferente (para o relatório).
std::string firstDifference(const std::string& a, const std::string& b)
{
    const std::vector<std::string> la = lines(a), lb = lines(b);
    for (std::size_t i = 0; i < la.size() || i < lb.size(); ++i)
    {
        const std::string x = i < la.size() ? la[i] : "(fim)";
        const std::string y = i < lb.size() ? lb[i] : "(fim)";
        if (x != y)
            return "linha " + std::to_string(i + 1) + ": C++ [" + x + "] | MATLAB [" + y + "]";
    }
    return "tamanhos diferentes";
}

// ------------------------------------------------------------------ referência MATLAB
struct RefCase
{
    std::string name;
    double lco = 0, lcr = 0, epr = 0, enr = 0, edg = 0;
    std::vector<std::int8_t> tp;
    std::vector<double> li, gf;
    std::string text; // saída exata de lawnumbers.m
};

std::vector<RefCase> readCases(const char* path)
{
    std::vector<RefCase> cases;
    const std::string all = readAll(path);
    std::size_t p = 0;
    while ((p = all.find("#CASE ", p)) != std::string::npos)
    {
        RefCase c;
        const auto line = [&]() {
            const std::size_t e = all.find('\n', p);
            std::string l = all.substr(p, e - p);
            p = e + 1;
            return l;
        };
        std::istringstream h(line());
        std::string tag;
        h >> tag >> c.name;
        std::istringstream k(line());
        k >> tag >> c.lco >> c.lcr >> c.epr >> c.enr >> c.edg;
        std::istringstream t(line());
        t >> tag;
        int v;
        while (t >> v)
            c.tp.push_back(std::int8_t(v));
        std::istringstream l(line());
        l >> tag;
        double d;
        while (l >> d)
            c.li.push_back(d);
        std::istringstream g(line());
        g >> tag;
        while (g >> d)
            c.gf.push_back(d);
        std::istringstream b(line());
        std::size_t n = 0;
        b >> tag >> n; // #BEGIN n
        c.text = all.substr(p, n);
        p += n;
        CAD_CHECK(all.compare(p, 5, "#END\n") == 0);
        cases.push_back(c);
    }
    return cases;
}

dem::MatlabLawTable tableFor(const RefCase& c)
{
    dem::LatticeConstants k;
    k.lco = c.lco;
    k.lcr = c.lcr;
    k.epr = c.epr;
    k.enr = c.enr;
    k.edg = c.edg;
    return dem::buildMatlabLawTable(k, c.tp, c.li, c.gf);
}

// ------------------------------------------------------------------ [3] ordem dos comandos
// Chave de cada linha: comando APDL ou tipo de linha.
std::string key(const std::string& l)
{
    if (l.empty())
        return "vazia";
    if (l.rfind("!************************INICIO DA LEI ", 0) == 0)
        return "INICIO";
    if (l.rfind("!************************FIM DA LEI ", 0) == 0)
        return "FIM";
    if (l == "! ***********************DEFINE A LEI EM CADA MOLA e ENERGIAS")
        return "DEFINE";
    if (l.rfind("*DIM,", 0) == 0)
        return "*DIM";
    if (l.rfind("EDCURVE,", 0) == 0)
        return "EDCURVE";
    if (l.rfind("MAT,", 0) == 0)
        return "MAT";
    if (l.rfind("TB,DISCRETE,", 0) == 0)
        return "TB";
    if (l.rfind("TBDAT,", 0) == 0)
        return "TBDAT" + l.substr(6, 1);
    if (l.rfind("EMODIF,", 0) == 0)
        return "EMODIF";
    if (l.find("(1)= ") != std::string::npos)
        return l.substr(0, 2); // f1, d1, f2, d2
    return "?";
}

const std::vector<std::string> kLawPattern = {"vazia", "INICIO", "*DIM", "*DIM", "f1", "d1", "EDCURVE", "*DIM",
                                              "*DIM", "f2", "d2", "EDCURVE", "MAT", "TB", "TBDAT1", "TBDAT2",
                                              "TBDAT3", "TBDAT4", "TBDAT5", "FIM"};

bool commandOrderOk(const std::string& text, std::int64_t laws, std::int64_t bonds)
{
    const std::vector<std::string> ls = lines(text);
    std::size_t i = 0;
    for (std::int64_t j = 0; j < laws; ++j)
        for (const std::string& k : kLawPattern)
            if (i >= ls.size() || key(ls[i++]) != k)
                return false;
    if (i >= ls.size() || key(ls[i++]) != "DEFINE")
        return false;
    for (std::int64_t b = 0; b < bonds; ++b)
        if (i >= ls.size() || key(ls[i++]) != "EMODIF")
            return false;
    return i == ls.size();
}

// ------------------------------------------------------------------ [4] numeração
// Relê o texto gerado: leis 1..k em ordem, MAT/TB/TBDAT coerentes, curvas sem colisão, EMODIF = tabela.
bool numberingOk(const std::string& text, const dem::MatlabLawTable& t, std::string& why)
{
    std::set<long> curves;
    std::vector<long> loadIds, unloadIds;
    std::vector<int> emodif;
    long expectedLaw = 0, currentLaw = 0;
    for (const std::string& l : lines(text))
    {
        const std::string k = key(l);
        if (k == "INICIO")
        {
            currentLaw = std::atol(l.c_str() + 39);
            if (currentLaw != ++expectedLaw)
                return why = "leis fora de ordem", false;
        }
        else if (k == "EDCURVE")
        {
            const long id = std::atol(l.c_str() + 12);
            if (!curves.insert(id).second)
                return why = "ID de curva repetido", false;
            (l.find(",d1") != std::string::npos ? loadIds : unloadIds).push_back(id);
        }
        else if (k == "MAT" || k == "TB")
        {
            if (std::atol(l.c_str() + (k == "MAT" ? 4 : 12)) != currentLaw)
                return why = "MAT/TB diferente da lei", false;
        }
        else if (k == "TBDAT1" || k == "TBDAT2")
        {
            const long id = std::atol(l.c_str() + 8);
            if (id != (k == "TBDAT1" ? loadIds.back() : unloadIds.back()))
                return why = "TBDAT 1/2 diferente do EDCURVE", false;
        }
        else if (k == "EMODIF")
        {
            const double b = std::strtod(l.c_str() + 7, nullptr);
            if (b != double(emodif.size() + 1))
                return why = "EMODIF fora da ordem dos elementos", false;
            emodif.push_back(int(std::strtod(l.c_str() + l.find(",MAT,") + 5, nullptr)));
        }
    }
    if (expectedLaw != t.lawCount())
        return why = "número de leis", false;
    for (std::size_t j = 0; j < loadIds.size(); ++j)
        if (loadIds[j] != long(j + 1) || unloadIds[j] != long(j + 1) + t.curveOffset)
            return why = "IDs de curva diferentes de j e j + jj", false;
    if (curves.size() != std::size_t(2 * t.lawCount()))
        return why = "colisão de curvas", false;
    if (emodif.size() != t.bondLaw.size())
        return why = "número de EMODIF", false;
    for (std::size_t b = 0; b < emodif.size(); ++b)
        if (emodif[b] != t.bondLaw[b])
            return why = "EMODIF diferente de mat_bonds", false;
    // Ordem de primeira ocorrência: a primeira barra de cada lei j é a primeira com mat_bonds = j.
    std::int32_t maxSoFar = 0;
    for (std::int32_t m : t.bondLaw)
    {
        if (m > maxSoFar + 1)
            return why = "ordem de primeira ocorrência", false;
        maxSoFar = m > maxSoFar ? m : maxSoFar;
    }
    return true;
}

// ------------------------------------------------------------------ [1] casos MATLAB
void testMatlabCases(const std::vector<RefCase>& cases)
{
    std::printf("[1] texto de lawnumbers.m (MATLAB R2023b) x C++, %zu casos\n", cases.size());
    CAD_CHECK(cases.size() == 15);
    int identical = 0;
    for (const RefCase& c : cases)
    {
        const dem::MatlabLawTable t = tableFor(c);
        CAD_CHECK(t.ok);
        const std::string gen = matlabLawBlock(t) + matlabEmodifBlock(t);
        const bool same = gen == c.text;
        identical += same ? 1 : 0;
        CAD_CHECK(same);
        CAD_CHECK(commandOrderOk(gen, t.lawCount(), t.bonds()));
        std::string why;
        const bool num = numberingOk(gen, t, why);
        CAD_CHECK(num);
        std::printf("  %-13s barras = %3lld  leis = %2d  jj = %3d  bytes = %5zu  %s%s%s\n", c.name.c_str(),
                    (long long)t.bonds(), t.lawCount(), t.curveOffset, gen.size(),
                    same ? "identico byte a byte" : "DIFERENTE", num ? "" : "; numeracao: ", num ? "" : why.c_str());
        if (!same)
            std::printf("    %s\n", firstDifference(gen, c.text).c_str());

        // Verificações específicas.
        if (c.name == "tp3_nula")
        {
            CAD_CHECK(gen.find("f11(1)= -0,0,0,0,0\n") != std::string::npos);
            CAD_CHECK(gen.find("d21(1)= -0,0,0,0,0\n") != std::string::npos);
            CAD_CHECK(gen.find("TBDAT,4,0 !TYI \nTBDAT,5,-0 !CYI \n") != std::string::npos);
            CAD_CHECK(gen.find("EMODIF,3.000000,MAT,1.000000, \n") != std::string::npos); // 2ª barra tp = 3
        }
        if (c.name == "gf_zero")
            CAD_CHECK(gen.find("f11(1)= -0,0,0,0,0\n") != std::string::npos && t.laws[0].tp == 4);
        if (c.name == "compartilhada")
            CAD_CHECK(t.lawCount() == 1 && gen.find("EMODIF,6.000000,MAT,1.000000, \n") != std::string::npos);
        if (c.name == "comprimentos")
            CAD_CHECK(t.lawsWithMixedLengths().size() == 2);
        if (c.name == "pequenos")
            CAD_CHECK(gen.find("d11(1)= -8.8759e-12,0,8.8759e-12,") != std::string::npos);
        if (c.name == "grandes")
            CAD_CHECK(gen.find("e+06") != std::string::npos);
        if (c.name.rfind("jj_k", 0) == 0)
        {
            const int k = std::atoi(c.name.c_str() + 4);
            const int jj = int(((k + 5) / 10) * 10 + 10);
            CAD_CHECK(t.lawCount() == k && t.curveOffset == jj);
            CAD_CHECK(gen.find("EDCURVE,ADD," + std::to_string(1 + jj) + ",d21(1),f21(1)\n") != std::string::npos);
            CAD_CHECK(gen.find("TBDAT,2," + std::to_string(k + jj) + " !CURVA DESCARGA \n") != std::string::npos);
        }
    }
    std::printf("  %d/%zu casos identicos byte a byte (leis + EMODIF)\n", identical, cases.size());
}

// ------------------------------------------------------------------ [2] DEM_DYNA.txt
cad::discretization::Discretization buildBox()
{
    VoxelGrid g = VoxelGrid::make(gp_XYZ(0.0, 0.0, 0.0), 1.0);
    g.nx = 4;
    g.ny = 4;
    g.nz = 2;
    Parameters p;
    p.lco = 1.0;
    p.criterion = InclusionCriterion::CenterInside;
    Discretization d;
    CAD_CHECK(d.build(g, p, std::vector<std::uint8_t>(32, 1)));
    return d;
}

void testDemDyna(const char* path)
{
    std::printf("[2] DEM_DYNA.txt: secoes das leis e do EMODIF byte a byte\n");
    const std::string file = readAll(path);
    CAD_CHECK(!file.empty());
    if (file.empty())
    {
        std::printf("  arquivo de referencia ausente: %s\n", path);
        return;
    }
    // Extração: do "\n" que precede "INICIO DA LEI 1" (fprintf(fid,'\n') de lawnumbers.m l. 82) até
    // o fim de "FIM DA LEI k\n"; o bloco EMODIF vai do cabeçalho DEFINE até a linha ESEL (principal l. 181).
    const std::string first = "\n\n!************************INICIO DA LEI 1\n";
    const std::size_t lawStart = file.find(first);
    const std::size_t define = file.find("! ***********************DEFINE A LEI EM CADA MOLA e ENERGIAS\n");
    const std::size_t esel = file.find("ESEL,S,TYPE,,1\n");
    CAD_CHECK(lawStart != std::string::npos && define != std::string::npos && esel != std::string::npos);
    if (lawStart == std::string::npos || define == std::string::npos || esel == std::string::npos)
        return;
    const std::string refLaws = file.substr(lawStart + 1, define - lawStart - 1);
    const std::string refEmodif = file.substr(define, esel - define);

    // Tabela: tp e Li da discretização C++, constantes de computeLattice, Gf de cada barra recuperado de
    // FP (TBDAT,4 da lei atribuída em EMODIF) e ajustado à classe xm exata (como em test_e574b5).
    const Discretization d = buildBox();
    const LatticeTopology topo = classifyLattice(d);
    const std::vector<double> len = linkLengths(d, LengthUnit::Meter);
    const dem::LatticeConstants c = dem::computeLattice(dem::matlabDefaults(), 1e-3);
    const dem::WeibullClasses w = dem::weibullBins(0.8, 20);
    std::vector<double> fpOfLaw(1, 0.0);
    for (const std::string& l : lines(refLaws))
        if (l.rfind("TBDAT,4,", 0) == 0)
            fpOfLaw.push_back(std::strtod(l.c_str() + 8, nullptr));
    std::vector<int> lawOfBond;
    for (const std::string& l : lines(refEmodif))
        if (l.rfind("EMODIF,", 0) == 0)
            lawOfBond.push_back(int(std::strtod(l.c_str() + l.find(",MAT,") + 5, nullptr)));
    CAD_CHECK(fpOfLaw.size() == 52 && lawOfBond.size() == 490 && topo.links() == 490);
    if (fpOfLaw.size() != 52 || lawOfBond.size() != 490 || topo.links() != 490)
        return;
    std::vector<double> gf(490);
    for (std::size_t b = 0; b < 490; ++b)
    {
        const int tp = topo.linkTp[b];
        const double ea = tp == 1 ? c.enr / 4.0 : tp == 2 ? c.enr / 2.0 : tp == 4 ? c.enr : c.edg;
        const double approx = fpOfLaw[std::size_t(lawOfBond[b])] / (ea * c.epr);
        std::size_t best = 0;
        for (std::size_t i = 1; i < w.xm.size(); ++i)
            if (std::fabs(w.xm[i] - approx) < std::fabs(w.xm[best] - approx))
                best = i;
        gf[b] = w.xm[best];
    }
    const dem::MatlabLawTable t = dem::buildMatlabLawTable(c, topo.linkTp, len, gf);
    CAD_CHECK(t.ok && t.lawCount() == 51 && t.curveOffset == 60 && t.bonds() == 490);

    const std::string genLaws = matlabLawBlock(t);
    const std::string genEmodif = matlabEmodifBlock(t);
    const bool lawsSame = genLaws == refLaws, emodifSame = genEmodif == refEmodif;
    CAD_CHECK(lawsSame);
    CAD_CHECK(emodifSame);
    if (!lawsSame)
        std::printf("  leis: %s\n", firstDifference(genLaws, refLaws).c_str());
    if (!emodifSame)
        std::printf("  EMODIF: %s\n", firstDifference(genEmodif, refEmodif).c_str());
    const std::string gen = genLaws + genEmodif;
    CAD_CHECK(commandOrderOk(gen, 51, 490));
    std::string why;
    CAD_CHECK(numberingOk(gen, t, why));

    // Contagem dos comandos (iguais nos dois textos, pois são idênticos).
    std::map<std::string, int> count;
    for (const std::string& l : lines(gen))
        ++count[key(l).substr(0, 5) == "TBDAT" ? "TBDAT" : key(l)];
    CAD_CHECK(count["*DIM"] == 204 && count["EDCURVE"] == 102 && count["MAT"] == 51 && count["TB"] == 51);
    CAD_CHECK(count["TBDAT"] == 255 && count["EMODIF"] == 490);
    CAD_CHECK(count["f1"] == 51 && count["d1"] == 51 && count["f2"] == 51 && count["d2"] == 51);
    std::printf("  bloco de leis: %zu bytes %s; bloco EMODIF: %zu bytes %s\n", genLaws.size(),
                lawsSame ? "identico" : "DIFERENTE", genEmodif.size(), emodifSame ? "identico" : "DIFERENTE");
    std::printf("  leis = %d  jj = %d  curvas: carga 1-51, descarga 61-111 (102 IDs distintos)\n", t.lawCount(),
                t.curveOffset);
    std::printf("  comandos: *DIM %d  vetores f/d %d  EDCURVE %d  MAT %d  TB %d  TBDAT %d  EMODIF %d\n", count["*DIM"],
                count["f1"] + count["d1"] + count["f2"] + count["d2"], count["EDCURVE"], count["MAT"], count["TB"],
                count["TBDAT"], count["EMODIF"]);
}

// ------------------------------------------------------------------ [5] formatadores
void testFormatters()
{
    std::printf("[5] formatadores %%g/%%f (valores conferidos no MATLAB R2023b) e tabela invalida\n");
    CAD_CHECK(matlabF(1.0) == "1.000000");
    CAD_CHECK(matlabF(490.0) == "490.000000");
    CAD_CHECK(matlabF(1234567.0) == "1234567.000000"); // sprintf('%f',1234567)
    CAD_CHECK(matlabG(1e6) == "1e+06");                // sprintf('%g',1e6)
    CAD_CHECK(matlabG(123456789.0) == "1.23457e+08");
    CAD_CHECK(matlabG(1e-5) == "1e-05");
    CAD_CHECK(matlabG(-0.0) == "-0");                  // sprintf('%g',-0)
    CAD_CHECK(matlabG(0.0) == "0");
    dem::MatlabLawTable invalid;
    CAD_CHECK(matlabLawBlock(invalid).empty() && matlabEmodifBlock(invalid).empty());
    // Tabela vazia válida: nenhuma lei, só o cabeçalho do EMODIF (como lawnumbers.m com nBonds = 0).
    const dem::MatlabLawTable empty = dem::buildMatlabLawTable(dem::computeLattice(dem::matlabDefaults(), 1e-3), {}, {}, {});
    CAD_CHECK(matlabLawBlock(empty).empty());
    CAD_CHECK(matlabEmodifBlock(empty) == "! ***********************DEFINE A LEI EM CADA MOLA e ENERGIAS\n");
}

} // namespace

int main()
{
    const std::vector<RefCase> cases = readCases(CAD_E574B6_REFERENCE);
    CAD_CHECK(!cases.empty());
    if (cases.empty())
        std::printf("referencia MATLAB ausente: %s\n", CAD_E574B6_REFERENCE);
    testMatlabCases(cases);
    testDemDyna(CAD_MATLAB_REFERENCE_DIR "/DEM_DYNA.txt");
    std::printf("[3] ordem dos comandos e [4] numeracao: verificadas em todos os casos de [1] e [2]\n");
    testFormatters();
    return cad::test::finish("test_e574b6");
}
