// Exportação STEP (AP214, unidades em mm).
#pragma once

#include <TopoDS_Shape.hxx>

#include <filesystem>
#include <string>

namespace cad::io {

struct StepWriteResult
{
    bool ok = false;
    std::string message;
};

class StepExporter
{
public:
    StepWriteResult write(const TopoDS_Shape& shape, const std::filesystem::path& file) const;
};

} // namespace cad::io
