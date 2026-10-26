// Visualizador 3D: QOpenGLWidget renderizado pelo Open CASCADE (V3d/AIS).
#pragma once

#include <QImage>
#include <QList>
#include <QOpenGLWidget>
#include <QString>

#include <AIS_InteractiveContext.hxx>
#include <AIS_Shape.hxx>
#include <AIS_Trihedron.hxx>
#include <AIS_ViewController.hxx>
#include <AIS_ViewCube.hxx>
#include <TopoDS_Shape.hxx>
#include <V3d_View.hxx>
#include <V3d_Viewer.hxx>

#include "sketch/Sketch.h"

#include <QHash>

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace cad::discretization {
class Discretization;
}

namespace cad::viewer {

// Exibição da discretização volumétrica (E5.7.2).
struct DiscretizationDisplay
{
    bool solid = true;       // sólido original (translúcido enquanto a discretização é exibida)
    bool vertexNodes = true; // nós dos vértices (azul)
    bool centerNodes = true; // nós centrais (vermelho)
    bool cubeEdges = true;   // contornos dos cubos (cinza)
};

// Carga gráfica efetivamente enviada ao visualizador.
struct DiscretizationOverlayStats
{
    std::int64_t vertexPoints = 0;
    std::int64_t centerPoints = 0;
    std::int64_t segments = 0;
    bool reducedPoints = false; // amostragem dos nós (acima de kMaxDisplayPoints)
    bool surfaceEdgesOnly = false; // só arestas da superfície externa da discretização
    bool reducedEdges = false;  // amostragem das arestas (acima de kMaxDisplaySegments)
    double buildSeconds = 0.0;
};

enum class StandardView
{
    Front,
    Top,
    Right,
    Isometric
};

// Ferramentas do modo de esboço.
enum class SketchTool
{
    Select,
    Line,
    Rectangle,
    Circle,
    Arc,      // E5.5: centro -> ponto inicial -> ponto final
    Fillet2D, // E5.5: clique em um canto
    Chamfer2D // E5.5: clique em um canto
};

// Navegação: botão central gira, Ctrl + botão central desloca, roda aproxima no cursor,
// F ajusta à janela. Botão esquerdo seleciona faces, arestas e vértices (Shift alterna).
//
// Modo de esboço (E5.2/E5.2.1): câmera perpendicular ao plano, rotação e ViewCube desativados;
// sólidos, outros esboços e eixos globais ficam ocultos até sair do modo.
//   Botão esquerdo: ferramenta ativa. Selecionar = clique sobre uma entidade 2D; Linha e
//   Retângulo = clique no primeiro ponto e clique no segundo (pré-visualização entre eles);
//   Círculo = clique no centro e clique em um ponto da circunferência. Arco (E5.5) = clique no
//   centro, no ponto inicial (define o raio) e no ponto final (projetado na circunferência,
//   indicado por um raio auxiliar), no sentido escolhido em setArcClockwise (anti-horário por
//   padrão). Fillet 2D / Chamfer 2D = clique perto de um canto (sketchCornerPicked). Esc cancela
//   os pontos pendentes. Botão central (com ou sem Ctrl) desloca; roda aproxima no cursor; F ajusta.
//   Somente o esboço ativo e seus eixos locais H/V são exibidos.
class OcctViewerWidget : public QOpenGLWidget, public AIS_ViewController
{
    Q_OBJECT

public:
    explicit OcctViewerWidget(QWidget* parent = nullptr);
    ~OcctViewerWidget() override;

    // Exibe a peça (substitui a anterior). O shape não é modificado.
    void setShape(const TopoDS_Shape& shape);
    const TopoDS_Shape& shape() const { return m_shapeData; }

    // Sub-shapes (faces, arestas, vértices) da peça atualmente selecionados.
    QList<TopoDS_Shape> selectedShapes() const;
    // Seleciona exatamente as sub-shapes dadas (pertencentes à peça exibida).
    void selectSubShapes(const QList<TopoDS_Shape>& shapes);

    // Modo de seleção somente de arestas (Fillet 3D / Chamfer 3D, E5.5B): faces e vértices
    // deixam de ser selecionáveis e seleções que não sejam arestas são descartadas. Ao desligar,
    // os modos de face, aresta e vértice anteriores são restaurados.
    void setEdgeSelectionMode(bool edgesOnly);
    bool edgeSelectionMode() const { return m_edgesOnly; }

    // true após a criação bem-sucedida do contexto OpenGL e da janela OCCT.
    bool isViewerInitialized() const { return m_initialized; }
    // Fabricante, renderizador e versão do OpenGL em uso (vazio antes da inicialização).
    QString glInfo() const { return m_glInfo; }
    // Número de quadros efetivamente renderizados pelo OCCT.
    int frameCount() const { return m_frameCount; }

