#include "app/MainWindow.h"

#include "app/ParametersDialog.h"
#include "app/SketchEntityDialog.h"
#include "document/DocumentFile.h"
#include "io/StepExporter.h"
#include "modeling/SubShapeInfo.h"
#include "viewer/OcctViewerWidget.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QInputDialog>
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
using cad::sketch::EntityType;
using cad::sketch::PlaneType;
using cad::sketch::Sketch;
using cad::sketch::SketchEntity;
using cad::sketch::SketchGeometry;
using cad::sketch::SketchPlane;

namespace {

QString num(double v)
{
    return QLocale(QLocale::Portuguese, QLocale::Brazil).toString(v, 'f', 2);
}

QString planeDescription(PlaneType plane)
{
    switch (plane)
    {
    case PlaneType::XY: return MainWindow::tr("XY (vista Superior)");
    case PlaneType::XZ: return MainWindow::tr("XZ (vista Frente)");
    case PlaneType::YZ: return MainWindow::tr("YZ (vista Direita)");
    }
    return QString();
}

QString vectorText(const gp_Dir& d)
{
    return QStringLiteral("(%1; %2; %3)").arg(num(d.X()), num(d.Y()), num(d.Z()));
}

QString entityTypeName(EntityType type)
{
    switch (type)
    {
    case EntityType::Line:      return MainWindow::tr("Linha");
    case EntityType::Rectangle: return MainWindow::tr("Retângulo");
    case EntityType::Circle:    return MainWindow::tr("Círculo");
    }
    return QString();
}

// Resumo de uma entidade para a árvore.
QString entitySummary(const SketchEntity& e)
{
    const QString id = QString::fromStdString(e.id);
    if (const auto* l = std::get_if<cad::sketch::SketchLine>(&e.geometry))
        return MainWindow::tr("Linha %1: (%2; %3) → (%4; %5), L %6 mm")
            .arg(id, num(l->x1), num(l->y1), num(l->x2), num(l->y2), num(l->length()));
    if (const auto* r = std::get_if<cad::sketch::SketchRectangle>(&e.geometry))
        return MainWindow::tr("Retângulo %1: canto (%2; %3), %4 x %5 mm")
            .arg(id, num(r->x), num(r->y), num(r->width), num(r->height));
    const auto& c = std::get<cad::sketch::SketchCircle>(e.geometry);
    return MainWindow::tr("Círculo %1: centro (%2; %3), R %4 mm").arg(id, num(c.cx), num(c.cy), num(c.radius));
}

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
    m_actNew->setStatusTip(tr("Cria um novo documento vazio"));
    connect(m_actNew, &QAction::triggered, this, &MainWindow::newDocument);

    m_actExampleBlock = new QAction(tr("&Bloco com furo passante"), this);
    m_actExampleBlock->setObjectName("actionExemploBlocoFuro");
    m_actExampleBlock->setStatusTip(tr("Abre um novo documento de exemplo com o bloco 100 x 60 x 20 mm e furo R10"));
    connect(m_actExampleBlock, &QAction::triggered, this, &MainWindow::newExampleDocument);

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

    // Esboço
    m_actNewSketch = new QAction(st->standardIcon(QStyle::SP_FileDialogNewFolder), tr("&Novo esboço..."), this);
    m_actNewSketch->setObjectName("actionNovoEsboco");
    m_actNewSketch->setStatusTip(tr("Cria um esboço 2D no plano XY, XZ ou YZ"));
    connect(m_actNewSketch, &QAction::triggered, this, &MainWindow::newSketch);

    m_actEditSketch = new QAction(tr("&Editar esboço"), this);
    m_actEditSketch->setObjectName("actionEditarEsboco");
    m_actEditSketch->setStatusTip(tr("Abre o esboço selecionado na árvore para edição"));
    connect(m_actEditSketch, &QAction::triggered, this, &MainWindow::editSelectedSketch);

