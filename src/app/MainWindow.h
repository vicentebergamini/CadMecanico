// Janela principal do CAD Mecânico.
#pragma once

#include "document/CadDocument.h"

#include <QMainWindow>
#include <QString>

#include <TopoDS_Shape.hxx>

#include <optional>

class QAction;
class QCloseEvent;
class QDockWidget;
class QLabel;
class QMenu;
class QToolBar;
class QTreeWidget;
class QTreeWidgetItem;

namespace cad::viewer {
class OcctViewerWidget;
}

namespace cad::app {

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    // Papéis de dados dos itens da árvore de operações.
    static constexpr int kItemKindRole = Qt::UserRole;     // "documento", "operacao", "parametro", "estado"
    static constexpr int kItemIdRole = Qt::UserRole + 1;   // id da operação (itens de operação e filhos)

    enum class SaveChoice
    {
        Save,
        Discard,
        Cancel
    };

    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

    // Exporta a peça atual para o arquivo indicado, pedindo confirmação antes de
    // sobrescrever. Retorna true se o arquivo foi gravado.
    bool exportStepTo(const QString& filePath);

    const cad::document::CadDocument& document() const { return m_document; }
    const TopoDS_Shape& part() const { return m_document.shape(); }
    QString documentPath() const { return m_documentPath; }

    // Valida e aplica novos parâmetros (recalcula, atualiza visualizador, árvore e estado).
    // Parâmetros inválidos são recusados com mensagem e o documento não muda.
    bool applyParameters(const cad::document::PartParameters& parameters);

    // Salva no caminho indicado (pede confirmação se o arquivo existir).
    bool saveDocumentTo(const QString& filePath);
    // Abre o documento; em caso de falha o documento ativo é mantido.
    bool openDocumentFrom(const QString& filePath);

    cad::viewer::OcctViewerWidget* viewer() const { return m_viewer; }
    QTreeWidget* operationsTree() const { return m_operationsTree; }

public slots:
    void exportStep();
    void showAbout();
    // Mostra na barra de status as informações da entidade selecionada no visualizador.
    void updateSelectionStatus();

    void newDocument();
    void openDocument();
    bool saveDocument();
    bool saveDocumentAs();
    void editParameters();

protected:
    void closeEvent(QCloseEvent* event) override;

    // Pontos de interação com o usuário, substituíveis nos testes automatizados.
    virtual QString askExportPath(const QString& suggestedPath);
    virtual bool confirmOverwrite(const QString& filePath);
    virtual void reportInfo(const QString& title, const QString& text);
    virtual void reportError(const QString& title, const QString& text);
    virtual std::optional<cad::document::PartParameters> askParameters(const cad::document::PartParameters& current);
    virtual QString askOpenPath(const QString& directory);
    virtual QString askSavePath(const QString& suggestedPath);
    virtual SaveChoice askSaveChanges();

private:
    void createActions();
    void createMenus();
    void createToolBars();
    void createDocks();
    void createCentralArea();

    // Pergunta se deve salvar alterações pendentes; false se o usuário cancelar.
    bool maybeSaveChanges();
    bool writeDocument(const QString& filePath);
    // Atualiza árvore, título, propriedades e (se a geometria mudou) o visualizador.
    void refreshDocumentViews(bool geometryChanged);
    void rebuildTree();
    void updateTitle();
    void showItemProperties(QTreeWidgetItem* item);
    QString documentDirectory() const;

    cad::document::CadDocument m_document;
    QString m_documentPath;
    QString m_lastExportDir;
    QString m_lastDocumentDir;
    QString m_readyMessage;

    QAction* m_actNew = nullptr;
    QAction* m_actOpen = nullptr;
    QAction* m_actSave = nullptr;
    QAction* m_actSaveAs = nullptr;
    QAction* m_actExportStep = nullptr;
    QAction* m_actQuit = nullptr;
    QAction* m_actUndo = nullptr;
    QAction* m_actRedo = nullptr;
    QAction* m_actEditParameters = nullptr;
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
    QDockWidget* m_dockProperties = nullptr;
    QLabel* m_properties = nullptr;
    cad::viewer::OcctViewerWidget* m_viewer = nullptr;
};

} // namespace cad::app
