// Propriedades físicas do material e parâmetros do modelo MED (E5.7.4a, E5.7.4a.1), em unidades SI.
//
// Referência: DEM_HIAGO_PRINCIPAL.m (entradas lidas por lerValor) e lawnumbers.m (lei bilinear).
// Campos editáveis e variáveis correspondentes do MATLAB:
//   density          ROH      ρ     [kg/m³]
//   poissonRatio     PSN      ν     [-]
//   youngModulus     YNG      E     [Pa = N/m²]
//   fractureEnergy   GRF            energia específica de fratura por área [N/m = J/m²] (ver abaixo)
//   equivalentDefect d_eq           defeito intrínseco equivalente [m] (E5.7.4a.1; substitui RFC)
//   corrcaf()        CORRCAF        fator de compatibilização de malha, fixo em 1
// Grandezas derivadas (E5.7.4a.1), calculadas por derive() e nunca gravadas:
//   RFC = 1/√d_eq                      [m^(-1/2)]
//   POA = 1 − ν²                       [-]
//   EPR = √(GRF/(d_eq·E·POA))          [-]  (N/m ÷ (m·N/m²) = 1: deformação)
// Com RFC = 1/√d_eq, EPR coincide com a expressão do MATLAB EPR = RFC·√(GRF/(YNG·POA))
// (DEM_HIAGO_PRINCIPAL.m, linha 128), e LCR = 2·CAF·POA/(alfa·RFC²) = 2·CAF·POA·d_eq/alfa tem
// dimensão de comprimento [m]. EPR é a deformação de pico de referência usada em lawnumbers.m
// (EEP = EPR·Gf_bond). CAF, alfa, LCR, ENR e as leis NÃO são implementados nesta etapa.
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

    double equivalentDefect = 0.0; // d_eq [m]

    static constexpr double corrcaf() { return kCorrcaf; }

    bool operator==(const ModelParameters& o) const { return equivalentDefect == o.equivalentDefect; }
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
// PSN = 0.36, YNG = 3.5e9, GRF = 2400; d_eq = 0.01 m, que corresponde ao padrão RFC = 10
// (RFC = 1/√d_eq). São valores de entrada do script, não resultados.
DemProperties matlabDefaults();

// Grandezas derivadas do modelo MED (E5.7.4a.1). Só têm significado para propriedades válidas.
struct DerivedParameters
{
    double rfc = 0.0; // RFC = 1/√d_eq            [m^(-1/2)]
    double poa = 0.0; // POA = 1 − ν²             [-]
    double epr = 0.0; // EPR = √(GRF/(d_eq·E·POA)) [-]
};

DerivedParameters derive(const DemProperties& properties);

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
inline constexpr const char* kFieldEquivalentDefect = "equivalentDefect";
inline constexpr const char* kFieldCorrcaf = "corrcaf";

// Regras do material (ρ, ν, E, GRF), as mesmas de validate(): valores finitos, ρ > 0, ν em
// (−1; 0,5), E > 0 e GRF > 0. Usada também na leitura de arquivos de esquema 10, cujas propriedades
// MED são verificadas e descartadas sem conversão (ver DocumentFile.h).
PropertyValidation validateMaterial(const MaterialProperties& material);

// Verifica, na ordem dos campos:
//   - todos os valores finitos (NaN e infinito recusados);
//   - ρ > 0 e E > 0;
//   - ν no intervalo aberto (−1; 0,5), a mesma verificação de DEM_HIAGO_PRINCIPAL.m (linha 60);
//     garante POA = 1 − ν² > 0;
//   - GRF > 0: EPR = √(GRF/(d_eq·E·POA)) não é real com GRF < 0, e GRF = 0 anula a deformação de
//     pico EEP (curvas de carga com todas as abscissas nulas);
//   - d_eq > 0: RFC = 1/√d_eq e EPR não existem com d_eq ≤ 0;
//   - RFC e EPR finitos: d_eq (ou o produto d_eq·E·POA) pequeno demais estoura a aritmética de
//     ponto flutuante.
// O MATLAB não impõe outros limites, e nenhum outro é imposto aqui.
PropertyValidation validate(const DemProperties& properties);

// Converte texto em número: aceita ponto ou vírgula como separador decimal e notação científica
// (ex.: "3.5e9", "0,36"); recusa texto vazio, não numérico, com caracteres sobrando, separadores
// de milhar, NaN e infinito.
bool parseNumber(const std::string& text, double& value);

} // namespace cad::dem
