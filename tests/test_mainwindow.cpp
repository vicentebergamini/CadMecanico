// Testes da janela principal (Qt Test): estrutura, menus, barras e exportação STEP.
// A edição paramétrica e os arquivos .cadmec são testados em test_documento_ui.
#include "app/MainWindow.h"
#include "io/StepImporter.h"
#include "modeling/ShapeInspector.h"
#include "viewer/OcctViewerWidget.h"
#include "document/CadDocument.h"

#include <QAction>
#include <QApplication>
#include <QDir>
#include <QDockWidget>
#include <QFile>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QTreeWidget>

#include <filesystem>

namespace {

constexpr double kExpectedVolume = 100.0 * 60.0 * 20.0 - 3.14159265358979323846 * 10.0 * 10.0 * 20.0;

// Substitui os diálogos modais por respostas programadas.
class TestableMainWindow : public cad::app::MainWindow
{
public:
    QString nextPath;
    bool overwriteAnswer = false;
    int pathRequests = 0;
    int overwriteRequests = 0;
    QStringList infos;
    QStringList errors;

protected:
    QString askExportPath(const QString&) override
    {
        ++pathRequests;
        return nextPath;
    }
    bool confirmOverwrite(const QString&) override
    {
        ++overwriteRequests;
        return overwriteAnswer;
    }
    void reportInfo(const QString&, const QString& text) override { infos << text; }
    void reportError(const QString&, const QString& text) override { errors << text; }
};

QString plainText(const QString& s)
{
    QString t = s;
    return t.remove('&');
}

QStringList actionTexts(const QMenu* menu)
{
    QStringList texts;
    for (QAction* a : menu->actions())
        texts << (a->isSeparator() ? QStringLiteral("|") : plainText(a->text()));
    return texts;
}

QByteArray readAll(const QString& path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

void checkStepFile(const QString& path)
{
    const auto read = cad::io::StepImporter().read(std::filesystem::path(path.toStdWString()));
    QVERIFY2(read.ok, read.message.c_str());
    QCOMPARE(read.rootCount, 1);
    const auto s = cad::modeling::ShapeInspector::inspect(read.shape);
    QVERIFY(s.isValid);
    QCOMPARE(s.solids, 1);
    QCOMPARE(s.faces, 7);
    QVERIFY(qAbs(s.volume - kExpectedVolume) <= kExpectedVolume * 1e-9);
}

} // namespace

class TestMainWindow : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void abreJanela();
    void menus();
    void barrasDeFerramentas();
    void painelArvoreOperacoes();
    void areaCentral();
    void exportarPeloMenu();
    void exportarPelaBarra();
    void exportarAcrescentaExtensao();
    void exportarCancelado();
    void sobrescritaRecusada();
    void sobrescritaAceita();
    void exportarErroDiretorioInexistente();
    void geometriaNaoAlterada();
    void sobre();
    void sair();

private:
    QAction* action(const char* name) const { return m_window->findChild<QAction*>(name); }

    TestableMainWindow* m_window = nullptr;
    QTemporaryDir* m_dir = nullptr;
};

void TestMainWindow::init()
{
    m_dir = new QTemporaryDir;
    QVERIFY(m_dir->isValid());
    m_window = new TestableMainWindow;
    m_window->show();
    QVERIFY(QTest::qWaitForWindowExposed(m_window));
    // A peça de teste não é mais criada na inicialização: criada explicitamente (exemplo).
    m_window->findChild<QAction*>("actionExemploBlocoFuro")->trigger();
    QVERIFY(m_window->document().hasPart());
}


void TestMainWindow::cleanup()
{
    delete m_window;
    m_window = nullptr;
    delete m_dir;
    m_dir = nullptr;
}