    m_sketchTools = new QActionGroup(this);
    m_sketchTools->setExclusive(true);
    auto makeTool = [this](const QString& text, const char* name, cad::viewer::SketchTool tool, const QString& tip) {
        auto* act = new QAction(text, this);
        act->setObjectName(name);
        act->setCheckable(true);
        act->setStatusTip(tip);
        m_sketchTools->addAction(act);
        connect(act, &QAction::triggered, this, [this, tool, tip] {
            m_viewer->setSketchTool(tool);
            statusBar()->showMessage(tip);
        });
        return act;
    };
    m_actToolSelect = makeTool(tr("&Selecionar"), "actionFerramentaSelecionar", cad::viewer::SketchTool::Select,
                               tr("Selecionar: clique sobre uma entidade do esboço"));
    m_actToolLine = makeTool(tr("&Linha"), "actionFerramentaLinha", cad::viewer::SketchTool::Line,
                             tr("Linha: clique no ponto inicial e depois no ponto final (Esc cancela)"));
    m_actToolRectangle = makeTool(tr("&Retângulo"), "actionFerramentaRetangulo", cad::viewer::SketchTool::Rectangle,
                                  tr("Retângulo: clique em um canto e depois no canto oposto (Esc cancela)"));
    m_actToolCircle = makeTool(tr("&Círculo"), "actionFerramentaCirculo", cad::viewer::SketchTool::Circle,
                               tr("Círculo: clique no centro e depois em um ponto da circunferência (Esc cancela)"));
    m_actToolSelect->setChecked(true);

    m_actEntityDimensions = new QAction(tr("&Dimensões..."), this);
    m_actEntityDimensions->setObjectName("actionDimensoesEntidade");
    m_actEntityDimensions->setStatusTip(tr("Edita coordenadas e dimensões da entidade selecionada"));
    connect(m_actEntityDimensions, &QAction::triggered, this, &MainWindow::editEntityDimensions);

    m_actDeleteEntity = new QAction(tr("E&xcluir entidade"), this);
    m_actDeleteEntity->setObjectName("actionExcluirEntidade");
    m_actDeleteEntity->setShortcut(QKeySequence::Delete);
    connect(m_actDeleteEntity, &QAction::triggered, this, &MainWindow::deleteSelectedEntity);

    m_actFinishSketch = new QAction(st->standardIcon(QStyle::SP_DialogApplyButton), tr("C&oncluir esboço"), this);
    m_actFinishSketch->setObjectName("actionConcluirEsboco");
    m_actFinishSketch->setStatusTip(tr("Grava o esboço no documento e sai do modo de esboço"));
    connect(m_actFinishSketch, &QAction::triggered, this, &MainWindow::finishSketch);

    m_actCancelSketch = new QAction(st->standardIcon(QStyle::SP_DialogCancelButton), tr("C&ancelar esboço"), this);
    m_actCancelSketch->setObjectName("actionCancelarEsboco");
    m_actCancelSketch->setStatusTip(tr("Descarta as alterações do esboço e sai do modo de esboço"));
    connect(m_actCancelSketch, &QAction::triggered, this, &MainWindow::cancelSketch);

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
    QMenu* menuExamples = menuFile->addMenu(tr("E&xemplos"));
    menuExamples->setObjectName("menuExemplos");
    menuExamples->addAction(m_actExampleBlock);
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

    QMenu* menuSketch = menuBar()->addMenu(tr("E&sboço"));
    menuSketch->setObjectName("menuEsboco");
    menuSketch->addAction(m_actNewSketch);
    menuSketch->addAction(m_actEditSketch);
    menuSketch->addSeparator();
    menuSketch->addAction(m_actToolSelect);
    menuSketch->addAction(m_actToolLine);
    menuSketch->addAction(m_actToolRectangle);
    menuSketch->addAction(m_actToolCircle);
    menuSketch->addSeparator();
    menuSketch->addAction(m_actEntityDimensions);
    menuSketch->addAction(m_actDeleteEntity);
    menuSketch->addSeparator();
    menuSketch->addAction(m_actFinishSketch);
    menuSketch->addAction(m_actCancelSketch);

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
    m_toolBarModeling->addAction(m_actNewSketch);

