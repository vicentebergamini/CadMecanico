#include "discretization/ApdlExporter.h"

#include "discretization/NodeAsciiExporter.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iterator>
#include <sstream>
#include <system_error>
#include <vector>

namespace cad::discretization {

namespace {

ApdlExportResult fail(ApdlExportResult r, std::string message)
{
    r.ok = false;
    r.message = std::move(message);
    return r;
}

ApdlFileCheck failCheck(ApdlFileCheck c, std::string message)
{
    c.ok = false;
    c.message = std::move(message);
    return c;
}

void appendInt(std::string& out, std::int64_t v)
{
    char buf[32];
    const auto res = std::to_chars(buf, buf + sizeof(buf), v);
    out.append(buf, res.ptr);
}

std::string toString(std::int64_t v)
{
    std::string s;
    appendInt(s, v);
    return s;
}

// Texto somente ASCII (comentários APDL): cada caractere não ASCII (sequência UTF-8) vira '_'.
std::string asciiOnly(const std::string& text)
{
    std::string out;
    for (std::size_t n = 0; n < text.size(); ++n)
    {
        const unsigned char c = static_cast<unsigned char>(text[n]);
        if (c < 0x80)
        {
            out.push_back(c < 0x20 ? ' ' : char(c));
            continue;
        }
        out.push_back('_');
        while (n + 1 < text.size() && (static_cast<unsigned char>(text[n + 1]) & 0xC0) == 0x80)
            ++n;
    }
    return out;
}

std::string upper(std::string s)
{
    for (char& c : s)
        if (c >= 'a' && c <= 'z')
            c = char(c - 'a' + 'A');
    return s;
}

std::string trim(const std::string& s)
{
    std::size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r'))
        ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r'))
        --e;
    return s.substr(b, e - b);
}

// Campos de um comando APDL separados por vírgula (sem o comentário final "!...").
std::vector<std::string> fields(const std::string& line)
{
    std::string body = line;
    const std::size_t bang = body.find('!');
    if (bang != std::string::npos)
        body.resize(bang);
    std::vector<std::string> out;
    std::size_t start = 0;
    while (true)
    {
        const std::size_t comma = body.find(',', start);
        out.push_back(trim(body.substr(start, comma == std::string::npos ? std::string::npos : comma - start)));
        if (comma == std::string::npos)
            break;
        start = comma + 1;
    }
    return out;
}

bool parseInt(const std::string& s, std::int64_t& v)
{
    if (s.empty())
        return false;
    const auto res = std::from_chars(s.data(), s.data() + s.size(), v);
    if (res.ec == std::errc() && res.ptr == s.data() + s.size())
        return true;
    // Inteiros escritos como real ("782.000000"), comuns em arquivos APDL gerados por scripts.
    double d = 0.0;
    const auto rd = std::from_chars(s.data(), s.data() + s.size(), d);
    if (rd.ec != std::errc() || rd.ptr != s.data() + s.size() || d != std::floor(d) || std::fabs(d) > 9.0e15)
        return false;
    v = std::int64_t(d);
    return true;
}

bool parseReal(const std::string& s, double& v)
{
    if (s.empty())
    {
        v = 0.0; // campo vazio no APDL = 0
        return true;
    }
    const auto res = std::from_chars(s.data(), s.data() + s.size(), v);
    return res.ec == std::errc() && res.ptr == s.data() + s.size() && std::isfinite(v);
}

bool sameReal(double a, double b)
{
    return std::fabs(a - b) <= 1.0e-12 * std::max({1.0e-3, std::fabs(a), std::fabs(b)});
}

std::filesystem::path temporaryPath(const std::filesystem::path& file)
{
    std::filesystem::path tmp = file;
    tmp += ".parcial.tmp";
    return tmp;
}

// Comandos de solução/carregamento que nunca devem aparecer no arquivo exportado.
bool isForbiddenCommand(const std::string& command)
{
    static const char* const kForbidden[] = {"/SOLU", "SOLVE", "D", "F", "SF", "BF", "IC", "EDLOAD", "EDWRITE",
                                             "EDSOLV", "MP", "TB", "TBDAT", "R", "MAT", "EMODIF", "EDCURVE",
                                             "TIME", "ANTYPE"};
    for (const char* f : kForbidden)
        if (command == f)
            return true;
    return false;
}

} // namespace

const char* linkGroupLabel(LinkGroup group)
{
    switch (group)
    {
    case LinkGroup::VertexEdge: return "vertice-vertice";
    case LinkGroup::CenterCenter: return "centro-centro";
    case LinkGroup::CenterVertex: return "centro-vertice";
    }
    return "";
}

// --- Sequência das ligações -------------------------------------------------------

bool ApdlLinkSequence::next(LinkElement& out)
{
    const VoxelGrid& g = m_data.grid();
    // Grupo A: arestas X, Y e Z, cada direção em ordem crescente do ID do primeiro vértice.
    while (m_stage < 3)
    {
        const int axis = m_stage;
        const std::vector<std::int64_t>& points = m_data.vertexPoints();
        while (m_index < std::int64_t(points.size()))
        {
            int p[3];
            g.latticeIjk(points[std::size_t(m_index)], p[0], p[1], p[2]);
            ++m_index;
            // A aresta pertence às (até) quatro células com o mesmo índice no eixo da aresta e
            // índices p ou p - 1 nos outros dois eixos; basta uma delas estar aceita.
            const int a = (axis + 1) % 3, b = (axis + 2) % 3;
            bool inCube = false;
            for (int da = 0; da <= 1 && !inCube; ++da)
                for (int db = 0; db <= 1 && !inCube; ++db)
                {
                    int c[3] = {p[0], p[1], p[2]};
                    c[a] -= da;
                    c[b] -= db;
                    inCube = m_data.cellAccepted(c[0], c[1], c[2]);
                }
            if (!inCube)
                continue;
            int q[3] = {p[0], p[1], p[2]};
            ++q[axis];
            out.id = m_nextId++;
            out.n1 = m_data.vertexId(p[0], p[1], p[2]);
            out.n2 = m_data.vertexId(q[0], q[1], q[2]);
            out.group = LinkGroup::VertexEdge;
            out.axis = axis;
            return true;
        }
        ++m_stage;
        m_index = 0;
    }
    // Grupos B e C intercalados por cubo, na ordem dos IDs dos centros.
    if (m_stage == 3)
    {
        const std::vector<std::int64_t>& cubes = m_data.cubeCells();
        while (m_index < std::int64_t(cubes.size()))
        {
            int i, j, k;
            g.cellIjk(cubes[std::size_t(m_index)], i, j, k);
            const std::int64_t center = m_data.vertexNodeCount() + m_index + 1;
            while (m_step < 11)
            {
                const int step = m_step++;
                if (step < 3)
                {
                    const std::int64_t neighbor =
                        m_data.cubeId(i + (step == 0 ? 1 : 0), j + (step == 1 ? 1 : 0), k + (step == 2 ? 1 : 0));
                    if (neighbor == 0)
                        continue; // vazio, cavidade ou fora da grade: sem ligação
                    out.id = m_nextId++;
                    out.n1 = center;
                    out.n2 = m_data.vertexNodeCount() + neighbor;
                    out.group = LinkGroup::CenterCenter;
                    out.axis = step;
                    return true;
                }
                const int v = step - 3; // (dx, dy, dz), dz variando mais rapidamente
                out.id = m_nextId++;
                out.n1 = center;
                out.n2 = m_data.vertexId(i + ((v >> 2) & 1), j + ((v >> 1) & 1), k + (v & 1));
                out.group = LinkGroup::CenterVertex;
                out.axis = -1;
                return true;
            }
            m_step = 0;
            ++m_index;
        }
        m_stage = 4;
    }
    return false;
}

LinkCounts ApdlLinkSequence::count(const Discretization& data)
{
    LinkCounts counts;
    ApdlLinkSequence seq(data);
    LinkElement e;
    while (seq.next(e))
    {
        switch (e.group)
        {
        case LinkGroup::VertexEdge: ++counts.vertexEdges; break;
        case LinkGroup::CenterCenter: ++counts.centerCenter; break;
        case LinkGroup::CenterVertex: ++counts.centerVertex; break;
        }
    }
    return counts;
}

// --- Exportação -------------------------------------------------------------------

std::string ApdlExporter::suggestedFileName(const std::string& partName)
{
    std::string name = NodeAsciiExporter::suggestedFileName(partName); // "<base>_nodes.txt"
    name.resize(name.size() - std::strlen("_nodes.txt"));
    return name + "_PREP7.inp";
}

std::string ApdlExporter::currentTimestamp()
{
    const std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_s(&local, &now);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%d/%m/%Y, %H:%M:%S", &local);
    return buf;
}

ApdlExportResult ApdlExporter::write(const Discretization& data, const std::filesystem::path& file,
                                     const ApdlOptions& options, bool overwrite) const
{
    ApdlExportResult r;
    const auto t0 = std::chrono::steady_clock::now();
    if (data.empty())
        return fail(r, "Não há nós para exportar (discretização vazia)");
    if (file.empty() || !file.has_filename())
        return fail(r, "Caminho de arquivo inválido");
    std::error_code ec;
    const std::filesystem::path dir = file.has_parent_path() ? file.parent_path() : std::filesystem::current_path(ec);
    if (!std::filesystem::is_directory(dir, ec))
        return fail(r, "Diretório inexistente: " + dir.u8string());
    if (std::filesystem::is_directory(file, ec))
        return fail(r, "O caminho indicado é um diretório: " + file.u8string());
    if (std::filesystem::exists(file, ec) && !overwrite)
        return fail(r, "O arquivo já existe e não foi autorizada a substituição: " + file.u8string());

    ApdlOptions opts = options;
    if (opts.timestamp.empty())
        opts.timestamp = currentTimestamp();
    const LinkCounts counts = ApdlLinkSequence::count(data);
    const std::string unitText = opts.unit == LengthUnit::Meter ? "metros (m)" : "milimetros (mm)";

    const std::filesystem::path tmp = temporaryPath(file);
    auto removeTmp = [&tmp] {
        std::error_code e;
        std::filesystem::remove(tmp, e);
    };
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out)
            return fail(r, "Não foi possível criar o arquivo temporário (verifique permissões): " + tmp.u8string());
        std::string buffer;
        buffer.reserve(1 << 20);
        auto flushIfFull = [&] {
            if (buffer.size() > (1u << 20) - 256)
            {
                out.write(buffer.data(), std::streamsize(buffer.size()));
                buffer.clear();
            }
        };

