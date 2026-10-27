#include "discretization/NodeAsciiExporter.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>
#include <system_error>

namespace cad::discretization {

namespace {

NodeExportResult fail(NodeExportResult r, std::string message)
{
    r.ok = false;
    r.message = std::move(message);
    return r;
}

NodeFileCheck failCheck(NodeFileCheck c, std::string message)
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

void appendNode(std::string& out, std::int64_t id, const gp_XYZ& p)
{
    appendInt(out, id);
    for (int a = 1; a <= 3; ++a)
    {
        out.push_back(' ');
        out += NodeAsciiExporter::formatCoordinate(p.Coord(a));
    }
    out.push_back('\n');
}

// Ordem estrita Z → Y → X (Z mais significativo, X varia mais rapidamente).
bool zyxLess(const gp_XYZ& a, const gp_XYZ& b)
{
    if (a.Z() != b.Z())
        return a.Z() < b.Z();
    if (a.Y() != b.Y())
        return a.Y() < b.Y();
    return a.X() < b.X();
}

std::filesystem::path temporaryPath(const std::filesystem::path& file)
{
    std::filesystem::path tmp = file;
    tmp += ".parcial.tmp";
    return tmp;
}

} // namespace

std::string NodeAsciiExporter::formatCoordinate(double value)
{
    if (value == 0.0)
        return "0"; // também normaliza -0
    char buf[400];
    const auto res = std::to_chars(buf, buf + sizeof(buf), value, std::chars_format::fixed);
    if (res.ec != std::errc())
        return "nan";
    return std::string(buf, res.ptr);
}

std::string NodeAsciiExporter::suggestedFileName(const std::string& partName)
{
    std::string base;
    for (char c : partName)
    {
        // Caracteres inválidos em nomes de arquivo no Windows.
        if (c == '<' || c == '>' || c == ':' || c == '"' || c == '/' || c == '\\' || c == '|' || c == '?' || c == '*')
            c = '_';
        base.push_back(c);
    }
    while (!base.empty() && (base.back() == ' ' || base.back() == '.'))
        base.pop_back();
    if (base.empty())
        base = "peca";
    return base + "_nodes.txt";
}

NodeExportResult NodeAsciiExporter::write(const Discretization& data, const std::filesystem::path& file,
                                          LengthUnit unit, bool overwrite) const
{
    NodeExportResult r;
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
        buffer += kNodeFileHeader;
        buffer.push_back('\n');
        const std::int64_t total = data.totalNodeCount();
        for (std::int64_t id = 1; id <= total; ++id)
        {
            appendNode(buffer, id, data.node(id, unit).position);
            if (buffer.size() > (1u << 20) - 256)
            {
                out.write(buffer.data(), std::streamsize(buffer.size()));
                buffer.clear();
            }
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
    const NodeFileCheck check = verify(tmp, data, unit);
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
    r.lines = check.lines;
    r.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    return r;
}

NodeFileCheck NodeAsciiExporter::verify(const std::filesystem::path& file, const Discretization& data, LengthUnit unit)
{
    NodeFileCheck c;
    std::ifstream in(file, std::ios::binary);
    if (!in)
        return failCheck(c, "arquivo não pode ser aberto");
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const std::string header = std::string(kNodeFileHeader) + "\n";
    if (text.compare(0, header.size(), header) != 0)
        return failCheck(c, "cabeçalho ausente ou diferente de \"" + std::string(kNodeFileHeader) + "\"");
    if (text.empty() || text.back() != '\n')
        return failCheck(c, "arquivo incompleto (última linha sem fim de linha)");

    const std::int64_t expected = data.totalNodeCount();
    const std::int64_t vertices = data.vertexNodeCount();
    const char* p = text.data() + header.size();
    const char* end = text.data() + text.size();
    gp_XYZ previous;
    int previousIjk[3] = {0, 0, 0};
    while (p < end)
    {
        const char* eol = static_cast<const char*>(std::memchr(p, '\n', std::size_t(end - p)));
        const std::int64_t lineNo = c.lines + 2; // linha física (cabeçalho = 1)
        std::int64_t id = 0;
        auto res = std::from_chars(p, eol, id);
        if (res.ec != std::errc() || res.ptr == p)
            return failCheck(c, "ID inválido na linha " + std::to_string(lineNo));
        if (id != c.lines + 1)
            return failCheck(c, "ID fora de sequência (duplicado ou lacuna) na linha " + std::to_string(lineNo) + ": "
                                    + std::to_string(id));
        double v[3];
        const char* q = res.ptr;
        for (double& value : v)
        {
            if (q >= eol || *q != ' ')
                return failCheck(c, "linha " + std::to_string(lineNo) + " não tem quatro colunas separadas por espaço");
            ++q;
            res = std::from_chars(q, eol, value, std::chars_format::fixed);
            if (res.ec != std::errc() || res.ptr == q || !std::isfinite(value))
                return failCheck(c, "coordenada inválida na linha " + std::to_string(lineNo));
            q = res.ptr;
        }
        if (q != eol)
            return failCheck(c, "linha " + std::to_string(lineNo) + " tem conteúdo além de quatro colunas");
        if (id > expected)
            return failCheck(c, "mais linhas que nós (" + std::to_string(expected) + ")");
        const gp_XYZ current(v[0], v[1], v[2]);
        const NodeInfo node = data.node(id, unit);
        if (current.X() != node.position.X() || current.Y() != node.position.Y() || current.Z() != node.position.Z())
            return failCheck(c, "coordenadas do nó " + std::to_string(id) + " diferem da discretização");
        // Ordem Z → Y → X dentro de cada bloco (vértices; centros). Com a geometria ajustada
        // (projeção dos nós, E5.7.2.3) a ordem é a dos índices da grade antes da projeção.
        const int ijk[3] = {node.k, node.j, node.i};
        const bool ordered = data.adjusted() ? std::lexicographical_compare(previousIjk, previousIjk + 3, ijk, ijk + 3)
                                             : zyxLess(previous, current);
        if (id != 1 && id != vertices + 1 && !ordered)
            return failCheck(c, "ordenação Z → Y → X violada no nó " + std::to_string(id));
        previous = current;
        std::copy(ijk, ijk + 3, previousIjk);
        ++c.lines;
        p = eol + 1;
    }
    if (c.lines != expected)
        return failCheck(c, "número de linhas " + std::to_string(c.lines) + " diferente do número de nós "
                                + std::to_string(expected));
    c.vertexLines = vertices;
    c.centerLines = c.lines - vertices;
    c.ok = true;
    c.message = "arquivo conferido";
    return c;
}

} // namespace cad::discretization