    m_toolBarSketch = addToolBar(tr("Esboço"));
    m_toolBarSketch->setObjectName("toolBarEsboco");
    m_toolBarSketch->addAction(m_actToolSelect);
    m_toolBarSketch->addAction(m_actToolLine);
    m_toolBarSketch->addAction(m_actToolRectangle);
    m_toolBarSketch->addAction(m_actToolCircle);
    m_toolBarSketch->addSeparator();
    m_toolBarSketch->addAction(m_actEntityDimensions);
    m_toolBarSketch->addAction(m_actDeleteEntity);
    m_toolBarSketch->addSeparator();
    m_toolBarSketch->addAction(m_actFinishSketch);
    m_toolBarSketch->addAction(m_actCancelSketch);

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
            [this](QTreeWidgetItem* current, QTreeWidgetItem*) { onTreeItemChanged(current); });
    connect(m_operationsTree, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* item, int) {
        if (!item)
            return;
        const QString kind = item->data(0, kItemKindRole).toString();
        if (kind == QLatin1String("esboco") && !isEditingSketch())
            editSketch(item->data(0, kItemIdRole).toString());
        else if (kind == QLatin1String("entidade") && isEditingSketch())
            editEntityDimensions();
        else if (!item->data(0, kItemIdRole).toString().isEmpty() && kind != QLatin1String("esboco")
                 && kind != QLatin1String("entidade") && !isEditingSketch())
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
    connect(m_viewer, &cad::viewer::OcctViewerWidget::sketchGeometryRequested, this,
            [this](const SketchGeometry& g) { addSketchGeometry(g); });
    connect(m_viewer, &cad::viewer::OcctViewerWidget::sketchEntityClicked, this, &MainWindow::selectSketchEntity);
    setSketchModeUi(false);
}

// --- Documento ---------------------------------------------------------------

void MainWindow::refreshDocumentViews(bool geometryChanged)
{
    if (geometryChanged)
    {
        m_viewer->setShape(m_document.shape());
        updateSelectionStatus();
    }
    m_viewer->setDocumentSketches(displayedSketches(false));
    rebuildTree();
    updateTitle();
    updateActionStates();
}

void MainWindow::updateActionStates()
{
    const bool editing = isEditingSketch();
    m_actEditParameters->setEnabled(!editing && m_document.hasPart());
    m_actExportStep->setEnabled(!m_document.shape().IsNull());
}

std::vector<Sketch> MainWindow::displayedSketches(bool includeEditing) const
{
    std::vector<Sketch> sketches;
    bool editingFound = false;
    for (const Sketch& s : m_document.sketches())
    {
        if (m_editSketch && s.id() == m_editSketch->id())
        {
            editingFound = true;
            if (includeEditing)
                sketches.push_back(*m_editSketch);
            continue;
        }
        sketches.push_back(s);
    }
    if (includeEditing && m_editSketch && !editingFound)
        sketches.push_back(*m_editSketch);
    return sketches;
}

void MainWindow::updateTitle()
{
    setWindowTitle(tr("%1[*] — CAD Mecânico").arg(QString::fromStdString(m_document.name())));
    setWindowModified(m_document.isModified());
}

