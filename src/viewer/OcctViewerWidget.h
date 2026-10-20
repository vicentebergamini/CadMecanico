// Visualizador 3D: QOpenGLWidget renderizado pelo Open CASCADE (V3d/AIS).
#pragma once

#include <QImage>
#include <QOpenGLWidget>
#include <QString>

#include <AIS_InteractiveContext.hxx>
#include <AIS_Shape.hxx>
#include <AIS_ViewController.hxx>
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

class OcctViewerWidget : public QOpenGLWidget, public AIS_ViewController
{
    Q_OBJECT

public:
    explicit OcctViewerWidget(QWidget* parent = nullptr);
    ~OcctViewerWidget() override;

    // Exibe a peça (substitui a anterior). O shape não é modificado.
    void setShape(const TopoDS_Shape& shape);
    const TopoDS_Shape& shape() const { return m_shapeData; }

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

    // Imagem do framebuffer (pixels físicos).
    QImage grabViewerImage();

public slots:
    void fitAll();
    void setStandardView(cad::viewer::StandardView view);
    void viewFront() { setStandardView(StandardView::Front); }
    void viewTop() { setStandardView(StandardView::Top); }
    void viewRight() { setStandardView(StandardView::Right); }
    void viewIsometric() { setStandardView(StandardView::Isometric); }

signals:
    void viewerInitialized();
    void initializationFailed(const QString& message);

protected:
    void initializeGL() override;
    void paintGL() override;

    // AIS_ViewController
    void handleViewRedraw(const occ::handle<AIS_InteractiveContext>& theCtx,
                          const occ::handle<V3d_View>& theView) override;

private:
    void setupViewer();
    void releaseGraphics();
    void updateView();

    occ::handle<V3d_Viewer> m_viewer;
    occ::handle<V3d_View> m_view;
    occ::handle<AIS_InteractiveContext> m_context;
    occ::handle<AIS_Shape> m_shape;
    TopoDS_Shape m_shapeData;

    bool m_initialized = false;
    bool m_fitPending = true;
    int m_frameCount = 0;
    QString m_glInfo;
};

} // namespace cad::viewer
