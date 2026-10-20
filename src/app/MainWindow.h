// Janela principal do CAD Mecânico.
#pragma once

#include <QMainWindow>
#include <QString>

#include <TopoDS_Shape.hxx>

class QAction;
class QDockWidget;
class QMenu;
class QToolBar;
class QTreeWidget;

namespace cad::viewer {
class OcctViewerWidget;
}

namespace cad::app {

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

    // Exporta a peça atual para o arquivo indicado, pedindo confirmação antes de
    // sobrescrever. Retorna true se o arquivo foi gravado.
    bool exportStepTo(const QString& filePath);

    const TopoDS_Shape& part() const { return m_part; }

    cad::viewer::OcctViewerWidget* viewer() const { return m_viewer; }

public slots:
    void exportStep();
    void showAbout();
    // Mostra na barra de status as informações da entidade selecionada no visualizador.
    void updateSelectionStatus();

protected:
    // Pontos de interação com o usuário, substituíveis nos testes automatizados.
    virtual QString askExportPath(const QString& suggestedPath);
    virtual bool confirmOverwrite(const QString& filePath);
    virtual void reportInfo(const QString& title, const QString& text);
    virtual void reportError(const QString& title, const QString& text);

private:
    void createActions();
    void createMenus();
    void createToolBars();
    void createDocks();
    void createCentralArea();

    TopoDS_Shape m_part;
    QString m_lastExportDir;
    QString m_readyMessage;

    QAction* m_actNew = nullptr;
    QAction* m_actOpen = nullptr;
    QAction* m_actExportStep = nullptr;
    QAction* m_actQuit = nullptr;
    QAction* m_actUndo = nullptr;
    QAction* m_actRedo = nullptr;
    QAction* m_actFitAll = nullptr;
    QAction* m_actViewFront = nullptr;
    QAction* m_actViewTop = nullptr;
    QAction* m_actViewRight = nullptr;
    QAction* m_actViewIso = nullptr;
    QAction* m_actAbout = nullptr;

    QMenu* m_menuStandardViews = nullptr;
    QToolBar* m_toolBarFile = nullptr;
    QToolBar* m_toolBarModeling = nullptr;
    QToolBar* m_toolBarView = nullptr;
    QDockWidget* m_dockOperations = nullptr;
    QTreeWidget* m_operationsTree = nullptr;
    cad::viewer::OcctViewerWidget* m_viewer = nullptr;
};

} // namespace cad::app
