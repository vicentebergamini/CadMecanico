#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wingdi.h>
#include <winuser.h>
#endif

#include "viewer/OcctViewerWidget.h"

#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QSurfaceFormat>

#include <Aspect_DisplayConnection.hxx>
#include <Aspect_NeutralWindow.hxx>
#include <Graphic3d_MaterialAspect.hxx>
#include <Message.hxx>
#include <OpenGl_Context.hxx>
#include <OpenGl_FrameBuffer.hxx>
#include <OpenGl_GraphicDriver.hxx>
#include <Prs3d_Drawer.hxx>
#include <Prs3d_LineAspect.hxx>
#include <Quantity_Color.hxx>
#include <V3d_AmbientLight.hxx>
#include <V3d_DirectionalLight.hxx>

namespace cad::viewer {

namespace {

// Margem do enquadramento (fração da janela) em Ajustar à janela.
constexpr double kFitMargin = 0.05;

// O QOpenGLWidget renderiza em um FBO próprio, sem sRGB; o OCCT precisa saber disso
// para não aplicar a conversão de cor duas vezes.
class QtFrameBuffer : public OpenGl_FrameBuffer
{
public:
    void BindBuffer(const occ::handle<OpenGl_Context>& theGlCtx) override
    {
        OpenGl_FrameBuffer::BindBuffer(theGlCtx);
        theGlCtx->SetFrameBufferSRGB(true, false);
    }

    void BindDrawBuffer(const occ::handle<OpenGl_Context>& theGlCtx) override
    {
        OpenGl_FrameBuffer::BindDrawBuffer(theGlCtx);
        theGlCtx->SetFrameBufferSRGB(true, false);
    }

