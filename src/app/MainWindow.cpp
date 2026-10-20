#include "app/MainWindow.h"

#include "app/ParametersDialog.h"
#include "document/DocumentFile.h"
#include "io/StepExporter.h"
#include "modeling/SubShapeInfo.h"
#include "viewer/OcctViewerWidget.h"

#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QDir>
#include <QDockWidget>
#include <QFileDialog>
#include <QFileInfo>
#include <QLabel>
#include <QLocale>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QStandardPaths>
#include <QStatusBar>
#include <QStyle>
#include <QToolBar>
#include <QTreeWidget>

#include <Standard_Version.hxx>

#include <filesystem>

namespace cad::app {

using cad::document::CadDocument;
using cad::document::DocumentFile;
using cad::document::Operation;
using cad::document::OperationStatus;
using cad::document::OperationType;
using cad::document::PartParameters;

namespace {

QString mm(double v)
{
    return QLocale(QLocale::Portuguese, QLocale::Brazil).toString(v, 'f', 2) + QStringLiteral(" mm");
}

QString parameterLabel(const std::string& key)
{
    if (key == "length") return MainWindow::tr("Comprimento");
    if (key == "width")  return MainWindow::tr("Largura");
    if (key == "height") return MainWindow::tr("Altura");
    if (key == "radius") return MainWindow::tr("Raio");
    return QString::fromStdString(key);
}

QString operationTypeName(OperationType type)
{
    switch (type)
    {
    case OperationType::Block:       return MainWindow::tr("Bloco");
    case OperationType::ThroughHole: return MainWindow::tr("Furo passante");
    }
    return QString();
}

QString statusName(OperationStatus status)
{
    return status == OperationStatus::Valid ? MainWindow::tr("Válida") : MainWindow::tr("Inválida");
}

} // namespace

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
    , m_lastExportDir(QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation))
    , m_lastDocumentDir(QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation))
{
    setObjectName("MainWindow");

    createActions();
    createMenus();
    createToolBars();
    createDocks();
    createCentralArea();

    m_readyMessage = tr("Pronto  |  Qt %1  |  Open CASCADE %2").arg(qVersion(), OCC_VERSION_COMPLETE);
    statusBar()->showMessage(m_readyMessage);
    refreshDocumentViews(true);
}

MainWindow::~MainWindow() = default;

