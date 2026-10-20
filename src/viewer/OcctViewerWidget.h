// Visualizador 3D: QOpenGLWidget renderizado pelo Open CASCADE (V3d/AIS).
#pragma once

#include <QImage>
#include <QList>
#include <QOpenGLWidget>
#include <QString>

#include <AIS_InteractiveContext.hxx>
#include <AIS_Shape.hxx>
#include <AIS_ViewController.hxx>
#include <AIS_ViewCube.hxx>
#include <TopoDS_Shape.hxx>
#include <V3d_View.hxx>
#include <V3d_Viewer.hxx>

#include "sketch/Sketch.h"

#include <QHash>

#include <optional>
#include <vector>

namespace cad::viewer {

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
    Circle
};

// Navegação: botão central gira, Ctrl + botão central desloca, roda aproxima no cursor,
// F ajusta à janela. Botão esquerdo seleciona faces, arestas e vértices (Shift alterna).
//
// Modo de esboço (E5.2): câmera perpendicular ao plano, rotação e ViewCube desativados.
//   Botão esquerdo: ferramenta ativa. Selecionar = clique sobre uma entidade 2D; Linha e
//   Retângulo = clique no primeiro ponto e clique no segundo (pré-visualização entre eles);
//   Círculo = clique no centro e clique em um ponto da circunferência. Esc cancela o ponto
//   pendente. Botão central (com ou sem Ctrl) desloca; roda aproxima no cursor; F ajusta.
//   O sólido continua visível, mas não é selecionável.
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

    // Imagem do framebuffer (pixels físicos).
    QImage grabViewerImage();

    // --- Esboços --------------------------------------------------------------
    // Esboços do documento exibidos fora da edição (somente visualização).
    void setDocumentSketches(const std::vector<cad::sketch::Sketch>& sketches);
    void enterSketchMode(const cad::sketch::Sketch& sketch);
    void updateEditingSketch(const cad::sketch::Sketch& sketch, const QString& selectedEntityId);
    void exitSketchMode();
    bool isSketchMode() const { return m_sketchPlane.has_value(); }
    void setSketchTool(SketchTool tool);
    SketchTool sketchTool() const { return m_sketchTool; }
    // Ponto do plano do esboço sob a posição lógica do mouse (coordenadas locais, mm,
    // arredondadas a 0,1 mm). Vazio fora do modo de esboço.
    std::optional<gp_Pnt2d> sketchPointAt(const QPointF& logicalPos) const;
    // Objeto exibido de uma entidade em edição (para testes e diagnóstico).
    occ::handle<AIS_Shape> sketchEntityPresentation(const QString& entityId) const;

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
    // Posição do mouse (pixels lógicos do Qt) em pixels físicos da janela OCCT.
    NCollection_Vec2<int> toViewPixels(const QPointF& pos) const;

    void setupSketchNavigation(bool sketchMode);
    void clearEditShapes();
    void updatePreview(const gp_Pnt2d& current);
    void removePreview();
    void handleSketchClick(const QPointF& pos);

    occ::handle<V3d_Viewer> m_viewer;
    occ::handle<V3d_View> m_view;
    occ::handle<AIS_InteractiveContext> m_context;
    occ::handle<AIS_Shape> m_shape;
    occ::handle<AIS_ViewCube> m_viewCube;
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
    std::optional<cad::sketch::SketchPlane> m_sketchPlane;
    SketchTool m_sketchTool = SketchTool::Select;
    std::optional<gp_Pnt2d> m_pendingPoint;
    QPointF m_leftPressPos;
    bool m_leftPressed = false;
};

} // namespace cad::viewer