void MainWindow::rebuildTree()
{
    // Mantém o item selecionado (pelos ids estáveis) após a reconstrução da árvore.
    QString selectedId;
    QString selectedKind;
    QString selectedEntity;
    if (QTreeWidgetItem* current = m_operationsTree->currentItem())
    {
        selectedId = current->data(0, kItemIdRole).toString();
        selectedKind = current->data(0, kItemKindRole).toString();
        selectedEntity = current->data(0, kItemEntityRole).toString();
    }
    if (isEditingSketch())
    {
        // No modo de esboço a seleção da árvore acompanha a entidade selecionada.
        selectedId = QString::fromStdString(m_editSketch->id());
        selectedKind = m_selectedEntity.isEmpty() ? QStringLiteral("esboco") : QStringLiteral("entidade");
        selectedEntity = m_selectedEntity;
    }

    m_updatingTree = true;
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
        item->setExpanded(true);
    }

    // Esboços (entidades visíveis ao expandir o esboço).
    for (const Sketch& sketch : displayedSketches(true))
    {
        const QString id = QString::fromStdString(sketch.id());
        const bool editing = m_editSketch && sketch.id() == m_editSketch->id();
        auto* item = new QTreeWidgetItem(root);
        item->setText(0, tr("%1 (%2)%3")
                             .arg(QString::fromStdString(sketch.name()),
                                  QString::fromLatin1(SketchPlane::name(sketch.plane().type())),
                                  editing ? tr(" — em edição") : QString()));
        item->setIcon(0, st->standardIcon(QStyle::SP_FileDialogContentsView));
        item->setData(0, kItemKindRole, QStringLiteral("esboco"));
        item->setData(0, kItemIdRole, id);
        for (const SketchEntity& e : sketch.entities())
        {
            auto* child = new QTreeWidgetItem(item);
            child->setText(0, entitySummary(e));
            child->setData(0, kItemKindRole, QStringLiteral("entidade"));
            child->setData(0, kItemIdRole, id);
            child->setData(0, kItemEntityRole, QString::fromStdString(e.id));
            if (id == selectedId && selectedKind == QLatin1String("entidade")
                && QString::fromStdString(e.id) == selectedEntity)
                toSelect = child;
        }
        item->setExpanded(editing);
        if (!toSelect && id == selectedId)
            toSelect = item;
    }

    root->setExpanded(true);
    m_operationsTree->setCurrentItem(toSelect ? toSelect : root);
    m_updatingTree = false;
    showItemProperties(m_operationsTree->currentItem());
}

void MainWindow::onTreeItemChanged(QTreeWidgetItem* item)
{
    showItemProperties(item);
    if (m_updatingTree || !item || !isEditingSketch())
        return;
    // Selecionar uma entidade do esboço em edição na árvore seleciona-a no visualizador.
    const QString kind = item->data(0, kItemKindRole).toString();
    if (item->data(0, kItemIdRole).toString() != QString::fromStdString(m_editSketch->id()))
        return;
    const QString entity = kind == QLatin1String("entidade") ? item->data(0, kItemEntityRole).toString() : QString();
    if (entity != m_selectedEntity)
    {
        m_selectedEntity = entity;
        m_viewer->updateEditingSketch(*m_editSketch, m_selectedEntity);
        setSketchModeUi(true);
    }
}