void MainWindow::createActions()
{
    QStyle* st = style();

    // Arquivo
    m_actNew = new QAction(st->standardIcon(QStyle::SP_FileIcon), tr("&Novo"), this);
    m_actNew->setObjectName("actionNovo");
    m_actNew->setShortcut(QKeySequence::New);
    m_actNew->setStatusTip(tr("Cria um novo documento com a peça padrão"));
    connect(m_actNew, &QAction::triggered, this, &MainWindow::newDocument);

    m_actOpen = new QAction(st->standardIcon(QStyle::SP_DialogOpenButton), tr("&Abrir..."), this);
    m_actOpen->setObjectName("actionAbrir");
    m_actOpen->setShortcut(QKeySequence::Open);
    m_actOpen->setStatusTip(tr("Abre um documento CadMecanico (.cadmec)"));
    connect(m_actOpen, &QAction::triggered, this, &MainWindow::openDocument);

    m_actSave = new QAction(tr("&Salvar"), this);
    m_actSave->setObjectName("actionSalvar");
    m_actSave->setShortcut(QKeySequence::Save);
    m_actSave->setStatusTip(tr("Salva o documento"));
    connect(m_actSave, &QAction::triggered, this, &MainWindow::saveDocument);

    m_actSaveAs = new QAction(tr("Salvar &como..."), this);
    m_actSaveAs->setObjectName("actionSalvarComo");
    m_actSaveAs->setShortcut(QKeySequence(tr("Ctrl+Shift+S")));
    m_actSaveAs->setStatusTip(tr("Salva o documento em outro arquivo"));
    connect(m_actSaveAs, &QAction::triggered, this, &MainWindow::saveDocumentAs);

    m_actExportStep = new QAction(st->standardIcon(QStyle::SP_DialogSaveButton), tr("&Exportar STEP..."), this);
    m_actExportStep->setObjectName("actionExportarStep");
    m_actExportStep->setShortcut(QKeySequence(tr("Ctrl+E")));
    m_actExportStep->setStatusTip(tr("Exporta a peça atual em formato STEP"));
    connect(m_actExportStep, &QAction::triggered, this, &MainWindow::exportStep);

    m_actQuit = new QAction(st->standardIcon(QStyle::SP_DialogCloseButton), tr("&Sair"), this);
    m_actQuit->setObjectName("actionSair");
    m_actQuit->setShortcut(QKeySequence(tr("Ctrl+Q")));
    m_actQuit->setStatusTip(tr("Encerra o CAD Mecânico"));
    connect(m_actQuit, &QAction::triggered, this, &QWidget::close);

    // Editar
    m_actUndo = new QAction(tr("&Desfazer"), this);
    m_actUndo->setObjectName("actionDesfazer");
    m_actUndo->setShortcut(QKeySequence::Undo);
    m_actUndo->setEnabled(false);

    m_actRedo = new QAction(tr("&Refazer"), this);
    m_actRedo->setObjectName("actionRefazer");
    m_actRedo->setShortcut(QKeySequence::Redo);
    m_actRedo->setEnabled(false);

    m_actEditParameters = new QAction(st->standardIcon(QStyle::SP_FileDialogDetailedView),
                                      tr("&Parâmetros da peça..."), this);
    m_actEditParameters->setObjectName("actionEditarParametros");
    m_actEditParameters->setStatusTip(tr("Edita comprimento, largura, altura e raio do furo"));
    connect(m_actEditParameters, &QAction::triggered, this, &MainWindow::editParameters);

    // Visualizar (conectados ao visualizador 3D em createCentralArea())
    m_actFitAll = new QAction(tr("&Ajustar à janela"), this);
    m_actFitAll->setObjectName("actionAjustarJanela");
    m_actFitAll->setShortcut(QKeySequence(Qt::Key_F));
    m_actFitAll->setStatusTip(tr("Enquadra a peça inteira na janela (F)"));

    auto makeViewAction = [this](const QString& text, const char* name, const QString& tip) {
        auto* act = new QAction(text, this);
        act->setObjectName(name);
        act->setStatusTip(tip);
        return act;
    };
    m_actViewFront = makeViewAction(tr("&Frente"), "actionVistaFrente", tr("Vista frontal (ortográfica)"));
    m_actViewTop   = makeViewAction(tr("&Superior"), "actionVistaSuperior", tr("Vista superior (ortográfica)"));
    m_actViewRight = makeViewAction(tr("&Direita"), "actionVistaDireita", tr("Vista lateral direita (ortográfica)"));
    m_actViewIso   = makeViewAction(tr("&Isométrica"), "actionVistaIsometrica", tr("Vista isométrica"));

    // Ajuda
    m_actAbout = new QAction(tr("&Sobre o CAD Mecânico"), this);
    m_actAbout->setObjectName("actionSobre");
    connect(m_actAbout, &QAction::triggered, this, &MainWindow::showAbout);
}

void MainWindow::createMenus()
{
    QMenu* menuFile = menuBar()->addMenu(tr("&Arquivo"));
    menuFile->setObjectName("menuArquivo");
    menuFile->addAction(m_actNew);
    menuFile->addAction(m_actOpen);
    menuFile->addAction(m_actSave);
    menuFile->addAction(m_actSaveAs);
    menuFile->addSeparator();
    menuFile->addAction(m_actExportStep);
    menuFile->addSeparator();
    menuFile->addAction(m_actQuit);

    QMenu* menuEdit = menuBar()->addMenu(tr("&Editar"));
    menuEdit->setObjectName("menuEditar");
    menuEdit->addAction(m_actUndo);
    menuEdit->addAction(m_actRedo);
    menuEdit->addSeparator();
    menuEdit->addAction(m_actEditParameters);

    QMenu* menuView = menuBar()->addMenu(tr("&Visualizar"));
    menuView->setObjectName("menuVisualizar");
    menuView->addAction(m_actFitAll);
    m_menuStandardViews = menuView->addMenu(tr("&Vistas padrão"));
    m_menuStandardViews->setObjectName("menuVistasPadrao");
    m_menuStandardViews->addAction(m_actViewFront);
    m_menuStandardViews->addAction(m_actViewTop);
    m_menuStandardViews->addAction(m_actViewRight);
    m_menuStandardViews->addAction(m_actViewIso);
    menuView->addSeparator();
    // As ações de mostrar/ocultar os painéis são adicionadas em createDocks().

    QMenu* menuHelp = menuBar()->addMenu(tr("A&juda"));
    menuHelp->setObjectName("menuAjuda");
    menuHelp->addAction(m_actAbout);
}