        // Cabeçalho (estrutura do exemplo DEM_DYNA.txt).
        buffer += "!seed == 10\n";
        buffer += "FINISH\n";
        if (opts.includeClear)
            buffer += "/CLEAR\n";
        else
            buffer += "! /CLEAR omitido por opcao do usuario: o banco de dados corrente do ANSYS e mantido\n";
        buffer += "!Code generate on " + opts.timestamp + "\n";
        buffer += "/TITLE, DEM-DYNA\n";
        buffer += "/PREP7\n";
        // Comentários informativos (ignorados pelo ANSYS).
        buffer += "! CAD Academico - E5.7.2.2: geometria discretizada (somente /PREP7; sem materiais,\n";
        buffer += "! propriedades, carregamentos, condicoes de contorno ou comandos de solucao)\n";
        if (!opts.partName.empty())
            buffer += "! Peca: " + asciiOnly(opts.partName) + "\n";
        buffer += "! Unidade das coordenadas: " + unitText
                  + ". O APDL nao impoe um sistema de unidades: os dados\n";
        buffer += "! acrescentados depois (materiais, cargas) devem usar unidades coerentes com esta.\n";
        buffer += "! lco = " + NodeAsciiExporter::formatCoordinate(data.grid().lco) + " mm; cubos = "
                  + toString(data.cubeCount()) + "\n";
        buffer += "! Nos: " + toString(data.totalNodeCount()) + " (1-" + toString(data.vertexNodeCount())
                  + " vertices; " + toString(data.vertexNodeCount() + 1) + "-" + toString(data.totalNodeCount())
                  + " centrais), ordem Z -> Y -> X\n";
        buffer += "! Elementos ET,1,165 (COMBI165, dois nos): " + toString(counts.total()) + "\n";
        buffer += "!   " + toString(counts.vertexEdges) + " vertice-vertice (EN 1-" + toString(counts.vertexEdges)
                  + "; paralelas a X, depois Y, depois Z)\n";
        buffer += "!   " + toString(counts.centerCenter) + " centro-centro e " + toString(counts.centerVertex)
                  + " centro-vertice, intercaladas por cubo\n";
        buffer += "\n";

