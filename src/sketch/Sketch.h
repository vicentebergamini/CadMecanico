// Esboço 2D: plano de referência e lista ordenada de entidades com identificadores estáveis.
// Sem dependência de Qt. Operações inválidas não alteram o esboço.
#pragma once

#include "sketch/SketchEntity.h"
#include "sketch/SketchPlane.h"

#include <string>
#include <vector>

namespace cad::sketch {

class Sketch
{
public:
    Sketch() = default;
    Sketch(std::string id, std::string name, PlaneType plane);

    const std::string& id() const { return m_id; }
    const std::string& name() const { return m_name; }
    void setName(const std::string& name) { m_name = name; }
    const SketchPlane& plane() const { return m_plane; }

    const std::vector<SketchEntity>& entities() const { return m_entities; }
    const SketchEntity* findEntity(const std::string& entityId) const;

    // Próximo número usado em identificadores de entidade ("e-N"); nunca diminui, de modo que
    // identificadores de entidades removidas não são reutilizados.
    int nextEntityNumber() const { return m_nextEntityNumber; }

    // Acrescenta a entidade (validada; modificadores de canto também precisam ser aplicáveis).
    // Retorna o novo id em 'newId'.
    EntityValidation addEntity(const SketchGeometry& geometry, std::string* newId = nullptr);
    // Substitui a geometria (mesmo tipo), mantendo o id.
    EntityValidation updateEntity(const std::string& entityId, const SketchGeometry& geometry);
    // Altera uma dimensão editável.
    EntityValidation setDimension(const std::string& entityId, const std::string& key, double value);
    // Recusado se a entidade for referenciada por um Fillet 2D ou Chamfer 2D.
    bool removeEntity(const std::string& entityId);
    bool removeEntity(const std::string& entityId, std::string& error);
    // Modificadores de canto que referenciam a entidade.
    std::vector<std::string> modifiersUsing(const std::string& entityId) const;
    // Modificadores de canto aplicáveis (referências existentes, cantos válidos, comprimentos
    // suficientes). Toda alteração bem-sucedida mantém o esboço resolúvel.
    EntityValidation checkResolvable() const;

    // Todas as dimensões das entidades, na ordem das entidades.
    std::vector<SketchDimension> dimensions() const;

    // Usado na leitura do arquivo: entidades com ids já definidos. Falha (sem alterar 'out')
    // se algum id for inválido/duplicado, alguma geometria for inválida ou o contador for menor
    // que algum id existente.
    static bool fromData(const std::string& id, const std::string& name, PlaneType plane,
                         const std::vector<SketchEntity>& entities, int nextEntityNumber, Sketch& out,
                         std::string& error);

private:
    std::string m_id;
    std::string m_name;
    SketchPlane m_plane;
    std::vector<SketchEntity> m_entities;
    int m_nextEntityNumber = 1;
};

// Número N de um identificador "e-N" (0 se o formato for outro).
int entityNumber(const std::string& entityId);

} // namespace cad::sketch