    const occ::handle<V3d_Viewer>& viewer() const { return m_viewer; }
    const occ::handle<V3d_View>& view() const { return m_view; }
    const occ::handle<AIS_InteractiveContext>& aisContext() const { return m_context; }
    const occ::handle<AIS_Shape>& shapePresentation() const { return m_shape; }
    const occ::handle<AIS_ViewCube>& viewCube() const { return m_viewCube; }
    // Eixos de referência X/Y/Z na origem global (tamanho constante na tela).
    occ::handle<AIS_InteractiveObject> originAxes() const { return m_originAxes; }
    // Objetos exibidos dos esboços do documento (fora da edição).
    const std::vector<occ::handle<AIS_Shape>>& documentSketchPresentations() const { return m_documentSketchShapes; }

    // Imagem do framebuffer (pixels físicos).
    QImage grabViewerImage();

    // Pré-visualização temporária de um sólido (ex.: extrusão antes de confirmar): translúcida,
    // não selecionável e fora do documento. Forma nula remove a pré-visualização.
    void setPreviewShape(const TopoDS_Shape& shape);
    // Pré-visualização de material removido (corte, E5.6): vermelho translúcido.
    void setPreviewShape(const TopoDS_Shape& shape, bool removal);
    void clearPreviewShape() { setPreviewShape(TopoDS_Shape()); }
    const occ::handle<AIS_Shape>& previewPresentation() const { return m_previewSolid; }

    // Destaque dos operandos de uma operação booleana (E5.4): A (corpo principal) em azul e B
    // (ferramenta) em vermelho, translúcidos e não selecionáveis. Formas nulas removem o destaque.
    void setOperandHighlight(const TopoDS_Shape& a, const TopoDS_Shape& b);
    void clearOperandHighlight() { setOperandHighlight(TopoDS_Shape(), TopoDS_Shape()); }
    const occ::handle<AIS_Shape>& operandAPresentation() const { return m_operandA; }
    const occ::handle<AIS_Shape>& operandBPresentation() const { return m_operandB; }

    // --- Discretização volumétrica (E5.7.2) -----------------------------------
    // Nós e contornos dos cubos exibidos como objetos AGRUPADOS (uma nuvem de pontos para os
    // vértices, uma para os centros e um conjunto de segmentos para as arestas), não selecionáveis;
    // o B-Rep do sólido não é alterado. Limites de carga: até kMaxDisplayPoints pontos por nuvem
    // (acima disso, amostragem regular) e até kMaxDisplaySegments segmentos (acima disso, só as
    // arestas da superfície externa da discretização e, se ainda exceder, amostragem). Nula remove.
    // A discretização exibida é descartada em setShape() (pertence à forma anterior).
    static constexpr std::int64_t kMaxDisplayPoints = 1'000'000;
    static constexpr std::int64_t kMaxDisplaySegments = 1'500'000;
    void setDiscretization(std::shared_ptr<const cad::discretization::Discretization> data);
    void clearDiscretization() { setDiscretization(nullptr); }
    bool hasDiscretization() const { return m_discretization != nullptr; }
    void setDiscretizationDisplay(const DiscretizationDisplay& display);
    const DiscretizationDisplay& discretizationDisplay() const { return m_discretizationDisplay; }
    const DiscretizationOverlayStats& discretizationOverlayStats() const { return m_discretizationStats; }
    occ::handle<AIS_InteractiveObject> vertexNodesPresentation() const { return m_vertexNodes; }
    occ::handle<AIS_InteractiveObject> centerNodesPresentation() const { return m_centerNodes; }
    occ::handle<AIS_InteractiveObject> cubeEdgesPresentation() const { return m_cubeEdges; }
    // true se o sólido está exibido (falso quando ocultado pela opção da discretização).
    bool isSolidDisplayed() const;

    // --- Esboços --------------------------------------------------------------
    // Esboços do documento exibidos fora da edição (somente visualização).
    void setDocumentSketches(const std::vector<cad::sketch::Sketch>& sketches);
    // Câmera perpendicular ao plano do esboço (olhando contra a normal, eixo vertical local para
    // cima), para qualquer plano — padrão ou de face (E5.6).
    void enterSketchMode(const cad::sketch::Sketch& sketch);
    // Contorno da face de suporte exibido como referência durante a edição (esboço sobre face).
    void setSketchReference(const TopoDS_Shape& reference);
    void updateEditingSketch(const cad::sketch::Sketch& sketch, const QString& selectedEntityId);
    void exitSketchMode();
    bool isSketchMode() const { return m_sketchPlane.has_value(); }
    void setSketchTool(SketchTool tool);
    SketchTool sketchTool() const { return m_sketchTool; }
    // Sentido dos arcos criados com o mouse (false = anti-horário).
    void setArcClockwise(bool clockwise);
    bool arcClockwise() const { return m_arcClockwise; }
    // Distância no plano do esboço (mm) correspondente a 'pixels' lógicos na tela.
    double sketchDistanceForPixels(double pixels) const;
    // Ponto do plano do esboço sob a posição lógica do mouse (coordenadas locais, mm,
    // arredondadas a 0,1 mm). Vazio fora do modo de esboço.
    std::optional<gp_Pnt2d> sketchPointAt(const QPointF& logicalPos) const;
    // Objeto exibido de uma entidade em edição (para testes e diagnóstico).
    occ::handle<AIS_Shape> sketchEntityPresentation(const QString& entityId) const;
    // Entrada em andamento (E5.6.1): primeiro ponto já definido e pré-visualização dinâmica.
    bool hasPendingSketchPoint() const { return m_pendingPoint.has_value(); }
    const occ::handle<AIS_Shape>& sketchPreviewPresentation() const { return m_preview; }

public slots:
    void fitAll();
    void setStandardView(cad::viewer::StandardView view);
    void viewFront() { setStandardView(StandardView::Front); }
    void viewTop() { setStandardView(StandardView::Top); }
    void viewRight() { setStandardView(StandardView::Right); }
    void viewIsometric() { setStandardView(StandardView::Isometric); }
    void clearSelection();
    void cancelSketchInput();

signals:
    void viewerInitialized();
    void initializationFailed(const QString& message);
    void selectionChanged();
    // Geometria criada com o mouse no modo de esboço (coordenadas locais do plano).
    void sketchGeometryRequested(const cad::sketch::SketchGeometry& geometry);
    // Clique com a ferramenta Selecionar: id da entidade ou vazio (clique fora das entidades).
    void sketchEntityClicked(const QString& entityId);
    // Clique com Fillet 2D / Chamfer 2D: ponto local (mm, sem arredondamento) e tolerância de
    // proximidade do canto (mm).
    void sketchCornerPicked(cad::viewer::SketchTool tool, double u, double v, double tolerance);

protected:
    void initializeGL() override;
    void paintGL() override;

    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;

