// Diálogo "Discretização volumétrica" (E5.7.2): sólido selecionado, comprimento da aresta lco
// (mm), critério de inclusão, unidade da exportação, progresso, resultados e opções de exibição.
// Não calcula nada: emite pedidos (gerar, exportar, cancelar) atendidos pela janela principal, que
// executa o cálculo fora da thread da interface e informa progresso e resultado.
//
// Botões: "Gerar discretização", "Exportar ASCII" e "Exportar ANSYS APDL" (E5.7.2.2; ambos
// independentes e habilitados somente quando há resultado para os parâmetros atuais — alterar lco
// exige regenerar), "Cancelar" (interrompe a geração em andamento; sem geração, fecha a janela)
// e "Fechar". Durante a geração os parâmetros ficam bloqueados e fechar a janela pede o
// cancelamento. Nada disso altera o sólido.
//
// Dimensionamento (E5.7.2.3.1): o tamanho inicial é ajustado à área útil do monitor (no máximo
// ~90 % da altura), a janela é centralizada e pode ser redimensionada; os resultados têm rolagem
// vertical e os botões ficam sempre fora das áreas de rolagem.
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
class QScrollArea;
class QSplitter;

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
    // Opções da exportação ANSYS APDL (/PREP7): unidade das coordenadas e inclusão de /CLEAR.
    void setApdlOptions(cad::discretization::LengthUnit unit, bool includeClear);
    cad::discretization::LengthUnit apdlUnit() const;
    bool apdlIncludeClear() const;
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

    // Tamanho desejado (largura dos controles sem cortes e altura de projeto) antes de limitar à tela.
    QSize preferredSize() const;
    // Geometria inicial (área cliente) para a área útil 'available' do monitor, com a moldura
    // 'frame' da janela: limitada a ~90 % da altura e ~95 % da largura úteis, nunca abaixo de
    // 'minimum', centralizada e com a barra de título dentro da área útil.
    static QRect fitGeometry(const QSize& preferred, const QSize& minimum, const QRect& available,
                             const QMargins& frame);

signals:
    void generateRequested();
    void exportRequested();
    void apdlExportRequested();
    void cancelRequested();
    void displayChanged();
    void parametersChanged();

public slots:
    void reject() override;

protected:
    void showEvent(QShowEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    void updateState();
    void applySplit();
    bool resultMatchesFields() const;

    QComboBox* m_body = nullptr;
    QDoubleSpinBox* m_lco = nullptr;
    QComboBox* m_criterion = nullptr;
    QComboBox* m_unit = nullptr;
    QComboBox* m_apdlUnit = nullptr;
    QCheckBox* m_apdlClear = nullptr;
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
    QPushButton* m_exportApdl = nullptr;
    QPushButton* m_cancel = nullptr;
    QPushButton* m_close = nullptr;
    QScrollArea* m_resultArea = nullptr;
    QScrollArea* m_optionsArea = nullptr;
    QSplitter* m_splitter = nullptr;
    bool m_placed = false;
    int m_optionsHeight = 0; // altura desejada da área de opções no divisor (0 = antes de exibir)
    bool m_running = false;
    bool m_hasResult = false;
    QString m_resultBody;
    cad::discretization::Parameters m_resultParameters;
};

} // namespace cad::app