void MainWindow::createToolBars()
{
    m_toolBarFile = addToolBar(tr("Arquivo"));
    m_toolBarFile->setObjectName("toolBarArquivo");
    m_toolBarFile->addAction(m_actNew);
    m_toolBarFile->addAction(m_actOpen);
    m_toolBarFile->addAction(m_actExportStep);
    m_toolBarFile->addAction(m_actQuit);

    // Ferramentas de modelagem (esboço, extrusão, furo... nas próximas etapas).
    m_toolBarModeling = addToolBar(tr("Modelagem"));
    m_toolBarModeling->setObjectName("toolBarModelagem");
    m_toolBarModeling->addAction(m_actEditParameters);

    m_toolBarView = addToolBar(tr("Visualizar"));
    m_toolBarView->setObjectName("toolBarVisualizar");
    m_toolBarView->addAction(m_actFitAll);
    m_toolBarView->addSeparator();
    m_toolBarView->addAction(m_actViewFront);
    m_toolBarView->addAction(m_actViewTop);
    m_toolBarView->addAction(m_actViewRight);
    m_toolBarView->addAction(m_actViewIso);
}

void MainWindow::createDocks()
{
    m_operationsTree = new QTreeWidget;
    m_operationsTree->setObjectName("arvoreOperacoes");
    m_operationsTree->setHeaderHidden(true);
    m_operationsTree->setSelectionMode(QAbstractItemView::SingleSelection);
    connect(m_operationsTree, &QTreeWidget::currentItemChanged, this,
            [this](QTreeWidgetItem* current, QTreeWidgetItem*) { showItemProperties(current); });
    connect(m_operationsTree, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* item, int) {
        if (item && !item->data(0, kItemIdRole).toString().isEmpty())
            editParameters();
    });

    m_dockOperations = new QDockWidget(tr("Árvore de Operações"), this);
    m_dockOperations->setObjectName("dockArvoreOperacoes");
    m_dockOperations->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
    m_dockOperations->setWidget(m_operationsTree);
    addDockWidget(Qt::LeftDockWidgetArea, m_dockOperations);

    m_properties = new QLabel;
    m_properties->setObjectName("painelPropriedades");
    m_properties->setTextFormat(Qt::RichText);
    m_properties->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    m_properties->setWordWrap(true);
    m_properties->setMargin(6);
    m_properties->setTextInteractionFlags(Qt::TextSelectableByMouse);
    // Caminhos longos não devem alargar o painel.
    m_properties->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);

    m_dockProperties = new QDockWidget(tr("Propriedades"), this);
    m_dockProperties->setObjectName("dockPropriedades");
    m_dockProperties->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
    m_dockProperties->setWidget(m_properties);
    addDockWidget(Qt::LeftDockWidgetArea, m_dockProperties);
    splitDockWidget(m_dockOperations, m_dockProperties, Qt::Vertical);

    if (auto* menuView = menuBar()->findChild<QMenu*>("menuVisualizar"))
    {
        menuView->addAction(m_dockOperations->toggleViewAction());
        menuView->addAction(m_dockProperties->toggleViewAction());
    }
}

void MainWindow::createCentralArea()
{
    m_viewer = new cad::viewer::OcctViewerWidget;
    setCentralWidget(m_viewer);

    connect(m_actFitAll, &QAction::triggered, m_viewer, &cad::viewer::OcctViewerWidget::fitAll);
    connect(m_actViewFront, &QAction::triggered, m_viewer, &cad::viewer::OcctViewerWidget::viewFront);
    connect(m_actViewTop, &QAction::triggered, m_viewer, &cad::viewer::OcctViewerWidget::viewTop);
    connect(m_actViewRight, &QAction::triggered, m_viewer, &cad::viewer::OcctViewerWidget::viewRight);
    connect(m_actViewIso, &QAction::triggered, m_viewer, &cad::viewer::OcctViewerWidget::viewIsometric);
    connect(m_viewer, &cad::viewer::OcctViewerWidget::initializationFailed, this, [this](const QString& message) {
        statusBar()->showMessage(message);
    });
    connect(m_viewer, &cad::viewer::OcctViewerWidget::selectionChanged, this, &MainWindow::updateSelectionStatus);
}