void TestMainWindow::abreJanela()
{
    QVERIFY(m_window->isVisible());
    QVERIFY(m_window->windowTitle().contains(QStringLiteral("CAD Mecânico")));
    QVERIFY(m_window->windowTitle().contains(QStringLiteral("Peça de teste")));
    QVERIFY(!m_window->isWindowModified());
    QVERIFY(m_window->statusBar()->currentMessage().contains("Open CASCADE"));
}

void TestMainWindow::menus()
{
    QStringList titles;
    for (QAction* a : m_window->menuBar()->actions())
        titles << plainText(a->text());
    // E5.7.4a: menu "Análise MED" entre Ferramentas e Visualizar.
    QCOMPARE(titles, QStringList({"Arquivo", "Editar", "Operações", "Esboço", "Ferramentas", "Análise MED", "Visualizar",
                                  "Ajuda"}));

    auto* file = m_window->findChild<QMenu*>("menuArquivo");
    QVERIFY(file);
    QCOMPARE(actionTexts(file), QStringList({"Novo", "Exemplos", "Abrir...", "Salvar", "Salvar como...", "|", "Exportar", "|", "Sair"}));
    auto* exportMenu = m_window->findChild<QMenu*>("menuExportar");
    QVERIFY(exportMenu);
    QCOMPARE(actionTexts(exportMenu), QStringList({"STEP (*.step)...", "STL (*.stl)...", "Nuvem de pontos ASCII (*.txt)..."}));
    // E5.7.2: Ferramentas > Discretização volumétrica (exportação só com discretização gerada).
    auto* tools = m_window->findChild<QMenu*>("menuFerramentas");
    QVERIFY(tools);
    QCOMPARE(actionTexts(tools), QStringList({"Discretização volumétrica...", "Exibição da discretização",
                                              "Remover discretização da vista"}));
    QVERIFY(action("actionDiscretizacao")->isEnabled());
    QVERIFY(!action("actionExportarNuvemPontos")->isEnabled());
    auto* operations = m_window->findChild<QMenu*>("menuOperacoes");
    QVERIFY(operations);
    QCOMPARE(actionTexts(operations), QStringList({"Extrusão...", "Extrusão de Novo Corpo...", "Extrusão de Saliência...", "Extrusão de Corte...", "|", "União...", "Subtração...", "Interseção...", "|",
                                                    "Fillet 3D...", "Chamfer 3D...", "|", "Editar operação..."}));
    auto* sketchMenu = m_window->findChild<QMenu*>("menuEsboco");
    QVERIFY(sketchMenu);
    QCOMPARE(actionTexts(sketchMenu), QStringList({"Novo esboço...", "Criar Esboço na Face", "Editar esboço", "|", "Selecionar", "Linha",
                                                   "Retângulo", "Círculo", "Arco circular", "Sentido horário do arco",
                                                   "Fillet 2D", "Chamfer 2D", "|", "Dimensões...", "Excluir entidade", "|",
                                                   "Concluir esboço", "Cancelar esboço", "|",
                                                   "Abrir extrusão ao concluir esboço na face"}));
    QVERIFY(!action("actionExtrusao")->isEnabled()); // nenhum esboço selecionado
    QVERIFY(!action("actionExtrusaoUnificada")->isEnabled());
    QVERIFY(action("actionExtrusaoAoConcluir")->isChecked());
    QVERIFY(action("actionNovo")->isEnabled());
    QVERIFY(action("actionAbrir")->isEnabled());
    QVERIFY(action("actionSalvar")->isEnabled());
    QVERIFY(action("actionSalvarComo")->isEnabled());
    QVERIFY(action("actionExportarStep")->isEnabled());
    QVERIFY(action("actionSair")->isEnabled());

    auto* edit = m_window->findChild<QMenu*>("menuEditar");
    QVERIFY(edit);
    QCOMPARE(actionTexts(edit), QStringList({"Desfazer", "Refazer", "|", "Parâmetros da peça..."}));
    QVERIFY(action("actionEditarParametros")->isEnabled());
    QVERIFY(!action("actionDesfazer")->isEnabled());
    QVERIFY(!action("actionRefazer")->isEnabled());

    auto* view = m_window->findChild<QMenu*>("menuVisualizar");
    QVERIFY(view);
    QCOMPARE(actionTexts(view).mid(0, 2), QStringList({"Ajustar à janela", "Vistas padrão"}));
    QVERIFY(actionTexts(view).contains(QStringLiteral("Diagnóstico"))); // painel da E5.7
    QVERIFY(action("actionAjustarJanela")->isEnabled());
    auto* views = m_window->findChild<QMenu*>("menuVistasPadrao");
    QVERIFY(views);
    QVERIFY(views->isEnabled());
    QCOMPARE(actionTexts(views), QStringList({"Frente", "Superior", "Direita", "Isométrica"}));
    for (QAction* a : views->actions())
        QVERIFY(a->isEnabled());

    auto* help = m_window->findChild<QMenu*>("menuAjuda");
    QVERIFY(help);
    QCOMPARE(actionTexts(help), QStringList({"Sobre o CAD Mecânico"}));
}

