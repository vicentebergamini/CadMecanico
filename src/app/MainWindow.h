// Janela principal do CAD Mecânico.
#pragma once

#include "document/CadDocument.h"
#include "document/Diagnostics.h"
#include "io/StlExporter.h"

#include <QMainWindow>
#include <QPointer>
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

class FinishDialog;

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
    // Exportação STL da geometria sólida atual (confirmação antes de sobrescrever).
    bool exportStlTo(const QString& filePath, const cad::io::StlExportOptions& options);

    // Pedido de exportação STL (caminho + opções), devolvido pelo diálogo.
    struct StlExportRequest
    {
        QString filePath;
        cad::io::StlExportOptions options;
    };

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
    // Esboço sobre a face plana dada (E5.6): vista normal à face e eixos locais. Recusado com
    // mensagem para faces curvas ou fora de um corpo.
    bool startSketchOnFace(const TopoDS_Shape& face);
    bool editSketch(const QString& sketchId);
    bool finishSketch();
    // "Concluir esboço" da interface (E5.7, P1): conclui e, para um esboço novo criado sobre uma
    // face pelo fluxo rápido com perfil fechado, abre o diálogo de extrusão (nada é extrudado sem
    // a confirmação dos parâmetros). A opção "actionExtrusaoAoConcluir" desativa a abertura.
    bool finishSketchCommand();
    void cancelSketch();
    // Acrescenta uma entidade ao esboço em edição (criação pelo mouse ou programática).
    bool addSketchGeometry(const cad::sketch::SketchGeometry& geometry);
    // Aplica nova geometria à entidade (mesmo tipo); valores inválidos são recusados.
    bool updateSketchEntity(const QString& entityId, const cad::sketch::SketchGeometry& geometry);
    void selectSketchEntity(const QString& entityId);
    // Fillet 2D / Chamfer 2D no canto mais próximo de (u, v) dentro da tolerância (mm), com o
    // raio/distância dado (E5.5). Retorna false (com mensagem) se não houver canto ou o
    // modificador for inviável.
    bool applyCornerModifier(cad::sketch::EntityType type, double u, double v, double tolerance, double value);
    // Chamfer 2D com modo/distâncias/ângulo (E5.6) no canto mais próximo de (u, v).
    bool applyChamfer2D(double u, double v, double tolerance, const cad::sketch::SketchChamfer& parameters);

    // --- Extrusões (E5.3) -----------------------------------------------------
    // Esboço/operação selecionados na árvore (vazio se nenhum).
    QString selectedSketchId() const;
    QString selectedOperationId() const;
    // Cria/edita a extrusão; erros são informados e o documento não muda.
    bool addExtrusion(const cad::document::ExtrudeParameters& parameters);
    bool updateExtrusion(const QString& operationId, const cad::document::ExtrudeParameters& parameters);
    // Seleciona um item da árvore pelo id (operação ou esboço).
    void selectTreeItem(const QString& id);

    // --- Operações booleanas (E5.4) ---------------------------------------------
    // Cria/edita a booleana; erros são informados e o documento não muda.
    bool addBoolean(const cad::document::BooleanParameters& parameters);
    bool updateBoolean(const QString& operationId, const cad::document::BooleanParameters& parameters);
    // Descrição de um corpo para a interface: "Extrusão 1 (op-1)".
    QString bodyDescription(const std::string& bodyId) const;

    // --- Acabamentos 3D (E5.5B) ---------------------------------------------------
    // Cria o acabamento nas arestas dadas do corpo (sem diálogo); erros são informados.
    bool addFinish(cad::document::OperationType type, const QString& sourceId, const QList<TopoDS_Shape>& edges,
                   double value);
    // Diálogo não modal aberto pelo comando Fillet 3D / Chamfer 3D (nulo fora do comando).
    FinishDialog* finishDialog() const;

    // --- Diagnóstico (E5.7, P2) -----------------------------------------------------
    // Erros e avisos do documento (operações inválidas, esboços sem suporte ou sem perfil) e as
    // alterações recusadas desde a última alteração aplicada.
    std::vector<cad::document::Diagnostic> diagnostics() const;
    QTreeWidget* diagnosticsList() const { return m_diagnostics; }

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

    void extrude();
    void extrudeCommand(cad::document::ExtrudeMode mode);
    void sketchOnSelectedFace();
    void booleanCommand(cad::modeling::BooleanKind kind);
    // Inicia o comando (seleção de arestas + diálogo não modal).
    void finishCommand(cad::document::OperationType type);
    void editOperation();
    void exportStl();

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
    // Diálogo da extrusão (com pré-visualização); vazio = cancelado.
    virtual std::optional<cad::document::ExtrudeParameters> askExtrusion(const cad::document::ExtrudeParameters& initial,
                                                                        bool editing);
    virtual std::optional<StlExportRequest> askStlExport(const QString& suggestedPath);
    // Diálogo da booleana; 'operationId' = operação em edição (vazio ao criar). Vazio = cancelado.
    virtual std::optional<cad::document::BooleanParameters> askBoolean(const cad::document::BooleanParameters& initial,
                                                                      const QString& operationId);
    virtual std::optional<cad::sketch::SketchGeometry> askEntityDimensions(const cad::sketch::SketchEntity& entity);
    // Raio do Fillet 2D; vazio = cancelado.
    virtual std::optional<double> askCornerValue(cad::sketch::EntityType type, double initial);
    // Parâmetros do Chamfer 2D (modo, D1, D2, ângulo, lado de referência); vazio = cancelado.
    virtual std::optional<cad::sketch::SketchChamfer> askChamfer2D(const cad::sketch::SketchChamfer& initial);
    // Saliência que não toca o corpo: perguntar se deve ser criada como novo corpo.
    virtual bool askConvertToNewBody(const QString& message);
    // "Até a face" (E5.7.1): face plana limite clicada no visualizador (corpo + referência
    // persistente); vazio = cancelado (Esc).
    virtual std::optional<std::pair<std::string, cad::modeling::FaceSignature>> askLimitFace();

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
    // Face plana selecionada no visualizador (nula se a seleção não for uma única face).
    TopoDS_Shape selectedFace() const;
    // Fluxo rápido (P1): face plana selecionada fora do esboço.
    bool planarFaceSelected() const;
    // Tipo padrão da extrusão do esboço selecionado: saliência para esboço sobre face com corpo
    // disponível; novo corpo nos demais casos.
    cad::document::ExtrudeMode defaultExtrudeMode() const;
    // Diagnóstico: atualiza o painel; registra uma alteração recusada (o documento foi mantido).
    void refreshDiagnostics();
    void recordRejected(const std::string& itemId, const std::string& itemName, cad::document::OperationType type,
                        const std::string& message);
    // Corpos disponíveis para saliência/corte (ativos; na edição, também o consumido pela operação).
    std::vector<std::pair<std::string, QString>> extrusionTargets(const std::string& editingId) const;
    // Acabamentos 3D: sessão de seleção de arestas.
    void startFinish(cad::document::OperationType type, const QString& editId);
    void updateFinishSelection();
    std::vector<TopoDS_Edge> finishSelection(QString* problem) const;
    cad::modeling::FeatureResult evaluateFinish(double value) const;
    void applyChamferOptions(cad::document::FinishParameters& p, const std::vector<TopoDS_Edge>& edges) const;
    void applyFinishDialog();
    void endFinish();

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
    QAction* m_actExportStl = nullptr;
    QAction* m_actExtrudeUnified = nullptr;
    QAction* m_actAutoExtrude = nullptr;
    QAction* m_actExtrude = nullptr;
    QAction* m_actExtrudeBoss = nullptr;
    QAction* m_actExtrudeCut = nullptr;
    QAction* m_actSketchOnFace = nullptr;
    QAction* m_actEditOperation = nullptr;
    QAction* m_actFuse = nullptr;
    QAction* m_actCut = nullptr;
    QAction* m_actCommon = nullptr;
    QAction* m_actFillet3D = nullptr;
    QAction* m_actChamfer3D = nullptr;
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
    QAction* m_actToolArc = nullptr;
    QAction* m_actToolFillet2D = nullptr;
    QAction* m_actToolChamfer2D = nullptr;
    QAction* m_actArcClockwise = nullptr;
    QAction* m_actEntityDimensions = nullptr;
    QAction* m_actDeleteEntity = nullptr;
    QAction* m_actFinishSketch = nullptr;
    QAction* m_actCancelSketch = nullptr;
    QActionGroup* m_sketchTools = nullptr;
    QToolBar* m_toolBarSketch = nullptr;

    std::optional<cad::sketch::Sketch> m_editSketch;
    bool m_editSketchIsNew = false;
    bool m_editSketchOnFace = false; // esboço novo sobre face (fluxo rápido P1)
    std::vector<cad::document::Diagnostic> m_rejected; // alterações recusadas (sessão)
    QString m_selectedEntity;
    bool m_updatingTree = false;
    QString m_extrusionEditId; // extrusão em edição no diálogo
    QPointer<FinishDialog> m_finishDialog;
    QString m_finishEditId;          // acabamento em edição (vazio ao criar)
    mutable QString m_finishSource;  // corpo de origem das arestas selecionadas

    QMenu* m_menuStandardViews = nullptr;
    QToolBar* m_toolBarFile = nullptr;
    QToolBar* m_toolBarModeling = nullptr;
    QToolBar* m_toolBarView = nullptr;
    QDockWidget* m_dockOperations = nullptr;
    QTreeWidget* m_operationsTree = nullptr;
    QDockWidget* m_dockProperties = nullptr;
    QLabel* m_properties = nullptr;
    QDockWidget* m_dockDiagnostics = nullptr;
    QTreeWidget* m_diagnostics = nullptr;
    QLabel* m_diagnosticsSummary = nullptr;
    QLabel* m_diagnosticsDetail = nullptr;
    int m_lastDiagnosticErrors = 0;
    cad::viewer::OcctViewerWidget* m_viewer = nullptr;
};

} // namespace cad::app