        // Nós.
        const std::int64_t total = data.totalNodeCount();
        for (std::int64_t id = 1; id <= total; ++id)
        {
            const gp_XYZ p = data.node(id, opts.unit).position;
            buffer += "N,";
            appendInt(buffer, id);
            for (int a = 1; a <= 3; ++a)
            {
                buffer.push_back(',');
                buffer += NodeAsciiExporter::formatCoordinate(p.Coord(a));
            }
            buffer.push_back('\n');
            flushIfFull();
        }

        // Tipo de elemento e ligações.
        buffer += "ET,1," + toString(kApdlLinkElementType) + "\n";
        ApdlLinkSequence seq(data);
        LinkElement e;
        while (seq.next(e))
        {
            buffer += "EN,";
            appendInt(buffer, e.id);
            buffer.push_back(',');
            appendInt(buffer, e.n1);
            buffer.push_back(',');
            appendInt(buffer, e.n2);
            buffer.push_back('\n');
            flushIfFull();
        }
        out.write(buffer.data(), std::streamsize(buffer.size()));
        out.flush();
        if (!out.good())
        {
            out.close();
            removeTmp();
            return fail(r, "Falha ao gravar o arquivo (espaço em disco ou permissões); exportação cancelada");
        }
    }
    const ApdlFileCheck check = verify(tmp, data, opts);
    if (!check.ok)
    {
        removeTmp();
        return fail(r, "Arquivo gravado não passou na verificação (" + check.message + "); exportação cancelada");
    }
    r.bytes = std::filesystem::file_size(tmp, ec);
    std::filesystem::rename(tmp, file, ec); // substituição atômica do destino
    if (ec)
    {
        removeTmp();
        return fail(r, "Não foi possível gravar o destino (" + ec.message() + "); o arquivo existente foi mantido");
    }
    r.ok = true;
    r.nodes = check.nodes;
    r.links = counts;
    r.message = "arquivo exportado e conferido";
    r.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    return r;
}