void MainWindow::showItemProperties(QTreeWidgetItem* item)
{
    if (!item)
    {
        m_properties->clear();
        return;
    }
    const QString id = item->data(0, kItemIdRole).toString();
    const QString kind = item->data(0, kItemKindRole).toString();
    const Operation* op = id.isEmpty() ? nullptr : m_document.findOperation(id.toStdString());
    QString html;
    if (kind == QLatin1String("esboco") || kind == QLatin1String("entidade"))
    {
        const Sketch* sketch = nullptr;
        if (m_editSketch && m_editSketch->id() == id.toStdString())
            sketch = &*m_editSketch;
        else
            sketch = m_document.findSketch(id.toStdString());
        if (!sketch)
        {
            m_properties->clear();
            return;
        }
        const SketchEntity* entity = kind == QLatin1String("entidade")
                                         ? sketch->findEntity(item->data(0, kItemEntityRole).toString().toStdString())
                                         : nullptr;
        if (entity)
        {
            html = tr("<b>%1 %2</b><br>Esboço: %3 (%4)")
                       .arg(entityTypeName(entity->type()), QString::fromStdString(entity->id),
                            QString::fromStdString(sketch->name()).toHtmlEscaped(), id);
            for (const auto& d : cad::sketch::dimensionsOf(*entity))
                html += tr("<br>%1%2: %3 mm")
                            .arg(QString::fromStdString(d.label), d.editable ? QString() : tr(" (calculado)"), num(d.value));
        }
        else
        {
            const SketchPlane& plane = sketch->plane();
            html = tr("<b>%1</b><br>Identificador: %2<br>Plano: %3<br>Origem: (%4; %5; %6)"
                      "<br>Eixo horizontal: %7<br>Eixo vertical: %8<br>Normal: %9<br>Entidades: %10")
                       .arg(QString::fromStdString(sketch->name()).toHtmlEscaped(), id, planeDescription(plane.type()),
                            num(plane.origin().X()), num(plane.origin().Y()), num(plane.origin().Z()),
                            vectorText(plane.horizontalAxis()), vectorText(plane.verticalAxis()),
                            vectorText(plane.normal()))
                       .arg(sketch->entities().size());
        }
        m_properties->setText(html);
        return;
    }
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
        html = tr("<b>%1</b><br>Unidade: %2<br>Operações e esboços: %3<br>Arquivo: %4<br>Alterações não salvas: %5")
                   .arg(QString::fromStdString(m_document.name()).toHtmlEscaped(), QString::fromLatin1(m_document.unit()))
                   .arg(m_document.operations().size() + m_document.sketches().size())
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
    if (isEditingSketch())
        cancelSketch();
    if (!maybeSaveChanges())
        return;
    m_document = CadDocument(tr("Sem título").toStdString()); // vazio: sem operações nem esboços
    m_documentPath.clear();
    refreshDocumentViews(true);
    m_viewer->viewIsometric();
    statusBar()->showMessage(tr("Novo documento"), 5000);
}

void MainWindow::newExampleDocument()
{
    if (isEditingSketch())
        cancelSketch();
    if (!maybeSaveChanges())
        return;
    m_document = CadDocument::withTestPart(tr("Peça de teste").toStdString());
    m_documentPath.clear();
    refreshDocumentViews(true);
    m_viewer->viewIsometric();
    statusBar()->showMessage(m_readyMessage);
}

void MainWindow::openDocument()
{
    if (isEditingSketch())
        cancelSketch();
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
    if (isEditingSketch())
        cancelSketch();
    m_document = std::move(loaded);
    m_documentPath = QFileInfo(filePath).absoluteFilePath();
    m_lastDocumentDir = QFileInfo(filePath).absolutePath();
    refreshDocumentViews(true);
    m_viewer->viewIsometric(); // documento aberto: vista isométrica enquadrando sólido e esboços
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
    // Esboço em edição não concluído é descartado (o documento não foi alterado por ele).
    if (isEditingSketch())
        cancelSketch();
    if (maybeSaveChanges())
        event->accept();
    else
        event->ignore();
}

// --- Esboços -----------------------------------------------------------------

void MainWindow::setSketchModeUi(bool editing)
{
    for (QAction* a : {m_actToolSelect, m_actToolLine, m_actToolRectangle, m_actToolCircle, m_actFinishSketch,
                       m_actCancelSketch})
        a->setEnabled(editing);
    const bool hasEntity = editing && !m_selectedEntity.isEmpty();
    m_actEntityDimensions->setEnabled(hasEntity);
    m_actDeleteEntity->setEnabled(hasEntity);
    // Fora do esboço: documento, parâmetros e vistas padrão; no esboço a câmera fica no plano.
    for (QAction* a : {m_actNew, m_actExampleBlock, m_actOpen, m_actSave, m_actSaveAs, m_actNewSketch,
                       m_actEditSketch, m_actViewFront, m_actViewTop, m_actViewRight, m_actViewIso})
        a->setEnabled(!editing);
    m_menuStandardViews->setEnabled(!editing);
    updateActionStates();
}

void MainWindow::refreshSketchViews()
{
    if (m_editSketch)
        m_viewer->updateEditingSketch(*m_editSketch, m_selectedEntity);
    rebuildTree();
    setSketchModeUi(isEditingSketch());
}

