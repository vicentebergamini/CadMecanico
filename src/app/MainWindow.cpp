#include "app/MainWindow.h"

#include "app/BooleanDialog.h"
#include "app/ExtrusionDialog.h"
#include "app/FinishDialog.h"
#include "app/ParametersDialog.h"
#include "app/SketchEntityDialog.h"
#include "app/StlExportDialog.h"
#include "modeling/ShapeInspector.h"
#include "sketch/SketchProfile.h"
#include "sketch/SketchResolver.h"
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

#include <BRep_Builder.hxx>
#include <Standard_Version.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>

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
    case EntityType::Arc:       return MainWindow::tr("Arco");
    case EntityType::Fillet:    return MainWindow::tr("Fillet 2D");
    case EntityType::Chamfer:   return MainWindow::tr("Chamfer 2D");
    }
    return QString();
}

// Canto referenciado por um modificador: "e-1, canto 2" (retângulo) ou "e-1 / e-2" (linhas).
QString cornerText(const std::string& a, const std::string& b, int corner)
{
    if (a == b)
        return MainWindow::tr("%1, canto %2").arg(QString::fromStdString(a)).arg(corner);
    return MainWindow::tr("%1 / %2").arg(QString::fromStdString(a), QString::fromStdString(b));
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
    if (const auto* c = std::get_if<cad::sketch::SketchCircle>(&e.geometry))
        return MainWindow::tr("Círculo %1: centro (%2; %3), R %4 mm").arg(id, num(c->cx), num(c->cy), num(c->radius));
    if (const auto* a = std::get_if<cad::sketch::SketchArc>(&e.geometry))
        return MainWindow::tr("Arco %1: centro (%2; %3), R %4 mm, %5° → %6° (%7)")
            .arg(id, num(a->cx), num(a->cy), num(a->radius), num(a->startAngle), num(a->endAngle),
                 a->clockwise ? MainWindow::tr("horário") : MainWindow::tr("anti-horário"));
    if (const auto* f = std::get_if<cad::sketch::SketchFillet>(&e.geometry))
        return MainWindow::tr("Fillet 2D %1: R %2 mm (%3)").arg(id, num(f->radius), cornerText(f->entityA, f->entityB, f->corner));
    const auto& c = std::get<cad::sketch::SketchChamfer>(e.geometry);
    return MainWindow::tr("Chamfer 2D %1: %2 mm (%3)").arg(id, num(c.distance), cornerText(c.entityA, c.entityB, c.corner));
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
    if (key == "depth")  return MainWindow::tr("Profundidade");
    if (key == "distance") return MainWindow::tr("Distância");
    return QString::fromStdString(key);
}

QString operationTypeName(OperationType type)
{
    switch (type)
    {
    case OperationType::Block:       return MainWindow::tr("Bloco");
    case OperationType::ThroughHole: return MainWindow::tr("Furo passante");
    case OperationType::Extrude:     return MainWindow::tr("Extrusão");
    case OperationType::Boolean:     return MainWindow::tr("Operação booleana");
    case OperationType::Fillet3D:    return MainWindow::tr("Fillet 3D");
    case OperationType::Chamfer3D:   return MainWindow::tr("Chamfer 3D");
    }
    return QString();
}

QString booleanKindName(cad::modeling::BooleanKind kind)
{
    switch (kind)
    {
    case cad::modeling::BooleanKind::Fuse:   return MainWindow::tr("União (A ∪ B)");
    case cad::modeling::BooleanKind::Cut:    return MainWindow::tr("Subtração (A − B)");
    case cad::modeling::BooleanKind::Common: return MainWindow::tr("Interseção (A ∩ B)");
    }
    return QString();
}

QString booleanSymbol(cad::modeling::BooleanKind kind)
{
    switch (kind)
    {
    case cad::modeling::BooleanKind::Fuse:   return QStringLiteral("∪");
    case cad::modeling::BooleanKind::Cut:    return QStringLiteral("−");
    case cad::modeling::BooleanKind::Common: return QStringLiteral("∩");
    }
    return QString();
}

