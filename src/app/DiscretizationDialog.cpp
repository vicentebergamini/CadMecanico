#include "app/DiscretizationDialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace cad::app {

using cad::discretization::InclusionCriterion;
using cad::discretization::LengthUnit;
using cad::discretization::Parameters;

DiscretizationDialog::DiscretizationDialog(const std::vector<Body>& bodies, const QString& preselected,
                                           const Parameters& initial, LengthUnit unit,
                                           const cad::viewer::DiscretizationDisplay& display, QWidget* parent)
    : QDialog(parent)
{
    setObjectName("dialogoDiscretizacao");
    setWindowTitle(tr("Discretização volumétrica"));

    m_body = new QComboBox;
    m_body->setObjectName("campoSolido");
    if (bodies.size() != 1)
        m_body->addItem(tr("— selecione o sólido —"), QString());
    for (const Body& b : bodies)
        m_body->addItem(b.description, b.id);
    const int pre = preselected.isEmpty() ? -1 : m_body->findData(preselected);
    m_body->setCurrentIndex(pre >= 0 ? pre : 0);

    m_lco = new QDoubleSpinBox;
    m_lco->setObjectName("campoLco");
    m_lco->setRange(0.001, 10000.0);
    m_lco->setDecimals(4);
    m_lco->setSingleStep(0.5);
    m_lco->setSuffix(tr(" mm"));
    m_lco->setValue(initial.lco);
    m_lco->setToolTip(tr("Comprimento da aresta dos cubos elementares, em milímetros (unidade do CAD); lco > 0"));

    m_criterion = new QComboBox;
    m_criterion->setObjectName("campoCriterio");
    for (InclusionCriterion c : {InclusionCriterion::FullyInside, InclusionCriterion::CenterInside,
                                 InclusionCriterion::PositiveIntersection})
        m_criterion->addItem(QString::fromUtf8(cad::discretization::criterionLabel(c)), int(c));
    m_criterion->setCurrentIndex(m_criterion->findData(int(initial.criterion)));

    m_unit = new QComboBox;
    m_unit->setObjectName("campoUnidade");
    m_unit->addItem(tr("Milímetros (mm)"), int(LengthUnit::Millimeter));
    m_unit->addItem(tr("Metros (m)"), int(LengthUnit::Meter));
    m_unit->setCurrentIndex(m_unit->findData(int(unit)));
    m_unit->setToolTip(tr("Unidade das coordenadas no arquivo ASCII (não altera IDs, ordem nem a geometria)"));

    auto* form = new QFormLayout;
    form->addRow(tr("Sólido selecionado:"), m_body);
    form->addRow(tr("Comprimento da aresta — lco:"), m_lco);
    form->addRow(tr("Critério de inclusão dos cubos:"), m_criterion);
    form->addRow(tr("Unidade da exportação:"), m_unit);

    m_estimate = new QLabel;
    m_estimate->setObjectName("rotuloEstimativa");
    m_estimate->setWordWrap(true);

    auto* system = new QLabel(tr("Grade alinhada aos eixos X, Y e Z do CAD, com origem no canto mínimo da caixa "
                                 "envolvente do sólido (coordenadas reais, sem deslocamento para a origem)."));
    system->setObjectName("rotuloSistemaCoordenadas");
    system->setWordWrap(true);

    m_progress = new QProgressBar;
    m_progress->setObjectName("barraProgresso");
    m_progress->setRange(0, 1000);
    m_progress->setValue(0);
    m_progress->setFormat(tr("%p%"));

    m_result = new QLabel;
    m_result->setObjectName("resultadoDiscretizacao");
    m_result->setWordWrap(true);
    m_result->setTextFormat(Qt::RichText);
    m_result->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_result->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    m_result->setText(tr("Nenhuma discretização gerada."));
    auto* resultArea = new QScrollArea;
    resultArea->setObjectName("areaResultado");
    resultArea->setWidget(m_result);
    resultArea->setWidgetResizable(true);
    resultArea->setFrameShape(QFrame::StyledPanel);
    resultArea->setMinimumHeight(280);

    m_message = new QLabel;
    m_message->setObjectName("mensagemDiscretizacao");
    m_message->setWordWrap(true);

    auto* note = new QLabel(tr("Limitação: os cubos aproximam superfícies curvas e inclinadas por degraus. A diferença "
                               "entre os volumes mede essa aproximação geométrica; não indica, necessariamente, erro "
                               "de cálculo."));
    note->setObjectName("notaAproximacao");
    note->setWordWrap(true);

    m_showSolid = new QCheckBox(tr("Sólido original"));
    m_showSolid->setObjectName("opcaoExibirSolido");
    m_showVertexNodes = new QCheckBox(tr("Nós dos vértices (azul)"));
    m_showVertexNodes->setObjectName("opcaoExibirNosVertices");
    m_showCenterNodes = new QCheckBox(tr("Nós centrais (vermelho)"));
    m_showCenterNodes->setObjectName("opcaoExibirNosCentrais");
    m_showEdges = new QCheckBox(tr("Contornos dos cubos"));
    m_showEdges->setObjectName("opcaoExibirContornos");
    setDisplay(display);
    auto* displayBox = new QGroupBox(tr("Exibição na janela 3D"));
    auto* displayLayout = new QHBoxLayout(displayBox);
    for (QCheckBox* c : {m_showSolid, m_showVertexNodes, m_showCenterNodes, m_showEdges})
    {
        displayLayout->addWidget(c);
        connect(c, &QCheckBox::toggled, this, &DiscretizationDialog::displayChanged);
    }

    m_generate = new QPushButton(tr("Gerar discretização"));
    m_generate->setObjectName("botaoGerar");
    m_generate->setDefault(true);
    m_export = new QPushButton(tr("Exportar ASCII..."));
    m_export->setObjectName("botaoExportar");
    m_cancel = new QPushButton(tr("Cancelar"));
    m_cancel->setObjectName("botaoCancelar");
    m_close = new QPushButton(tr("Fechar"));
    m_close->setObjectName("botaoFechar");
    connect(m_generate, &QPushButton::clicked, this, [this] {
        const QString problem = validationMessage();
        if (!problem.isEmpty())
        {
            setMessage(problem, true);
            return;
        }
        emit generateRequested();
    });
    connect(m_export, &QPushButton::clicked, this, &DiscretizationDialog::exportRequested);
    connect(m_cancel, &QPushButton::clicked, this, [this] {
        if (m_running)
            emit cancelRequested();
        else
            QDialog::reject();
    });
    connect(m_close, &QPushButton::clicked, this, &DiscretizationDialog::reject);
    auto* buttons = new QHBoxLayout;
    buttons->addWidget(m_generate);
    buttons->addWidget(m_export);
    buttons->addStretch();
    buttons->addWidget(m_cancel);
    buttons->addWidget(m_close);

    auto* layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addWidget(system);
    layout->addWidget(m_estimate);
    layout->addWidget(m_progress);
    layout->addWidget(resultArea, 1);
    layout->addWidget(note);
    layout->addWidget(displayBox);
    layout->addWidget(m_message);
    layout->addLayout(buttons);
    setMinimumWidth(620);
    resize(700, 760);

    auto changed = [this] {
        setMessage(QString(), false);
        updateState();
        emit parametersChanged();
    };
    connect(m_body, &QComboBox::currentIndexChanged, this, changed);
    connect(m_lco, &QDoubleSpinBox::valueChanged, this, changed);
    connect(m_criterion, &QComboBox::currentIndexChanged, this, changed);
    updateState();
}

