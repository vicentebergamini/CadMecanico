#include "io/StlExporter.h"

#include <BRepBndLib.hxx>
#include <BRepBuilderAPI_Copy.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRep_Tool.hxx>
#include <Poly_Triangulation.hxx>
#include <Standard_Failure.hxx>
#include <StlAPI_Writer.hxx>
#include <TopExp_Explorer.hxx>
#include <TopLoc_Location.hxx>
#include <TopoDS.hxx>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <fstream>
#include <sstream>

#ifdef _WIN32
#include <process.h>
#define CADMEC_GETPID _getpid
#else
#include <unistd.h>
#define CADMEC_GETPID getpid
#endif

namespace cad::io {

namespace {

constexpr double kPi = 3.14159265358979323846;

std::string num(double v, int precision = 3)
{
    std::ostringstream s;
    s.precision(precision);
    s << std::fixed << v;
    std::string t = s.str();
    std::replace(t.begin(), t.end(), '.', ',');
    return t;
}

StlExportResult fail(StlExportResult r, const std::string& message)
{
    r.ok = false;
    r.message = message;
    return r;
}

std::filesystem::path temporaryPath(const std::filesystem::path& target)
{
    static std::atomic<int> counter{0};
    std::filesystem::path tmp = target;
    tmp += ".~cadmec-" + std::to_string(CADMEC_GETPID()) + "-" + std::to_string(++counter) + ".tmp";
    return tmp;
}

} // namespace

const char* qualityName(StlQuality quality)
{
    switch (quality)
    {
    case StlQuality::Low:    return "Baixa";
    case StlQuality::Medium: return "Média";
    case StlQuality::High:   return "Alta";
    }
    return "";
}

StlMeshParameters StlExporter::presetParameters(StlQuality quality, const TopoDS_Shape& shape)
{
    double diagonal = 100.0;
    if (!shape.IsNull())
    {
        Bnd_Box box;
        BRepBndLib::Add(shape, box, false);
        if (!box.IsVoid())
            diagonal = std::sqrt(box.SquareExtent());
    }
    StlMeshParameters p;
    switch (quality)
    {
    case StlQuality::Low:
        p.linearDeflection = std::clamp(0.005 * diagonal, 0.05, 2.0);
        p.angularDeflectionDeg = 30.0;
        break;
    case StlQuality::Medium:
        p.linearDeflection = std::clamp(0.001 * diagonal, 0.01, 0.5);
        p.angularDeflectionDeg = 15.0;
        break;
    case StlQuality::High:
        p.linearDeflection = std::clamp(0.0002 * diagonal, 0.005, 0.1);
        p.angularDeflectionDeg = 5.0;
        break;
    }
    return p;
}

std::optional<StlMeshParameters> StlExporter::effectiveParameters(const StlExportOptions& options,
                                                                  const TopoDS_Shape& shape, std::string* error)
{
    StlMeshParameters p = presetParameters(options.quality, shape);
    if (options.linearDeflection)
    {
        const double v = *options.linearDeflection;
        if (!std::isfinite(v) || v < kMinLinearDeflection || v > kMaxLinearDeflection)
        {
            if (error)
                *error = "Tolerância linear fora do intervalo (" + num(kMinLinearDeflection) + " a "
                         + num(kMaxLinearDeflection, 0) + " mm)";
            return std::nullopt;
        }
        p.linearDeflection = v;
    }
    if (options.angularDeflectionDeg)
    {
        const double v = *options.angularDeflectionDeg;
        if (!std::isfinite(v) || v < kMinAngularDeflectionDeg || v > kMaxAngularDeflectionDeg)
        {
            if (error)
                *error = "Tolerância angular fora do intervalo (" + num(kMinAngularDeflectionDeg, 1) + " a "
                         + num(kMaxAngularDeflectionDeg, 0) + " graus)";
            return std::nullopt;
        }
        p.angularDeflectionDeg = v;
    }
    return p;
}

StlExportResult StlExporter::write(const TopoDS_Shape& shape, const std::filesystem::path& file,
                                   const StlExportOptions& options) const
{
    StlExportResult r;
    if (shape.IsNull())
        return fail(r, "O documento não contém geometria sólida para exportar");
    for (TopExp_Explorer ex(shape, TopAbs_SOLID); ex.More(); ex.Next())
        ++r.solids;
    if (r.solids == 0)
        return fail(r, "O documento não contém sólido para exportar");
    if (!BRepCheck_Analyzer(shape).IsValid())
        return fail(r, "A geometria sólida é topologicamente inválida (BRepCheck); exportação cancelada");

    std::string paramError;
    const auto parameters = effectiveParameters(options, shape, &paramError);
    if (!parameters)
        return fail(r, paramError);
    r.parameters = *parameters;

    // Destino: diretório existente e nome de arquivo válido.
    if (file.empty() || !file.has_filename())
        return fail(r, "Caminho de arquivo inválido");
    std::error_code ec;
    const std::filesystem::path dir = file.has_parent_path() ? file.parent_path() : std::filesystem::current_path(ec);
    if (!std::filesystem::is_directory(dir, ec))
        return fail(r, "Diretório inexistente: " + dir.u8string());
    if (std::filesystem::is_directory(file, ec))
        return fail(r, "O caminho indicado é um diretório: " + file.u8string());

    // Triangulação de uma cópia: a forma do documento não é modificada.
    TopoDS_Shape copy;
    try
    {
        copy = BRepBuilderAPI_Copy(shape, true, false).Shape();
        BRepMesh_IncrementalMesh mesh(copy, r.parameters.linearDeflection, false,
                                      r.parameters.angularDeflectionDeg * kPi / 180.0, true);
        if (!mesh.IsDone())
            return fail(r, "A triangulação (BRepMesh_IncrementalMesh) falhou");
    }
    catch (const Standard_Failure& e)
    {
        return fail(r, std::string("Falha na triangulação: ") + e.what());
    }
    for (TopExp_Explorer ex(copy, TopAbs_FACE); ex.More(); ex.Next())
    {
        ++r.faces;
        TopLoc_Location loc;
        const auto tri = BRep_Tool::Triangulation(TopoDS::Face(ex.Current()), loc);
        if (tri.IsNull() || tri->NbTriangles() == 0)
            return fail(r, "Triangulação incompleta: face sem triângulos");
        r.triangles += tri->NbTriangles();
        const gp_Trsf trsf = loc.Transformation();
        for (int i = 1; i <= tri->NbNodes(); ++i)
            r.meshBox.Add(tri->Node(i).Transformed(trsf));
    }
    if (r.triangles == 0)
        return fail(r, "A triangulação não produziu triângulos");

    // Gravação em temporário + verificação + substituição.
    const std::filesystem::path tmp = temporaryPath(file);
    auto removeTmp = [&tmp] {
        std::error_code e;
        std::filesystem::remove(tmp, e);
    };
    bool written = false;
    try
    {
        // Fluxo aberto com o caminho nativo (UTF-16 no Windows): nomes com acentos funcionam e o
        // OCCT grava exatamente no temporário criado aqui.
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (out)
        {
            StlAPI_Writer writer;
            writer.ASCIIMode() = options.format == StlFormat::Ascii;
            written = writer.Write(copy, out);
            out.flush();
            written = written && out.good();
        }
    }
    catch (const Standard_Failure& e)
    {
        removeTmp();
        return fail(r, std::string("Falha ao gravar o STL: ") + e.what());
    }
    if (!written || !std::filesystem::exists(tmp, ec))
    {
        removeTmp();
        return fail(r, "Não foi possível gravar o arquivo STL (verifique permissões e espaço em disco)");
    }
    const auto size = std::filesystem::file_size(tmp, ec);
    bool complete = false;
    if (!ec)
    {
        if (options.format == StlFormat::Binary)
            complete = size == 84u + 50u * std::uintmax_t(r.triangles);
        else
        {
            std::ifstream in(tmp, std::ios::binary);
            std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            complete = text.rfind("endsolid") != std::string::npos && text.find("facet normal") != std::string::npos;
        }
    }
    if (!complete)
    {
        removeTmp();
        return fail(r, "Arquivo STL gravado incompleto; exportação cancelada");
    }
    std::filesystem::rename(tmp, file, ec); // substitui o destino de forma atômica
    if (ec)
    {
        removeTmp();
        return fail(r, "Não foi possível gravar o destino (" + ec.message() + "); o arquivo existente foi mantido");
    }
    r.ok = true;
    return r;
}

} // namespace cad::io
