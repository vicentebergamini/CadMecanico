// Diálogo de edição dos parâmetros da peça (mm). Só fecha com OK se os valores forem válidos.
#pragma once

#include "document/CadDocument.h"

#include <QDialog>

class QDialogButtonBox;
class QDoubleSpinBox;
class QLabel;

namespace cad::app {

class ParametersDialog : public QDialog
{
    Q_OBJECT

public:
    explicit ParametersDialog(const cad::document::PartParameters& current, QWidget* parent = nullptr);

    cad::document::PartParameters parameters() const;
    void setParameters(const cad::document::PartParameters& parameters);

    // Mensagem da última validação recusada (vazia se nenhuma).
    QString validationMessage() const;

public slots:
    void accept() override;

private:
    QDoubleSpinBox* makeField(const char* objectName);

    QDoubleSpinBox* m_length = nullptr;
    QDoubleSpinBox* m_width = nullptr;
    QDoubleSpinBox* m_height = nullptr;
    QDoubleSpinBox* m_holeRadius = nullptr;
    QLabel* m_message = nullptr;
    QDialogButtonBox* m_buttons = nullptr;
};

} // namespace cad::app
