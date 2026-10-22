// Nomes topológicos persistentes de faces e arestas (E5.7.1 — P3), com o histórico de
// modificações do Open CASCADE (BRepTools_History das booleanas e dos acabamentos; faces geradas
// pelas arestas no prisma e nos acabamentos).
//
// Estratégia:
//  1. Identificar a entidade original: cada face recebe um nome quando é criada, formado pela
//     operação de origem e pelo papel da face nessa operação:
//       "<op>:base" / "<op>:topo"        faces inicial e final do prisma de uma extrusão
//                                        ("base2"/"topo2" na direção 2);
//       "<op>:lado:<entidade>[:<k>]"     face lateral gerada pela aresta do perfil que vem da
//                                        entidade do esboço (k = lado do retângulo);
//       "<op>:xmin" ... "<op>:zmax"      faces do bloco base (pela normal externa);
//       "<op>:furo"                      parede do furo passante;
//       "<op>:acab:<i>"                  face criada pelo Fillet/Chamfer 3D na aresta i;
//       "<op>:aux:<k>"                   faces de sólidos auxiliares (laje, semiespaço, ...).
//  2. Registrar a operação de origem: o prefixo do nome.
//  3. Rastrear modificações: a cada operação, os nomes das faces de entrada seguem o histórico —
//     face modificada (inclusive dividida) mantém o nome; face removida perde o nome; face
//     inalterada mantém o nome.
//  4. Remapear referências: a referência guarda o nome (e a assinatura geométrica). Ao
//     reconstruir, procura-se a face com esse nome no corpo referenciado.
//  5. Detectar ambiguidades: várias faces com o mesmo nome (face dividida) são desempatadas pela
//     assinatura geométrica com margem; sem margem, a referência é ambígua (recusada).
//  6. Invalidar referências não recuperáveis: nome ausente no corpo e nenhuma correspondência
//     geométrica exata e única → referência inválida, com mensagem (nunca se associa a outra
//     face em silêncio).
// Arestas: nome derivado das faces adjacentes ("<faceA>|<faceB>", em ordem), desempatado pela
// assinatura geométrica (EdgeNaming) quando várias arestas ligam as mesmas faces.
// Referências sem nome (documentos anteriores à E5.7.1) usam apenas a geometria, como antes, e
// recebem o nome na primeira reconstrução bem-sucedida.
#pragma once

#include "modeling/EdgeNaming.h"
#include "modeling/FaceNaming.h"
#include "modeling/Features.h"

#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>

#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace cad::modeling {

// Nome de cada face de um sólido.
class FaceNames
{
public:
    void add(const TopoDS_Shape& face, const std::string& name);
    // Nome da face (vazio se sem nome).
    std::string nameOf(const TopoDS_Shape& face) const;
    // Faces de 'shape' com o nome dado (orientação de 'shape').
    std::vector<TopoDS_Face> facesNamed(const TopoDS_Shape& shape, const std::string& name) const;
    bool empty() const { return m_items.empty(); }
    size_t size() const { return m_items.size(); }
    const std::vector<std::pair<TopoDS_Shape, std::string>>& items() const { return m_items; }

private:
    std::vector<std::pair<TopoDS_Shape, std::string>> m_items;
};

// Faces do bloco pela normal externa: "<prefix>:xmin" ... "<prefix>:zmax".
FaceNames nameBoxFaces(const TopoDS_Shape& box, const std::string& prefix);

// Nomes propagados pelo histórico de 'result': faces nomeadas das entradas que continuam no
// resultado (inalteradas ou modificadas) mantêm o nome; removidas são descartadas.
FaceNames propagateNames(const std::vector<const FaceNames*>& inputs, const FeatureResult& result);
// Faces geradas por arestas de entrada ('result.generated'): nome = labelFor(aresta) (vazio =
// ignorar). Faces já nomeadas não são renomeadas.
void nameGenerated(FaceNames& names, const FeatureResult& result,
                   const std::function<std::string(const TopoDS_Shape& edge)>& labelFor);
// Faces ainda sem nome recebem "<prefix>:<k>" (ordem do explorador).
void completeNames(FaceNames& names, const TopoDS_Shape& result, const std::string& prefix);

// Nome topológico de uma aresta do sólido (faces adjacentes nomeadas, em ordem; vazio se alguma
// face adjacente não tiver nome).
std::string edgeName(const TopoDS_Shape& solid, const FaceNames& names, const TopoDS_Edge& edge);

struct NamedFaceMatch
{
    bool ok = false;
    bool byName = false; // encontrada pelo nome topológico (false = correspondência geométrica)
    TopoDS_Face face;
    std::string message;
};
// Resolve uma face pelo nome (preferencial) e pela assinatura (desempate/legado). Referência sem
// nome: somente a geometria (resolvePlanarFace).
NamedFaceMatch resolveNamedFace(const TopoDS_Shape& body, const FaceNames& names, const FaceSignature& signature);

struct NamedEdgeMatch
{
    bool ok = false;
    bool byName = false;
    TopoDS_Edge edge;
    std::string message;
};
NamedEdgeMatch resolveNamedEdge(const TopoDS_Shape& body, const FaceNames& names, const EdgeSignature& signature);

} // namespace cad::modeling
