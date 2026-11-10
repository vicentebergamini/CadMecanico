// E5.7.4b.8b sem interface — composição em memória do arquivo APDL/LS-DYNA DEM-DYNA
// (discretization/DemDynaApdl.h), sem escrita em disco:
//  [A] DEM_DYNA.txt: 51 leis, 490 barras, 107 massas; arquivo composto idêntico byte a byte à
//      referência MATLAB (com os dados de cabeçalho da própria referência) e, sem eles, idêntico a
//      menos das linhas de cabeçalho opcionais; nós, EN, massas, leis e EMODIF seção a seção;
//  [B] CVgf = 0: Gf = 1 fornecido explicitamente; leis iguais às de lawnumbers.m executado no MATLAB
//      (caso caixa_g1 de dados/matlab_e574b5_ref.txt);
//  [C] lei inválida (tp = 3 em barra ativa; e só em barras excluídas): composição bloqueada,
//      diagnósticos preservados, nenhum texto;
//  [D] IDs grandes: inteiros exatos (o "%g" do MATLAB arredondaria a partir de 10^6);
//  [E] dados inconsistentes da entrada pública: Gf ausente, de tamanho errado ou não finito, massas
//      inválidas (densidade), discretização vazia, propriedades e cabeçalho inválidos, exclusões sem EDELE;
//  [F] API pública: somente composeDemDyna(const DemDynaInput&); nenhum modelo intermediário aceito;
//  [G] precisão das coordenadas escritas com %g: colapso de nós e barras degeneradas recusados, nós
//      originalmente coincidentes não confundidos com colapso, geometria normal aceita.
// Nenhuma comparação é dispensada por ausência do arquivo de referência: arquivo ausente = falha.
#include "TestCheck.h"

#include "dem/DemProperties.h"
#include "dem/LawValidation.h"
#include "discretization/ApdlExporter.h" // linkLengths, ApdlLinkSequence (somente leitura)
#include "discretization/DemDynaApdl.h"
#include "discretization/LatticeTopology.h"
#include "discretization/MatlabLawApdl.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <iterator>
#include <limits>
#include <sstream>
#include <string>
#include <type_traits>
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
    std::size_t start = 0;
    while (start < text.size())
    {
        const std::size_t end = text.find('\n', start);
        out.push_back(text.substr(start, end == std::string::npos ? std::string::npos : end - start));
        if (end == std::string::npos)
            break;
        start = end + 1;
    }
    return out;
}

// Linhas que começam com o prefixo dado, concatenadas com "\n".
std::string linesStartingWith(const std::string& text, const std::string& prefix)
{
    std::string out;
    for (const std::string& l : lines(text))
        if (l.rfind(prefix, 0) == 0)
            out += l + "\n";
    return out;
}

// Trecho [início de 'from', início de 'to').
std::string between(const std::string& text, const std::string& from, const std::string& to)
{
    const std::size_t a = text.find(from);
    const std::size_t b = a == std::string::npos ? std::string::npos : text.find(to, a);
    return a == std::string::npos || b == std::string::npos ? std::string() : text.substr(a, b - a);
}