    // AIS_ViewController
    void handleViewRedraw(const occ::handle<AIS_InteractiveContext>& theCtx,
                          const occ::handle<V3d_View>& theView) override;
    void OnSelectionChanged(const occ::handle<AIS_InteractiveContext>& theCtx,
                            const occ::handle<V3d_View>& theView) override;

private:
    void setupViewer();
    void setupNavigation();
    void applyDevicePixelRatio(qreal dpr);
    void releaseGraphics();
    void updateView();
    // Redesenho completo (após exibir/ocultar objetos).
    void redrawScene();
    // Posição do mouse (pixels lógicos do Qt) em pixels físicos da janela OCCT.
    NCollection_Vec2<int> toViewPixels(const QPointF& pos) const;

    void setupSketchNavigation(bool sketchMode);
    bool hasFittableContent() const;
    // Ajusta à janela; cena vazia enquadra uma região padrão em torno da origem.
    void fitScene();
    void clearEditShapes();
    void updatePreview(const gp_Pnt2d& current);
    void removePreview();
    void handleSketchClick(const QPointF& pos);
    // Ponto local sob o mouse, sem arredondamento.
    std::optional<gp_Pnt2d> rawSketchPointAt(const QPointF& logicalPos) const;

    occ::handle<V3d_Viewer> m_viewer;
    occ::handle<V3d_View> m_view;
    occ::handle<AIS_InteractiveContext> m_context;
    occ::handle<AIS_Shape> m_shape;
    occ::handle<AIS_ViewCube> m_viewCube;
    occ::handle<AIS_Trihedron> m_originAxes;
    TopoDS_Shape m_shapeData;

    bool m_initialized = false;
    bool m_fitPending = true;
    int m_frameCount = 0;
    qreal m_appliedDpr = 0.0;
    QString m_glInfo;

    // Esboços
    std::vector<occ::handle<AIS_Shape>> m_documentSketchShapes;
    std::vector<occ::handle<AIS_InteractiveObject>> m_sketchDecorations; // eixos locais e rótulos
    QHash<QString, occ::handle<AIS_Shape>> m_editShapes;               // id da entidade -> objeto
    occ::handle<AIS_Shape> m_preview;
    occ::handle<AIS_Shape> m_previewSolid;
    occ::handle<AIS_Shape> m_operandA;
    occ::handle<AIS_Shape> m_operandB;
    std::optional<cad::sketch::SketchPlane> m_sketchPlane;
    SketchTool m_sketchTool = SketchTool::Select;
    std::optional<gp_Pnt2d> m_pendingPoint;
    std::optional<gp_Pnt2d> m_arcStart; // arco: ponto inicial já definido (centro = m_pendingPoint)
    bool m_arcClockwise = false;
    QPointF m_leftPressPos;
    bool m_leftPressed = false;
    bool m_leftDragging = false; // arrasto do botão esquerdo iniciado (desenho por arrasto)
    bool m_edgesOnly = false;
    // Ativa no objeto da peça os modos de seleção correspondentes ao modo atual.
    void activateSelectionModes();

    // Discretização (E5.7.2)
    void removeDiscretizationObjects();
    // Exibe/oculta os objetos da discretização e o sólido conforme as opções e o modo atual.
    void applyDiscretizationDisplay();
    std::shared_ptr<const cad::discretization::Discretization> m_discretization;
    DiscretizationDisplay m_discretizationDisplay;
    DiscretizationOverlayStats m_discretizationStats;
    occ::handle<AIS_InteractiveObject> m_vertexNodes;
    occ::handle<AIS_InteractiveObject> m_centerNodes;
    occ::handle<AIS_InteractiveObject> m_cubeEdges;
};

} // namespace cad::viewer
