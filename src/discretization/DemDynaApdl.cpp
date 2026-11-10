#include "discretization/DemDynaApdl.h"

#include "dem/DemUnits.h"
#include "dem/LatticeConstants.h"
#include "dem/LawValidation.h" // validateLawTable, formatIssueValue
#include "dem/MatlabLawTable.h"
#include "discretization/ApdlExporter.h" // ApdlLinkSequence, linkLengths (somente leitura)
#include "discretization/LatticeTopology.h"
#include "discretization/MatlabLawApdl.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <numeric>
#include <utility>

namespace cad::discretization {

namespace {

// IDs de lei e de curva escritos com "%g" por MatlabLawApdl: exatos somente abaixo de 10^6.
constexpr std::int64_t kMatlabGExactLimit = 1000000;

std::string id(std::int64_t v)
{
    return std::to_string(v);
}

// Acumula um erro por verificação: primeiro elemento afetado e quantidade.
struct Counter
{
    std::int64_t first = 0;
    std::int64_t count = 0;
    void add(std::int64_t element)
    {
        if (count++ == 0)
            first = element;
    }
};

void report(std::vector<DemDynaIssue>& errors, const char* stage, const Counter& c, const std::string& what)
{
    if (c.count == 0)
        return;
    errors.push_back({stage, c.first,
                      what + " (" + id(c.count) + (c.count == 1 ? " ocorrência" : " ocorrências") + "; primeira: "
                          + id(c.first) + ")."});
}

bool textLine(const std::string& s)
{
    for (char ch : s)
        if (ch == '\n' || ch == '\r')
            return false;
    return true;
}

// Modelo intermediário (interno): tudo reconstruído da discretização e das propriedades por
// assembleModel; nunca recebido de fora.
struct DemDynaModel
{
    std::vector<std::array<double, 3>> nodes;       // [m]; índice = ID do nó − 1
    std::vector<std::array<std::int64_t, 2>> links; // (I, J); índice = ID do elemento − 1
    std::vector<std::int8_t> linkTp;                // classifyLattice
    std::vector<double> lengths;                    // Li [m] (linkLengths)
    NodalMasses masses;                             // computeNodalMasses
    dem::LatticeConstants lattice;                  // computeLattice
    std::vector<double> gf;
    std::vector<bool> deleted;
    DemDynaHeader header;
    std::optional<DemDynaGfFieldNote> gfField;
};

// Coordenada escrita na linha "n" (matlabG) e relida.
double writtenValue(double v)
{
    return std::strtod(matlabG(v).c_str(), nullptr);
}

std::string originalText(const std::array<double, 3>& p)
{
    return "(" + dem::formatIssueValue(p[0]) + ", " + dem::formatIssueValue(p[1]) + ", "
           + dem::formatIssueValue(p[2]) + ") m";
}

std::string writtenText(const std::array<double, 3>& p)
{
    return "\"" + matlabG(p[0]) + "," + matlabG(p[1]) + "," + matlabG(p[2]) + "\"";
}

constexpr std::size_t kMaxCoordinateDetails = 10; // detalhes por tipo; os totais vão no resumo

} // namespace

// --- Linhas ---------------------------------------------------------------------------------

std::string demDynaHeaderText(const DemDynaHeader& h)
{
    std::string s;
    if (h.seed)
        s += "!seed == " + matlabG(*h.seed) + "\n";       // DEM_HIAGO_PRINCIPAL.m l. 137
    s += "Finish\n";                                       // l. 139
    s += "/clear\n";                                       // l. 140
    if (h.timestamp)
        s += "!Code generate on " + *h.timestamp + "\n";   // l. 141 (texto fornecido)
    if (h.workingDirectory)
        s += "/CWD,'" + *h.workingDirectory + "'\n";       // l. 142 (opcional)
    s += "/title, DEM-DYNA\n";                             // l. 143
    s += "/PREP7\n";                                       // l. 146-147
    return s;
}

std::string demDynaNodeLine(std::int64_t nodeId, const std::array<double, 3>& p)
{
    // malha_por_mascara.m l. 66/78: 'n, %u,%g,%g,%g\n' (o ID já é inteiro exato no MATLAB).
    return "n, " + id(nodeId) + "," + matlabG(p[0]) + "," + matlabG(p[1]) + "," + matlabG(p[2]) + "\n";
}

std::string demDynaElementLine(std::int64_t elementId, std::int64_t i, std::int64_t j)
{
    // malha_por_mascara.m: 'EN, %u,%g,%g\n'. I e J como inteiros exatos (correção de segurança: "%g"
    // arredonda IDs ≥ 10^6); idêntico ao MATLAB para IDs < 10^6.
    return "EN, " + id(elementId) + "," + id(i) + "," + id(j) + "\n";
}

std::string demDynaGfFieldText(const DemDynaGfFieldNote& note)
{
    // GF_field.m l. 22-29.
    std::string s = "\n!************************CAMPO Gf ***********\n";
    const auto& r = note.rotationDegrees;
    if (r[0] != 0.0 || r[1] != 0.0 || r[2] != 0.0)
    {
        char buf[256];
        std::snprintf(buf, sizeof buf, "! Rota\xC3\xA7\xC3\xA3o: X=%.1f\xC2\xB0, Y=%.1f\xC2\xB0, Z=%.1f\xC2\xB0\n", r[0],
                      r[1], r[2]);
        s += buf;
    }
    return s;
}

// --- Representação APDL das coordenadas ------------------------------------------------------

DemDynaCoordinateReport checkApdlCoordinates(const std::vector<std::array<double, 3>>& nodes,
                                             const std::vector<std::array<std::int64_t, 2>>& links)
{
    DemDynaCoordinateReport r;
    const std::size_t n = nodes.size();
    std::vector<std::array<double, 3>> w(n);
    for (std::size_t i = 0; i < n; ++i)
    {
        bool finiteAll = true;
        for (int a = 0; a < 3; ++a)
        {
            w[i][std::size_t(a)] = writtenValue(nodes[i][std::size_t(a)]) + 0.0; // −0 escrito = 0 para o ANSYS
            finiteAll = finiteAll && std::isfinite(w[i][std::size_t(a)]);
        }
        if (!finiteAll)
        {
            if (r.nonFinite++ < std::int64_t(kMaxCoordinateDetails))
                r.issues.push_back({"representação APDL", std::int64_t(i) + 1,
                                    "Nó " + std::to_string(i + 1) + ": coordenada original " + originalText(nodes[i])
                                        + " escrita como " + writtenText(nodes[i])
                                        + ", valor não finito no APDL."});
        }
    }

    if (r.nonFinite > 0)
        return r; // sem ordem total entre valores não finitos: o agrupamento abaixo exige valores finitos

    // Nós agrupados pelas coordenadas escritas (ordem lexicográfica exata, sem tolerância).
    std::vector<std::size_t> order(n);
    std::iota(order.begin(), order.end(), std::size_t(0));
    std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
        return w[a] != w[b] ? w[a] < w[b] : a < b;
    });
    std::size_t detail = 0;
    for (std::size_t g = 0; g < n;)
    {
        std::size_t e = g + 1;
        while (e < n && w[order[e]] == w[order[g]])
            ++e;
        // Grupo [g, e): mesmas coordenadas escritas. Cada nó é comparado ao primeiro do grupo.
        const std::size_t first = order[g];
        for (std::size_t k = g + 1; k < e; ++k)
        {
            const std::size_t other = order[k];
            if (nodes[other] == nodes[first])
            {
                ++r.coincidentOriginal; // já coincidentes na geometria: não é colapso de formatação
                continue;
            }
            ++r.collapsedNodes;
            if (detail++ < kMaxCoordinateDetails)
                r.issues.push_back(
                    {"representação APDL", std::int64_t(other) + 1,
                     "Nós " + std::to_string(first + 1) + " e " + std::to_string(other + 1)
                         + " são distintos na geometria (" + originalText(nodes[first]) + " e "
                         + originalText(nodes[other]) + "), mas ficam iguais no APDL com %g (" + writtenText(nodes[first])
                         + "): colapso introduzido pela representação textual; nenhuma coordenada é corrigida."});
        }
        g = e;
    }

    // Barras: extremidades originais distintas e escritas iguais.
    std::size_t barDetail = 0;
    for (std::size_t b = 0; b < links.size(); ++b)
    {
        const std::int64_t i = links[b][0], j = links[b][1];
        if (i < 1 || j < 1 || i > std::int64_t(n) || j > std::int64_t(n))
            continue;
        const std::size_t a = std::size_t(i - 1), c = std::size_t(j - 1);
        if (nodes[a] != nodes[c] && w[a] == w[c])
        {
            ++r.degenerateBars;
            if (barDetail++ < kMaxCoordinateDetails)
                r.issues.push_back({"representação APDL", std::int64_t(b) + 1,
                                    "Barra EN " + std::to_string(b + 1) + " (nós " + std::to_string(i) + " e "
                                        + std::to_string(j) + ") tem comprimento positivo na geometria ("
                                        + originalText(nodes[a]) + " → " + originalText(nodes[c])
                                        + "), mas comprimento nulo no APDL (ambas as extremidades escritas como "
                                        + writtenText(nodes[a]) + ")."});
        }
    }
    return r;
}