ApdlFileCheck ApdlExporter::verify(const std::filesystem::path& file, const Discretization& data,
                                   const ApdlOptions& options)
{
    ApdlFileCheck c;
    std::ifstream in(file, std::ios::binary);
    if (!in)
        return failCheck(c, "arquivo não pode ser aberto");
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (text.empty() || text.back() != '\n')
        return failCheck(c, "arquivo incompleto (última linha sem fim de linha)");
    for (char ch : text)
        if (static_cast<unsigned char>(ch) >= 0x80)
            return failCheck(c, "arquivo contém caracteres não ASCII");

    // Estados: 0 cabeçalho, 1 nós, 2 elementos.
    const std::vector<std::string> header = options.includeClear
                                                ? std::vector<std::string>{"FINISH", "/CLEAR", "/TITLE", "/PREP7"}
                                                : std::vector<std::string>{"FINISH", "/TITLE", "/PREP7"};
    std::size_t headerPos = 0;
    int state = 0;
    bool elementType = false;
    const std::int64_t totalNodes = data.totalNodeCount();
    ApdlLinkSequence seq(data);
    LinkElement expected;
    std::int64_t lineNo = 0;
    // Conferência de um nó e de um elemento (comuns aos caminhos rápido e geral). Vazio = ok.
    auto checkNode = [&](std::int64_t id, const double v[3]) -> std::string {
        if (id != c.nodes + 1 || id > totalNodes)
            return "ID de nó fora de sequência (duplicado ou lacuna) na linha " + toString(lineNo) + ": " + toString(id);
        const gp_XYZ p = data.node(id, options.unit).position;
        if (v[0] != p.X() || v[1] != p.Y() || v[2] != p.Z())
            return "coordenadas do nó " + toString(id) + " diferem da discretização";
        ++c.nodes;
        return {};
    };
    auto checkElement = [&](std::int64_t id, std::int64_t n1, std::int64_t n2) -> std::string {
        if (!seq.next(expected))
            return "mais elementos que ligações previstas na linha " + toString(lineNo);
        if (id != expected.id || n1 != expected.n1 || n2 != expected.n2)
            return "elemento " + toString(id) + " difere da sequência de ligações (esperado EN," + toString(expected.id)
                   + "," + toString(expected.n1) + "," + toString(expected.n2) + ")";
        if (n1 == n2 || n1 < 1 || n2 < 1 || n1 > totalNodes || n2 > totalNodes)
            return "conectividade inválida no elemento " + toString(id);
        ++c.elements;
        return {};
    };
    // Caminho rápido (sem alocação) para as linhas "N,id,x,y,z" e "EN,id,n1,n2" no formato exato
    // gravado; qualquer outra forma segue para a análise geral.
    auto scanInt = [](const char*& p, const char* end, std::int64_t& v) {
        const auto r = std::from_chars(p, end, v);
        if (r.ec != std::errc() || r.ptr == p)
            return false;
        p = r.ptr;
        return true;
    };
    auto scanReal = [](const char*& p, const char* end, double& v) {
        const auto r = std::from_chars(p, end, v);
        if (r.ec != std::errc() || r.ptr == p)
            return false;
        p = r.ptr;
        return true;
    };
    auto comma = [](const char*& p, const char* end) { return p < end && *p++ == ','; };
    std::size_t pos = 0;
    while (pos < text.size())
    {
        const std::size_t eol = text.find('\n', pos);
        const char* b = text.data() + pos;
        const char* e = text.data() + eol;
        pos = eol + 1;
        ++lineNo;
        if (state == 1 && e - b > 2 && b[0] == 'N' && b[1] == ',')
        {
            const char* p = b + 2;
            std::int64_t id = 0;
            double v[3];
            if (scanInt(p, e, id) && comma(p, e) && scanReal(p, e, v[0]) && comma(p, e) && scanReal(p, e, v[1])
                && comma(p, e) && scanReal(p, e, v[2]) && p == e)
            {
                if (std::string problem = checkNode(id, v); !problem.empty())
                    return failCheck(c, problem);
                continue;
            }
        }
        else if (state == 2 && e - b > 3 && b[0] == 'E' && b[1] == 'N' && b[2] == ',')
        {
            const char* p = b + 3;
            std::int64_t id = 0, n1 = 0, n2 = 0;
            if (scanInt(p, e, id) && comma(p, e) && scanInt(p, e, n1) && comma(p, e) && scanInt(p, e, n2) && p == e)
            {
                if (std::string problem = checkElement(id, n1, n2); !problem.empty())
                    return failCheck(c, problem);
                continue;
            }
        }
        const std::string line = trim(std::string(b, e));
        if (line.empty() || line[0] == '!')
            continue;
        const std::vector<std::string> f = fields(line);
        const std::string command = upper(f[0]);
        const std::string where = " na linha " + toString(lineNo);
        if (isForbiddenCommand(command))
            return failCheck(c, "comando fora do escopo geométrico (" + command + ")" + where);
        if (state == 0)
        {
            if (headerPos >= header.size() || command != header[headerPos])
                return failCheck(c, "cabeçalho inesperado (" + f[0] + ")" + where);
            if (command == "/TITLE" && (f.size() != 2 || f[1] != "DEM-DYNA"))
                return failCheck(c, "título diferente de DEM-DYNA" + where);
            if (++headerPos == header.size())
                state = 1;
            continue;
        }
        if (state == 1 && command == "N")
        {
            std::int64_t id = 0;
            double v[3];
            if (f.size() != 5 || !parseInt(f[1], id) || !parseReal(f[2], v[0]) || !parseReal(f[3], v[1])
                || !parseReal(f[4], v[2]))
                return failCheck(c, "comando N inválido" + where);
            if (std::string problem = checkNode(id, v); !problem.empty())
                return failCheck(c, problem);
            continue;
        }
        if (state == 1 && command == "ET")
        {
            if (c.nodes != totalNodes)
                return failCheck(c, "número de nós " + toString(c.nodes) + " diferente de " + toString(totalNodes));
            std::int64_t type = 0, number = 0;
            if (f.size() != 3 || !parseInt(f[1], number) || !parseInt(f[2], type) || number != 1
                || type != kApdlLinkElementType)
                return failCheck(c, "tipo de elemento diferente de ET,1,165" + where);
            elementType = true;
            state = 2;
            continue;
        }
        if (state == 2 && command == "EN")
        {
            std::int64_t id = 0, n1 = 0, n2 = 0;
            if (f.size() != 4 || !parseInt(f[1], id) || !parseInt(f[2], n1) || !parseInt(f[3], n2))
                return failCheck(c, "comando EN inválido (exige exatamente dois nós)" + where);
            if (std::string problem = checkElement(id, n1, n2); !problem.empty())
                return failCheck(c, problem);
            continue;
        }
        return failCheck(c, "comando inesperado (" + f[0] + ")" + where);
    }
    if (!elementType)
        return failCheck(c, "definição ET,1,165 ausente");
    if (seq.next(expected))
        return failCheck(c, "faltam elementos (o arquivo termina antes do elemento " + toString(expected.id) + ")");
    c.ok = true;
    c.message = "arquivo conferido";
    return c;
}