// "1 sólido" ou "N sólidos desconectados (corpo composto)".
QString solidsText(const TopoDS_Shape& shape)
{
    const int n = cad::modeling::countSolids(shape);
    return n == 1 ? MainWindow::tr("1 sólido") : MainWindow::tr("%1 sólidos desconectados (corpo composto)").arg(n);
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

    m_actExportStep = new QAction(st->standardIcon(QStyle::SP_DialogSaveButton), tr("&STEP (*.step)..."), this);
    m_actExportStep->setObjectName("actionExportarStep");
    m_actExportStep->setShortcut(QKeySequence(tr("Ctrl+E")));
    m_actExportStep->setStatusTip(tr("Exporta a peça atual em formato STEP"));
    connect(m_actExportStep, &QAction::triggered, this, &MainWindow::exportStep);

    m_actExportStl = new QAction(st->standardIcon(QStyle::SP_DriveFDIcon), tr("S&TL (*.stl)..."), this);
    m_actExportStl->setObjectName("actionExportarStl");
    m_actExportStl->setStatusTip(tr("Exporta a geometria sólida atual em STL (malha triangular, mm)"));
    connect(m_actExportStl, &QAction::triggered, this, &MainWindow::exportStl);

    // Operações
    m_actExtrude = new QAction(st->standardIcon(QStyle::SP_ArrowUp), tr("&Extrusão..."), this);
    m_actExtrude->setObjectName("actionExtrusao");
    connect(m_actExtrude, &QAction::triggered, this, &MainWindow::extrude);

    auto makeBoolean = [this](const QString& text, const char* name, cad::modeling::BooleanKind kind, const QString& tip) {
        auto* act = new QAction(text, this);
        act->setObjectName(name);
        act->setStatusTip(tip);
        connect(act, &QAction::triggered, this, [this, kind] { booleanCommand(kind); });
        return act;
    };
    m_actFuse = makeBoolean(tr("&União..."), "actionUniao", cad::modeling::BooleanKind::Fuse,
                            tr("União de dois corpos (A ∪ B)"));
    m_actCut = makeBoolean(tr("&Subtração..."), "actionSubtracao", cad::modeling::BooleanKind::Cut,
                           tr("Subtração: corpo principal A menos a ferramenta de corte B (A − B)"));
    m_actCommon = makeBoolean(tr("&Interseção..."), "actionIntersecao", cad::modeling::BooleanKind::Common,
                              tr("Interseção de dois corpos (A ∩ B)"));

    m_actFillet3D = new QAction(tr("&Fillet 3D..."), this);
    m_actFillet3D->setObjectName("actionFillet3D");
    m_actFillet3D->setStatusTip(tr("Arredonda arestas de um corpo: selecione as arestas no visualizador e informe o raio"));
    connect(m_actFillet3D, &QAction::triggered, this, [this] { finishCommand(OperationType::Fillet3D); });
    m_actChamfer3D = new QAction(tr("C&hamfer 3D..."), this);
    m_actChamfer3D->setObjectName("actionChamfer3D");
    m_actChamfer3D->setStatusTip(tr("Chanfra arestas de um corpo (distância simétrica): selecione as arestas e informe "
                                    "a distância"));
    connect(m_actChamfer3D, &QAction::triggered, this, [this] { finishCommand(OperationType::Chamfer3D); });

    m_actEditOperation = new QAction(tr("Editar &operação..."), this);
    m_actEditOperation->setObjectName("actionEditarOperacao");
    m_actEditOperation->setStatusTip(tr("Edita os parâmetros da operação selecionada na árvore"));
    connect(m_actEditOperation, &QAction::triggered, this, &MainWindow::editOperation);

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
    m_actToolArc = makeTool(tr("Arco circ&ular"), "actionFerramentaArco", cad::viewer::SketchTool::Arc,
                            tr("Arco: clique no centro, no ponto inicial (raio) e no ponto final; sentido conforme "
                               "\"Sentido horário do arco\" (Esc cancela)"));
    m_actToolFillet2D = makeTool(tr("&Fillet 2D"), "actionFerramentaFillet2D", cad::viewer::SketchTool::Fillet2D,
                                 tr("Fillet 2D: clique em um canto entre dois segmentos retos e informe o raio"));
    m_actToolChamfer2D = makeTool(tr("C&hamfer 2D"), "actionFerramentaChamfer2D", cad::viewer::SketchTool::Chamfer2D,
                                  tr("Chamfer 2D: clique em um canto entre dois segmentos retos e informe a distância"));
    m_actArcClockwise = new QAction(tr("Sen&tido horário do arco"), this);
    m_actArcClockwise->setObjectName("actionArcoHorario");
    m_actArcClockwise->setCheckable(true);
    m_actArcClockwise->setStatusTip(tr("Desmarcado: arcos no sentido anti-horário (padrão); marcado: sentido horário"));
    connect(m_actArcClockwise, &QAction::toggled, this, [this](bool cw) {
        m_viewer->setArcClockwise(cw);
        statusBar()->showMessage(cw ? tr("Arcos no sentido horário") : tr("Arcos no sentido anti-horário"), 5000);
    });
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
    QMenu* menuExport = menuFile->addMenu(tr("Ex&portar"));
    menuExport->setObjectName("menuExportar");
    menuExport->addAction(m_actExportStep);
    menuExport->addAction(m_actExportStl);
    menuFile->addSeparator();
    menuFile->addAction(m_actQuit);

    QMenu* menuEdit = menuBar()->addMenu(tr("&Editar"));
    menuEdit->setObjectName("menuEditar");
    menuEdit->addAction(m_actUndo);
    menuEdit->addAction(m_actRedo);
    menuEdit->addSeparator();
    menuEdit->addAction(m_actEditParameters);

    QMenu* menuOperations = menuBar()->addMenu(tr("&Operações"));
    menuOperations->setObjectName("menuOperacoes");
    menuOperations->addAction(m_actExtrude);
    menuOperations->addSeparator();
    menuOperations->addAction(m_actFuse);
    menuOperations->addAction(m_actCut);
    menuOperations->addAction(m_actCommon);
    menuOperations->addSeparator();
    menuOperations->addAction(m_actFillet3D);
    menuOperations->addAction(m_actChamfer3D);
    menuOperations->addSeparator();
    menuOperations->addAction(m_actEditOperation);

    QMenu* menuSketch = menuBar()->addMenu(tr("E&sboço"));
    menuSketch->setObjectName("menuEsboco");
    menuSketch->addAction(m_actNewSketch);
    menuSketch->addAction(m_actEditSketch);
    menuSketch->addSeparator();
    menuSketch->addAction(m_actToolSelect);
    menuSketch->addAction(m_actToolLine);
    menuSketch->addAction(m_actToolRectangle);
    menuSketch->addAction(m_actToolCircle);
    menuSketch->addAction(m_actToolArc);
    menuSketch->addAction(m_actArcClockwise);
    menuSketch->addAction(m_actToolFillet2D);
    menuSketch->addAction(m_actToolChamfer2D);
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
    m_toolBarFile->addAction(m_actExportStl);
    m_toolBarFile->addAction(m_actQuit);

    // Ferramentas de modelagem (esboço, extrusão, furo... nas próximas etapas).
    m_toolBarModeling = addToolBar(tr("Modelagem"));
    m_toolBarModeling->setObjectName("toolBarModelagem");
    m_toolBarModeling->addAction(m_actEditParameters);
    m_toolBarModeling->addAction(m_actNewSketch);
    m_toolBarModeling->addAction(m_actExtrude);
    m_toolBarModeling->addAction(m_actFuse);
    m_toolBarModeling->addAction(m_actCut);
    m_toolBarModeling->addAction(m_actCommon);
    m_toolBarModeling->addAction(m_actFillet3D);
    m_toolBarModeling->addAction(m_actChamfer3D);

    m_toolBarSketch = addToolBar(tr("Esboço"));
    m_toolBarSketch->setObjectName("toolBarEsboco");
    m_toolBarSketch->addAction(m_actToolSelect);
    m_toolBarSketch->addAction(m_actToolLine);
    m_toolBarSketch->addAction(m_actToolRectangle);
    m_toolBarSketch->addAction(m_actToolCircle);
    m_toolBarSketch->addAction(m_actToolArc);
    m_toolBarSketch->addAction(m_actToolFillet2D);
    m_toolBarSketch->addAction(m_actToolChamfer2D);
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
        m_operationsTree->setCurrentItem(item); // a ação atua sobre o item clicado
        const QString kind = item->data(0, kItemKindRole).toString();
        if (kind == QLatin1String("esboco") && !isEditingSketch())
            editSketch(item->data(0, kItemIdRole).toString());
        else if (kind == QLatin1String("entidade") && isEditingSketch())
            editEntityDimensions();
        else if (!item->data(0, kItemIdRole).toString().isEmpty() && kind != QLatin1String("esboco")
                 && kind != QLatin1String("entidade") && !isEditingSketch())
            editOperation();
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
    connect(m_viewer, &cad::viewer::OcctViewerWidget::sketchCornerPicked, this,
            [this](cad::viewer::SketchTool tool, double u, double v, double tolerance) {
                const EntityType type = tool == cad::viewer::SketchTool::Fillet2D ? EntityType::Fillet : EntityType::Chamfer;
                if (!isEditingSketch())
                    return;
                if (!cad::sketch::nearestCorner(*m_editSketch, gp_Pnt2d(u, v), tolerance))
                {
                    statusBar()->showMessage(tr("Clique próximo a um canto entre dois segmentos retos (linhas conectadas "
                                                "ou canto de retângulo)"),
                                             8000);
                    return;
                }
                const auto value = askCornerValue(type, type == EntityType::Fillet ? 5.0 : 2.0);
                if (!value)
                {
                    statusBar()->showMessage(tr("%1 cancelado").arg(entityTypeName(type)), 5000);
                    return;
                }
                applyCornerModifier(type, u, v, tolerance, *value);
            });
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
    m_actExportStl->setEnabled(!m_document.shape().IsNull());
    // Extrusão: somente com um esboço concluído selecionado na árvore.
    const bool sketchSelected = !editing && !selectedSketchId().isEmpty();
    m_actExtrude->setEnabled(sketchSelected);
    m_actExtrude->setStatusTip(sketchSelected ? tr("Extruda o esboço selecionado (perfil fechado)")
                                              : tr("Selecione na árvore um esboço concluído para extrudar"));
    m_actExtrude->setToolTip(m_actExtrude->statusTip());
    m_actEditOperation->setEnabled(!editing && !selectedOperationId().isEmpty());
    // Booleanas: pelo menos dois corpos ativos.
    const bool finishing = m_finishDialog != nullptr;
    const bool twoBodies = !editing && !finishing && m_document.activeBodies().size() >= 2;
    for (QAction* a : {m_actFuse, m_actCut, m_actCommon})
    {
        a->setEnabled(twoBodies);
        a->setToolTip(twoBodies ? a->statusTip()
                                : tr("Operações booleanas exigem pelo menos dois corpos sólidos (ex.: duas extrusões)"));
    }
    // Acabamentos 3D: um corpo ativo; durante o comando os demais comandos de modelagem ficam
    // indisponíveis até concluir ou cancelar.
    const bool hasBody = !editing && !finishing && !m_document.activeBodies().empty();
    for (QAction* a : {m_actFillet3D, m_actChamfer3D})
    {
        a->setEnabled(hasBody);
        a->setToolTip(hasBody ? a->statusTip() : tr("Fillet 3D e Chamfer 3D exigem um corpo sólido"));
    }
    if (finishing)
    {
        for (QAction* a : {m_actExtrude, m_actEditOperation, m_actNewSketch, m_actEditSketch, m_actEditParameters, m_actNew,
                           m_actOpen, m_actExampleBlock})
            a->setEnabled(false);
    }
    else if (!editing)
    {
        for (QAction* a : {m_actNewSketch, m_actEditSketch, m_actNew, m_actOpen, m_actExampleBlock})
            a->setEnabled(true);
    }
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
        if (!(op.type == OperationType::Block || op.type == OperationType::ThroughHole))
            continue; // listadas depois dos esboços de que dependem
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

    // Extrusões e booleanas (dependem dos esboços), na ordem do histórico. Corpos consumidos por
    // uma booleana continuam listados, marcados como consumidos.
    for (const Operation& op : m_document.operations())
    {
        if (op.type == OperationType::Block || op.type == OperationType::ThroughHole)
            continue;
        const QString id = QString::fromStdString(op.id);
        auto* item = new QTreeWidgetItem(root);
        QString text = QString::fromStdString(op.name);
        if (op.type == OperationType::Boolean)
        {
            const Operation* a = m_document.findOperation(op.operandA);
            const Operation* b = m_document.findOperation(op.operandB);
            text += tr(" — %1 %2 %3")
                        .arg(a ? QString::fromStdString(a->name) : QString::fromStdString(op.operandA),
                             booleanSymbol(op.booleanKind),
                             b ? QString::fromStdString(b->name) : QString::fromStdString(op.operandB));
        }
        const Operation* consumer = op.consumedBy.empty() ? nullptr : m_document.findOperation(op.consumedBy);
        if (consumer)
        {
            text += tr(" (consumido por %1)").arg(QString::fromStdString(consumer->name));
            item->setForeground(0, palette().color(QPalette::Disabled, QPalette::Text));
        }
        item->setText(0, text);
        const QStyle::StandardPixmap icon = op.status != OperationStatus::Valid ? QStyle::SP_MessageBoxWarning
                                            : op.type == OperationType::Boolean ? QStyle::SP_BrowserReload
                                            : op.type == OperationType::Extrude ? QStyle::SP_ArrowUp
                                                                                : QStyle::SP_DialogResetButton;
        item->setIcon(0, st->standardIcon(icon));
        item->setData(0, kItemKindRole, QStringLiteral("operacao"));
        item->setData(0, kItemIdRole, id);
        item->setToolTip(0, tr("%1 (%2) — %3%4")
                                .arg(QString::fromStdString(op.name), id, statusName(op.status),
                                     consumer ? tr(" — consumido por %1, oculto no visualizador e na exportação")
                                                    .arg(QString::fromStdString(consumer->name))
                                              : QString()));
        auto addChild = [&](const QString& childText, const char* kind) {
            auto* child = new QTreeWidgetItem(item);
            child->setText(0, childText);
            child->setData(0, kItemKindRole, QString::fromLatin1(kind));
            child->setData(0, kItemIdRole, id);
        };
        if (op.type == OperationType::Extrude)
        {
            const Sketch* sketch = m_document.findSketch(op.sketchId);
            addChild(tr("Esboço de origem: %1 (%2)")
                         .arg(sketch ? QString::fromStdString(sketch->name()) : tr("inexistente"),
                              QString::fromStdString(op.sketchId)),
                     "referencia");
            addChild(tr("Profundidade: %1").arg(mm(op.parameter("depth"))), "parametro");
            QString direction;
            if (sketch)
            {
                const gp_Dir n = op.reversed ? sketch->plane().normal().Reversed() : sketch->plane().normal();
                direction = vectorText(n);
            }
            addChild(tr("Sentido: %1 %2").arg(op.reversed ? tr("oposto à normal") : tr("normal do plano"), direction),
                     "parametro");
        }
        else if (op.type == OperationType::Fillet3D || op.type == OperationType::Chamfer3D)
        {
            addChild(tr("Corpo de origem: %1").arg(bodyDescription(op.sourceId)), "referencia");
            const char* key = op.type == OperationType::Fillet3D ? "radius" : "distance";
            addChild(tr("%1: %2").arg(op.type == OperationType::Fillet3D ? tr("Raio") : tr("Distância"),
                                      mm(op.parameter(key))),
                     "parametro");
            addChild(op.edges.size() == 1 ? tr("Arestas: 1") : tr("Arestas: %1").arg(op.edges.size()), "parametro");
        }
        else
        {
            addChild(tr("Tipo: %1").arg(booleanKindName(op.booleanKind)), "parametro");
            addChild(tr("%1: %2").arg(BooleanDialog::operandLabel(op.booleanKind, true), bodyDescription(op.operandA)),
                     "referencia");
            addChild(tr("%1: %2").arg(BooleanDialog::operandLabel(op.booleanKind, false), bodyDescription(op.operandB)),
                     "referencia");
            if (!op.result.IsNull())
                addChild(tr("Resultado: %1, volume %2 mm³")
                             .arg(solidsText(op.result), num(cad::modeling::ShapeInspector::volume(op.result))),
                         "parametro");
        }
        addChild(tr("Estado: %1").arg(statusName(op.status)), "estado");
        if (!toSelect && id == selectedId)
            toSelect = item;
        item->setExpanded(!consumer);
    }
    root->setExpanded(true);
    m_operationsTree->setCurrentItem(toSelect ? toSelect : root);
    m_updatingTree = false;
    showItemProperties(m_operationsTree->currentItem());
}