// --- Documento ---------------------------------------------------------------

void MainWindow::refreshDocumentViews(bool geometryChanged)
{
    if (geometryChanged)
    {
        m_viewer->setShape(m_document.shape());
        updateSelectionStatus();
    }
    rebuildTree();
    updateTitle();
}

void MainWindow::updateTitle()
{
    setWindowTitle(tr("%1[*] — CAD Mecânico").arg(QString::fromStdString(m_document.name())));
    setWindowModified(m_document.isModified());
}

void MainWindow::rebuildTree()
{
    // Mantém a operação selecionada (pelo id estável) após a reconstrução da árvore.
    QString selectedId;
    QString selectedKind;
    if (QTreeWidgetItem* current = m_operationsTree->currentItem())
    {
        selectedId = current->data(0, kItemIdRole).toString();
        selectedKind = current->data(0, kItemKindRole).toString();
    }

    m_operationsTree->clear();
    QStyle* st = style();

    auto* root = new QTreeWidgetItem(m_operationsTree);
    root->setText(0, QString::fromStdString(m_document.name()) + (m_document.isModified() ? QStringLiteral(" *") : QString()));
    root->setIcon(0, st->standardIcon(QStyle::SP_FileIcon));
    root->setData(0, kItemKindRole, QStringLiteral("documento"));
    QTreeWidgetItem* toSelect = selectedKind == QLatin1String("documento") ? root : nullptr;

    for (const Operation& op : m_document.operations())
    {
        const QString id = QString::fromStdString(op.id);
        auto* item = new QTreeWidgetItem(root);
        item->setText(0, QString::fromStdString(op.name));
        item->setIcon(0, st->standardIcon(op.status == OperationStatus::Valid ? QStyle::SP_DialogApplyButton
                                                                              : QStyle::SP_MessageBoxWarning));
        item->setData(0, kItemKindRole, QStringLiteral("operacao"));
        item->setData(0, kItemIdRole, id);
        item->setToolTip(0, tr("%1 (%2) — %3").arg(QString::fromStdString(op.name), id, statusName(op.status)));

        for (const auto& p : op.parameters)
        {
            auto* param = new QTreeWidgetItem(item);
            param->setText(0, tr("%1: %2").arg(parameterLabel(p.key), mm(p.value)));
            param->setData(0, kItemKindRole, QStringLiteral("parametro"));
            param->setData(0, kItemIdRole, id);
        }
        auto* status = new QTreeWidgetItem(item);
        status->setText(0, tr("Estado: %1").arg(statusName(op.status)));
        status->setData(0, kItemKindRole, QStringLiteral("estado"));
        status->setData(0, kItemIdRole, id);

        if (!toSelect && id == selectedId)
            toSelect = item;
    }
    m_operationsTree->expandAll();
    m_operationsTree->setCurrentItem(toSelect ? toSelect : root);
    showItemProperties(m_operationsTree->currentItem());
}

void MainWindow::showItemProperties(QTreeWidgetItem* item)
{
    if (!item)
    {
        m_properties->clear();
        return;
    }
    const QString id = item->data(0, kItemIdRole).toString();
    const Operation* op = id.isEmpty() ? nullptr : m_document.findOperation(id.toStdString());
    QString html;
    if (op)
    {
        html = tr("<b>%1</b><br>Identificador: %2<br>Tipo: %3<br>Estado: %4")
                   .arg(QString::fromStdString(op->name).toHtmlEscaped(), id.toHtmlEscaped(),
                        operationTypeName(op->type), statusName(op->status));
        if (!op->statusMessage.empty())
            html += tr("<br>Mensagem: %1").arg(QString::fromStdString(op->statusMessage).toHtmlEscaped());
        for (const auto& p : op->parameters)
            html += tr("<br>%1: %2").arg(parameterLabel(p.key), mm(p.value));
        if (op->type == OperationType::ThroughHole)
        {
            const PartParameters pp = m_document.parameters();
            html += tr("<br>Diâmetro: %1<br>Centro: X %2, Y %3 (centro do bloco)")
                        .arg(mm(2.0 * pp.holeRadius), mm(pp.length / 2.0), mm(pp.width / 2.0));
        }
    }
    else
    {
        html = tr("<b>%1</b><br>Unidade: %2<br>Operações: %3<br>Arquivo: %4<br>Alterações não salvas: %5")
                   .arg(QString::fromStdString(m_document.name()).toHtmlEscaped(), QString::fromLatin1(m_document.unit()))
                   .arg(m_document.operations().size())
                   .arg(m_documentPath.isEmpty() ? tr("(não salvo)") : QDir::toNativeSeparators(m_documentPath).toHtmlEscaped(),
                        m_document.isModified() ? tr("sim") : tr("não"));
    }
    m_properties->setText(html);
}

