// Verificações mínimas para testes executados pelo CTest (código de saída 0 = aprovado).
#pragma once

#include <cmath>
#include <cstdio>

namespace cad::test {

inline int& failures()
{
    static int n = 0;
    return n;
}

inline void check(bool cond, const char* expr, const char* file, int line)
{
    if (!cond)
    {
        ++failures();
        std::printf("FALHOU: %s (%s:%d)\n", expr, file, line);
    }
}

inline bool near(double a, double b, double tol)
{
    return std::fabs(a - b) <= tol;
}

inline int finish(const char* name)
{
    std::printf("%s: %s\n", name, failures() == 0 ? "APROVADO" : "REPROVADO");
    return failures() == 0 ? 0 : 1;
}

} // namespace cad::test

#define CAD_CHECK(cond) ::cad::test::check((cond), #cond, __FILE__, __LINE__)
#define CAD_CHECK_NEAR(a, b, tol) ::cad::test::check(::cad::test::near((a), (b), (tol)), #a " ~= " #b, __FILE__, __LINE__)