    void BindReadBuffer(const occ::handle<OpenGl_Context>& theGlCtx) override
    {
        OpenGl_FrameBuffer::BindReadBuffer(theGlCtx);
    }
};

// Janela nativa associada ao contexto OpenGL corrente (no Windows, a janela do
// contexto criado pelo Qt, que pode diferir de winId()).
Aspect_Drawable currentNativeWindow(const QWidget* widget)
{
#ifdef _WIN32
    if (HDC dc = wglGetCurrentDC())
        if (HWND wnd = WindowFromDC(dc))
            return reinterpret_cast<Aspect_Drawable>(wnd);
#endif
    return reinterpret_cast<Aspect_Drawable>(widget->winId());
}

V3d_TypeOfOrientation toOrientation(StandardView view)
{
    switch (view)
    {
    case StandardView::Front: return V3d_TypeOfOrientation_Zup_Front;
    case StandardView::Top:   return V3d_TypeOfOrientation_Zup_Top;
    case StandardView::Right: return V3d_TypeOfOrientation_Zup_Right;
    case StandardView::Isometric: break;
    }
    return V3d_TypeOfOrientation_Zup_AxoRight;
}

} // namespace

OcctViewerWidget::OcctViewerWidget(QWidget* parent)
    : QOpenGLWidget(parent)
{
    setObjectName("visualizador3D");
    setMouseTracking(true);
    setBackgroundRole(QPalette::NoRole);
    setFocusPolicy(Qt::StrongFocus);
    setUpdatesEnabled(true);
    setUpdateBehavior(QOpenGLWidget::NoPartialUpdate);
    setMinimumSize(400, 300);

    QSurfaceFormat format;
    format.setDepthBufferSize(24);
    format.setStencilBufferSize(8);
    setFormat(format);

    setupViewer();
}

OcctViewerWidget::~OcctViewerWidget()
{
    releaseGraphics();
}

void OcctViewerWidget::setupViewer()
{
    occ::handle<Aspect_DisplayConnection> display = new Aspect_DisplayConnection();
    // O contexto OpenGL é criado pelo Qt; o driver apenas o reutiliza.
    occ::handle<OpenGl_GraphicDriver> driver = new OpenGl_GraphicDriver(display, false);
    driver->ChangeOptions().buffersNoSwap = true;
    driver->ChangeOptions().buffersOpaqueAlpha = true;
    driver->ChangeOptions().useSystemBuffer = false;

    m_viewer = new V3d_Viewer(driver);
    m_viewer->SetDefaultTypeOfView(V3d_ORTHOGRAPHIC);

    // Luz ambiente + luz direcional presa à câmera, vinda de cima/esquerda: na vista
    // isométrica as três faces visíveis recebem iluminação diferente.
    occ::handle<V3d_AmbientLight> ambient = new V3d_AmbientLight(Quantity_Color(0.60, 0.60, 0.60, Quantity_TOC_RGB));
    occ::handle<V3d_DirectionalLight> key = new V3d_DirectionalLight(gp_Dir(0.45, -0.6, -0.65), Quantity_NOC_WHITE, true);
    m_viewer->AddLight(ambient);
    m_viewer->AddLight(key);
    m_viewer->SetLightOn();

    m_context = new AIS_InteractiveContext(m_viewer);
    m_context->SetDisplayMode(AIS_Shaded, false);

    m_view = m_viewer->CreateView();
    m_view->SetImmediateUpdate(false);
    m_view->Camera()->SetProjectionType(Graphic3d_Camera::Projection_Orthographic);
    m_view->ChangeRenderingParams().NbMsaaSamples = 4;

    // Fundo em degradê azul-acinzentado (claro em cima), usual em CAD mecânico.
    m_view->SetBgGradientColors(Quantity_Color(0.82, 0.86, 0.91, Quantity_TOC_sRGB),
                                Quantity_Color(0.36, 0.42, 0.52, Quantity_TOC_sRGB),
                                Aspect_GradientFillMethod_Vertical);
    m_view->TriedronDisplay(Aspect_TOTP_LEFT_LOWER, Quantity_NOC_BLACK, 0.08, V3d_ZBUFFER);
    m_view->SetProj(V3d_TypeOfOrientation_Zup_AxoRight);
}

void OcctViewerWidget::releaseGraphics()
{
    if (m_viewer.IsNull())
        return;

    // Os recursos OpenGL do OCCT pertencem ao contexto do Qt: liberar com ele ativo.
    const bool hasGl = context() != nullptr && context()->isValid();
    if (hasGl)
        makeCurrent();

    m_context->RemoveAll(false);
    m_context.Nullify();
    m_shape.Nullify();
    m_view->Remove();
    m_view.Nullify();
    m_viewer.Nullify();
    m_initialized = false;

    if (hasGl)
        doneCurrent();
}

void OcctViewerWidget::setShape(const TopoDS_Shape& shape)
{
    if (!m_shape.IsNull())
        m_context->Remove(m_shape, false);

    m_shapeData = shape;
    m_shape.Nullify();
    if (shape.IsNull())
    {
        updateView();
        return;
    }

    m_shape = new AIS_Shape(shape);
    m_shape->SetMaterial(Graphic3d_MaterialAspect(Graphic3d_NameOfMaterial_Plastified));
    m_shape->SetColor(Quantity_Color(0.70, 0.73, 0.77, Quantity_TOC_sRGB));

    // Arestas depois da cor: SetColor() também altera os aspectos de linha.
    const occ::handle<Prs3d_Drawer>& drawer = m_shape->Attributes();
    drawer->SetFaceBoundaryDraw(true);
    drawer->SetFaceBoundaryAspect(new Prs3d_LineAspect(Quantity_Color(0.05, 0.05, 0.05, Quantity_TOC_sRGB),
                                                       Aspect_TOL_SOLID, 1.5));
    drawer->SetDeviationAngle(5.0 * (3.14159265358979323846 / 180.0)); // furo com aparência suave

    m_context->Display(m_shape, AIS_Shaded, 0, false);

    m_fitPending = true;
    updateView();
}

QImage OcctViewerWidget::grabViewerImage()
{
    return grabFramebuffer();
}

void OcctViewerWidget::fitAll()
{
    AbortViewAnimation();
    if (m_view.IsNull() || !m_initialized)
    {
        m_fitPending = true;
        return;
    }
    m_view->FitAll(kFitMargin, false);
    m_view->Invalidate();
    updateView();
}

void OcctViewerWidget::setStandardView(StandardView view)
{
    if (m_view.IsNull())
        return;
    AbortViewAnimation();
    m_view->SetProj(toOrientation(view), false);
    fitAll();
}

void OcctViewerWidget::initializeGL()
{
    if (m_view.IsNull())
        return;

    const qreal dpr = devicePixelRatioF();
    const int width = qMax(1, qRound(this->width() * dpr));
    const int height = qMax(1, qRound(this->height() * dpr));
    const Aspect_Drawable nativeWindow = currentNativeWindow(this);

    occ::handle<OpenGl_Context> glContext = new OpenGl_Context();
    if (!glContext->Init(false))
    {
        const QString message = tr("Falha ao inicializar o contexto OpenGL do Open CASCADE");
        Message::SendFail() << message.toStdString().c_str();
        emit initializationFailed(message);
        return;
    }

    occ::handle<Aspect_NeutralWindow> window = occ::down_cast<Aspect_NeutralWindow>(m_view->Window());
    if (window.IsNull())
    {
        window = new Aspect_NeutralWindow();
        window->SetVirtual(true);
    }
    window->SetNativeHandle(nativeWindow);
    window->SetSize(width, height);
    m_view->SetWindow(window, glContext->RenderingContext());

    // Textos e linhas do OCCT acompanham a escala da tela (ex.: 125%).
    m_view->ChangeRenderingParams().Resolution = static_cast<unsigned int>(96.0 * dpr + 0.5);

    if (QOpenGLContext* ctx = QOpenGLContext::currentContext())
    {
        QOpenGLFunctions* gl = ctx->functions();
        auto str = [gl](GLenum name) {
            const auto* s = reinterpret_cast<const char*>(gl->glGetString(name));
            return s ? QString::fromLatin1(s) : QString();
        };
        m_glInfo = QStringLiteral("%1 | %2 | OpenGL %3")
                       .arg(str(GL_VENDOR), str(GL_RENDERER), str(GL_VERSION));
    }

    m_initialized = true;
    m_fitPending = true;
    emit viewerInitialized();
}

void OcctViewerWidget::paintGL()
{
    if (m_view.IsNull() || !m_initialized || m_view->Window().IsNull())
        return;

    // O Qt recria o contexto se o widget mudar de janela de nível superior.
    if (m_view->Window()->NativeHandle() != currentNativeWindow(this))
    {
        initializeGL();
        if (!m_initialized)
            return;
    }

    occ::handle<OpenGl_GraphicDriver> driver = occ::down_cast<OpenGl_GraphicDriver>(m_viewer->Driver());
    const occ::handle<OpenGl_Context>& glContext = driver->GetSharedContext();

    // Envolve o FBO criado pelo QOpenGLWidget como framebuffer padrão do OCCT.
    occ::handle<OpenGl_FrameBuffer> fbo = glContext->DefaultFrameBuffer();
    if (fbo.IsNull())
    {
        fbo = new QtFrameBuffer();
        glContext->SetDefaultFrameBuffer(fbo);
    }
    if (!fbo->InitWrapper(glContext))
    {
        glContext->SetDefaultFrameBuffer(occ::handle<OpenGl_FrameBuffer>());
        Message::SendFail() << "OcctViewerWidget: falha ao envolver o FBO do Qt";
        return;
    }

    // Tamanho em pixels físicos do FBO do Qt, calculado como o Qt o aloca (tamanho lógico x
    // devicePixelRatio, arredondado). Com escala fracionária o viewport deixado pelo Qt pode
    // ser 1 pixel menor (truncado); usá-lo deixaria uma linha/coluna sem renderizar.
    occ::handle<Aspect_NeutralWindow> window = occ::down_cast<Aspect_NeutralWindow>(m_view->Window());
    const QSize fboSize = size() * devicePixelRatioF();
    NCollection_Vec2<int> newSize(fboSize.width(), fboSize.height());
    if (fboSize.isEmpty())
        newSize = fbo->GetVPSize();
    int oldWidth = 0, oldHeight = 0;
    window->Size(oldWidth, oldHeight);
    if (newSize.x() != oldWidth || newSize.y() != oldHeight)
    {
        window->SetSize(newSize.x(), newSize.y());
        m_view->MustBeResized();
        m_view->Invalidate();
    }

    const auto resolution = static_cast<unsigned int>(96.0 * devicePixelRatioF() + 0.5);
    if (m_view->RenderingParams().Resolution != resolution)
    {
        m_view->ChangeRenderingParams().Resolution = resolution;
        m_view->Invalidate();
    }

    if (m_fitPending && !m_shape.IsNull())
    {
        m_fitPending = false;
        m_view->FitAll(kFitMargin, false);
        m_view->Invalidate();
    }

    m_view->InvalidateImmediate();
    FlushViewEvents(m_context, m_view, true);
    ++m_frameCount;
}

void OcctViewerWidget::handleViewRedraw(const occ::handle<AIS_InteractiveContext>& theCtx,
                                        const occ::handle<V3d_View>& theView)
{
    AIS_ViewController::handleViewRedraw(theCtx, theView);
    // Animações (ex.: ViewCube) pedem quadros adicionais.
    if (toAskNextFrame())
        updateView();
}

void OcctViewerWidget::updateView()
{
    update();
}

} // namespace cad::viewer