bool MainWindow::applyParameters(const PartParameters& parameters)
{
    const auto result = m_document.setParameters(parameters);
    if (!result.ok)
    {
        statusBar()->showMessage(tr("Parâmetros recusados: %1").arg(QString::fromStdString(result.message)), 8000);
        reportError(tr("Parâmetros da peça"),
                    tr("Os parâmetros não foram aplicados:\n%1\n\nA peça foi mantida no último estado válido.")
                        .arg(QString::fromStdString(result.message)));
        return false;
    }
    refreshDocumentViews(true);
    statusBar()->showMessage(tr("Peça recalculada"), 5000);
    return true;
}

void MainWindow::editParameters()
{
    const auto parameters = askParameters(m_document.parameters());
    if (!parameters)
    {
        statusBar()->showMessage(tr("Edição de parâmetros cancelada"), 5000);
        return;
    }
    if (*parameters == m_document.parameters())
        return;
    applyParameters(*parameters);
}

void MainWindow::newDocument()
{
    if (!maybeSaveChanges())
        return;
    m_document = CadDocument(tr("Nova peça").toStdString());
    m_documentPath.clear();
    refreshDocumentViews(true);
    statusBar()->showMessage(tr("Novo documento"), 5000);
}

void MainWindow::openDocument()
{
    if (!maybeSaveChanges())
        return;
    const QString path = askOpenPath(documentDirectory());
    if (path.isEmpty())
    {
        statusBar()->showMessage(tr("Abertura cancelada"), 5000);
        return;
    }
    openDocumentFrom(path);
}

bool MainWindow::openDocumentFrom(const QString& filePath)
{
    CadDocument loaded;
    const auto result = DocumentFile::load(filePath, loaded);
    const QString nativePath = QDir::toNativeSeparators(filePath);
    if (!result.ok)
    {
        statusBar()->showMessage(tr("Falha ao abrir o documento"), 5000);
        reportError(tr("Abrir documento"),
                    tr("Não foi possível abrir o arquivo:\n%1\n\n%2\n\nO documento atual foi mantido.")
                        .arg(nativePath, result.message));
        return false;
    }
    m_document = std::move(loaded);
    m_documentPath = QFileInfo(filePath).absoluteFilePath();
    m_lastDocumentDir = QFileInfo(filePath).absolutePath();
    refreshDocumentViews(true);
    statusBar()->showMessage(tr("Documento aberto: %1").arg(nativePath), 5000);
    return true;
}

bool MainWindow::saveDocument()
{
    if (m_documentPath.isEmpty())
        return saveDocumentAs();
    return writeDocument(m_documentPath);
}

bool MainWindow::saveDocumentAs()
{
    const QString suggested = QDir(documentDirectory()).filePath(QString::fromStdString(m_document.name()) + ".cadmec");
    QString path = askSavePath(suggested);
    if (path.isEmpty())
    {
        statusBar()->showMessage(tr("Salvamento cancelado"), 5000);
        return false;
    }
    if (QFileInfo(path).suffix().isEmpty())
        path += QStringLiteral(".cadmec");
    return saveDocumentTo(path);
}

bool MainWindow::saveDocumentTo(const QString& filePath)
{
    if (QFileInfo::exists(filePath) && QFileInfo(filePath).absoluteFilePath() != m_documentPath
        && !confirmOverwrite(QDir::toNativeSeparators(filePath)))
    {
        statusBar()->showMessage(tr("Salvamento cancelado: arquivo existente mantido"), 5000);
        return false;
    }
    return writeDocument(filePath);
}