void TestMainWindow::barrasDeFerramentas()
{
    auto* file = m_window->findChild<QToolBar*>("toolBarArquivo");
    QVERIFY(file);
    QVERIFY(file->isVisible());
    QVERIFY(file->actions().contains(action("actionExportarStep")));
    QVERIFY(file->actions().contains(action("actionSair")));

    auto* modeling = m_window->findChild<QToolBar*>("toolBarModelagem");
    QVERIFY(modeling);
    QVERIFY(modeling->actions().contains(action("actionEditarParametros")));

    auto* view = m_window->findChild<QToolBar*>("toolBarVisualizar");
    QVERIFY(view);
    QVERIFY(view->actions().contains(action("actionAjustarJanela")));
    QVERIFY(view->actions().contains(action("actionVistaIsometrica")));
}

void TestMainWindow::painelArvoreOperacoes()
{
    auto* dock = m_window->findChild<QDockWidget*>("dockArvoreOperacoes");
    QVERIFY(dock);
    QVERIFY(dock->isVisible());
    QCOMPARE(dock->windowTitle(), QStringLiteral("Árvore de Operações"));
    QCOMPARE(m_window->dockWidgetArea(dock), Qt::LeftDockWidgetArea);

    auto* tree = m_window->findChild<QTreeWidget*>("arvoreOperacoes");
    QVERIFY(tree);
    QCOMPARE(tree->topLevelItemCount(), 1);
    QCOMPARE(tree->topLevelItem(0)->text(0), QStringLiteral("Peça de teste"));
    QCOMPARE(tree->topLevelItem(0)->childCount(), 2); // bloco base e furo passante
}

void TestMainWindow::areaCentral()
{
    // Visualizador 3D (a renderização com OpenGL real é testada em test_viewer/test_app_viewer;
    // aqui a plataforma offscreen não oferece contexto OpenGL).
    auto* viewer = qobject_cast<cad::viewer::OcctViewerWidget*>(m_window->centralWidget());
    QVERIFY(viewer);
    QCOMPARE(viewer, m_window->viewer());
    QCOMPARE(viewer->objectName(), QStringLiteral("visualizador3D"));
    QVERIFY(viewer->shape().IsSame(m_window->part()));
    QVERIFY(!m_window->findChild<QLabel*>("areaCentral"));
}

void TestMainWindow::exportarPeloMenu()
{
    const QString path = m_dir->filePath("menu.step");
    m_window->nextPath = path;
    action("actionExportarStep")->trigger();

    QCOMPARE(m_window->pathRequests, 1);
    QCOMPARE(m_window->overwriteRequests, 0);
    QCOMPARE(m_window->infos.size(), 1);
    QVERIFY(m_window->errors.isEmpty());
    QVERIFY(QFile::exists(path));
    checkStepFile(path);
}

