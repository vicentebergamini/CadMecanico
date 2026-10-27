#include "app/DiscretizationDialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGuiApplication>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QResizeEvent>
#include <QShowEvent>
#include <QSplitter>
#include <QVBoxLayout>
#include <QWindow>

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
                                 InclusionCriterion::PositiveIntersection, InclusionCriterion::BoundaryProjection})
        m_criterion->addItem(QString::fromUtf8(cad::discretization::criterionLabel(c)), int(c));
    // E5.7.2.3: a quarta opção parte dos cubos com centro no material e projeta os nós de contorno
    // sobre as faces, arestas e vértices do sólido CAD original (células hexaédricas deformadas).
    m_criterion->setItemData(3,
                             tr("Parte dos cubos com centro no material e projeta os nós de contorno sobre a "
                                "superfície CAD original (faces, arestas vivas e vértices). As células tornam-se "
                                "hexaedros deformados, verificados quanto a inversão, volume e distorção; a "
                                "numeração e a conectividade continuam as da grade cartesiana."),
                             Qt::ToolTipRole);
    m_criterion->setCurrentIndex(m_criterion->findData(int(initial.criterion)));

    m_unit = new QComboBox;
    m_unit->setObjectName("campoUnidade");
    m_unit->addItem(tr("Milímetros (mm)"), int(LengthUnit::Millimeter));
    m_unit->addItem(tr("Metros (m)"), int(LengthUnit::Meter));
    m_unit->setCurrentIndex(m_unit->findData(int(unit)));
    m_unit->setToolTip(tr("Unidade das coordenadas no arquivo ASCII (não altera IDs, ordem nem a geometria)"));

    // Exportação ANSYS APDL (/PREP7) (E5.7.2.2): opções independentes da exportação ASCII.
    m_apdlUnit = new QComboBox;
    m_apdlUnit->setObjectName("campoUnidadeApdl");
    m_apdlUnit->addItem(tr("Metros (m)"), int(LengthUnit::Meter));
    m_apdlUnit->addItem(tr("Milímetros (mm)"), int(LengthUnit::Millimeter));
    m_apdlUnit->setCurrentIndex(0); // metros: reproduz o arquivo de exemplo
    m_apdlUnit->setToolTip(tr("Unidade das coordenadas dos comandos N no arquivo APDL (não altera IDs, ordem nem "
                              "a geometria). O APDL não impõe um sistema de unidades: materiais e cargas "
                              "acrescentados depois devem ser coerentes com esta unidade."));
    m_apdlClear = new QCheckBox(tr("Incluir /CLEAR no início do arquivo"));
    m_apdlClear->setObjectName("opcaoApdlClear");
    m_apdlClear->setChecked(true);
    m_apdlClear->setToolTip(tr("/CLEAR apaga todo o banco de dados corrente do ANSYS (modelo, nós, elementos e "
                               "resultados) quando o arquivo é lido"));
    auto* clearWarning = new QLabel(tr("Atenção: com /CLEAR, ler o arquivo no ANSYS apaga o banco de dados corrente "
                                       "(modelo e resultados em memória). Desmarque para acrescentar a geometria a "
                                       "um modelo já aberto."));
    clearWarning->setObjectName("avisoApdlClear");
    clearWarning->setWordWrap(true);
    auto* apdlBox = new QGroupBox(tr("Exportação ANSYS APDL (/PREP7)"));
    apdlBox->setObjectName("grupoExportacaoApdl");
    auto* apdlForm = new QFormLayout(apdlBox);
    apdlForm->addRow(tr("Unidade das coordenadas:"), m_apdlUnit);
    apdlForm->addRow(m_apdlClear);
    apdlForm->addRow(clearWarning);

    auto* form = new QFormLayout;
    form->addRow(tr("Sólido selecionado:"), m_body);
    form->addRow(tr("Comprimento da aresta — lco:"), m_lco);
    form->addRow(tr("Critério de inclusão dos cubos:"), m_criterion);
    form->addRow(tr("Unidade da exportação ASCII:"), m_unit);

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
    resultArea->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    resultArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    // E5.7.2.3.1: altura mínima pequena; as linhas excedentes são consultadas pela rolagem, sem
    // aumentar a janela.
    resultArea->setMinimumHeight(fontMetrics().lineSpacing() * 4);
    resultArea->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    m_resultArea = resultArea;

    m_message = new QLabel;
    m_message->setObjectName("mensagemDiscretizacao");
    m_message->setWordWrap(true);
    m_message->setVisible(false); // ocupa espaço somente quando há mensagem

    auto* note = new QLabel(tr("Limitação: os cubos aproximam superfícies curvas e inclinadas por degraus. A diferença "
                               "entre os volumes mede essa aproximação geométrica; não indica, necessariamente, erro "
                               "de cálculo. A projeção dos nós sobre o contorno reduz os degraus, mas altera "
                               "comprimentos e orientações das ligações."));
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
    m_exportApdl = new QPushButton(tr("Exportar ANSYS APDL..."));
    m_exportApdl->setObjectName("botaoExportarApdl");
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
    connect(m_exportApdl, &QPushButton::clicked, this, &DiscretizationDialog::apdlExportRequested);
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
    buttons->addWidget(m_exportApdl);
    buttons->addStretch();
    buttons->addWidget(m_cancel);
    buttons->addWidget(m_close);

    // E5.7.2.3.1: três regiões. Superior (configurações, sempre visível, sem rolagem); central
    // (resultados com rolagem vertical e, abaixo dela, as opções de exibição e da exportação APDL,
    // com rolagem própria quando a tela é pequena), separadas por um divisor ajustável; inferior
    // (mensagem e botões, fora de qualquer rolagem). A altura da janela não depende do volume de
    // resultados: o tamanho inicial é ajustado à área útil do monitor em showEvent().
    auto* options = new QWidget;
    options->setObjectName("conteudoOpcoes");
    auto* optionsLayout = new QVBoxLayout(options);
    optionsLayout->setContentsMargins(0, 0, 0, 0);
    optionsLayout->addWidget(system);
    optionsLayout->addWidget(note);
    optionsLayout->addWidget(displayBox);
    optionsLayout->addWidget(apdlBox);
    optionsLayout->addStretch();
    auto* optionsArea = new QScrollArea;
    optionsArea->setObjectName("areaOpcoes");
    optionsArea->setWidget(options);
    optionsArea->setWidgetResizable(true);
    optionsArea->setFrameShape(QFrame::NoFrame);
    optionsArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    optionsArea->setMinimumHeight(fontMetrics().lineSpacing() * 4);
    // Sem rolagem horizontal: a área nunca fica mais estreita que o conteúdo (caixas de seleção e
    // campos inteiros), somada a barra de rolagem vertical.
    optionsArea->setMinimumWidth(options->minimumSizeHint().width()
                                 + optionsArea->verticalScrollBar()->sizeHint().width());
    m_optionsArea = optionsArea;

    auto* splitter = new QSplitter(Qt::Vertical);
    splitter->setObjectName("divisorResultados");
    splitter->setChildrenCollapsible(false);
    splitter->addWidget(resultArea);
    splitter->addWidget(optionsArea);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 0);
    m_splitter = splitter;
    // A altura das opções é a escolhida pelo usuário ao arrastar o divisor (ou a do conteúdo);
    // mudanças no tamanho da janela vão para os resultados (applySplit()).
    connect(splitter, &QSplitter::splitterMoved, this, [this] { m_optionsHeight = m_optionsArea->height(); });

    auto* layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addWidget(m_estimate);
    layout->addWidget(m_progress);
    layout->addWidget(splitter, 1);
    layout->addWidget(m_message);
    layout->addLayout(buttons);
    setSizeGripEnabled(true);

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