// --- Comparação com o arquivo de referência ----------------------------------------

namespace {

struct ParsedApdl
{
    struct Node
    {
        std::int64_t id;
        double x, y, z;
    };
    struct Element
    {
        std::int64_t id;
        std::vector<std::int64_t> nodes;
    };
    bool ok = false;
    std::string error;
    std::vector<Node> nodes;
    std::vector<Element> elements;
    std::map<std::int64_t, std::int64_t> elementTypes; // ET número -> tipo
    std::map<std::string, std::int64_t> other;         // comandos não geométricos
};

ParsedApdl parseApdl(const std::filesystem::path& file)
{
    ParsedApdl p;
    std::ifstream in(file, std::ios::binary);
    if (!in)
    {
        p.error = "arquivo não pode ser aberto: " + file.u8string();
        return p;
    }
    std::string raw;
    std::int64_t lineNo = 0;
    while (std::getline(in, raw))
    {
        ++lineNo;
        const std::string line = trim(raw);
        if (line.empty() || line[0] == '!')
            continue;
        const std::vector<std::string> f = fields(line);
        const std::string command = upper(f[0]);
        const std::string where = " (linha " + toString(lineNo) + ")";
        if (command == "N")
        {
            ParsedApdl::Node n{};
            double v[3] = {0.0, 0.0, 0.0};
            if (f.size() < 2 || !parseInt(f[1], n.id))
            {
                p.error = "comando N inválido" + where;
                return p;
            }
            for (int a = 0; a < 3; ++a)
                if (std::size_t(a + 2) < f.size() && !parseReal(f[std::size_t(a + 2)], v[a]))
                {
                    p.error = "coordenada inválida" + where;
                    return p;
                }
            n.x = v[0];
            n.y = v[1];
            n.z = v[2];
            p.nodes.push_back(n);
        }
        else if (command == "EN")
        {
            ParsedApdl::Element e{};
            if (f.size() < 2 || !parseInt(f[1], e.id))
            {
                p.error = "comando EN inválido" + where;
                return p;
            }
            for (std::size_t n = 2; n < f.size(); ++n)
            {
                std::int64_t id = 0;
                if (f[n].empty())
                    continue;
                if (!parseInt(f[n], id))
                {
                    p.error = "nó inválido em EN" + where;
                    return p;
                }
                e.nodes.push_back(id);
            }
            p.elements.push_back(std::move(e));
        }
        else if (command == "ET")
        {
            std::int64_t number = 0, type = 0;
            if (f.size() >= 3 && parseInt(f[1], number) && parseInt(f[2], type))
                p.elementTypes[number] = type;
        }
        else
        {
            // Nome do comando (atribuições como "f11(1)= ..." contam como "parâmetro APDL").
            if (command.find('=') != std::string::npos)
                ++p.other["(atribuicao de parametro)"];
            else
                ++p.other[command.substr(0, command.find(' '))];
        }
    }
    p.ok = true;
    return p;
}

std::string formatNode(const ParsedApdl::Node& n)
{
    return "N," + toString(n.id) + "," + NodeAsciiExporter::formatCoordinate(n.x) + ","
           + NodeAsciiExporter::formatCoordinate(n.y) + "," + NodeAsciiExporter::formatCoordinate(n.z);
}

std::string formatElement(const ParsedApdl::Element& e)
{
    std::string s = "EN," + toString(e.id);
    for (std::int64_t n : e.nodes)
        s += "," + toString(n);
    return s;
}

} // namespace

