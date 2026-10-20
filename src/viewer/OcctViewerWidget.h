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

namespace cad::viewer {

enum class StandardView
{
    Front,
    Top,
    Right,
    Isometric
};

// Navegação: botão central gira, Ctrl + botão central desloca, roda aproxima no cursor,
// F ajusta à janela. Botão esquerdo seleciona faces, arestas e vértices (Shift alterna).
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

public slots:
    void fitAll();
    void setStandardView(cad::viewer::StandardView view);
    void viewFront() { setStandardView(StandardView::Front); }
    void viewTop() { setStandardView(StandardView::Top); }
    void viewRight() { setStandardView(StandardView::Right); }
    void viewIsometric() { setStandardView(StandardView::Isometric); }
    void clearSelection();

signals:
    void viewerInitialized();
    void initializationFailed(const QString& message);
    void selectionChanged();

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
};

} // namespace cad::viewer
