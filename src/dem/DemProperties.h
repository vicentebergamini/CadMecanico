// Propriedades físicas do material e parâmetros do modelo MED (E5.7.4a), em unidades SI.
//
// Referência: DEM_HIAGO_PRINCIPAL.m (entradas lidas por lerValor) e lawnumbers.m (lei bilinear).
// Cada campo corresponde a UMA variável do MATLAB, com o mesmo significado:
//   density        ROH      ρ    [kg/m³]
//   poissonRatio   PSN      ν    [-]
//   youngModulus   YNG      E    [Pa = N/m²]
//   fractureEnergy GRF           energia específica de fratura por área [N/m = J/m²] (ver abaixo)
//   failureFactor  RFC           fator de falha [adimensional, conforme a especificação E5.7.4a]
//   corrcaf()      CORRCAF       fator de compatibilização de malha, fixo em 1
// O MATLAB usa essas entradas apenas em (DEM_HIAGO_PRINCIPAL.m, linhas 123–131):
//   CAF = 0.1385·CORRCAF; POA = 1−PSN²; delta = 9·PSN/(4−8·PSN); alfa = (9+8·delta)/(18+24·delta);
//   LCR = 2·CAF·POA/(alfa·RFC²); EPR = RFC·√(GRF/(YNG·POA)); ENR = YNG·LCO²·alfa; ...
// e ROH na massa nodal (malha_por_mascara.m). Essas equações NÃO são implementadas nesta etapa.
//
// Energia de fratura: o MATLAB tem uma única entrada (GRF). Ela não tem unidade declarada no
// script; o significado "energia específica de fratura por área" [N/m] vem da documentação
// complementar (Gf [N/m]) e da especificação da etapa. Não existe no MATLAB entrada nem valor
// calculado de energia de fratura em N·m; nenhuma relação entre as duas grandezas é criada aqui.
// O fator Gf_bond de GF_field.m (campo aleatório adimensional, média 1) multiplica EPR em
// lawnumbers.m e não é uma propriedade do material.
//
// Sem dependência de Qt.
#pragma once

#include <string>

namespace cad::dem {

// Propriedades do material.
struct MaterialProperties
{
    double density = 0.0;        // ρ   [kg/m³]     (ROH)
    double poissonRatio = 0.0;   // ν   [-]         (PSN)
    double youngModulus = 0.0;   // E   [Pa]        (YNG)
    double fractureEnergy = 0.0; // GRF [N/m]       (GRF)

    bool operator==(const MaterialProperties& o) const
    {
        return density == o.density && poissonRatio == o.poissonRatio && youngModulus == o.youngModulus
               && fractureEnergy == o.fractureEnergy;
    }
    bool operator!=(const MaterialProperties& o) const { return !(*this == o); }
};

// Parâmetros do modelo MED.
struct ModelParameters
{
    // CORRCAF não é um campo: é uma constante do modelo e não pode ser alterada.
    static constexpr double kCorrcaf = 1.0;

    double failureFactor = 0.0; // RFC [-]

    static constexpr double corrcaf() { return kCorrcaf; }

    bool operator==(const ModelParameters& o) const { return failureFactor == o.failureFactor; }
    bool operator!=(const ModelParameters& o) const { return !(*this == o); }
};

// Propriedades MED de um projeto.
struct DemProperties
{
    MaterialProperties material;
    ModelParameters model;

    bool operator==(const DemProperties& o) const { return material == o.material && model == o.model; }
    bool operator!=(const DemProperties& o) const { return !(*this == o); }
};

// Valores padrão das entradas de DEM_HIAGO_PRINCIPAL.m (segundo argumento de lerValor): ROH = 1240,
// PSN = 0.36, YNG = 3.5e9, GRF = 2400, RFC = 10. São valores de entrada do script, não resultados.
DemProperties matlabDefaults();

struct PropertyValidation
{
    bool ok = true;
    std::string field;   // chave do campo (ver kField*)
    std::string message; // mensagem para o usuário
};

// Chaves estáveis dos campos (validação, interface e arquivo .cadmec).
inline constexpr const char* kFieldDensity = "density";
inline constexpr const char* kFieldPoissonRatio = "poissonRatio";
inline constexpr const char* kFieldYoungModulus = "youngModulus";
inline constexpr const char* kFieldFractureEnergy = "fractureEnergy";
inline constexpr const char* kFieldFailureFactor = "failureFactor";
inline constexpr const char* kFieldCorrcaf = "corrcaf";

// Verifica, na ordem dos campos:
//   - todos os valores finitos (NaN e infinito recusados);
//   - ρ > 0 e E > 0;
//   - ν no intervalo aberto (−1; 0,5), a mesma verificação de DEM_HIAGO_PRINCIPAL.m (linha 60);
//   - GRF > 0: EPR = RFC·√(GRF/(YNG·POA)) não é real com GRF < 0, e GRF = 0 anula a deformação de
//     pico EEP (curvas de carga com todas as abscissas nulas);
//   - RFC > 0: LCR = 2·CAF·POA/(alfa·RFC²) não existe com RFC = 0, e RFC < 0 tornaria EPR (e a
//     deformação de pico) negativo, invertendo a ordem das abscissas das curvas.
// O MATLAB não impõe outros limites, e nenhum outro é imposto aqui.
PropertyValidation validate(const DemProperties& properties);

// Converte texto em número: aceita ponto ou vírgula como separador decimal e notação científica
// (ex.: "3.5e9", "0,36"); recusa texto vazio, não numérico, com caracteres sobrando, separadores
// de milhar, NaN e infinito.
bool parseNumber(const std::string& text, double& value);

} // namespace cad::dem