QSize DiscretizationDialog::preferredSize() const
{
    // Largura para os controles sem cortes; altura de projeto (a de antes da E5.7.2.3.1), que
    // fitGeometry() limita à área útil do monitor.
    const QSize hint = sizeHint();
    return {std::max({hint.width(), minimumSizeHint().width(), 720}), std::max(hint.height(), 860)};
}

QRect DiscretizationDialog::fitGeometry(const QSize& preferred, const QSize& minimum, const QRect& available,
                                        const QMargins& frame)
{
    // Moldura (barra de título e bordas) incluída: a janela inteira cabe em ~90 % da altura útil e
    // ~95 % da largura útil, nunca menor que o mínimo do layout, e fica centralizada na área útil.
    const int frameW = frame.left() + frame.right();
    const int frameH = frame.top() + frame.bottom();
    const int maxW = int(available.width() * 0.95) - frameW;
    const int maxH = int(available.height() * 0.90) - frameH;
    const int w = std::max(minimum.width(), std::min(preferred.width(), maxW));
    const int h = std::max(minimum.height(), std::min(preferred.height(), maxH));
    int x = available.left() + (available.width() - (w + frameW)) / 2 + frame.left();
    int y = available.top() + (available.height() - (h + frameH)) / 2 + frame.top();
    // Se o mínimo do layout exceder a área útil, a barra de título continua dentro dela.
    x = std::max(x, available.left() + frame.left());
    y = std::max(y, available.top() + frame.top());
    return {x, y, w, h};
}

