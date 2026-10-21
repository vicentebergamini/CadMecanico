// Diálogo não modal de Fillet 3D / Chamfer 3D (E5.5B). Enquanto está aberto, o visualizador fica
// no modo de seleção de arestas: o usuário clica nas arestas (Shift + clique adiciona/remove) e o
// diálogo mostra a quantidade selecionada, o corpo de origem, o raio/distância e o resultado
// avaliado (volume ou erro) com pré-visualização. "Limpar seleção" desmarca as arestas.
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
class QPushButton;

namespace cad::app {

class FinishDialog : public QDialog
{
    Q_OBJECT

public:
    // Avalia o acabamento com o valor dado e a seleção atual (sem alterar o documento).
    using Evaluator = std::function<cad::modeling::FeatureResult(double value)>;

    FinishDialog(cad::document::OperationType type, const QString& name, double value, bool editing,
                 QWidget* parent = nullptr);

    void setEvaluator(Evaluator evaluator);
    // Seleção atual: quantidade de arestas, descrição do corpo e problema (vazio = seleção válida).
    void setSelectionInfo(int edgeCount, const QString& body, const QString& problem);

    cad::document::OperationType type() const { return m_type; }
    double value() const;
    QString name() const;
    QString resultMessage() const;
    int edgeCount() const { return m_edgeCount; }

public slots:
    void accept() override;
    void evaluate();

signals:
    void clearSelectionRequested();
    void previewChanged(const cad::modeling::FeatureResult& result, bool show);

private:
    cad::document::OperationType m_type;
    Evaluator m_evaluator;
    QLineEdit* m_name = nullptr;
    QLabel* m_body = nullptr;
    QLabel* m_edges = nullptr;
    QDoubleSpinBox* m_value = nullptr;
    QCheckBox* m_preview = nullptr;
    QPushButton* m_clear = nullptr;
    QLabel* m_message = nullptr;
    QDialogButtonBox* m_buttons = nullptr;
    int m_edgeCount = 0;
    QString m_problem;
    bool m_lastOk = false;
};

} // namespace cad::app
