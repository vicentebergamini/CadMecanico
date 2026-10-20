// Janela principal do CAD Mecânico.
#pragma once

#include "document/CadDocument.h"

#include <QMainWindow>
#include <QString>

#include <TopoDS_Shape.hxx>

#include <optional>

class QAction;
class QActionGroup;
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
    static constexpr int kItemKindRole = Qt::UserRole;     // "documento", "operacao", "parametro", "estado",
                                                           // "esboco", "entidade"
    static constexpr int kItemIdRole = Qt::UserRole + 1;   // id da operação ou do esboço
    static constexpr int kItemEntityRole = Qt::UserRole + 2; // id da entidade (itens "entidade")

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

    // --- Esboços (E5.2) -------------------------------------------------------
    // A edição trabalha sobre uma cópia; o documento só muda em finishSketch().
    bool isEditingSketch() const { return m_editSketch.has_value(); }
    const cad::sketch::Sketch* editingSketch() const { return m_editSketch ? &*m_editSketch : nullptr; }
    QString selectedSketchEntity() const { return m_selectedEntity; }
    bool startNewSketch(cad::sketch::PlaneType plane);
    bool editSketch(const QString& sketchId);
    bool finishSketch();
    void cancelSketch();
    // Acrescenta uma entidade ao esboço em edição (criação pelo mouse ou programática).
    bool addSketchGeometry(const cad::sketch::SketchGeometry& geometry);
    // Aplica nova geometria à entidade (mesmo tipo); valores inválidos são recusados.
    bool updateSketchEntity(const QString& entityId, const cad::sketch::SketchGeometry& geometry);
    void selectSketchEntity(const QString& entityId);

public slots:
    void exportStep();
    void showAbout();
    // Mostra na barra de status as informações da entidade selecionada no visualizador.
    void updateSelectionStatus();

    // Documento vazio (sem operações nem esboços).
    void newDocument();
    // Documento de exemplo com a peça de teste (bloco com furo); nunca criado automaticamente.
    void newExampleDocument();
    void openDocument();
    bool saveDocument();
    bool saveDocumentAs();
    void editParameters();

    void newSketch();
    void editSelectedSketch();
    void editEntityDimensions();
    void deleteSelectedEntity();

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
    virtual std::optional<cad::sketch::PlaneType> askSketchPlane();
    virtual std::optional<cad::sketch::SketchGeometry> askEntityDimensions(const cad::sketch::SketchEntity& entity);

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
    void setSketchModeUi(bool editing);
    void updateActionStates();
    void refreshSketchViews();
    void onTreeItemChanged(QTreeWidgetItem* item);
    // Esboços exibidos na árvore/visualizador: os do documento, com o esboço em edição substituído.
    std::vector<cad::sketch::Sketch> displayedSketches(bool includeEditing) const;

    cad::document::CadDocument m_document;
    QString m_documentPath;
    QString m_lastExportDir;
    QString m_lastDocumentDir;
    QString m_readyMessage;

    QAction* m_actNew = nullptr;
    QAction* m_actExampleBlock = nullptr;
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
    QAction* m_actNewSketch = nullptr;
    QAction* m_actEditSketch = nullptr;
    QAction* m_actToolSelect = nullptr;
    QAction* m_actToolLine = nullptr;
    QAction* m_actToolRectangle = nullptr;
    QAction* m_actToolCircle = nullptr;
    QAction* m_actEntityDimensions = nullptr;
    QAction* m_actDeleteEntity = nullptr;
    QAction* m_actFinishSketch = nullptr;
    QAction* m_actCancelSketch = nullptr;
    QActionGroup* m_sketchTools = nullptr;
    QToolBar* m_toolBarSketch = nullptr;

    std::optional<cad::sketch::Sketch> m_editSketch;
    bool m_editSketchIsNew = false;
    QString m_selectedEntity;
    bool m_updatingTree = false;

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