bool MainWindow::writeDocument(const QString& filePath)
{
    const QString nativePath = QDir::toNativeSeparators(filePath);
    const auto result = DocumentFile::save(m_document, filePath);
    if (!result.ok)
    {
        statusBar()->showMessage(tr("Falha ao salvar o documento"), 5000);
        reportError(tr("Salvar documento"), tr("Não foi possível salvar:\n%1\n\n%2").arg(nativePath, result.message));
        return false;
    }
    m_document.setModified(false);
    m_documentPath = QFileInfo(filePath).absoluteFilePath();
    m_lastDocumentDir = QFileInfo(filePath).absolutePath();
    refreshDocumentViews(false);
    statusBar()->showMessage(tr("Documento salvo: %1").arg(nativePath), 5000);
    return true;
}

bool MainWindow::maybeSaveChanges()
{
    if (!m_document.isModified())
        return true;
    switch (askSaveChanges())
    {
    case SaveChoice::Save:    return saveDocument();
    case SaveChoice::Discard: return true;
    case SaveChoice::Cancel:  break;
    }
    return false;
}

QString MainWindow::documentDirectory() const
{
    return m_documentPath.isEmpty() ? m_lastDocumentDir : QFileInfo(m_documentPath).absolutePath();
}

void MainWindow::closeEvent(QCloseEvent* event)
{
    if (maybeSaveChanges())
        event->accept();
    else
        event->ignore();
}

// --- Seleção no visualizador -------------------------------------------------

namespace {

QString geometryName(cad::modeling::GeometryKind kind)
{
    using cad::modeling::GeometryKind;
    switch (kind)
    {
    case GeometryKind::Plane:        return MainWindow::tr("Plana");
    case GeometryKind::Cylinder:     return MainWindow::tr("Cilíndrica");
    case GeometryKind::Cone:         return MainWindow::tr("Cônica");
    case GeometryKind::Sphere:       return MainWindow::tr("Esférica");
    case GeometryKind::Torus:        return MainWindow::tr("Toroidal");
    case GeometryKind::OtherSurface: return MainWindow::tr("Superfície livre");
    case GeometryKind::Line:         return MainWindow::tr("Reta");
    case GeometryKind::Circle:       return MainWindow::tr("Circular");
    case GeometryKind::Ellipse:      return MainWindow::tr("Elíptica");
    case GeometryKind::OtherCurve:   return MainWindow::tr("Curva livre");
    case GeometryKind::Point:
    case GeometryKind::None:         break;
    }
    return QString();
}

} // namespace

void MainWindow::updateSelectionStatus()
{
    using cad::modeling::SubShapeKind;

    const QList<TopoDS_Shape> selected = m_viewer->selectedShapes();
    if (selected.isEmpty())
    {
        statusBar()->showMessage(m_readyMessage);
        return;
    }
    if (selected.size() > 1)
    {
        statusBar()->showMessage(tr("%1 entidades selecionadas").arg(selected.size()));
        return;
    }

    const QLocale locale(QLocale::Portuguese, QLocale::Brazil);
    auto num = [&locale](double v) { return locale.toString(v, 'f', 2); };

    const auto info = cad::modeling::describeSubShape(part(), selected.first());
    QString geometry = geometryName(info.geometry);
    if (info.radius > 0.0)
        geometry += tr(" (R %1 mm)").arg(num(info.radius));

    QString text;
    switch (info.kind)
    {
    case SubShapeKind::Face:
        text = tr("Face %1/%2  |  %3  |  Área: %4 mm²").arg(info.index).arg(info.count).arg(geometry, num(info.area));
        break;
    case SubShapeKind::Edge:
        text = tr("Aresta %1/%2  |  %3  |  Comprimento: %4 mm")
                   .arg(info.index).arg(info.count).arg(geometry, num(info.length));
        break;
    case SubShapeKind::Vertex:
        text = tr("Vértice %1/%2  |  X %3  Y %4  Z %5 mm")
                   .arg(info.index).arg(info.count)
                   .arg(num(info.point.X()), num(info.point.Y()), num(info.point.Z()));
        break;
    default:
        text = tr("Entidade selecionada");
        break;
    }
    statusBar()->showMessage(text);
}

// --- Exportação STEP ---------------------------------------------------------

