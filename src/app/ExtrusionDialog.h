// Diálogo da extrusão (E5.3/E5.6): nome, profundidade (mm), inversão do sentido e
// pré-visualização; na saliência e no corte também o corpo de destino e, no corte, a condição
// final (profundidade definida ou atravessar tudo). O resultado de cada combinação de parâmetros
// é avaliado por uma função fornecida pela janela principal (CadDocument::previewExtrusion), que
// não altera o documento.
//
// Sentido: novo corpo e saliência seguem a normal do plano local do esboço (para fora da face);
// o corte segue o sentido oposto (para dentro da face). "Inverter sentido" troca o sentido.
#pragma once

#include "document/CadDocument.h"
#include "modeling/Features.h"

#include <QDialog>

#include <functional>
#include <utility>
#include <vector>

class QCheckBox;
class QComboBox;
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
    using BodyList = std::vector<std::pair<std::string, QString>>;

    ExtrusionDialog(const cad::document::ExtrudeParameters& initial, const QString& sketchDescription,
                    Evaluator evaluator, bool editing, QWidget* parent = nullptr, const BodyList& bodies = {});

    cad::document::ExtrudeParameters parameters() const;
    bool previewEnabled() const;
    // Mensagem atual (volume ou erro).
    QString resultMessage() const;

    static QString modeTitle(cad::document::ExtrudeMode mode, bool editing);

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
    QComboBox* m_target = nullptr;
    QComboBox* m_end = nullptr;
    QDoubleSpinBox* m_depth = nullptr;
    QCheckBox* m_reversed = nullptr;
    QCheckBox* m_preview = nullptr;
    QLabel* m_direction = nullptr;
    QLabel* m_message = nullptr;
    QDialogButtonBox* m_buttons = nullptr;
    bool m_lastOk = false;
};

} // namespace cad::app