namespace {

// --- Montagem (interna) ---------------------------------------------------------------------

DemDynaModel assembleModel(const DemDynaInput& in, std::vector<DemDynaIssue>& errors)
{
    DemDynaModel m;
    m.gf = in.gf;
    m.deleted = in.deleted;
    m.header = in.header;
    m.gfField = in.gfField;

    const Discretization* d = in.discretization;
    if (d == nullptr || d->empty())
    {
        errors.push_back({"entrada", 0, "Discretização ausente ou vazia: não há nós nem barras para compor."});
        return m;
    }
    const double lcoMeters = dem::units::mmToM(d->grid().lco);
    if (const dem::PropertyValidation v = dem::validateLattice(in.properties, lcoMeters); !v.ok)
    {
        errors.push_back({"propriedades", 0, "Propriedades MED ou LCO inválidos (" + v.field + "): " + v.message});
        return m;
    }
    m.lattice = dem::computeLattice(in.properties, lcoMeters);

    const LatticeTopology topology = classifyLattice(*d);
    if (!topology.ok)
    {
        errors.push_back({"rede", 0, "Classificação da rede (tp) recusada: " + topology.message});
        return m;
    }
    m.linkTp = topology.linkTp;
    m.masses = computeNodalMasses(*d, topology, in.properties.material.density);
    if (!m.masses.ok)
    {
        errors.push_back({"massas", 0, "Massas nodais recusadas: " + m.masses.message});
        return m;
    }

    const std::int64_t total = d->totalNodeCount();
    m.nodes.reserve(std::size_t(total));
    for (std::int64_t n = 1; n <= total; ++n)
    {
        const gp_XYZ p = d->node(n, LengthUnit::Meter).position;
        m.nodes.push_back({p.X(), p.Y(), p.Z()});
    }
    m.links.reserve(std::size_t(ApdlLinkSequence::count(*d).total()));
    ApdlLinkSequence seq(*d);
    LinkElement e;
    while (seq.next(e))
        m.links.push_back({e.n1, e.n2});
    m.lengths = linkLengths(*d, LengthUnit::Meter);
    return m;
}

// --- Composição (interna) -------------------------------------------------------------------

DemDynaComposition composeModel(const DemDynaModel& m)
{
    DemDynaComposition r;
    auto& errors = r.errors;
    const std::int64_t nNodes = std::int64_t(m.nodes.size());
    const std::int64_t nLinks = std::int64_t(m.links.size());
    r.nodes = nNodes;
    r.links = nLinks;
    r.masses = std::int64_t(m.masses.mass.size());

    // Nós.
    if (nNodes == 0)
        errors.push_back({"nós", 0, "Nenhum nó no modelo."});
    Counter badCoord;
    for (std::int64_t n = 0; n < nNodes; ++n)
        for (double v : m.nodes[std::size_t(n)])
            if (!std::isfinite(v))
            {
                badCoord.add(n + 1);
                break;
            }
    report(errors, "nós", badCoord, "Coordenada de nó não finita");

    // Conectividade.
    if (nLinks == 0)
        errors.push_back({"conectividade", 0, "Nenhuma barra (EN) no modelo."});
    Counter badRef, selfLink;
    for (std::int64_t b = 0; b < nLinks; ++b)
    {
        const auto& l = m.links[std::size_t(b)];
        if (l[0] < 1 || l[0] > nNodes || l[1] < 1 || l[1] > nNodes)
            badRef.add(b + 1);
        else if (l[0] == l[1])
            selfLink.add(b + 1);
    }
    report(errors, "conectividade", badRef, "Barra referencia nó inexistente (fora de 1.." + id(nNodes) + ")");
    report(errors, "conectividade", selfLink, "Barra com os dois nós iguais");

    // Vetores por barra.
    const auto sizeCheck = [&](const char* stage, std::size_t size, const char* name) {
        if (std::int64_t(size) != nLinks)
            errors.push_back({stage, 0,
                              std::string(name) + " tem " + id(std::int64_t(size)) + " valores para " + id(nLinks)
                                  + " barras."});
    };
    sizeCheck("rede", m.linkTp.size(), "O vetor tp");
    sizeCheck("comprimentos", m.lengths.size(), "O vetor de comprimentos Li");
    sizeCheck("Gf", m.gf.size(), "O vetor Gf (obrigatório, um valor por barra)");
    if (!m.deleted.empty())
        sizeCheck("exclusões", m.deleted.size(), "O vetor de barras excluídas");
    Counter badLength;
    for (std::size_t b = 0; b < m.lengths.size(); ++b)
        if (!(std::isfinite(m.lengths[b]) && m.lengths[b] > 0.0))
            badLength.add(std::int64_t(b) + 1);
    report(errors, "comprimentos", badLength, "Comprimento Li não finito ou não positivo");
    Counter badGf;
    for (std::size_t b = 0; b < m.gf.size(); ++b)
        if (!std::isfinite(m.gf[b]))
            badGf.add(std::int64_t(b) + 1);
    report(errors, "Gf", badGf, "Gf não finito");

    // Massas: uma por nó, finitas e positivas; LCO igual ao da rede.
    if (!m.masses.ok)
        errors.push_back({"massas", 0, "Massas nodais ausentes ou recusadas: " + m.masses.message});
    if (std::int64_t(m.masses.mass.size()) != nNodes)
        errors.push_back({"massas", 0,
                          "Há " + id(std::int64_t(m.masses.mass.size())) + " massas para " + id(nNodes) + " nós."});
    Counter badMass;
    for (std::size_t n = 0; n < m.masses.mass.size(); ++n)
        if (!(std::isfinite(m.masses.mass[n]) && m.masses.mass[n] > 0.0))
            badMass.add(std::int64_t(n) + 1);
    report(errors, "massas", badMass, "Massa nodal não finita ou não positiva");
    if (m.masses.ok && m.masses.lcoMeters != m.lattice.lco)
        errors.push_back({"massas", 0, "LCO das massas diferente do LCO da rede."});

    // Constantes da rede (EDG pode ser ≤ 0 com ν ≤ 0, como no MATLAB: a validação das leis decide).
    const double positive[] = {m.lattice.lco, m.lattice.lcr, m.lattice.epr, m.lattice.enr};
    bool latticeOk = std::isfinite(m.lattice.edg);
    for (double v : positive)
        latticeOk = latticeOk && std::isfinite(v) && v > 0.0;
    if (!latticeOk)
        errors.push_back({"rede", 0, "Constantes da rede (LCO, LCR, EPR, ENR, EDG) ausentes ou não finitas."});

    // Cabeçalho: cada texto fornecido ocupa uma única linha.
    if (m.header.seed && !std::isfinite(*m.header.seed))
        errors.push_back({"cabeçalho", 0, "seed não finito."});
    if (m.header.timestamp && !textLine(*m.header.timestamp))
        errors.push_back({"cabeçalho", 0, "Data/hora com quebra de linha."});
    if (m.header.workingDirectory
        && (!textLine(*m.header.workingDirectory) || m.header.workingDirectory->find('\'') != std::string::npos))
        errors.push_back({"cabeçalho", 0, "Diretório /CWD com quebra de linha ou apóstrofo."});
    if (m.gfField)
        for (double v : m.gfField->rotationDegrees)
            if (!std::isfinite(v))
            {
                errors.push_back({"cabeçalho", 0, "Rotação do comentário do campo Gf não finita."});
                break;
            }

    if (!errors.empty())
        return r;

    // Representação APDL das coordenadas: nenhum colapso introduzido pelo "%g".
    const DemDynaCoordinateReport coords = checkApdlCoordinates(m.nodes, m.links);
    if (!coords.ok())
    {
        errors.push_back({"representação APDL", 0,
                          "A formatação %g das coordenadas (6 algarismos, como no MATLAB) degenera a geometria no "
                          "APDL: "
                              + id(coords.collapsedNodes) + " nó(s) colapsado(s) sobre outro nó distinto, "
                              + id(coords.degenerateBars) + " barra(s) com comprimento nulo, " + id(coords.nonFinite)
                              + " nó(s) com coordenada não finita. A falha está na representação textual, não na "
                                "geometria; nenhuma coordenada é corrigida."});
        errors.insert(errors.end(), coords.issues.begin(), coords.issues.end());
        return r;
    }

    // Tabela de leis (lawnumbers.m) e validação para exportação.
    const dem::MatlabLawTable table = dem::buildMatlabLawTable(m.lattice, m.linkTp, m.lengths, m.gf);
    if (!table.ok)
    {
        errors.push_back({"leis", 0, "Tabela de leis recusada: " + table.message});
        return r;
    }
    r.laws = table.lawCount();
    r.curveOffset = table.curveOffset;
    r.lawReport = dem::validateLawTable(table, m.deleted);
    r.lawsValidated = true;
    if (r.lawReport.blocksExport())
    {
        errors.push_back({"leis", 0,
                          "Validação das leis bloqueia a exportação (" + id(std::int64_t(r.lawReport.invalidLaws.size()))
                              + " lei(s) inválida(s), " + id(std::int64_t(r.lawReport.blockedElements.size()))
                              + " barra(s) ativa(s) afetada(s)); nenhuma lei é corrigida."});
        return r;
    }
    if (std::int64_t(table.lawCount()) + table.curveOffset >= kMatlabGExactLimit)
    {
        errors.push_back({"leis", 0,
                          "IDs de lei/curva ≥ 10^6 não são exatos no formato %g dos comandos MATLAB das leis."});
        return r;
    }
    Counter excluded;
    for (std::size_t b = 0; b < m.deleted.size(); ++b)
        if (m.deleted[b])
            excluded.add(std::int64_t(b) + 1);
    report(errors, "exclusões", excluded,
           "Barras excluídas fornecidas, mas o bloco EDELE não tem comportamento de referência validado "
           "(buildcrack.m não transcrito); composição recusada");
    if (!errors.empty())
        return r;

    // Composição na ordem do MATLAB.
    std::string s;
    s.reserve(std::size_t(nNodes) * 90 + std::size_t(nLinks) * 70 + std::size_t(table.lawCount()) * 640 + 4096);
    s += demDynaHeaderText(m.header);
    for (std::int64_t n = 0; n < nNodes; ++n)
        s += demDynaNodeLine(n + 1, m.nodes[std::size_t(n)]);
    s += "ET,1,165\n";
    for (std::int64_t b = 0; b < nLinks; ++b)
        s += demDynaElementLine(b + 1, m.links[std::size_t(b)][0], m.links[std::size_t(b)][1]);
    s += matlabMassBlock(m.masses, nLinks);
    if (m.gfField)
        s += demDynaGfFieldText(*m.gfField);
    s += matlabLawBlock(table);
    s += matlabEmodifBlock(table);
    s += "ESEL,S,TYPE,,1\n"; // DEM_HIAGO_PRINCIPAL.m l. 181
    s += "EPLOT\n";          // l. 182
    r.text = std::move(s);
    r.ok = true;
    return r;
}

} // namespace

DemDynaComposition composeDemDyna(const DemDynaInput& input)
{
    std::vector<DemDynaIssue> errors;
    const DemDynaModel model = assembleModel(input, errors);
    if (!errors.empty())
    {
        DemDynaComposition r;
        r.errors = std::move(errors);
        return r;
    }
    return composeModel(model);
}

} // namespace cad::discretization