void MainWindow::exportStep()
{
    const QString suggested = QDir(m_lastExportDir).filePath("peca_teste.step");
    QString path = askExportPath(suggested);
    if (path.isEmpty())
    {
        statusBar()->showMessage(tr("Exportação cancelada"), 5000);
        return;
    }

    if (QFileInfo(path).suffix().isEmpty())
        path += ".step";

    if (exportStepTo(path))
        m_lastExportDir = QFileInfo(path).absolutePath();
}

bool MainWindow::exportStepTo(const QString& filePath)
{
    const QString nativePath = QDir::toNativeSeparators(filePath);

    if (QFileInfo::exists(filePath) && !confirmOverwrite(nativePath))
    {
        statusBar()->showMessage(tr("Exportação cancelada: arquivo existente mantido"), 5000);
        return false;
    }

    QApplication::setOverrideCursor(Qt::WaitCursor);
    const auto result = cad::io::StepExporter().write(part(), std::filesystem::path(filePath.toStdWString()));
    QApplication::restoreOverrideCursor();

    if (!result.ok)
    {
        statusBar()->showMessage(tr("Falha na exportação STEP"), 5000);
        reportError(tr("Exportar STEP"),
                    tr("Não foi possível exportar o arquivo:\n%1\n\n%2")
                        .arg(nativePath, QString::fromStdString(result.message)));
        return false;
    }

    statusBar()->showMessage(tr("STEP exportado: %1").arg(nativePath), 5000);
    reportInfo(tr("Exportar STEP"), tr("Arquivo exportado com sucesso:\n%1").arg(nativePath));
    return true;
}

void MainWindow::showAbout()
{
    QMessageBox::about(this, tr("Sobre o CAD Mecânico"),
                       tr("<h3>CAD Mecânico %1</h3>"
                          "<p>Modelador CAD mecânico em desenvolvimento.</p>"
                          "<p>Qt %2<br>Open CASCADE %3</p>")
                           .arg(QApplication::applicationVersion(), qVersion(), OCC_VERSION_COMPLETE));
}

// --- Interação com o usuário (substituível nos testes) -----------------------

QString MainWindow::askExportPath(const QString& suggestedPath)
{
    // A confirmação de sobrescrita é feita em exportStepTo().
    return QFileDialog::getSaveFileName(this, tr("Exportar STEP"), suggestedPath,
                                        tr("Arquivos STEP (*.step *.stp)"), nullptr,
                                        QFileDialog::DontConfirmOverwrite);
}

bool MainWindow::confirmOverwrite(const QString& filePath)
{
    return QMessageBox::question(this, tr("Substituir arquivo"),
                                 tr("O arquivo já existe:\n%1\n\nDeseja substituí-lo?").arg(filePath),
                                 QMessageBox::Yes | QMessageBox::No, QMessageBox::No)
           == QMessageBox::Yes;
}

void MainWindow::reportInfo(const QString& title, const QString& text)
{
    QMessageBox::information(this, title, text);
}

void MainWindow::reportError(const QString& title, const QString& text)
{
    QMessageBox::critical(this, title, text);
}

std::optional<PartParameters> MainWindow::askParameters(const PartParameters& current)
{
    ParametersDialog dialog(current, this);
    if (dialog.exec() != QDialog::Accepted)
        return std::nullopt;
    return dialog.parameters();
}

QString MainWindow::askOpenPath(const QString& directory)
{
    return QFileDialog::getOpenFileName(this, tr("Abrir documento"), directory,
                                        tr("Documentos CadMecanico (*.cadmec)"));
}

QString MainWindow::askSavePath(const QString& suggestedPath)
{
    // A confirmação de sobrescrita é feita em saveDocumentTo().
    return QFileDialog::getSaveFileName(this, tr("Salvar documento"), suggestedPath,
                                        tr("Documentos CadMecanico (*.cadmec)"), nullptr,
                                        QFileDialog::DontConfirmOverwrite);
}

MainWindow::SaveChoice MainWindow::askSaveChanges()
{
    const auto answer = QMessageBox::question(
        this, tr("Alterações não salvas"),
        tr("O documento \"%1\" tem alterações não salvas.\n\nDeseja salvá-las?")
            .arg(QString::fromStdString(m_document.name())),
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);
    if (answer == QMessageBox::Save)
        return SaveChoice::Save;
    if (answer == QMessageBox::Discard)
        return SaveChoice::Discard;
    return SaveChoice::Cancel;
}

} // namespace cad::app
