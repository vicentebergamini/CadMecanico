// Importação STEP.
#pragma once

#include <TopoDS_Shape.hxx>

#include <filesystem>
#include <string>

namespace cad::io {

struct StepReadResult
{
    bool ok = false;
    std::string message;
    int rootCount = 0;
    int transferredCount = 0;
    TopoDS_Shape shape;
};

class StepImporter
{
public:
    StepReadResult read(const std::filesystem::path& file) const;
};

} // namespace cad::io
