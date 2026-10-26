// Diálogo "Discretização volumétrica" (E5.7.2): sólido selecionado, comprimento da aresta lco
// (mm), critério de inclusão, unidade da exportação, progresso, resultados e opções de exibição.
// Não calcula nada: emite pedidos (gerar, exportar, cancelar) atendidos pela janela principal, que
// executa o cálculo fora da thread da interface e informa progresso e resultado.
//
// Botões: "Gerar discretização", "Exportar ASCII" (habilitado quando há resultado para os
// parâmetros atuais), "Cancelar" (interrompe a geração em andamento; sem geração, fecha a janela)
// e "Fechar". Durante a geração os parâmetros ficam bloqueados e fechar a janela pede o
// cancelamento. Nada disso altera o sólido.
#pragma once

#include "discretization/Discretization.h"
#include "viewer/OcctViewerWidget.h"

#include <QDialog>
#include <QString>

#include <vector>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QProgressBar;
class QPushButton;

namespace cad::app {

class DiscretizationDialog : public QDialog
{
    Q_OBJECT

public:
    struct Body
    {
        QString id;
        QString description;
    };

    // 'preselected' vazio com vários sólidos: o usuário precisa escolher explicitamente.
    DiscretizationDialog(const std::vector<Body>& bodies, const QString& preselected,
                         const cad::discretization::Parameters& initial, cad::discretization::LengthUnit unit,
                         const cad::viewer::DiscretizationDisplay& display, QWidget* parent = nullptr);

    QString bodyId() const; // vazio = nenhum sólido escolhido
    cad::discretization::Parameters parameters() const;
    cad::discretization::LengthUnit unit() const;
    cad::viewer::DiscretizationDisplay display() const;
    void setDisplay(const cad::viewer::DiscretizationDisplay& display);
    // Mensagem de validação dos campos (vazia se válidos).
    QString validationMessage() const;

    void setEstimate(const QString& text, bool ok);
    void setRunning(bool running);
    bool isRunning() const { return m_running; }
    void setProgress(double fraction);
    // Resultado da última geração (HTML) para o sólido e parâmetros dados; vazio limpa. A
    // exportação fica disponível enquanto os campos corresponderem ao resultado.
    void setResult(const QString& html, const QString& bodyId, const cad::discretization::Parameters& parameters);
    void setMessage(const QString& text, bool error);

signals:
    void generateRequested();
    void exportRequested();
    void cancelRequested();
    void displayChanged();
    void parametersChanged();

public slots:
    void reject() override;

private:
    void updateState();
    bool resultMatchesFields() const;

    QComboBox* m_body = nullptr;
    QDoubleSpinBox* m_lco = nullptr;
    QComboBox* m_criterion = nullptr;
    QComboBox* m_unit = nullptr;
    QLabel* m_estimate = nullptr;
    QProgressBar* m_progress = nullptr;
    QLabel* m_result = nullptr;
    QLabel* m_message = nullptr;
    QCheckBox* m_showSolid = nullptr;
    QCheckBox* m_showVertexNodes = nullptr;
    QCheckBox* m_showCenterNodes = nullptr;
    QCheckBox* m_showEdges = nullptr;
    QPushButton* m_generate = nullptr;
    QPushButton* m_export = nullptr;
    QPushButton* m_cancel = nullptr;
    QPushButton* m_close = nullptr;
    bool m_running = false;
    bool m_hasResult = false;
    QString m_resultBody;
    cad::discretization::Parameters m_resultParameters;
};

} // namespace cad::app