QString DiscretizationDialog::bodyId() const
{
    return m_body->currentData().toString();
}

Parameters DiscretizationDialog::parameters() const
{
    Parameters p;
    p.lco = m_lco->value();
    p.criterion = InclusionCriterion(m_criterion->currentData().toInt());
    return p;
}

LengthUnit DiscretizationDialog::unit() const
{
    return LengthUnit(m_unit->currentData().toInt());
}

cad::viewer::DiscretizationDisplay DiscretizationDialog::display() const
{
    cad::viewer::DiscretizationDisplay d;
    d.solid = m_showSolid->isChecked();
    d.vertexNodes = m_showVertexNodes->isChecked();
    d.centerNodes = m_showCenterNodes->isChecked();
    d.cubeEdges = m_showEdges->isChecked();
    return d;
}

void DiscretizationDialog::setDisplay(const cad::viewer::DiscretizationDisplay& display)
{
    const QSignalBlocker b1(m_showSolid), b2(m_showVertexNodes), b3(m_showCenterNodes), b4(m_showEdges);
    m_showSolid->setChecked(display.solid);
    m_showVertexNodes->setChecked(display.vertexNodes);
    m_showCenterNodes->setChecked(display.centerNodes);
    m_showEdges->setChecked(display.cubeEdges);
}

