// Edição numérica das dimensões de uma entidade de esboço (coordenadas, largura/altura, centro
// e raio). Valores calculados (comprimento, diâmetro) são exibidos apenas para consulta.
#pragma once

#include "sketch/SketchEntity.h"

#include <QDialog>
#include <QMap>

class QDialogButtonBox;
class QDoubleSpinBox;
class QLabel;

namespace cad::app {

class SketchEntityDialog : public QDialog
{
    Q_OBJECT

public:
    explicit SketchEntityDialog(const cad::sketch::SketchEntity& entity, QWidget* parent = nullptr);

    // Geometria com os valores atuais dos campos (pode ser inválida antes de accept()).
    cad::sketch::SketchGeometry geometry() const;
    QString validationMessage() const;

public slots:
    void accept() override;

private:
    void updateComputed();

    cad::sketch::SketchEntity m_entity;
    QMap<QString, QDoubleSpinBox*> m_fields;
    QMap<QString, QLabel*> m_computed;
    QLabel* m_message = nullptr;
    QDialogButtonBox* m_buttons = nullptr;
};

} // namespace cad::app
