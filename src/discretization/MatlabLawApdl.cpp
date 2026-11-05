#include "discretization/MatlabLawApdl.h"

#include "discretization/LatticeTopology.h" // matlabG

#include <array>
#include <cstddef>
#include <cstdio>

namespace cad::discretization {

namespace {

// "%g,0,%g,%g,%g": pontos 1, 3, 4 e 5 formatados; o segundo é o literal "0" do formato do MATLAB.
std::string curvePoints(const std::array<double, 5>& v)
{
    return matlabG(v[0]) + ",0," + matlabG(v[2]) + "," + matlabG(v[3]) + "," + matlabG(v[4]);
}

} // namespace

std::string matlabF(double value)
{
    char buf[512];
    std::snprintf(buf, sizeof buf, "%f", value);
    return buf;
}

std::string matlabLawText(const dem::MatlabLaw& w, std::int32_t jj)
{
    const std::string j = matlabG(double(w.id));                       // b e j (iguais em lawnumbers.m)
    const std::string jUnload = matlabG(double(w.id) + double(jj));    // j + jj
    std::string s;
    s.reserve(640);
    s += "\n";                                                              // l. 82
    s += "!************************INICIO DA LEI " + j + "\n";                 // l. 83
    s += "*DIM,f1" + j + ",,5\n";                                           // l. 85
    s += "*DIM,d1" + j + ",,5\n";                                           // l. 86
    s += "f1" + j + "(1)= " + curvePoints(w.loading.force) + "\n";          // l. 88
    s += "d1" + j + "(1)= " + curvePoints(w.loading.displacement) + "\n";   // l. 89
    s += "EDCURVE,ADD," + j + ",d1" + j + "(1),f1" + j + "(1)\n";            // l. 90
    s += "*DIM,f2" + j + ",,5\n";                                           // l. 93
    s += "*DIM,d2" + j + ",,5\n";                                           // l. 94
    s += "f2" + j + "(1)= " + curvePoints(w.unloading.force) + "\n";        // l. 96
    s += "d2" + j + "(1)= " + curvePoints(w.unloading.displacement) + "\n"; // l. 97
    s += "EDCURVE,ADD," + jUnload + ",d2" + j + "(1),f2" + j + "(1)\n";      // l. 98
    s += "MAT," + j + "\n";                                                 // l. 100
    s += "TB,DISCRETE," + j + ",,,5\n";                                     // l. 101
    s += "TBDAT,1," + j + "    !CURVA DE CARGA \n";                         // l. 102
    s += "TBDAT,2," + jUnload + " !CURVA DESCARGA \n";                      // l. 103
    s += "TBDAT,3,1    !BETA \n";                                           // l. 104
    s += "TBDAT,4," + matlabG(w.fp) + " !TYI \n";                           // l. 105
    s += "TBDAT,5," + matlabG(-w.fp) + " !CYI \n";                          // l. 106
    s += "!************************FIM DA LEI " + j + "\n";                    // l. 107
    return s;
}

std::string matlabLawBlock(const dem::MatlabLawTable& t)
{
    if (!t.ok)
        return {};
    std::string s;
    s.reserve(t.laws.size() * 640);
    for (const dem::MatlabLaw& w : t.laws)
        s += matlabLawText(w, t.curveOffset);
    return s;
}

std::string matlabEmodifBlock(const dem::MatlabLawTable& t)
{
    if (!t.ok)
        return {};
    std::string s;
    s.reserve(64 + t.bondLaw.size() * 40);
    s += "! ***********************DEFINE A LEI EM CADA MOLA e ENERGIAS\n"; // l. 114
    for (std::size_t b = 0; b < t.bondLaw.size(); ++b)                      // l. 115-117
        s += "EMODIF," + matlabF(double(b + 1)) + ",MAT," + matlabF(double(t.bondLaw[b])) + ", \n";
    return s;
}

} // namespace cad::discretization