void MainWindow::newSketch()
{
    if (isEditingSketch())
        return;
    const auto plane = askSketchPlane();
    if (!plane)
    {
        statusBar()->showMessage(tr("Criação de esboço cancelada"), 5000);
        return;
    }
    startNewSketch(*plane);
}

bool MainWindow::startNewSketch(PlaneType plane)
{
    if (isEditingSketch())
        return false;
    m_editSketch = m_document.createSketch(plane);
    m_editSketchIsNew = true;
    m_selectedEntity.clear();
    m_viewer->clearSelection();
    m_viewer->setDocumentSketches(displayedSketches(false));
    m_viewer->enterSketchMode(*m_editSketch);
    m_actToolSelect->setChecked(true);
    refreshSketchViews();
    statusBar()->showMessage(tr("Modo de esboço: %1 no plano %2. Escolha uma ferramenta (Linha, Retângulo, Círculo).")
                                 .arg(QString::fromStdString(m_editSketch->name()), planeDescription(plane)));
    return true;
}

void MainWindow::editSelectedSketch()
{
    if (QTreeWidgetItem* item = m_operationsTree->currentItem())
        if (item->data(0, kItemKindRole).toString() == QLatin1String("esboco")
            || item->data(0, kItemKindRole).toString() == QLatin1String("entidade"))
        {
            editSketch(item->data(0, kItemIdRole).toString());
            return;
        }
    statusBar()->showMessage(tr("Selecione um esboço na árvore para editar"), 5000);
}

bool MainWindow::editSketch(const QString& sketchId)
{
    if (isEditingSketch())
        return false;
    const Sketch* sketch = m_document.findSketch(sketchId.toStdString());
    if (!sketch)
        return false;
    m_editSketch = *sketch; // cópia de trabalho
    m_editSketchIsNew = false;
    m_selectedEntity.clear();
    m_viewer->clearSelection();
    m_viewer->setDocumentSketches(displayedSketches(false));
    m_viewer->enterSketchMode(*m_editSketch);
    m_actToolSelect->setChecked(true);
    refreshSketchViews();
    statusBar()->showMessage(tr("Editando %1 (plano %2)")
                                 .arg(QString::fromStdString(m_editSketch->name()), planeDescription(m_editSketch->plane().type())));
    return true;
}

bool MainWindow::finishSketch()
{
    if (!isEditingSketch())
        return false;
    std::string error;
    if (!m_document.commitSketch(*m_editSketch, error))
    {
        reportError(tr("Concluir esboço"), tr("O esboço não pôde ser gravado:\n%1").arg(QString::fromStdString(error)));
        return false;
    }
    const QString name = QString::fromStdString(m_editSketch->name());
    const QString id = QString::fromStdString(m_editSketch->id());
    m_editSketch.reset();
    m_selectedEntity.clear();
    m_viewer->exitSketchMode();
    setSketchModeUi(false);
    refreshDocumentViews(false);
    // Seleciona o esboço concluído na árvore.
    QTreeWidgetItem* root = m_operationsTree->topLevelItem(0);
    for (int i = 0; root && i < root->childCount(); ++i)
        if (root->child(i)->data(0, kItemKindRole).toString() == QLatin1String("esboco")
            && root->child(i)->data(0, kItemIdRole).toString() == id)
            m_operationsTree->setCurrentItem(root->child(i));
    statusBar()->showMessage(tr("%1 concluído").arg(name), 5000);
    return true;
}

void MainWindow::cancelSketch()
{
    if (!isEditingSketch())
        return;
    m_editSketch.reset();
    m_selectedEntity.clear();
    m_viewer->exitSketchMode();
    setSketchModeUi(false);
    refreshDocumentViews(false);
    statusBar()->showMessage(tr("Edição do esboço cancelada; documento mantido"), 5000);
}