QString DiscretizationDialog::validationMessage() const
{
    if (bodyId().isEmpty())
        return tr("Selecione o sólido que será discretizado.");
    if (!(m_lco->value() > 0.0))
        return tr("O comprimento da aresta lco deve ser maior que zero.");
    return QString();
}

void DiscretizationDialog::setEstimate(const QString& text, bool ok)
{
    m_estimate->setText(text);
    m_estimate->setStyleSheet(ok ? QString() : QStringLiteral("color: #b00020;"));
}

void DiscretizationDialog::setRunning(bool running)
{
    m_running = running;
    if (running)
        m_progress->setValue(0);
    updateState();
}

void DiscretizationDialog::setProgress(double fraction)
{
    m_progress->setValue(int(std::lround(std::clamp(fraction, 0.0, 1.0) * 1000.0)));
}

void DiscretizationDialog::setResult(const QString& html, const QString& bodyId, const Parameters& parameters)
{
    m_result->setText(html.isEmpty() ? tr("Nenhuma discretização gerada.") : html);
    m_hasResult = !html.isEmpty();
    m_resultBody = bodyId;
    m_resultParameters = parameters;
    if (m_hasResult)
        m_progress->setValue(1000);
    updateState();
}

void DiscretizationDialog::setMessage(const QString& text, bool error)
{
    m_message->setText(text);
    m_message->setStyleSheet(error ? QStringLiteral("color: #b00020;") : QString());
}

bool DiscretizationDialog::resultMatchesFields() const
{
    const Parameters p = parameters();
    return m_hasResult && bodyId() == m_resultBody && p.lco == m_resultParameters.lco
           && p.criterion == m_resultParameters.criterion;
}

void DiscretizationDialog::updateState()
{
    for (QWidget* w : std::initializer_list<QWidget*>{m_body, m_lco, m_criterion, m_unit, m_generate, m_close})
        w->setEnabled(!m_running);
    const bool current = resultMatchesFields();
    m_export->setEnabled(!m_running && current);
    m_export->setToolTip(current ? tr("Salva a tabela de nós (ID_node Coord_X Coord_Y Coord_Z)")
                                 : tr("Gere a discretização com os parâmetros atuais para exportar"));
    m_cancel->setToolTip(m_running ? tr("Interrompe a geração sem alterar o sólido")
                                   : tr("Fecha a janela sem alterar o sólido"));
    // Resultado de outros parâmetros continua visível, marcado como desatualizado.
    m_result->setEnabled(current || !m_hasResult);
}

void DiscretizationDialog::reject()
{
    if (m_running)
    {
        emit cancelRequested(); // fecha somente depois que a geração terminar
        return;
    }
    QDialog::reject();
}

} // namespace cad::app
