// Exportação STL da geometria sólida (E5.3), sem dependência de Qt.
//
// Fluxo: valida o sólido (BRepCheck_Analyzer) -> triangula uma CÓPIA do B-Rep
// (BRepMesh_IncrementalMesh; a forma do documento não recebe malha nem é alterada) -> confere a
// triangulação de todas as faces -> grava num arquivo temporário no mesmo diretório
// (StlAPI_Writer) -> confere o arquivo (tamanho binário = 84 + 50 x triângulos, ou "endsolid"
// no ASCII) -> substitui o destino de forma atômica. Em qualquer falha o destino não é criado
// nem alterado e o temporário é removido.
//
// Unidades: coordenadas gravadas em milímetros, exatamente como no documento (sem conversão).
// O formato STL não guarda a unidade.
//
// Qualidade da malha: deflexão linear proporcional à diagonal D da caixa envolvente do sólido,
// limitada a um intervalo para evitar malhas excessivas; deflexão angular fixa por preset:
//   Baixa: linear = 0,5 % de D, entre 0,05 e 2,0 mm; angular = 30°
//   Média: linear = 0,1 % de D, entre 0,01 e 0,5 mm; angular = 15°
//   Alta:  linear = 0,02 % de D, entre 0,005 e 0,1 mm; angular = 5°
// Tolerâncias personalizadas: linear 0,001..100 mm e angular 0,5..90°.
#pragma once

#include <Bnd_Box.hxx>
#include <TopoDS_Shape.hxx>

#include <filesystem>
#include <optional>
#include <string>

namespace cad::io {

enum class StlFormat
{
    Binary,
    Ascii
};

enum class StlQuality
{
    Low,
    Medium,
    High
};

struct StlMeshParameters
{
    double linearDeflection = 0.1;      // mm
    double angularDeflectionDeg = 15.0; // graus
};

struct StlExportOptions
{
    StlFormat format = StlFormat::Binary;
    StlQuality quality = StlQuality::Medium;
    std::optional<double> linearDeflection;     // substitui o preset (mm)
    std::optional<double> angularDeflectionDeg; // substitui o preset (graus)
};

struct StlExportResult
{
    bool ok = false;
    std::string message;
    StlMeshParameters parameters; // parâmetros efetivamente usados
    int solids = 0;
    int faces = 0;
    int triangles = 0;
    Bnd_Box meshBox; // caixa envolvente dos nós da malha (mm)
};

class StlExporter
{
public:
    static constexpr double kMinLinearDeflection = 0.001;
    static constexpr double kMaxLinearDeflection = 100.0;
    static constexpr double kMinAngularDeflectionDeg = 0.5;
    static constexpr double kMaxAngularDeflectionDeg = 90.0;

    // Parâmetros do preset para a forma dada (dependem do tamanho da peça).
    static StlMeshParameters presetParameters(StlQuality quality, const TopoDS_Shape& shape);
    // Parâmetros efetivos (preset ou personalizados). Retorna mensagem se forem inválidos.
    static std::optional<StlMeshParameters> effectiveParameters(const StlExportOptions& options,
                                                                const TopoDS_Shape& shape, std::string* error);

    StlExportResult write(const TopoDS_Shape& shape, const std::filesystem::path& file,
                          const StlExportOptions& options = StlExportOptions()) const;
};

const char* qualityName(StlQuality quality); // "Baixa", "Média", "Alta"

} // namespace cad::io