void TestMainWindow::exportarPelaBarra()
{
    auto* toolBar = m_window->findChild<QToolBar*>("toolBarArquivo");
    auto* button = qobject_cast<QToolButton*>(toolBar->widgetForAction(action("actionExportarStep")));
    QVERIFY(button);

    const QString path = m_dir->filePath("barra.step");
    m_window->nextPath = path;
    QTest::mouseClick(button, Qt::LeftButton);

    QCOMPARE(m_window->pathRequests, 1);
    QVERIFY(QFile::exists(path));
    checkStepFile(path);
}

void TestMainWindow::exportarAcrescentaExtensao()
{
    m_window->nextPath = m_dir->filePath("sem_extensao");
    action("actionExportarStep")->trigger();

    QVERIFY(QFile::exists(m_dir->filePath("sem_extensao.step")));
    QVERIFY(!QFile::exists(m_dir->filePath("sem_extensao")));
}

void TestMainWindow::exportarCancelado()
{
    m_window->nextPath.clear();
    action("actionExportarStep")->trigger();

    QCOMPARE(m_window->pathRequests, 1);
    QVERIFY(m_window->infos.isEmpty());
    QVERIFY(m_window->errors.isEmpty());
    QVERIFY(QDir(m_dir->path()).entryList(QDir::Files).isEmpty());
}

void TestMainWindow::sobrescritaRecusada()
{
    const QString path = m_dir->filePath("existente.step");
    const QByteArray original("conteudo original");
    {
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(original);
    }

    m_window->nextPath = path;
    m_window->overwriteAnswer = false;
    action("actionExportarStep")->trigger();

    QCOMPARE(m_window->overwriteRequests, 1);
    QVERIFY(m_window->infos.isEmpty());
    QCOMPARE(readAll(path), original);
}

void TestMainWindow::sobrescritaAceita()
{
    const QString path = m_dir->filePath("existente.step");
    {
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("conteudo original");
    }

    m_window->nextPath = path;
    m_window->overwriteAnswer = true;
    action("actionExportarStep")->trigger();

    QCOMPARE(m_window->overwriteRequests, 1);
    QCOMPARE(m_window->infos.size(), 1);
    checkStepFile(path);
}

void TestMainWindow::exportarErroDiretorioInexistente()
{
    const QString path = m_dir->filePath("nao_existe/peca.step");
    QVERIFY(!m_window->exportStepTo(path));
    QCOMPARE(m_window->errors.size(), 1);
    QVERIFY(m_window->infos.isEmpty());
    QVERIFY(!QFile::exists(path));
}

void TestMainWindow::geometriaNaoAlterada()
{
    const TopoDS_Shape before = m_window->part();
    const double volumeBefore = cad::modeling::ShapeInspector::volume(before);

    m_window->nextPath = m_dir->filePath("a.step");
    action("actionExportarStep")->trigger();
    m_window->nextPath = m_dir->filePath("b.step");
    action("actionExportarStep")->trigger();

    QVERIFY(m_window->part().IsSame(before));
    QCOMPARE(cad::modeling::ShapeInspector::volume(m_window->part()), volumeBefore);
    QVERIFY(qAbs(volumeBefore - kExpectedVolume) <= kExpectedVolume * 1e-9);
}

void TestMainWindow::sobre()
{
    QString title;
    QString text;
    QTimer::singleShot(0, [&] {
        if (auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget()))
        {
            title = box->windowTitle();
            text = box->text();
            box->accept();
        }
    });
    action("actionSobre")->trigger();

    QCOMPARE(title, QStringLiteral("Sobre o CAD Mecânico"));
    QVERIFY(text.contains("Qt"));
    QVERIFY(text.contains("Open CASCADE"));
}

void TestMainWindow::sair()
{
    action("actionSair")->trigger();
    QTRY_VERIFY(!m_window->isVisible());
}

QTEST_MAIN(TestMainWindow)
#include "test_mainwindow.moc"