ApdlComparison ApdlExporter::compare(const std::filesystem::path& generated, const std::filesystem::path& reference)
{
    ApdlComparison r;
    const ParsedApdl gen = parseApdl(generated);
    const ParsedApdl ref = parseApdl(reference);
    std::ostringstream out;
    if (!gen.ok || !ref.ok)
    {
        r.divergenceKind = "leitura";
        r.cause = !gen.ok ? "arquivo gerado: " + gen.error : "arquivo de referência: " + ref.error;
        r.summary = "Comparação impossível: " + r.cause + "\n";
        return r;
    }
    r.nodesGenerated = std::int64_t(gen.nodes.size());
    r.nodesReference = std::int64_t(ref.nodes.size());
    r.elementsGenerated = std::int64_t(gen.elements.size());
    r.elementsReference = std::int64_t(ref.elements.size());
    const auto genType = gen.elementTypes.find(1);
    const auto refType = ref.elementTypes.find(1);
    r.elementTypeMatches = genType != gen.elementTypes.end() && refType != ref.elementTypes.end()
                           && genType->second == refType->second;
    for (const auto& [number, type] : ref.elementTypes)
        if (number != 1)
            r.ignoredReferenceCommands["ET," + toString(number) + "," + toString(type)] += 1;
    for (const auto& [command, count] : ref.other)
        r.ignoredReferenceCommands[command] += count;

    auto diverge = [&r](std::string kind, std::int64_t id, std::string expected, std::string obtained,
                        std::string cause) {
        if (!r.divergenceKind.empty())
            return;
        r.divergenceKind = std::move(kind);
        r.divergenceId = id;
        r.expected = std::move(expected);
        r.obtained = std::move(obtained);
        r.cause = std::move(cause);
    };

    // Nós: mesma sequência de IDs e coordenadas numericamente equivalentes.
    const std::size_t commonNodes = std::min(gen.nodes.size(), ref.nodes.size());
    for (std::size_t n = 0; n < commonNodes && r.divergenceKind.empty(); ++n)
    {
        const auto& a = ref.nodes[n];
        const auto& b = gen.nodes[n];
        if (a.id != b.id)
            diverge("N", a.id, formatNode(a), formatNode(b), "numeração dos nós diferente (lacuna, duplicata ou ordem)");
        else if (!sameReal(a.x, b.x) || !sameReal(a.y, b.y) || !sameReal(a.z, b.z))
        {
            const bool scaled = sameReal(a.x * 1000.0, b.x) && sameReal(a.y * 1000.0, b.y) && sameReal(a.z * 1000.0, b.z);
            diverge("N", a.id, formatNode(a), formatNode(b),
                    scaled ? "unidade diferente (o arquivo gerado está em mm; a referência, em m)"
                           : "coordenadas diferentes: lco, dimensões, origem da grade ou ordem Z -> Y -> X");
        }
    }
    if (gen.nodes.size() != ref.nodes.size())
        diverge("contagem", std::int64_t(commonNodes) + 1,
                toString(r.nodesReference) + " nós", toString(r.nodesGenerated) + " nós",
                "discretização diferente (dimensões, lco ou critério de inclusão)");
    if (!r.elementTypeMatches)
        diverge("ET", 1, refType != ref.elementTypes.end() ? "ET,1," + toString(refType->second) : "(ausente)",
                genType != gen.elementTypes.end() ? "ET,1," + toString(genType->second) : "(ausente)",
                "tipo de elemento diferente");

    // Elementos: mesma numeração e mesma conectividade, na mesma ordem.
    const std::size_t commonElements = std::min(gen.elements.size(), ref.elements.size());
    for (std::size_t n = 0; n < commonElements && r.divergenceKind.empty(); ++n)
    {
        const auto& a = ref.elements[n];
        const auto& b = gen.elements[n];
        if (a.id != b.id || a.nodes != b.nodes)
        {
            const bool reversed = a.nodes.size() == 2 && b.nodes.size() == 2 && a.nodes[0] == b.nodes[1]
                                  && a.nodes[1] == b.nodes[0];
            diverge("EN", a.id, formatElement(a), formatElement(b),
                    a.id != b.id ? "numeração dos elementos diferente"
                    : reversed   ? "mesma ligação com a ordem dos nós invertida"
                                 : "ordem de geração das ligações diferente (grupos A/B/C ou ordem dos vértices)");
        }
    }
    if (gen.elements.size() != ref.elements.size())
        diverge("contagem", std::int64_t(commonElements) + 1, toString(r.elementsReference) + " elementos",
                toString(r.elementsGenerated) + " elementos",
                "conjunto de ligações diferente (cubos aceitos, vizinhança ou duplicações)");

    r.ok = r.divergenceKind.empty();
    out << "Comparação geométrica (N, ET,1 e EN)\n";
    out << "  Arquivo gerado:     " << generated.u8string() << "\n";
    out << "  Arquivo referência: " << reference.u8string() << "\n";
    out << "  Nós:       gerado " << r.nodesGenerated << " | referência " << r.nodesReference << "\n";
    out << "  Elementos: gerado " << r.elementsGenerated << " | referência " << r.elementsReference << "\n";
    out << "  ET,1:      " << (genType != gen.elementTypes.end() ? toString(genType->second) : "-") << " | "
        << (refType != ref.elementTypes.end() ? toString(refType->second) : "-") << "\n";
    if (r.ok)
        out << "  Resultado: IDÊNTICO — sequência dos nós (IDs e coordenadas) e conectividade/ordem de todos os "
               "elementos coincidem.\n";
    else
        out << "  Resultado: DIVERGENTE\n"
            << "    Primeiro ID divergente: " << r.divergenceKind << " " << r.divergenceId << "\n"
            << "    Valor esperado (referência): " << r.expected << "\n"
            << "    Valor obtido (gerado):       " << r.obtained << "\n"
            << "    Possível causa: " << r.cause << "\n";
    if (!r.ignoredReferenceCommands.empty())
    {
        out << "  Comandos da referência fora do escopo geométrico (ignorados na comparação):\n";
        for (const auto& [command, count] : r.ignoredReferenceCommands)
            out << "    " << command << ": " << count << "\n";
    }
    r.summary = out.str();
    return r;
}

} // namespace cad::discretization