void DiscretizationDialog::showEvent(QShowEvent* event)
{
    if (!m_placed && !event->spontaneous())
    {
        m_placed = true;
        // Monitor atual: o da janela principal (se houver) ou o do próprio diálogo.
        QScreen* scr = parentWidget() ? parentWidget()->window()->screen() : screen();
        if (!scr)
            scr = QGuiApplication::primaryScreen();
        if (scr)
        {
            QMargins frame;
            if (QWindow* handle = windowHandle())
                frame = handle->frameMargins();
            if (frame.isNull()) // moldura ainda desconhecida: estimativa da barra de título
                frame = QMargins(8, fontMetrics().height() * 2 + 8, 8, 8);
            setGeometry(fitGeometry(preferredSize(), minimumSizeHint().expandedTo(minimumSize()),
                                    scr->availableGeometry(), frame));
            // Divisor: as opções recebem a altura do conteúdo, limitada à metade do divisor em telas
            // pequenas (o excedente rola), e os resultados o restante.
            if (QLayout* l = layout())
                l->activate();
            const int half = (m_splitter->height() - m_splitter->handleWidth()) / 2;
            m_optionsHeight = std::min(m_optionsArea->widget()->sizeHint().height() + 2,
                                       std::max(m_optionsArea->minimumHeight(), half));
            applySplit();
        }
    }
    QDialog::showEvent(event);
}

void DiscretizationDialog::resizeEvent(QResizeEvent* event)
{
    QDialog::resizeEvent(event); // o layout já redimensionou o divisor
    applySplit();
}

void DiscretizationDialog::applySplit()
{
    if (m_optionsHeight <= 0)
        return;
    // Ao reduzir a janela, os resultados diminuem primeiro (com rolagem) até a altura mínima; só
    // então as opções diminuem e passam a rolar. Ao ampliar, os resultados crescem.
    const int total = m_splitter->height() - m_splitter->handleWidth();
    const int resultMin = m_resultArea->minimumHeight();
    const int optionsMin = m_optionsArea->minimumHeight();
    const int options = std::clamp(m_optionsHeight, optionsMin, std::max(optionsMin, total - resultMin));
    const QList<int> sizes{std::max(resultMin, total - options), options};
    if (m_splitter->sizes() != sizes)
        m_splitter->setSizes(sizes);
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

void DiscretizationDialog::setApdlOptions(LengthUnit unit, bool includeClear)
{
    m_apdlUnit->setCurrentIndex(m_apdlUnit->findData(int(unit)));
    m_apdlClear->setChecked(includeClear);
}

LengthUnit DiscretizationDialog::apdlUnit() const
{
    return LengthUnit(m_apdlUnit->currentData().toInt());
}

bool DiscretizationDialog::apdlIncludeClear() const
{
    return m_apdlClear->isChecked();
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
    m_message->setVisible(!text.isEmpty());
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
    for (QWidget* w : std::initializer_list<QWidget*>{m_body, m_lco, m_criterion, m_unit, m_apdlUnit, m_apdlClear,
                                                      m_generate, m_close})
        w->setEnabled(!m_running);
    const bool current = resultMatchesFields();
    m_export->setEnabled(!m_running && current);
    m_export->setToolTip(current ? tr("Salva a tabela de nós (ID_node Coord_X Coord_Y Coord_Z)")
                                 : tr("Gere a discretização com os parâmetros atuais para exportar"));
    m_exportApdl->setEnabled(!m_running && current);
    m_exportApdl->setToolTip(current ? tr("Salva a geometria discretizada em comandos ANSYS APDL /PREP7 (N, ET,1,165 "
                                          "e EN de dois nós), sem propriedades físicas nem solução")
                                     : tr("Gere a discretização com os parâmetros atuais (lco, critério e sólido) "
                                          "para exportar"));
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