struct Mask
{
    int nx = 0, ny = 0, nz = 0;
    std::vector<std::uint8_t> on; // X mais rápido, depois Y, depois Z
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

Discretization build(const Mask& m)
{
    VoxelGrid g = VoxelGrid::make(gp_XYZ(0.0, 0.0, 0.0), 1.0);
    g.nx = m.nx;
    g.ny = m.ny;
    g.nz = m.nz;
    Parameters p;
    p.lco = 1.0;
    p.criterion = InclusionCriterion::CenterInside;
    Discretization d;
    CAD_CHECK(d.build(g, p, m.on));
    return d;
}

Mask caixa()
{
    return makeMask(4, 4, 2, [](int, int, int) { return true; });
}

Mask ele() // gerar_ref_e574b5.m: ele(3:4,3:4,:) = false (canto reentrante: tp = 3)
{
    return makeMask(4, 4, 2, [](int i, int j, int) { return !(i >= 2 && j >= 2); });
}

bool hasStage(const DemDynaComposition& c, const std::string& stage, std::int64_t firstId = -1)
{
    for (const DemDynaIssue& e : c.errors)
        if (e.stage == stage && (firstId < 0 || e.id == firstId))
            return true;
    return false;
}

void showErrors(const char* label, const DemDynaComposition& c)
{
    std::printf("  %s: ok = %s, texto = %zu bytes, %zu erro(s)\n", label, c.ok ? "sim" : "nao", c.text.size(),
                c.errors.size());
    for (const DemDynaIssue& e : c.errors)
        std::printf("    [%s] %s\n", e.stage.c_str(), e.message.c_str());
}

// Composição recusada sem nenhum texto utilizável.
bool refused(const DemDynaComposition& c)
{
    return !c.ok && c.text.empty() && !c.errors.empty();
}

// ------------------------------------------------------------------ [A]
void testDemDyna(const char* path)
{
    std::printf("[A] DEM_DYNA.txt: arquivo composto x referencia MATLAB\n");
    const std::string ref = readAll(path);
    CAD_CHECK(!ref.empty()); // referência obrigatória: ausência é falha, não comparação ignorada
    if (ref.empty())
    {
        std::printf("  FALHA: referencia ausente (%s)\n", path);
        return;
    }
    const std::vector<std::string> refLines = lines(ref);
    CAD_CHECK(refLines.size() > 7 && refLines[3].rfind("!Code generate on ", 0) == 0
              && refLines[4].rfind("/CWD,'", 0) == 0);

    // Gf da referência, reconstruído como em test_e574b6/b7 (FP da lei de cada barra → classe xm).
    const Discretization d = build(caixa());
    const LatticeTopology topo = classifyLattice(d);
    const dem::LatticeConstants c = dem::computeLattice(dem::matlabDefaults(), 1e-3);
    const dem::WeibullClasses w = dem::weibullBins(0.8, 20);
    const std::string refLaws = between(ref, "!************************INICIO DA LEI 1\n", "! ***********************DEFINE");
    const std::string refEmodif = between(ref, "! ***********************DEFINE", "ESEL,S,TYPE,,1\n");
    std::vector<double> fpOfLaw(1, 0.0);
    std::size_t q = 0;
    while ((q = refLaws.find("TBDAT,4,", q)) != std::string::npos)
        fpOfLaw.push_back(std::strtod(refLaws.c_str() + (q += 8), nullptr));
    std::vector<int> lawOfBond;
    q = 0;
    while ((q = refEmodif.find(",MAT,", q)) != std::string::npos)
        lawOfBond.push_back(int(std::strtod(refEmodif.c_str() + (q += 5), nullptr)));
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

    DemDynaInput in;
    in.discretization = &d;
    in.properties = dem::matlabDefaults();
    in.gf = gf;
    const std::vector<double> gfCopy = in.gf;

    // 1. Sem dados opcionais de cabeçalho: nada é inventado.
    const DemDynaComposition bare = composeDemDyna(in);
    CAD_CHECK(bare.ok && bare.errors.empty() && bare.lawsValidated && !bare.lawReport.blocksExport());
    CAD_CHECK(bare.nodes == 107 && bare.links == 490 && bare.masses == 107 && bare.laws == 51 && bare.curveOffset == 60);
    CAD_CHECK(bare.text.rfind("Finish\n/clear\n/title, DEM-DYNA\n/PREP7\nn, 1,0,0,0\n", 0) == 0);
    CAD_CHECK(bare.text.find("!seed") == std::string::npos && bare.text.find("/CWD") == std::string::npos
              && bare.text.find("Code generate") == std::string::npos && bare.text.find("CAMPO Gf") == std::string::npos);
    // A referência sem as linhas que dependem de dados externos (seed, data, /CWD, comentário do campo Gf).
    std::string refBare;
    for (std::size_t i = 0; i < refLines.size(); ++i)
    {
        const std::string& l = refLines[i];
        if (i == 0 || i == 3 || i == 4 || l == "!************************CAMPO Gf ***********"
            || l.rfind("! Rota", 0) == 0)
            continue;
        refBare += l + "\n";
    }
    // A linha vazia antes de "CAMPO Gf" também pertence ao comentário de GF_field.m.
    const std::string blankGf = "E,107\n\n\n!************************INICIO DA LEI 1";
    const std::size_t pos = refBare.find(blankGf);
    if (pos != std::string::npos)
        refBare.erase(pos + 6, 1);
    CAD_CHECK(bare.text == refBare);

    // 2. Com os dados de cabeçalho e do campo Gf da própria referência: arquivo idêntico byte a byte.
    in.header.seed = 10.0;
    in.header.timestamp = refLines[3].substr(std::string("!Code generate on ").size());
    in.header.workingDirectory = refLines[4].substr(6, refLines[4].size() - 7);
    in.gfField = DemDynaGfFieldNote{{0.0, 0.0, 45.0}};
    const DemDynaComposition full = composeDemDyna(in);
    CAD_CHECK(full.ok);
    const bool identical = full.text == ref;
    CAD_CHECK(identical && full.text.size() == 58181);
    if (!identical)
    {
        const std::vector<std::string> a = lines(full.text);
        for (std::size_t i = 0; i < a.size() && i < refLines.size(); ++i)
            if (a[i] != refLines[i])
            {
                std::printf("  primeira diferenca na linha %zu:\n    C++:    %s\n    MATLAB: %s\n", i + 1, a[i].c_str(),
                            refLines[i].c_str());
                break;
            }
    }

    // 3. Seção a seção (diagnóstico detalhado da equivalência).
    const bool nodesSame = linesStartingWith(full.text, "n, ") == linesStartingWith(ref, "n, ");
    const bool elemsSame = linesStartingWith(full.text, "EN, ") == linesStartingWith(ref, "EN, ")
                           && linesStartingWith(full.text, "ET,") == linesStartingWith(ref, "ET,");
    const std::string massMark = "!************************NODAL DENSITY***********\n";
    const bool massSame = between(full.text, massMark, "!************************CAMPO Gf")
                          == between(ref, massMark, "!************************CAMPO Gf");
    const bool lawsSame = between(full.text, "!************************INICIO DA LEI 1\n", "! ***********************DEFINE")
                          == refLaws;
    const bool emodifSame = between(full.text, "! ***********************DEFINE", "ESEL,S,TYPE,,1\n") == refEmodif;
    CAD_CHECK(nodesSame && elemsSame && massSame && lawsSame && emodifSame && !refLaws.empty() && !refEmodif.empty());
    const std::string refNodes = linesStartingWith(ref, "n, "), refEn = linesStartingWith(ref, "EN, ");
    CAD_CHECK(lines(refNodes).size() == 107 && lines(refEn).size() == 490);
    CAD_CHECK(lines(linesStartingWith(full.text, "E,")).size() == 107 && lines(linesStartingWith(full.text, "EMODIF,")).size() == 490);
    // Cada EN do texto composto corresponde à sequência ApdlLinkSequence da discretização (mesma
    // conectividade usada pelo exportador geométrico).
    bool enExact = true;
    std::int64_t idx = 0;
    ApdlLinkSequence seq(d);
    LinkElement e;
    while (seq.next(e))
    {
        ++idx;
        enExact = enExact
                  && full.text.find("\nEN, " + std::to_string(e.id) + "," + std::to_string(e.n1) + ","
                                    + std::to_string(e.n2) + "\n")
                         != std::string::npos;
    }
    CAD_CHECK(enExact && idx == 490);
    CAD_CHECK(in.gf == gfCopy); // entradas inalteradas
    std::printf("  leis = %d  jj = %d  barras = %lld  nos = %lld  massas = %lld  diagnosticos = %zu\n", full.laws,
                full.curveOffset, (long long)full.links, (long long)full.nodes, (long long)full.masses,
                full.lawReport.issues.size());
    std::printf("  secoes: nos %s, ET/EN %s, massas %s, leis %s, EMODIF %s\n", nodesSame ? "identicos" : "DIFERENTES",
                elemsSame ? "identicos" : "DIFERENTES", massSame ? "identicas" : "DIFERENTES",
                lawsSame ? "identicas" : "DIFERENTES", emodifSame ? "identico" : "DIFERENTE");
    std::printf("  arquivo completo (%zu bytes): %s; sem dados opcionais de cabecalho: %s\n", full.text.size(),
                identical ? "identico byte a byte ao DEM_DYNA.txt" : "DIFERENTE",
                bare.text == refBare ? "identico a referencia sem seed, data, /CWD e comentario do campo Gf" : "DIFERENTE");
}

// ------------------------------------------------------------------ [B]
struct RefLaw
{
    int id = 0, tp = 0;
    long long firstBond = 0;
    double gf = 0, li = 0, kr = 0, eep = 0, fp = 0, eeu = 0;
};

void testCvgfZero(const char* refPath)
{
    std::printf("[B] CVgf = 0: Gf = 1 explicito x lawnumbers.m (caixa_g1, MATLAB)\n");
    // Caso caixa_g1 de gerar_ref_e574b5.m: lawnumbers.m com Gf = ones(1, nBonds) (GF_field.m l. 278).
    std::ifstream in(refPath);
    CAD_CHECK(bool(in)); // referência obrigatória
    std::string line;
    bool inCase = false;
    std::vector<int> refMat;
    std::vector<RefLaw> refLaws;
    int refK = 0, refJj = 0;
    while (std::getline(in, line))
    {
        std::istringstream s(line);
        std::string tag;
        s >> tag;
        if (tag == "CASE")
        {
            std::string name;
            long long bonds = 0;
            int k = 0, jj = 0;
            s >> name >> bonds >> k >> jj;
            if (!refLaws.empty())
                break; // fim do caso caixa_g1
            inCase = name == "caixa_g1" && bonds == 490;
            if (inCase)
            {
                refK = k;
                refJj = jj;
            }
        }
        else if (inCase && tag == "MAT")
        {
            int v;
            while (s >> v)
                refMat.push_back(v);
        }
        else if (inCase && tag == "LAW")
        {
            RefLaw w;
            s >> w.id >> w.tp >> w.firstBond >> w.gf >> w.li >> w.kr >> w.eep >> w.fp >> w.eeu;
            refLaws.push_back(w);
        }
    }
    CAD_CHECK(refMat.size() == 490 && refLaws.size() == 4 && refK == 4 && refJj == 10);
    if (refMat.size() != 490 || refLaws.size() != 4)
        return;

    const Discretization d = build(caixa());
    DemDynaInput input;
    input.discretization = &d;
    input.properties = dem::matlabDefaults();
    input.gf.assign(490, 1.0); // CVgf = 0: Gf = 1 fornecido explicitamente pelo caso de teste
    const DemDynaComposition r = composeDemDyna(input);
    CAD_CHECK(r.ok && r.laws == refK && r.curveOffset == refJj && r.links == 490);
    // Associações iguais às do MATLAB.
    bool mat = true;
    for (std::size_t b = 0; b < 490; ++b)
        mat = mat && r.text.find("EMODIF," + matlabF(double(b + 1)) + ",MAT," + matlabF(double(refMat[b])) + ", \n")
                         != std::string::npos;
    CAD_CHECK(mat);
    // Curvas e TBDAT com os valores calculados pelo MATLAB (mesmas expressões de lawnumbers.m l. 88-106).
    int lawsOk = 0;
    for (const RefLaw& w : refLaws)
    {
        const std::string j = matlabG(w.id), ju = matlabG(w.id + refJj);
        const auto pts = [](double a, double b, double c, double e) {
            return matlabG(a) + ",0," + matlabG(b) + "," + matlabG(c) + "," + matlabG(e);
        };
        const double eep = w.eep, li = w.li, kr = w.kr, fp = w.fp;
        const std::string expected =
            "f1" + j + "(1)= " + pts(-fp, fp, fp / 100.0, fp / 100.0) + "\n" + "d1" + j + "(1)= "
            + pts(-eep * li, eep * li, eep * kr * li, 2.0 * (eep * kr * li)) + "\n" + "EDCURVE,ADD," + j + ",d1" + j
            + "(1),f1" + j + "(1)\n";
        const std::string expectedUnload = "d2" + j + "(1)= "
                                           + pts(-li * eep, li * eep, li * kr * eep, 100.0 * li * kr * eep) + "\n"
                                           + "EDCURVE,ADD," + ju + ",d2" + j + "(1),f2" + j + "(1)\n";
        const std::string tb = "TBDAT,4," + matlabG(fp) + " !TYI \nTBDAT,5," + matlabG(-fp) + " !CYI \n";
        const bool ok = r.text.find(expected) != std::string::npos && r.text.find(expectedUnload) != std::string::npos
                        && r.text.find("MAT," + j + "\nTB,DISCRETE," + j + ",,,5\n") != std::string::npos
                        && r.text.find(tb) != std::string::npos;
        lawsOk += ok ? 1 : 0;
    }
    CAD_CHECK(lawsOk == 4);
    // Sem Gf, nenhuma composição: Gf = 1 nunca é implícito.
    DemDynaInput noGf = input;
    noGf.gf.clear();
    const DemDynaComposition rn = composeDemDyna(noGf);
    CAD_CHECK(refused(rn) && hasStage(rn, "Gf"));
    std::printf("  leis = %d (MATLAB %d)  jj = %d  EMODIF 490/490 %s  curvas e TBDAT iguais ao MATLAB: %d/4\n", r.laws,
                refK, r.curveOffset, mat ? "iguais" : "DIFERENTES", lawsOk);
    showErrors("Gf ausente", rn);
}

// ------------------------------------------------------------------ [C]
void testInvalidLaw()
{
    std::printf("[C] lei invalida: composicao bloqueada\n");
    const Discretization d = build(ele());
    DemDynaInput in;
    in.discretization = &d;
    in.properties = dem::matlabDefaults();
    in.gf.assign(std::size_t(ApdlLinkSequence::count(d).total()), 1.0);
    const DemDynaInput copy = in;
    const DemDynaComposition r = composeDemDyna(in);
    CAD_CHECK(refused(r) && r.lawsValidated && r.lawReport.blocksExport() && hasStage(r, "leis"));
    // Diagnóstico preservado: lei tp = 3 com barra ativa (InvalidExported), elemento e lei identificados.
    const dem::LawIssue* tp3 = nullptr;
    for (const dem::LawIssue& i : r.lawReport.issues)
        if (i.kind == dem::LawIssueKind::InvalidExported && i.tp == 3 && i.parameter == "FP")
            tp3 = &i;
    CAD_CHECK(tp3 != nullptr && tp3->lawId > 0 && tp3->elementId > 0);
    CAD_CHECK(in.gf == copy.gf && in.discretization == copy.discretization);
    showErrors("tp = 3 em barra ativa", r);
    if (tp3)
        std::printf("    %s\n", dem::describe(*tp3).c_str());

    // tp = 3 somente em barras excluídas: os comandos da lei continuam no arquivo → InvalidEmitted.
    const LatticeTopology topo = classifyLattice(d);
    DemDynaInput onlyDeleted = in;
    onlyDeleted.deleted.assign(topo.linkTp.size(), false);
    for (std::size_t b = 0; b < topo.linkTp.size(); ++b)
        onlyDeleted.deleted[b] = topo.linkTp[b] == 3;
    const DemDynaComposition re = composeDemDyna(onlyDeleted);
    CAD_CHECK(refused(re) && re.lawReport.count(dem::LawIssueKind::InvalidEmitted) > 0
              && re.lawReport.count(dem::LawIssueKind::InvalidExported) == 0 && re.lawReport.blockedElements.empty());
    showErrors("tp = 3 somente em barras excluidas", re);
}

// ------------------------------------------------------------------ [D]
void testLargeIds()
{
    std::printf("[D] IDs grandes: inteiros exatos\n");
    CAD_CHECK(demDynaElementLine(1234567, 1000000, 1234567) == "EN, 1234567,1000000,1234567\n");
    CAD_CHECK(matlabG(1234567.0) == "1.23457e+06"); // o que o "%g" do MATLAB escreveria (= 1234570)
    CAD_CHECK(demDynaElementLine(999999, 999998, 999999) == "EN, 999999,999998,999999\n");
    CAD_CHECK(matlabG(999999.0) == "999999"); // abaixo de 10^6 o MATLAB é exato: texto idêntico
    const std::int64_t big = 9007199254740993LL; // 2^53 + 1: nem um double representa
    CAD_CHECK(demDynaElementLine(big, big - 1, big) == "EN, 9007199254740993,9007199254740992,9007199254740993\n");
    CAD_CHECK(demDynaNodeLine(big, {0.001, 0.0, 1e-7}) == "n, 9007199254740993,0.001,0,1e-07\n");
    // Releitura: nenhum arredondamento de conectividade em toda a faixa 999990..1000010.
    bool exact = true;
    for (std::int64_t i = 999990; i <= 1000010; ++i)
    {
        const std::string l = demDynaElementLine(i, i, i + 1);
        long long a = 0, b = 0, c = 0;
        exact = exact && std::sscanf(l.c_str(), "EN, %lld,%lld,%lld", &a, &b, &c) == 3 && a == i && b == i && c == i + 1;
    }
    CAD_CHECK(exact);
    std::printf("  EN, 1234567,1000000,1234567 (MATLAB %%g: 1.23457e+06); 2^53+1 exato; 999990..1000010 relidos exatos\n");
}

// ------------------------------------------------------------------ [E]
void testInconsistent()
{
    std::printf("[E] dados inconsistentes\n");
    const Discretization d = build(caixa());
    DemDynaInput in;
    in.discretization = &d;
    in.properties = dem::matlabDefaults();
    in.gf.assign(490, 1.0);
    CAD_CHECK(composeDemDyna(in).ok);

    DemDynaInput wrongGf = in; // Gf com tamanho incorreto
    wrongGf.gf.pop_back();
    const DemDynaComposition a = composeDemDyna(wrongGf);
    CAD_CHECK(refused(a) && hasStage(a, "Gf") && !a.lawsValidated);
    showErrors("Gf com 489 valores", a);

    DemDynaInput nanGf = in;
    nanGf.gf[10] = std::numeric_limits<double>::quiet_NaN();
    CAD_CHECK(refused(composeDemDyna(nanGf)) && hasStage(composeDemDyna(nanGf), "Gf", 11));

    // Elementos com referências inválidas, massas, nós e comprimentos inconsistentes não podem mais
    // ser fornecidos: são reconstruídos internamente a partir da discretização (ver [F]). As
    // inconsistências que ainda chegam pela entrada pública são recusadas abaixo; massas inválidas
    // chegam pela densidade.
    DemDynaInput badMass = in; // massa inválida: ROH não finito
    badMass.properties.material.density = std::numeric_limits<double>::infinity();
    const DemDynaComposition m = composeDemDyna(badMass);
    CAD_CHECK(refused(m) && hasStage(m, "propriedades"));
    showErrors("densidade infinita (massas invalidas)", m);
    DemDynaInput negMass = in;
    negMass.properties.material.density = -1240.0;
    CAD_CHECK(refused(composeDemDyna(negMass)) && hasStage(composeDemDyna(negMass), "propriedades"));

    const Discretization empty; // discretização vazia
    DemDynaInput e = in;
    e.discretization = &empty;
    const DemDynaComposition re = composeDemDyna(e);
    CAD_CHECK(refused(re) && hasStage(re, "entrada"));
    e.discretization = nullptr;
    CAD_CHECK(refused(composeDemDyna(e)) && hasStage(composeDemDyna(e), "entrada"));
    showErrors("discretizacao vazia", re);

    DemDynaInput badProps = in; // propriedades inválidas
    badProps.properties.material.density = 0.0;
    const DemDynaComposition rp = composeDemDyna(badProps);
    CAD_CHECK(refused(rp) && hasStage(rp, "propriedades"));
    showErrors("densidade nula", rp);

    DemDynaInput badHeader = in; // cabeçalho com quebra de linha
    badHeader.header.timestamp = std::string("10/11/2026\n/SOLU");
    CAD_CHECK(refused(composeDemDyna(badHeader)) && hasStage(composeDemDyna(badHeader), "cabeçalho"));

    DemDynaInput deleted = in; // exclusões sem EDELE validado (leis válidas)
    deleted.deleted.assign(490, false);
    deleted.deleted[99] = true;
    const DemDynaComposition rd = composeDemDyna(deleted);
    CAD_CHECK(refused(rd) && rd.lawsValidated && !rd.lawReport.blocksExport() && hasStage(rd, "exclusões", 100));
    showErrors("barra 100 excluida (EDELE sem referencia)", rd);
    DemDynaInput allFalse = in;
    allFalse.deleted.assign(490, false);
    CAD_CHECK(composeDemDyna(allFalse).ok && composeDemDyna(allFalse).text == composeDemDyna(in).text);
    DemDynaInput wrongDeleted = in;
    wrongDeleted.deleted.assign(10, false);
    CAD_CHECK(refused(composeDemDyna(wrongDeleted)) && hasStage(composeDemDyna(wrongDeleted), "exclusões"));
}

// ------------------------------------------------------------------ [F]
// A API pública não aceita modelo intermediário: uma única sobrecarga de composeDemDyna (o endereço
// de uma função sobrecarregada sem tipo de destino não compila) e uma entrada só com dados primários
// (a decomposição estruturada exige exatamente estes seis membros, com estes tipos).
static_assert(std::is_same_v<decltype(&composeDemDyna), DemDynaComposition (*)(const DemDynaInput&)>,
              "composeDemDyna deve ter uma única sobrecarga pública, sobre DemDynaInput");

void testPublicApi()
{
    std::printf("[F] API publica: somente composeDemDyna(const DemDynaInput&)\n");
    [[maybe_unused]] auto [disc, props, gf, deleted, header, gfField] = DemDynaInput{};
    static_assert(std::is_same_v<decltype(disc), const Discretization*>);
    static_assert(std::is_same_v<decltype(props), dem::DemProperties>);
    static_assert(std::is_same_v<decltype(gf), std::vector<double>>);
    static_assert(std::is_same_v<decltype(deleted), std::vector<bool>>);
    static_assert(std::is_same_v<decltype(header), DemDynaHeader>);
    static_assert(std::is_same_v<decltype(gfField), std::optional<DemDynaGfFieldNote>>);
    // Nós, barras, tp, Li, massas, constantes e leis não fazem parte da entrada: são sempre
    // reconstruídos. Duas composições da mesma entrada são idênticas (nada é reaproveitado de fora).
    const Discretization d = build(caixa());
    DemDynaInput in;
    in.discretization = &d;
    in.properties = dem::matlabDefaults();
    in.gf.assign(490, 1.0);
    const DemDynaComposition a = composeDemDyna(in), b = composeDemDyna(in);
    CAD_CHECK(a.ok && b.ok && a.text == b.text && a.nodes == 107 && a.links == 490 && a.masses == 107 && a.laws == 4);
    std::printf("  sobrecarga unica e entrada com 6 membros primarios (verificados em compilacao); composicao normal ok\n");
}

// ------------------------------------------------------------------ [G]
Discretization caixaEm(double originMm)
{
    VoxelGrid g = VoxelGrid::make(gp_XYZ(originMm, 0.0, 0.0), 1.0);
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

void testCoordinates()
{
    std::printf("[G] precisao das coordenadas escritas com %%g\n");
    // E/F/I: caixa 4 x 4 x 2 (lco = 1 mm) deslocada em X. Até 10 m da origem as coordenadas escritas
    // continuam distintas; a 100 m e 1000 m o %g (6 algarismos, resolução 1 mm a 100 m) colapsa nós.
    for (double origin : {0.0, 1000.0, 10000.0, 100000.0, 1000000.0})
    {
        const Discretization d = caixaEm(origin);
        DemDynaInput in;
        in.discretization = &d;
        in.properties = dem::matlabDefaults();
        in.gf.assign(490, 1.0);
        const DemDynaComposition r = composeDemDyna(in);
        // Mesma verificação sobre a geometria da discretização (nós e barras reconstruídos aqui).
        std::vector<std::array<double, 3>> nodes;
        for (std::int64_t n = 1; n <= d.totalNodeCount(); ++n)
        {
            const gp_XYZ p = d.node(n, LengthUnit::Meter).position;
            nodes.push_back({p.X(), p.Y(), p.Z()});
        }
        std::vector<std::array<std::int64_t, 2>> links;
        ApdlLinkSequence seq(d);
        LinkElement e;
        while (seq.next(e))
            links.push_back({e.n1, e.n2});
        const DemDynaCoordinateReport c = checkApdlCoordinates(nodes, links);
        if (origin <= 10000.0)
            CAD_CHECK(r.ok && c.ok() && c.collapsedNodes == 0 && c.degenerateBars == 0);
        else
        {
            CAD_CHECK(refused(r) && hasStage(r, "representação APDL") && !r.lawsValidated && !c.ok()
                      && c.collapsedNodes > 0);
            bool detailed = false;
            for (const DemDynaIssue& i : r.errors)
                detailed = detailed || (i.id > 0 && i.message.find("distintos na geometria") != std::string::npos
                                        && i.message.find("ficam iguais no APDL com %g") != std::string::npos);
            CAD_CHECK(detailed);
        }
        if (origin == 1000000.0)
            CAD_CHECK(c.degenerateBars > 0); // a 1000 m há barras com as duas extremidades iguais no texto
        std::printf("  origem x = %.0f mm: ok = %s, nos colapsados = %lld, barras degeneradas = %lld, texto = %zu bytes\n",
                    origin, r.ok ? "sim" : "nao", (long long)c.collapsedNodes, (long long)c.degenerateBars, r.text.size());
        if (origin == 100000.0)
            showErrors("origem a 100 m", r);
    }

    // G: nós já coincidentes na geometria original não são colapso de formatação.
    {
        const std::vector<std::array<double, 3>> nodes = {{0.001, 0.002, 0.0}, {0.001, 0.002, 0.0}, {0.002, 0.002, 0.0},
                                                          {-0.0, 0.0, 0.0}, {0.0, 0.0, 0.0}};
        const DemDynaCoordinateReport c = checkApdlCoordinates(nodes, {{1, 3}, {1, 2}});
        CAD_CHECK(c.ok() && c.coincidentOriginal == 2 && c.collapsedNodes == 0 && c.degenerateBars == 0);
        std::printf("  nos originalmente coincidentes (1 = 2; -0 = 0): %lld, nao tratados como colapso; ok = %s\n",
                    (long long)c.coincidentOriginal, c.ok() ? "sim" : "nao");
    }

    // H: barra com comprimento positivo e extremidades iguais no texto (100.0001 e 100.0002 → "100").
    {
        const std::vector<std::array<double, 3>> nodes = {{100.0001, 0.0, 0.0}, {100.0002, 0.0, 0.0}, {100.1, 0.0, 0.0}};
        const DemDynaCoordinateReport c = checkApdlCoordinates(nodes, {{1, 2}, {2, 3}});
        CAD_CHECK(!c.ok() && c.collapsedNodes == 1 && c.degenerateBars == 1);
        bool bar = false;
        for (const DemDynaIssue& i : c.issues)
            bar = bar
                  || (i.id == 1 && i.message.find("Barra EN 1 (nós 1 e 2)") != std::string::npos
                      && i.message.find("\"100,0,0\"") != std::string::npos
                      && i.message.find("100.0001") != std::string::npos);
        CAD_CHECK(bar);
        for (const DemDynaIssue& i : c.issues)
            std::printf("    [%s] %s\n", i.stage.c_str(), i.message.c_str());
        // Valores próximos da origem, porém mais próximos que 6 algarismos: o critério é exato.
        const DemDynaCoordinateReport near = checkApdlCoordinates({{0.1234561, 0, 0}, {0.1234564, 0, 0}}, {{1, 2}});
        CAD_CHECK(!near.ok() && near.degenerateBars == 1);
        // Subnormais distintos continuam distintos com %g; nenhum falso positivo.
        const DemDynaCoordinateReport tiny = checkApdlCoordinates({{1e-320, 0, 0}, {2e-320, 0, 0}}, {{1, 2}});
        CAD_CHECK(tiny.ok());
    }
}

} // namespace

int main()
{
    testDemDyna(CAD_MATLAB_REFERENCE_DIR "/DEM_DYNA.txt");
    testCvgfZero(CAD_E574B5_REFERENCE);
    testInvalidLaw();
    testLargeIds();
    testInconsistent();
    testPublicApi();
    testCoordinates();
    return cad::test::finish("test_e574b8");
}
