// Diálogo das operações booleanas (E5.4): tipo (união, subtração, interseção), corpos A e B e
// nome. Na subtração A é o "corpo principal" e B a "ferramenta de corte". Cada combinação é
// avaliada por uma função da janela principal (CadDocument::previewBoolean), sem alterar o
// documento; o resultado (volume, número de sólidos ou erro) é mostrado no diálogo e os operandos
// são destacados no visualizador (A azul, B vermelho).
#pragma once

#include "document/CadDocument.h"
#include "modeling/Features.h"

#include <QDialog>

#include <functional>
#include <utility>
#include <vector>

class QComboBox;
class QDialogButtonBox;
class QLabel;
class QLineEdit;

namespace cad::app {

class BooleanDialog : public QDialog
{
    Q_OBJECT

public:
    using Evaluator = std::function<cad::modeling::FeatureResult(const cad::document::BooleanParameters&)>;
    // Corpos disponíveis: id e descrição ("Extrusão 1 (op-1)").
    using BodyList = std::vector<std::pair<std::string, QString>>;

    BooleanDialog(const cad::document::BooleanParameters& initial, const BodyList& bodies, Evaluator evaluator,
                  bool editing, QWidget* parent = nullptr);

    cad::document::BooleanParameters parameters() const;
    QString resultMessage() const;

    // Rótulos dos operandos para o tipo ("Corpo principal (A)" / "Ferramenta de corte (B)" na
    // subtração; "Corpo A" / "Corpo B" nas demais).
    static QString operandLabel(cad::modeling::BooleanKind kind, bool first);

public slots:
    void accept() override;

signals:
    // Operandos escolhidos (para o destaque no visualizador) e resultado avaliado.
    void operandsChanged(const QString& operandA, const QString& operandB);

private:
    void evaluate();

    cad::document::BooleanParameters m_initial;
    Evaluator m_evaluator;
    QLineEdit* m_name = nullptr;
    QComboBox* m_kind = nullptr;
    QComboBox* m_a = nullptr;
    QComboBox* m_b = nullptr;
    QLabel* m_labelA = nullptr;
    QLabel* m_labelB = nullptr;
    QLabel* m_message = nullptr;
    QDialogButtonBox* m_buttons = nullptr;
    bool m_lastOk = false;
};

} // namespace cad::app
