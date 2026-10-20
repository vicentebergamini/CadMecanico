#include "app/MainWindow.h"

#include "io/StepExporter.h"
#include "modeling/SubShapeInfo.h"
#include "modeling/TestPart.h"
#include "viewer/OcctViewerWidget.h"

#include <QAction>
#include <QApplication>
#include <QDir>
#include <QDockWidget>
#include <QFileDialog>
#include <QFileInfo>
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

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
    , m_part(cad::modeling::buildTestPart())
    , m_lastExportDir(QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation))
{
    setObjectName("MainWindow");
    setWindowTitle(tr("CAD Mecânico"));

    createActions();
    createMenus();
    createToolBars();
    createDocks();
    createCentralArea();

    m_readyMessage = tr("Pronto  |  Qt %1  |  Open CASCADE %2").arg(qVersion(), OCC_VERSION_COMPLETE);
    statusBar()->showMessage(m_readyMessage);
}

MainWindow::~MainWindow() = default;

void MainWindow::createActions()
{
    QStyle* st = style();

    // Arquivo
    m_actNew = new QAction(tr("&Novo"), this);
    m_actNew->setObjectName("actionNovo");
    m_actNew->setShortcut(QKeySequence::New);
    m_actNew->setEnabled(false);

    m_actOpen = new QAction(tr("&Abrir..."), this);
    m_actOpen->setObjectName("actionAbrir");
    m_actOpen->setShortcut(QKeySequence::Open);
    m_actOpen->setEnabled(false);

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

    // Editar (habilitados com a árvore paramétrica)
    m_actUndo = new QAction(tr("&Desfazer"), this);
    m_actUndo->setObjectName("actionDesfazer");
    m_actUndo->setShortcut(QKeySequence::Undo);
    m_actUndo->setEnabled(false);

    m_actRedo = new QAction(tr("&Refazer"), this);
    m_actRedo->setObjectName("actionRefazer");
    m_actRedo->setShortcut(QKeySequence::Redo);
    m_actRedo->setEnabled(false);

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
    menuFile->addSeparator();
    menuFile->addAction(m_actExportStep);
    menuFile->addSeparator();
    menuFile->addAction(m_actQuit);

    QMenu* menuEdit = menuBar()->addMenu(tr("&Editar"));
    menuEdit->setObjectName("menuEditar");
    menuEdit->addAction(m_actUndo);
    menuEdit->addAction(m_actRedo);

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
    // A ação de mostrar/ocultar o painel é adicionada em createDocks().

    QMenu* menuHelp = menuBar()->addMenu(tr("A&juda"));
    menuHelp->setObjectName("menuAjuda");
    menuHelp->addAction(m_actAbout);
}

void MainWindow::createToolBars()
{
    m_toolBarFile = addToolBar(tr("Arquivo"));
    m_toolBarFile->setObjectName("toolBarArquivo");
    m_toolBarFile->addAction(m_actExportStep);
    m_toolBarFile->addAction(m_actQuit);

    // Reservada para as ferramentas de modelagem (esboço, extrusão, furo...).
    m_toolBarModeling = addToolBar(tr("Modelagem"));
    m_toolBarModeling->setObjectName("toolBarModelagem");

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

    // Apenas visual nesta etapa; a árvore paramétrica vem na E5.
    auto* item = new QTreeWidgetItem(m_operationsTree, QStringList(tr("Peça de teste")));
    item->setIcon(0, style()->standardIcon(QStyle::SP_FileIcon));
    m_operationsTree->expandAll();

    m_dockOperations = new QDockWidget(tr("Árvore de Operações"), this);
    m_dockOperations->setObjectName("dockArvoreOperacoes");
    m_dockOperations->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
    m_dockOperations->setWidget(m_operationsTree);
    addDockWidget(Qt::LeftDockWidgetArea, m_dockOperations);

    if (auto* menuView = menuBar()->findChild<QMenu*>("menuVisualizar"))
        menuView->addAction(m_dockOperations->toggleViewAction());
}

void MainWindow::createCentralArea()
{
    m_viewer = new cad::viewer::OcctViewerWidget;
    m_viewer->setShape(m_part);
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
    auto mm = [&locale](double v) { return locale.toString(v, 'f', 2); };

    const auto info = cad::modeling::describeSubShape(m_part, selected.first());
    QString geometry = geometryName(info.geometry);
    if (info.radius > 0.0)
        geometry += tr(" (R %1 mm)").arg(mm(info.radius));

    QString text;
    switch (info.kind)
    {
    case SubShapeKind::Face:
        text = tr("Face %1/%2  |  %3  |  Área: %4 mm²").arg(info.index).arg(info.count).arg(geometry, mm(info.area));
        break;
    case SubShapeKind::Edge:
        text = tr("Aresta %1/%2  |  %3  |  Comprimento: %4 mm")
                   .arg(info.index).arg(info.count).arg(geometry, mm(info.length));
        break;
    case SubShapeKind::Vertex:
        text = tr("Vértice %1/%2  |  X %3  Y %4  Z %5 mm")
                   .arg(info.index).arg(info.count)
                   .arg(mm(info.point.X()), mm(info.point.Y()), mm(info.point.Z()));
        break;
    default:
        text = tr("Entidade selecionada");
        break;
    }
    statusBar()->showMessage(text);
}

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
    const auto result = cad::io::StepExporter().write(m_part, std::filesystem::path(filePath.toStdWString()));
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

QString MainWindow::askExportPath(const QString& suggestedPath)
{
    // A confirmação de sobrescrita é feita em exportStepTo().
    return QFileDialog::getSaveFileName(this, tr("Exportar STEP"), suggestedPath,
                                        tr("Arquivos STEP (*.step *.stp)"), nullptr,
                                        QFileDialog::DontConfirmOverwrite);
}

bool MainWindow::confirmOverwrite(const QString& filePath)
{
    return QMessageBox::question(this, tr("Exportar STEP"),
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

} // namespace cad::app