bool MainWindow::addSketchGeometry(const SketchGeometry& geometry)
{
    if (!isEditingSketch())
        return false;
    std::string newId;
    const auto result = m_editSketch->addEntity(geometry, &newId);
    if (!result.ok)
    {
        statusBar()->showMessage(tr("Entidade não criada: %1").arg(QString::fromStdString(result.message)), 8000);
        return false;
    }
    m_selectedEntity = QString::fromStdString(newId);
    refreshSketchViews();
    if (const SketchEntity* e = m_editSketch->findEntity(newId))
        statusBar()->showMessage(tr("Criado: %1").arg(entitySummary(*e)), 8000);
    return true;
}

bool MainWindow::updateSketchEntity(const QString& entityId, const SketchGeometry& geometry)
{
    if (!isEditingSketch())
        return false;
    const auto result = m_editSketch->updateEntity(entityId.toStdString(), geometry);
    if (!result.ok)
    {
        statusBar()->showMessage(tr("Dimensões recusadas: %1").arg(QString::fromStdString(result.message)), 8000);
        reportError(tr("Dimensões da entidade"),
                    tr("As dimensões não foram aplicadas:\n%1\n\nA entidade foi mantida.")
                        .arg(QString::fromStdString(result.message)));
        return false;
    }
    refreshSketchViews();
    if (const SketchEntity* e = m_editSketch->findEntity(entityId.toStdString()))
        statusBar()->showMessage(tr("Atualizado: %1").arg(entitySummary(*e)), 8000);
    return true;
}

void MainWindow::selectSketchEntity(const QString& entityId)
{
    if (!isEditingSketch())
        return;
    if (!entityId.isEmpty() && !m_editSketch->findEntity(entityId.toStdString()))
        return;
    m_selectedEntity = entityId;
    refreshSketchViews();
    if (const SketchEntity* e = m_editSketch->findEntity(entityId.toStdString()))
        statusBar()->showMessage(tr("Selecionado: %1").arg(entitySummary(*e)));
    else
        statusBar()->showMessage(tr("Nenhuma entidade selecionada"), 5000);
}

void MainWindow::editEntityDimensions()
{
    if (!isEditingSketch() || m_selectedEntity.isEmpty())
        return;
    const SketchEntity* e = m_editSketch->findEntity(m_selectedEntity.toStdString());
    if (!e)
        return;
    const auto geometry = askEntityDimensions(*e);
    if (!geometry)
    {
        statusBar()->showMessage(tr("Edição de dimensões cancelada"), 5000);
        return;
    }
    updateSketchEntity(m_selectedEntity, *geometry);
}

void MainWindow::deleteSelectedEntity()
{
    if (!isEditingSketch() || m_selectedEntity.isEmpty())
        return;
    m_editSketch->removeEntity(m_selectedEntity.toStdString());
    statusBar()->showMessage(tr("Entidade %1 excluída").arg(m_selectedEntity), 5000);
    m_selectedEntity.clear();
    refreshSketchViews();
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
    if (part().IsNull())
    {
        statusBar()->showMessage(tr("Nada para exportar: o documento não contém sólido"), 8000);
        reportError(tr("Exportar STEP"),
                    tr("O documento não contém sólido para exportar.\n\n"
                       "Esboços 2D não são exportados em STEP; crie ou abra uma peça com sólido."));
        return;
    }
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
    if (part().IsNull())
    {
        reportError(tr("Exportar STEP"), tr("O documento não contém sólido para exportar."));
        return false;
    }

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

std::optional<PlaneType> MainWindow::askSketchPlane()
{
    const QStringList items{planeDescription(PlaneType::XY), planeDescription(PlaneType::XZ),
                            planeDescription(PlaneType::YZ)};
    bool ok = false;
    const QString choice = QInputDialog::getItem(this, tr("Novo esboço"), tr("Plano de referência:"), items, 0, false, &ok);
    if (!ok)
        return std::nullopt;
    return PlaneType(items.indexOf(choice));
}

std::optional<SketchGeometry> MainWindow::askEntityDimensions(const SketchEntity& entity)
{
    SketchEntityDialog dialog(entity, this);
    if (dialog.exec() != QDialog::Accepted)
        return std::nullopt;
    return dialog.geometry();
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