void MainWindow::onTreeItemChanged(QTreeWidgetItem* item)
{
    showItemProperties(item);
    if (!m_updatingTree)
        updateActionStates();
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
        if (op->type == OperationType::Extrude)
        {
            const Sketch* sketch = m_document.findSketch(op->sketchId);
            html += tr("<br>Esboço de origem: %1 (%2)<br>Sentido: %3<br>Volume: %4 mm³")
                        .arg(sketch ? QString::fromStdString(sketch->name()).toHtmlEscaped() : tr("inexistente"),
                             QString::fromStdString(op->sketchId),
                             op->reversed ? tr("oposto à normal do plano") : tr("normal do plano"),
                             num(cad::modeling::ShapeInspector::volume(op->result)));
        }
        if (op->type == OperationType::Boolean)
        {
            html += tr("<br>Operação: %1<br>%2: %3<br>%4: %5<br>Resultado: %6<br>Volume: %7 mm³")
                        .arg(booleanKindName(op->booleanKind),
                             BooleanDialog::operandLabel(op->booleanKind, true), bodyDescription(op->operandA).toHtmlEscaped(),
                             BooleanDialog::operandLabel(op->booleanKind, false), bodyDescription(op->operandB).toHtmlEscaped(),
                             solidsText(op->result), num(cad::modeling::ShapeInspector::volume(op->result)));
        }
        if (op->type == OperationType::Fillet3D || op->type == OperationType::Chamfer3D)
            html += tr("<br>Corpo de origem: %1<br>Arestas: %2<br>Volume: %3 mm³")
                        .arg(bodyDescription(op->sourceId).toHtmlEscaped())
                        .arg(op->edges.size())
                        .arg(num(cad::modeling::ShapeInspector::volume(op->result)));
        if (!op->consumedBy.empty())
            html += tr("<br><i>Consumido por %1: oculto no visualizador e na exportação (mantido no histórico)</i>")
                        .arg(bodyDescription(op->consumedBy).toHtmlEscaped());
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

// --- Extrusões ---------------------------------------------------------------

QString MainWindow::selectedSketchId() const
{
    QTreeWidgetItem* item = m_operationsTree ? m_operationsTree->currentItem() : nullptr;
    if (!item)
        return QString();
    const QString kind = item->data(0, kItemKindRole).toString();
    if (kind != QLatin1String("esboco") && kind != QLatin1String("entidade"))
        return QString();
    const QString id = item->data(0, kItemIdRole).toString();
    return m_document.findSketch(id.toStdString()) ? id : QString();
}

QString MainWindow::selectedOperationId() const
{
    QTreeWidgetItem* item = m_operationsTree ? m_operationsTree->currentItem() : nullptr;
    if (!item)
        return QString();
    const QString kind = item->data(0, kItemKindRole).toString();
    if (kind == QLatin1String("esboco") || kind == QLatin1String("entidade") || kind == QLatin1String("documento"))
        return QString();
    const QString id = item->data(0, kItemIdRole).toString();
    return m_document.findOperation(id.toStdString()) ? id : QString();
}

void MainWindow::selectTreeItem(const QString& id)
{
    QTreeWidgetItem* root = m_operationsTree->topLevelItem(0);
    for (int i = 0; root && i < root->childCount(); ++i)
        if (root->child(i)->data(0, kItemIdRole).toString() == id)
        {
            m_operationsTree->setCurrentItem(root->child(i));
            return;
        }
}

void MainWindow::extrude()
{
    if (isEditingSketch())
        return;
    const QString sketchId = selectedSketchId();
    if (sketchId.isEmpty())
    {
        statusBar()->showMessage(tr("Selecione um esboço na árvore para extrudar"), 8000);
        reportError(tr("Extrusão"), tr("Selecione na árvore de operações um esboço concluído com um perfil fechado "
                                       "(retângulo, círculo ou contorno de linhas) e acione Extrusão novamente."));
        return;
    }
    const Sketch* sketch = m_document.findSketch(sketchId.toStdString());
    const cad::sketch::ProfileResult profile = cad::sketch::buildProfile(*sketch);
    if (!profile.ok)
    {
        statusBar()->showMessage(tr("Esboço sem perfil válido para extrusão"), 8000);
        reportError(tr("Extrusão"), tr("O %1 não pode ser extrudado:\n%2")
                                        .arg(QString::fromStdString(sketch->name()), QString::fromStdString(profile.message)));
        return;
    }
    cad::document::ExtrudeParameters initial;
    initial.name = m_document.defaultExtrusionName();
    initial.sketchId = sketchId.toStdString();
    initial.depth = 10.0;
    const auto parameters = askExtrusion(initial, false);
    m_viewer->clearPreviewShape();
    if (!parameters)
    {
        statusBar()->showMessage(tr("Extrusão cancelada; documento mantido"), 5000);
        return;
    }
    addExtrusion(*parameters);
}

bool MainWindow::addExtrusion(const cad::document::ExtrudeParameters& parameters)
{
    std::string newId;
    const auto result = m_document.addExtrusion(parameters, &newId);
    if (!result.ok)
    {
        statusBar()->showMessage(tr("Extrusão recusada: %1").arg(QString::fromStdString(result.message)), 8000);
        reportError(tr("Extrusão"), tr("A extrusão não foi criada:\n%1").arg(QString::fromStdString(result.message)));
        return false;
    }
    refreshDocumentViews(true);
    selectTreeItem(QString::fromStdString(newId));
    const Operation* op = m_document.findOperation(newId);
    statusBar()->showMessage(tr("%1 criada: volume %2 mm³")
                                 .arg(QString::fromStdString(op->name), num(cad::modeling::ShapeInspector::volume(op->result))),
                             8000);
    return true;
}

QString MainWindow::bodyDescription(const std::string& bodyId) const
{
    const Operation* op = m_document.findOperation(bodyId);
    if (!op)
        return tr("inexistente (%1)").arg(QString::fromStdString(bodyId));
    return tr("%1 (%2)").arg(QString::fromStdString(op->name), QString::fromStdString(op->id));
}

void MainWindow::booleanCommand(cad::modeling::BooleanKind kind)
{
    if (isEditingSketch())
        return;
    const std::vector<std::string> bodies = m_document.activeBodies();
    if (bodies.size() < 2)
    {
        statusBar()->showMessage(tr("Operação booleana indisponível: são necessários dois corpos sólidos"), 8000);
        reportError(tr("Operação booleana"),
                    tr("A operação booleana exige pelo menos dois corpos sólidos ativos.\n\n"
                       "Crie extrusões (ou abra uma peça) e acione o comando novamente."));
        return;
    }
    // Corpo selecionado na árvore como A (se ativo); B = primeiro outro corpo ativo.
    cad::document::BooleanParameters initial;
    initial.kind = kind;
    initial.name = m_document.defaultBooleanName(kind);
    const std::string selected = selectedOperationId().toStdString();
    initial.operandA = std::find(bodies.begin(), bodies.end(), selected) != bodies.end() ? selected : bodies.front();
    for (const std::string& b : bodies)
        if (b != initial.operandA)
        {
            initial.operandB = b;
            break;
        }
    const auto parameters = askBoolean(initial, QString());
    m_viewer->clearOperandHighlight();
    if (!parameters)
    {
        statusBar()->showMessage(tr("Operação booleana cancelada; documento mantido"), 5000);
        return;
    }
    addBoolean(*parameters);
}

bool MainWindow::addBoolean(const cad::document::BooleanParameters& parameters)
{
    std::string newId;
    const auto result = m_document.addBoolean(parameters, &newId);
    if (!result.ok)
    {
        statusBar()->showMessage(tr("Operação booleana recusada: %1").arg(QString::fromStdString(result.message)), 8000);
        reportError(tr("Operação booleana"), tr("A operação não foi criada:\n%1\n\nO documento foi mantido.")
                                                 .arg(QString::fromStdString(result.message)));
        return false;
    }
    refreshDocumentViews(true);
    selectTreeItem(QString::fromStdString(newId));
    const Operation* op = m_document.findOperation(newId);
    statusBar()->showMessage(tr("%1 criada: %2, volume %3 mm³")
                                 .arg(QString::fromStdString(op->name), solidsText(op->result),
                                      num(cad::modeling::ShapeInspector::volume(op->result))),
                             10000);
    return true;
}

bool MainWindow::updateBoolean(const QString& operationId, const cad::document::BooleanParameters& parameters)
{
    const int revision = m_document.revision();
    const auto result = m_document.updateBoolean(operationId.toStdString(), parameters);
    if (!result.ok)
    {
        statusBar()->showMessage(tr("Operação booleana não alterada: %1").arg(QString::fromStdString(result.message)), 8000);
        reportError(tr("Operação booleana"), tr("A alteração não foi aplicada:\n%1\n\nA geometria foi mantida no último "
                                                "estado válido.")
                                                 .arg(QString::fromStdString(result.message)));
        return false;
    }
    if (m_document.revision() != revision)
    {
        refreshDocumentViews(true);
        selectTreeItem(operationId);
        statusBar()->showMessage(tr("Operação booleana recalculada"), 5000);
    }
    return true;
}

void MainWindow::editOperation()
{
    if (isEditingSketch())
        return;
    const QString id = selectedOperationId();
    const Operation* op = m_document.findOperation(id.toStdString());
    if (!op)
        return;
    if (op->type == OperationType::Fillet3D || op->type == OperationType::Chamfer3D)
    {
        startFinish(op->type, id);
        return;
    }
    if (op->type == OperationType::Boolean)
    {
        const auto parameters = askBoolean(m_document.booleanParameters(id.toStdString()), id);
        m_viewer->clearOperandHighlight();
        if (!parameters)
        {
            statusBar()->showMessage(tr("Edição da operação booleana cancelada"), 5000);
            return;
        }
        updateBoolean(id, *parameters);
        return;
    }
    if (op->type != OperationType::Extrude)
    {
        editParameters(); // bloco base / furo passante
        return;
    }
    const auto parameters = askExtrusion(m_document.extrudeParameters(id.toStdString()), true);
    m_viewer->clearPreviewShape();
    if (!parameters)
    {
        statusBar()->showMessage(tr("Edição da extrusão cancelada"), 5000);
        return;
    }
    updateExtrusion(id, *parameters);
}

bool MainWindow::updateExtrusion(const QString& operationId, const cad::document::ExtrudeParameters& parameters)
{
    const int revision = m_document.revision();
    const auto result = m_document.updateExtrusion(operationId.toStdString(), parameters);
    if (!result.ok)
    {
        statusBar()->showMessage(tr("Extrusão não alterada: %1").arg(QString::fromStdString(result.message)), 8000);
        reportError(tr("Extrusão"), tr("Os parâmetros não foram aplicados:\n%1\n\nA geometria foi mantida no último "
                                       "estado válido.")
                                        .arg(QString::fromStdString(result.message)));
        return false;
    }
    if (m_document.revision() != revision)
    {
        refreshDocumentViews(true);
        selectTreeItem(operationId);
        statusBar()->showMessage(tr("Extrusão recalculada"), 5000);
    }
    return true;
}

// --- Acabamentos 3D ----------------------------------------------------------

bool MainWindow::addFinish(OperationType type, const QString& sourceId, const QList<TopoDS_Shape>& edges, double value)
{
    std::vector<TopoDS_Edge> list;
    for (const TopoDS_Shape& s : edges)
        if (!s.IsNull() && s.ShapeType() == TopAbs_EDGE)
            list.push_back(TopoDS::Edge(s));
    cad::document::FinishParameters p;
    p.sourceId = sourceId.toStdString();
    p.value = value;
    p.edges = m_document.edgeSignatures(p.sourceId, list);
    std::string newId;
    const auto result = m_document.addFinish(type, p, &newId);
    if (!result.ok)
    {
        statusBar()->showMessage(tr("%1 recusado: %2").arg(operationTypeName(type), QString::fromStdString(result.message)), 8000);
        reportError(operationTypeName(type), tr("A operação não foi criada:\n%1\n\nO documento foi mantido.")
                                                 .arg(QString::fromStdString(result.message)));
        return false;
    }
    refreshDocumentViews(true);
    selectTreeItem(QString::fromStdString(newId));
    statusBar()->showMessage(tr("%1 criado: volume %2 mm³")
                                 .arg(QString::fromStdString(m_document.findOperation(newId)->name),
                                      num(cad::modeling::ShapeInspector::volume(m_document.shape()))),
                             8000);
    return true;
}

FinishDialog* MainWindow::finishDialog() const
{
    return m_finishDialog.data();
}

void MainWindow::finishCommand(OperationType type)
{
    startFinish(type, QString());
}

void MainWindow::startFinish(OperationType type, const QString& editId)
{
    if (isEditingSketch())
        return;
    if (m_finishDialog)
    {
        m_finishDialog->raise();
        m_finishDialog->activateWindow();
        return;
    }
    if (editId.isEmpty() && m_document.activeBodies().empty())
    {
        reportError(operationTypeName(type), tr("O documento não contém corpo sólido.\n\nCrie uma extrusão (ou abra uma "
                                                "peça) e acione o comando novamente."));
        return;
    }
    const bool editing = !editId.isEmpty();
    m_finishEditId = editId;
    m_finishSource.clear();
    QString name = QString::fromStdString(m_document.defaultFinishName(type));
    double value = type == OperationType::Fillet3D ? 3.0 : 2.0;
    if (editing)
    {
        const cad::document::FinishParameters p = m_document.finishParameters(editId.toStdString());
        name = QString::fromStdString(p.name);
        value = p.value;
        m_finishSource = QString::fromStdString(p.sourceId);
        // Exibe o corpo de origem no lugar do resultado (e dos acabamentos seguintes da mesma
        // cadeia) para selecionar as arestas sobre a geometria de origem.
        std::string descendant = editId.toStdString();
        while (const Operation* op = m_document.findOperation(descendant))
        {
            if (op->consumedBy.empty())
                break;
            descendant = op->consumedBy;
        }
        BRep_Builder builder;
        TopoDS_Compound compound;
        builder.MakeCompound(compound);
        for (const std::string& body : m_document.activeBodies())
            if (body != descendant)
                builder.Add(compound, m_document.findOperation(body)->result);
        builder.Add(compound, m_document.findOperation(p.sourceId)->result);
        m_viewer->setShape(compound);
    }
    m_viewer->clearOperandHighlight();
    m_finishDialog = new FinishDialog(type, name, value, editing, this);
    connect(m_finishDialog, &FinishDialog::previewChanged, this, [this](const cad::modeling::FeatureResult& r, bool show) {
        m_viewer->setPreviewShape(show && r.ok ? r.shape : TopoDS_Shape());
    });
    connect(m_finishDialog, &FinishDialog::clearSelectionRequested, m_viewer, &cad::viewer::OcctViewerWidget::clearSelection);
    connect(m_finishDialog, &QDialog::accepted, this, &MainWindow::applyFinishDialog);
    connect(m_finishDialog, &QDialog::finished, this, &MainWindow::endFinish);
    m_viewer->setEdgeSelectionMode(true);
    if (editing)
    {
        QList<TopoDS_Shape> edges;
        for (const TopoDS_Edge& e : m_document.resolvedFinishEdges(editId.toStdString()))
            edges << e;
        m_viewer->selectSubShapes(edges);
    }
    m_finishDialog->setEvaluator([this](double v) { return evaluateFinish(v); });
    updateFinishSelection();
    updateActionStates();
    m_finishDialog->show();
    // Ao lado do visualizador, sem cobrir o centro da peça.
    const QPoint corner = m_viewer->mapToGlobal(QPoint(m_viewer->width() - m_finishDialog->width() - 20, 20));
    m_finishDialog->move(corner);
    statusBar()->showMessage(tr("%1: selecione as arestas no visualizador (Shift + clique adiciona ou remove)")
                                 .arg(operationTypeName(type)));
}

std::vector<TopoDS_Edge> MainWindow::finishSelection(QString* problem) const
{
    std::vector<TopoDS_Edge> edges;
    QString body = m_finishEditId.isEmpty() ? QString() : m_finishSource;
    for (const TopoDS_Shape& s : m_viewer->selectedShapes())
    {
        if (s.ShapeType() != TopAbs_EDGE)
            continue;
        const TopoDS_Edge e = TopoDS::Edge(s);
        if (!m_finishEditId.isEmpty())
        {
            const Operation* source = m_document.findOperation(m_finishSource.toStdString());
            if (!source || !cad::modeling::containsEdge(source->result, e))
            {
                if (problem)
                    *problem = tr("Selecione arestas do corpo de origem (%1)").arg(bodyDescription(m_finishSource.toStdString()));
                continue;
            }
        }
        else
        {
            const QString owner = QString::fromStdString(m_document.bodyContaining(e));
            if (body.isEmpty())
                body = owner;
            else if (owner != body && problem)
                *problem = tr("As arestas devem pertencer a um mesmo corpo");
        }
        edges.push_back(e);
    }
    if (m_finishEditId.isEmpty())
        m_finishSource = body;
    return edges;
}

void MainWindow::updateFinishSelection()
{
    if (!m_finishDialog)
        return;
    QString problem;
    const std::vector<TopoDS_Edge> edges = finishSelection(&problem);
    m_finishDialog->setSelectionInfo(int(edges.size()),
                                     m_finishSource.isEmpty() ? QString() : bodyDescription(m_finishSource.toStdString()),
                                     problem);
}

cad::modeling::FeatureResult MainWindow::evaluateFinish(double value) const
{
    QString problem;
    const std::vector<TopoDS_Edge> edges = finishSelection(&problem);
    cad::modeling::FeatureResult r;
    if (!problem.isEmpty())
    {
        r.message = problem.toStdString();
        return r;
    }
    cad::document::FinishParameters p;
    p.sourceId = m_finishSource.toStdString();
    p.value = value;
    p.edges = m_document.edgeSignatures(p.sourceId, edges);
    return m_document.previewFinish(m_finishDialog->type(), p);
}

void MainWindow::applyFinishDialog()
{
    if (!m_finishDialog)
        return;
    const OperationType type = m_finishDialog->type();
    QString problem;
    const std::vector<TopoDS_Edge> edges = finishSelection(&problem);
    cad::document::FinishParameters p;
    p.name = m_finishDialog->name().toStdString();
    p.sourceId = m_finishSource.toStdString();
    p.value = m_finishDialog->value();
    p.edges = m_document.edgeSignatures(p.sourceId, edges);
    std::string id = m_finishEditId.toStdString();
    const auto result = m_finishEditId.isEmpty() ? m_document.addFinish(type, p, &id)
                                                 : m_document.updateFinish(id, p);
    if (!result.ok)
    {
        reportError(operationTypeName(type), tr("A operação não foi aplicada:\n%1\n\nO documento foi mantido no último "
                                                "estado válido.")
                                                 .arg(QString::fromStdString(result.message)));
        return;
    }
    m_finishEditId = QString::fromStdString(id); // selecionado após encerrar o comando
    statusBar()->showMessage(tr("%1 aplicado: volume %2 mm³")
                                 .arg(QString::fromStdString(m_document.findOperation(id)->name),
                                      num(cad::modeling::ShapeInspector::volume(m_document.shape()))),
                             8000);
}

void MainWindow::endFinish()
{
    if (!m_finishDialog)
        return;
    FinishDialog* dialog = m_finishDialog;
    m_finishDialog = nullptr;
    dialog->deleteLater();
    const QString select = dialog->result() == QDialog::Accepted ? m_finishEditId : QString();
    m_finishEditId.clear();
    m_finishSource.clear();
    m_viewer->clearPreviewShape();
    m_viewer->setEdgeSelectionMode(false); // modos de face, aresta e vértice restaurados
    refreshDocumentViews(true);            // geometria do documento (e não a de origem) de volta
    if (!select.isEmpty())
        selectTreeItem(select);
    else
        statusBar()->showMessage(tr("Comando cancelado; documento mantido"), 5000);
}

// --- Esboços -----------------------------------------------------------------

void MainWindow::setSketchModeUi(bool editing)
{
    for (QAction* a : {m_actToolSelect, m_actToolLine, m_actToolRectangle, m_actToolCircle, m_actToolArc,
                       m_actArcClockwise, m_actToolFillet2D, m_actToolChamfer2D, m_actFinishSketch, m_actCancelSketch})
        a->setEnabled(editing);
    // Ao sair do esboço a ferramenta ativa volta a Selecionar (navegação 3D normal).
    if (!editing)
        m_actToolSelect->setChecked(true);
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
    const int revision = m_document.revision();
    if (!m_document.commitSketch(*m_editSketch, error))
    {
        // Ex.: a alteração invalida uma extrusão dependente; o documento e o esboço salvo ficam
        // como estavam e a edição continua aberta para correção ou cancelamento.
        statusBar()->showMessage(tr("Esboço não concluído: %1").arg(QString::fromStdString(error)), 10000);
        reportError(tr("Concluir esboço"),
                    tr("O esboço não pôde ser gravado:\n%1\n\nO documento foi mantido no último estado válido; "
                       "corrija o esboço ou cancele a edição.")
                        .arg(QString::fromStdString(error)));
        return false;
    }
    const bool geometryChanged = m_document.revision() != revision; // extrusões dependentes recalculadas
    const QString name = QString::fromStdString(m_editSketch->name());
    const QString id = QString::fromStdString(m_editSketch->id());
    m_editSketch.reset();
    m_selectedEntity.clear();
    m_viewer->exitSketchMode();
    setSketchModeUi(false);
    refreshDocumentViews(geometryChanged);
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

bool MainWindow::applyCornerModifier(EntityType type, double u, double v, double tolerance, double value)
{
    if (!isEditingSketch())
        return false;
    const auto corner = cad::sketch::nearestCorner(*m_editSketch, gp_Pnt2d(u, v), tolerance);
    if (!corner)
    {
        statusBar()->showMessage(tr("Nenhum canto livre entre segmentos retos nesse ponto"), 8000);
        reportError(entityTypeName(type), tr("Não há canto livre entre dois segmentos retos conectados nesse ponto.\n\n"
                                             "Clique sobre o vértice de um retângulo ou na extremidade comum de duas "
                                             "linhas que ainda não tenha fillet ou chanfro."));
        return false;
    }
    SketchGeometry g;
    if (type == EntityType::Fillet)
        g = cad::sketch::SketchFillet{corner->entityA, corner->entityB, corner->corner, value};
    else
        g = cad::sketch::SketchChamfer{corner->entityA, corner->entityB, corner->corner, value};
    std::string newId;
    const auto result = m_editSketch->addEntity(g, &newId);
    if (!result.ok)
    {
        statusBar()->showMessage(tr("%1 recusado: %2").arg(entityTypeName(type), QString::fromStdString(result.message)), 8000);
        reportError(entityTypeName(type), tr("O %1 não foi aplicado:\n%2\n\nO esboço foi mantido.")
                                              .arg(entityTypeName(type), QString::fromStdString(result.message)));
        return false;
    }
    m_selectedEntity = QString::fromStdString(newId);
    refreshSketchViews();
    if (const SketchEntity* e = m_editSketch->findEntity(newId))
        statusBar()->showMessage(tr("Criado: %1").arg(entitySummary(*e)), 8000);
    return true;
}

void MainWindow::deleteSelectedEntity()
{
    if (!isEditingSketch() || m_selectedEntity.isEmpty())
        return;
    std::string error;
    if (!m_editSketch->removeEntity(m_selectedEntity.toStdString(), error))
    {
        statusBar()->showMessage(tr("Entidade não excluída: %1").arg(QString::fromStdString(error)), 8000);
        reportError(tr("Excluir entidade"), QString::fromStdString(error));
        return;
    }
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
        if (m_finishDialog)
        {
            updateFinishSelection();
            statusBar()->showMessage(tr("Selecione as arestas no visualizador (Shift + clique adiciona ou remove)"));
            return;
        }
        statusBar()->showMessage(m_readyMessage);
        return;
    }
    if (m_finishDialog)
        updateFinishSelection();
    if (selected.size() > 1)
    {
        const bool edges = std::all_of(selected.begin(), selected.end(),
                                       [](const TopoDS_Shape& s) { return s.ShapeType() == TopAbs_EDGE; });
        statusBar()->showMessage(edges ? tr("%1 arestas selecionadas").arg(selected.size())
                                       : tr("%1 entidades selecionadas").arg(selected.size()));
        return;
    }

    const QLocale locale(QLocale::Portuguese, QLocale::Brazil);
    auto num = [&locale](double v) { return locale.toString(v, 'f', 2); };

    const auto info = cad::modeling::describeSubShape(m_viewer->shape(), selected.first());
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

void MainWindow::exportStl()
{
    if (part().IsNull())
    {
        statusBar()->showMessage(tr("Nada para exportar: o documento não contém sólido"), 8000);
        reportError(tr("Exportar STL"),
                    tr("O documento não contém sólido para exportar.\n\n"
                       "Esboços 2D não são exportados em STL; crie uma extrusão ou abra uma peça com sólido."));
        return;
    }
    const QString base = QString::fromStdString(m_document.name());
    const auto request = askStlExport(QDir(m_lastExportDir).filePath(base + QStringLiteral(".stl")));
    if (!request)
    {
        statusBar()->showMessage(tr("Exportação STL cancelada"), 5000);
        return;
    }
    QString path = request->filePath;
    if (QFileInfo(path).suffix().isEmpty())
        path += QStringLiteral(".stl");
    if (exportStlTo(path, request->options))
        m_lastExportDir = QFileInfo(path).absolutePath();
}

bool MainWindow::exportStlTo(const QString& filePath, const cad::io::StlExportOptions& options)
{
    const QString nativePath = QDir::toNativeSeparators(filePath);
    if (part().IsNull())
    {
        reportError(tr("Exportar STL"), tr("O documento não contém sólido para exportar."));
        return false;
    }
    if (QFileInfo::exists(filePath) && !confirmOverwrite(nativePath))
    {
        statusBar()->showMessage(tr("Exportação cancelada: arquivo existente mantido"), 5000);
        return false;
    }

    QApplication::setOverrideCursor(Qt::WaitCursor);
    const auto result = cad::io::StlExporter().write(part(), std::filesystem::path(filePath.toStdWString()), options);
    QApplication::restoreOverrideCursor();
    if (!result.ok)
    {
        statusBar()->showMessage(tr("Falha na exportação STL"), 8000);
        reportError(tr("Exportar STL"), tr("Não foi possível exportar o arquivo:\n%1\n\n%2")
                                            .arg(nativePath, QString::fromStdString(result.message)));
        return false;
    }
    const QLocale locale(QLocale::Portuguese, QLocale::Brazil);
    statusBar()->showMessage(tr("STL exportado: %1 (%2 triângulos)").arg(nativePath).arg(result.triangles), 8000);
    reportInfo(tr("Exportar STL"),
               tr("Arquivo exportado com sucesso:\n%1\n\nFormato: %2\nSólidos: %3\nTriângulos: %4\n"
                  "Deflexão linear: %5 mm\nDeflexão angular: %6°\n\n"
                  "As coordenadas estão em milímetros. O formato STL não armazena a unidade.")
                   .arg(nativePath, options.format == cad::io::StlFormat::Ascii ? tr("ASCII") : tr("binário"))
                   .arg(result.solids)
                   .arg(result.triangles)
                   .arg(locale.toString(result.parameters.linearDeflection, 'f', 4),
                        locale.toString(result.parameters.angularDeflectionDeg, 'f', 1)));
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

std::optional<cad::document::ExtrudeParameters> MainWindow::askExtrusion(const cad::document::ExtrudeParameters& initial,
                                                                        bool editing)
{
    const Sketch* sketch = m_document.findSketch(initial.sketchId);
    const QString description = sketch ? tr("%1 (%2, plano %3)")
                                             .arg(QString::fromStdString(sketch->name()),
                                                  QString::fromStdString(sketch->id()),
                                                  QString::fromLatin1(SketchPlane::name(sketch->plane().type())))
                                       : QString::fromStdString(initial.sketchId);
    // A pré-visualização avalia os parâmetros sem alterar o documento.
    ExtrusionDialog dialog(initial, description,
                           [this](const cad::document::ExtrudeParameters& p) { return m_document.previewExtrusion(p); },
                           editing, this);
    connect(&dialog, &ExtrusionDialog::previewChanged, this,
            [this](const cad::modeling::FeatureResult& r, bool show) {
                m_viewer->setPreviewShape(show && r.ok ? r.shape : TopoDS_Shape());
            });
    // Força a primeira pré-visualização (o diálogo avaliou antes da conexão).
    {
        const auto r = m_document.previewExtrusion(dialog.parameters());
        m_viewer->setPreviewShape(r.ok ? r.shape : TopoDS_Shape());
    }
    const bool accepted = dialog.exec() == QDialog::Accepted;
    m_viewer->clearPreviewShape();
    if (!accepted)
        return std::nullopt;
    return dialog.parameters();
}

std::optional<cad::document::BooleanParameters> MainWindow::askBoolean(const cad::document::BooleanParameters& initial,
                                                                      const QString& operationId)
{
    // Corpos disponíveis: ativos ao criar; na edição, os corpos anteriores à operação que estão
    // ativos ou são consumidos por ela.
    BooleanDialog::BodyList bodies;
    const std::string editing = operationId.toStdString();
    for (const Operation& op : m_document.operations())
    {
        if (!editing.empty() && op.id == editing)
            break;
        if (CadDocument::producesBody(op.type) && (op.consumedBy.empty() || op.consumedBy == editing))
            bodies.emplace_back(op.id, bodyDescription(op.id));
    }
    BooleanDialog dialog(
        initial, bodies,
        [this, editing](const cad::document::BooleanParameters& p) {
            if (editing.empty())
                return m_document.previewBoolean(p);
            // Edição: avalia diretamente com os resultados dos operandos (consumidos pela própria
            // operação), sem alterar o documento.
            const Operation* a = m_document.findOperation(p.operandA);
            const Operation* b = m_document.findOperation(p.operandB);
            if (!a || !b || a == b)
            {
                cad::modeling::FeatureResult r;
                r.message = tr("Selecione dois corpos diferentes").toStdString();
                return r;
            }
            return cad::modeling::booleanOperation(p.kind, a->result, b->result);
        },
        !editing.empty(), this);
    auto highlight = [this](const QString& a, const QString& b) {
        const Operation* opA = m_document.findOperation(a.toStdString());
        const Operation* opB = m_document.findOperation(b.toStdString());
        m_viewer->setOperandHighlight(opA ? opA->result : TopoDS_Shape(), opB ? opB->result : TopoDS_Shape());
    };
    connect(&dialog, &BooleanDialog::operandsChanged, this, highlight);
    highlight(QString::fromStdString(dialog.parameters().operandA), QString::fromStdString(dialog.parameters().operandB));
    const bool accepted = dialog.exec() == QDialog::Accepted;
    m_viewer->clearOperandHighlight();
    if (!accepted)
        return std::nullopt;
    return dialog.parameters();
}

std::optional<MainWindow::StlExportRequest> MainWindow::askStlExport(const QString& suggestedPath)
{
    StlExportDialog dialog(suggestedPath, part(), this);
    if (dialog.exec() != QDialog::Accepted)
        return std::nullopt;
    return StlExportRequest{dialog.filePath(), dialog.options()};
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

std::optional<double> MainWindow::askCornerValue(EntityType type, double initial)
{
    bool ok = false;
    const double value = QInputDialog::getDouble(
        this, entityTypeName(type),
        type == EntityType::Fillet ? tr("Raio do arredondamento (mm):") : tr("Distância do chanfro, medida em cada lado (mm):"),
        initial, 0.0, cad::sketch::kMaxSize, 2, &ok);
    if (!ok)
        return std::nullopt;
    return value;
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
