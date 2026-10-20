// Diálogo da extrusão (E5.3): nome, profundidade (mm), inversão do sentido e pré-visualização.
// O resultado de cada combinação de parâmetros é avaliado por uma função fornecida pela janela
// principal (CadDocument::previewExtrusion), que não altera o documento.
#pragma once

#include "document/CadDocument.h"
#include "modeling/Features.h"

#include <QDialog>

#include <functional>

class QCheckBox;
class QDialogButtonBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;

namespace cad::app {

class ExtrusionDialog : public QDialog
{
    Q_OBJECT

public:
    using Evaluator = std::function<cad::modeling::FeatureResult(const cad::document::ExtrudeParameters&)>;

    ExtrusionDialog(const cad::document::ExtrudeParameters& initial, const QString& sketchDescription,
                    Evaluator evaluator, bool editing, QWidget* parent = nullptr);

    cad::document::ExtrudeParameters parameters() const;
    bool previewEnabled() const;
    // Mensagem atual (volume ou erro).
    QString resultMessage() const;

public slots:
    void accept() override;

signals:
    // Parâmetros alterados e resultado avaliado (para a pré-visualização no visualizador).
    void previewChanged(const cad::modeling::FeatureResult& result, bool show);

private:
    void evaluate();

    cad::document::ExtrudeParameters m_initial;
    Evaluator m_evaluator;
    QLineEdit* m_name = nullptr;
    QDoubleSpinBox* m_depth = nullptr;
    QCheckBox* m_reversed = nullptr;
    QCheckBox* m_preview = nullptr;
    QLabel* m_message = nullptr;
    QDialogButtonBox* m_buttons = nullptr;
    bool m_lastOk = false;
};

} // namespace cad::app
