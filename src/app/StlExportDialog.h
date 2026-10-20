// Diálogo de exportação STL (E5.3): caminho, formato binário (padrão) ou ASCII, qualidade da malha
// e, opcionalmente, tolerâncias linear e angular. Mostra os parâmetros efetivos e avisa que o STL
// não guarda unidades (as coordenadas são gravadas em milímetros).
#pragma once

#include "io/StlExporter.h"

#include <QDialog>

#include <TopoDS_Shape.hxx>

class QCheckBox;
class QComboBox;
class QDialogButtonBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QRadioButton;

namespace cad::app {

class StlExportDialog : public QDialog
{
    Q_OBJECT

public:
    StlExportDialog(const QString& suggestedPath, const TopoDS_Shape& shape, QWidget* parent = nullptr);

    QString filePath() const; // com extensão .stl
    cad::io::StlExportOptions options() const;
    QString validationMessage() const;

public slots:
    void accept() override;

private:
    void browse();
    void updateParameters();

    TopoDS_Shape m_shape;
    QLineEdit* m_path = nullptr;
    QRadioButton* m_binary = nullptr;
    QRadioButton* m_ascii = nullptr;
    QComboBox* m_quality = nullptr;
    QCheckBox* m_custom = nullptr;
    QDoubleSpinBox* m_linear = nullptr;
    QDoubleSpinBox* m_angular = nullptr;
    QLabel* m_parameters = nullptr;
    QLabel* m_message = nullptr;
    QDialogButtonBox* m_buttons = nullptr;
};

} // namespace cad::app
