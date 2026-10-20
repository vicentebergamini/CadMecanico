#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wingdi.h>
#include <winuser.h>
#endif

#include "viewer/OcctViewerWidget.h"

#include <QKeyEvent>
#include <QMouseEvent>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QSurfaceFormat>
#include <QWheelEvent>

#include <Aspect_DisplayConnection.hxx>
#include <Aspect_NeutralWindow.hxx>
#include <Graphic3d_MaterialAspect.hxx>
#include <Graphic3d_TransformPers.hxx>
#include <Message.hxx>
#include <OpenGl_Context.hxx>
#include <OpenGl_FrameBuffer.hxx>
#include <OpenGl_GraphicDriver.hxx>
#include <Prs3d_Drawer.hxx>
#include <Prs3d_LineAspect.hxx>
#include <AIS_TextLabel.hxx>
#include <AIS_Trihedron.hxx>
#include <Geom_Axis2Placement.hxx>
#include <Prs3d_DatumParts.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <Quantity_Color.hxx>
#include <StdSelect_BRepOwner.hxx>

#include "modeling/ShapeInspector.h"
#include "sketch/SketchShapes.h"

#include <cmath>
#include <V3d_AmbientLight.hxx>
#include <V3d_DirectionalLight.hxx>

namespace cad::viewer {

namespace {

// Margem do enquadramento (fração da janela) em Ajustar à janela.
constexpr double kFitMargin = 0.05;

// ViewCube no canto superior direito; medidas em pixels lógicos (escaladas pelo devicePixelRatio).
constexpr double kViewCubeSize = 60.0;
constexpr int kViewCubeOffset = 90;
// Altura dos rótulos em pontos: o OCCT já a converte com a resolução (96 dpi x devicePixelRatio).
constexpr double kViewCubeFontHeight = 8.0;

// Tolerância de seleção por ponto, em pixels lógicos.
constexpr double kPickTolerance = 4.0;

const Quantity_Color kHoverColor(0.10, 0.65, 1.00, Quantity_TOC_sRGB);
const Quantity_Color kSelectionColor(1.00, 0.55, 0.00, Quantity_TOC_sRGB);

// Esboços: entidades do documento, entidades em edição, entidade selecionada e pré-visualização.
const Quantity_Color kSketchColor(0.10, 0.30, 0.70, Quantity_TOC_sRGB);
const Quantity_Color kSketchEditColor(0.00, 0.20, 0.85, Quantity_TOC_sRGB);
const Quantity_Color kSketchPreviewColor(0.25, 0.25, 0.25, Quantity_TOC_sRGB);
const Quantity_Color kAxisHColor(0.85, 0.10, 0.10, Quantity_TOC_sRGB);
const Quantity_Color kAxisVColor(0.05, 0.60, 0.10, Quantity_TOC_sRGB);

// Eixos de referência na origem global, com tamanho constante na tela (pixels lógicos).
constexpr double kOriginAxesSize = 70.0;

// Região enquadrada quando a cena não tem sólido nem esboço (mm, a partir da origem).
constexpr double kEmptySceneExtent = 100.0;

// Clique (sem arrasto) no modo de esboço: deslocamento máximo em pixels lógicos.
constexpr double kClickTolerance = 4.0;
// Coordenadas criadas com o mouse são arredondadas a 0,1 mm (1/kSnapDivisions).
constexpr double kSnapDivisions = 10.0;

// round(v * 10) / 10 dá o double mais próximo de x,y (ex.: 74.1), sem resíduos de ponto flutuante.
double snapValue(double v)
{
    const double r = std::round(v * kSnapDivisions) / kSnapDivisions;
    return r == 0.0 ? 0.0 : r; // evita -0
}

// Larguras, alturas e raios derivados de pontos arredondados também são arredondados.
cad::sketch::SketchGeometry snapGeometry(cad::sketch::SketchGeometry g)
{
    if (auto* l = std::get_if<cad::sketch::SketchLine>(&g))
    {
        l->x1 = snapValue(l->x1);
        l->y1 = snapValue(l->y1);
        l->x2 = snapValue(l->x2);
        l->y2 = snapValue(l->y2);
    }
    else if (auto* r = std::get_if<cad::sketch::SketchRectangle>(&g))
    {
        r->x = snapValue(r->x);
        r->y = snapValue(r->y);
        r->width = snapValue(r->width);
        r->height = snapValue(r->height);
    }
    else
    {
        auto& c = std::get<cad::sketch::SketchCircle>(g);
        c.cx = snapValue(c.cx);
        c.cy = snapValue(c.cy);
        c.radius = snapValue(c.radius);
    }
    return g;
}

V3d_TypeOfOrientation sketchOrientation(cad::sketch::PlaneType plane)
{
    switch (plane)
    {
    case cad::sketch::PlaneType::XY: return V3d_TypeOfOrientation_Zup_Top;   // olha -Z, vertical = +Y
    case cad::sketch::PlaneType::XZ: return V3d_TypeOfOrientation_Zup_Front; // olha +Y, vertical = +Z
    case cad::sketch::PlaneType::YZ: return V3d_TypeOfOrientation_Zup_Right; // olha -X, vertical = +Z
    }
    return V3d_TypeOfOrientation_Zup_Top;
}

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

Aspect_VKeyMouse toVKeyMouse(Qt::MouseButtons buttons)
{
    Aspect_VKeyMouse keys = Aspect_VKeyMouse_NONE;
    if (buttons & Qt::LeftButton)
        keys |= Aspect_VKeyMouse_LeftButton;
    if (buttons & Qt::MiddleButton)
        keys |= Aspect_VKeyMouse_MiddleButton;
    if (buttons & Qt::RightButton)
        keys |= Aspect_VKeyMouse_RightButton;
    return keys;
}

Aspect_VKeyFlags toVKeyFlags(Qt::KeyboardModifiers modifiers)
{
    Aspect_VKeyFlags flags = Aspect_VKeyFlags_NONE;
    if (modifiers & Qt::ShiftModifier)
        flags |= Aspect_VKeyFlags_SHIFT;
    if (modifiers & Qt::ControlModifier)
        flags |= Aspect_VKeyFlags_CTRL;
    if (modifiers & Qt::AltModifier)
        flags |= Aspect_VKeyFlags_ALT;
    return flags;
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
    setupNavigation();
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

    // Realce: pré-seleção (mouse sobre a entidade) em azul, seleção em laranja.
    for (Prs3d_TypeOfHighlight type : {Prs3d_TypeOfHighlight_Dynamic, Prs3d_TypeOfHighlight_LocalDynamic})
        m_context->HighlightStyle(type)->SetColor(kHoverColor);
    for (Prs3d_TypeOfHighlight type : {Prs3d_TypeOfHighlight_Selected, Prs3d_TypeOfHighlight_LocalSelected})
        m_context->HighlightStyle(type)->SetColor(kSelectionColor);
    // Faces realçadas preenchidas (o padrão do OCCT realça só o contorno).
    for (Prs3d_TypeOfHighlight type : {Prs3d_TypeOfHighlight_LocalDynamic, Prs3d_TypeOfHighlight_LocalSelected})
        m_context->HighlightStyle(type)->SetDisplayMode(AIS_Shaded);

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

    // ViewCube: clique em face, aresta ou canto anima a câmera até a orientação padrão
    // correspondente (Z para cima, coerente com as vistas Frente/Superior/Direita).
    m_viewCube = new AIS_ViewCube();
    m_viewCube->SetViewAnimation(ViewAnimation());
    m_viewCube->SetFixedAnimationLoop(false);
    m_viewCube->SetAutoStartAnimation(true);
    m_viewCube->SetDuration(0.35);
    m_viewCube->SetFitSelected(false);
    m_viewCube->SetResetCamera(true);
    m_viewCube->SetDrawAxes(false);
    m_viewCube->SetBoxColor(Quantity_Color(0.86, 0.88, 0.91, Quantity_TOC_sRGB));
    m_viewCube->SetTextColor(Quantity_Color(0.15, 0.15, 0.15, Quantity_TOC_sRGB));
    m_viewCube->SetBoxSideLabel(V3d_Yneg, "FRENTE");
    m_viewCube->SetBoxSideLabel(V3d_Ypos, "TRÁS");
    m_viewCube->SetBoxSideLabel(V3d_Zpos, "SUPERIOR");
    m_viewCube->SetBoxSideLabel(V3d_Zneg, "INFERIOR");
    m_viewCube->SetBoxSideLabel(V3d_Xpos, "DIREITA");
    m_viewCube->SetBoxSideLabel(V3d_Xneg, "ESQUERDA");

    // Eixos de referência X (vermelho), Y (verde) e Z (azul) na origem global, de tamanho
    // constante na tela e fora do enquadramento (Ajustar à janela ignora objetos persistentes).
    m_originAxes = new AIS_Trihedron(new Geom_Axis2Placement(gp::XOY()));
    m_originAxes->SetDatumDisplayMode(Prs3d_DM_WireFrame);
    m_originAxes->SetDatumPartColor(Prs3d_DatumParts_XAxis, Quantity_NOC_RED);
    m_originAxes->SetDatumPartColor(Prs3d_DatumParts_YAxis, Quantity_Color(0.0, 0.6, 0.0, Quantity_TOC_sRGB));
    m_originAxes->SetDatumPartColor(Prs3d_DatumParts_ZAxis, Quantity_NOC_BLUE1);
    m_originAxes->SetTextColor(Quantity_NOC_BLACK);
    m_originAxes->SetTransformPersistence(new Graphic3d_TransformPers(Graphic3d_TMF_ZoomPers, gp::Origin()));

    applyDevicePixelRatio(1.0);
    m_context->Display(m_viewCube, 0, 0, false);
    m_context->Display(m_originAxes, 0, -1, false);
}

void OcctViewerWidget::setupSketchNavigation(bool sketchMode)
{
    // Navegação CAD: botão central gira, Ctrl + botão central desloca. No modo de esboço a
    // rotação é desativada (câmera perpendicular ao plano): o botão central só desloca.
    myMouseGestureMap.Clear();
    myMouseGestureMap.Bind(Aspect_VKeyMouse_MiddleButton,
                           sketchMode ? AIS_MouseGesture_Pan : AIS_MouseGesture_RotateOrbit);
    myMouseGestureMap.Bind(Aspect_VKeyMouse_MiddleButton | Aspect_VKeyFlags_CTRL, AIS_MouseGesture_Pan);
}

void OcctViewerWidget::setupNavigation()
{
    // O botão esquerdo fica reservado à seleção (sem rotação nem arrasto de objetos).
    setupSketchNavigation(false);
    myMouseGestureMapDrag.Clear();
    SetAllowDragging(false);

    // Seleção só por clique (sem movimento além do limiar): navegar não seleciona.
    myMouseSelectionSchemes.Clear();
    myMouseSelectionSchemes.Bind(Aspect_VKeyMouse_LeftButton, AIS_SelectionScheme_Replace);
    myMouseSelectionSchemes.Bind(Aspect_VKeyMouse_LeftButton | Aspect_VKeyFlags_SHIFT, AIS_SelectionScheme_XOR);
}

void OcctViewerWidget::applyDevicePixelRatio(qreal dpr)
{
    if (qFuzzyCompare(dpr, m_appliedDpr))
        return;
    m_appliedDpr = dpr;

    // Textos e linhas do OCCT acompanham a escala da tela (ex.: 125%).
    m_view->ChangeRenderingParams().Resolution = static_cast<unsigned int>(96.0 * dpr + 0.5);

    // Tamanho e posição do ViewCube em pixels físicos.
    const int offset = qRound(kViewCubeOffset * dpr);
    m_viewCube->SetSize(kViewCubeSize * dpr);
    m_viewCube->SetFontHeight(kViewCubeFontHeight);
    m_viewCube->SetTransformPersistence(new Graphic3d_TransformPers(
        Graphic3d_TMF_TriedronPers, Aspect_TOTP_RIGHT_UPPER, NCollection_Vec2<int>(offset, offset)));
    if (m_context->IsDisplayed(m_viewCube))
    {
        m_context->Redisplay(m_viewCube, false, true);
        m_context->RecomputeSelectionOnly(m_viewCube);
    }
    if (!m_originAxes.IsNull())
    {
        m_originAxes->SetSize(kOriginAxesSize * dpr);
        if (m_context->IsDisplayed(m_originAxes))
            m_context->Redisplay(m_originAxes, false);
    }

    m_context->SetPixelTolerance(qRound(kPickTolerance * dpr));
    m_view->Invalidate();
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
    m_viewCube.Nullify();
    m_originAxes.Nullify();
    m_documentSketchShapes.clear();
    m_sketchDecorations.clear();
    m_editShapes.clear();
    m_preview.Nullify();
    m_sketchPlane.reset();
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
    {
        m_context->ClearSelected(false);
        m_context->Remove(m_shape, false);
    }

    m_shapeData = shape;
    m_shape.Nullify();
    if (shape.IsNull())
    {
        redrawScene();
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

    // Seleção de faces, arestas e vértices (a peça inteira não é selecionável). O objeto
    // precisa ser exibido já com um modo de seleção: com -1 ele não é carregado no seletor.
    m_context->Display(m_shape, AIS_Shaded, AIS_Shape::SelectionMode(TopAbs_FACE), false);
    for (TopAbs_ShapeEnum type : {TopAbs_EDGE, TopAbs_VERTEX})
        m_context->Activate(m_shape, AIS_Shape::SelectionMode(type));
    if (isSketchMode())
        m_context->Erase(m_shape, false); // oculto durante a edição do esboço

    m_fitPending = true;
    redrawScene();
}

QList<TopoDS_Shape> OcctViewerWidget::selectedShapes() const
{
    QList<TopoDS_Shape> shapes;
    if (m_context.IsNull() || m_shape.IsNull())
        return shapes;
    for (m_context->InitSelected(); m_context->MoreSelected(); m_context->NextSelected())
    {
        occ::handle<StdSelect_BRepOwner> owner = occ::down_cast<StdSelect_BRepOwner>(m_context->SelectedOwner());
        if (!owner.IsNull() && owner->Selectable().get() == m_shape.get())
            shapes << owner->Shape();
    }
    return shapes;
}

void OcctViewerWidget::clearSelection()
{
    if (m_context.IsNull())
        return;
    m_context->ClearSelected(false);
    redrawScene();
    emit selectionChanged();
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
    fitScene();
    m_view->Invalidate();
    updateView();
}

bool OcctViewerWidget::hasFittableContent() const
{
    if (isSketchMode())
        return !m_editShapes.isEmpty() || !m_sketchDecorations.empty();
    return !m_shape.IsNull() || !m_documentSketchShapes.empty();
}

void OcctViewerWidget::fitScene()
{
    if (!isSketchMode() && m_shape.IsNull() && !m_documentSketchShapes.empty())
    {
        // Documento só com esboços: caixa calculada a partir das arestas dos esboços.
        Bnd_Box box;
        for (const auto& prs : m_documentSketchShapes)
            box.Add(cad::modeling::ShapeInspector::boundingBox(prs->Shape()));
        box.Add(gp::Origin()); // mantém a origem (eixos de referência) no enquadramento
        if (!box.IsVoid())
        {
            box.Enlarge(1.0);
            m_view->FitAll(box, kFitMargin, false);
            return;
        }
    }
    if (hasFittableContent())
    {
        m_view->FitAll(kFitMargin, false);
        return;
    }
    // Cena vazia: enquadra uma região padrão centrada na origem (eixos de referência no centro).
    Bnd_Box box;
    const double e = 0.6 * kEmptySceneExtent;
    box.Update(-e, -e, -e, e, e, e);
    m_view->FitAll(box, kFitMargin, false);
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
    applyDevicePixelRatio(dpr);

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

    // A janela pode mudar de monitor (outra escala).
    applyDevicePixelRatio(devicePixelRatioF());

    if (m_fitPending)
    {
        m_fitPending = false;
        fitScene();
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

void OcctViewerWidget::OnSelectionChanged(const occ::handle<AIS_InteractiveContext>& theCtx,
                                          const occ::handle<V3d_View>& theView)
{
    AIS_ViewController::OnSelectionChanged(theCtx, theView);
    // Chamado durante o paintGL: notificar fora da renderização.
    QMetaObject::invokeMethod(this, [this] { emit selectionChanged(); }, Qt::QueuedConnection);
}

NCollection_Vec2<int> OcctViewerWidget::toViewPixels(const QPointF& pos) const
{
    const qreal dpr = devicePixelRatioF();
    return NCollection_Vec2<int>(qRound(pos.x() * dpr), qRound(pos.y() * dpr));
}

void OcctViewerWidget::mousePressEvent(QMouseEvent* event)
{
    QOpenGLWidget::mousePressEvent(event);
    if (!m_initialized)
        return;
    if (isSketchMode() && event->button() == Qt::LeftButton)
    {
        // Botão esquerdo no esboço: tratado pela ferramenta ativa, não pela seleção 3D.
        m_leftPressed = true;
        m_leftPressPos = event->position();
        event->accept();
        return;
    }
    if (UpdateMouseButtons(toViewPixels(event->position()), toVKeyMouse(event->buttons()),
                           toVKeyFlags(event->modifiers()), false))
        updateView();
    event->accept();
}

void OcctViewerWidget::mouseReleaseEvent(QMouseEvent* event)
{
    QOpenGLWidget::mouseReleaseEvent(event);
    if (!m_initialized)
        return;
    if (isSketchMode() && event->button() == Qt::LeftButton)
    {
        const bool wasPressed = m_leftPressed;
        m_leftPressed = false;
        const QPointF delta = event->position() - m_leftPressPos;
        if (wasPressed && std::hypot(delta.x(), delta.y()) <= kClickTolerance)
            handleSketchClick(event->position());
        event->accept();
        return;
    }
    if (UpdateMouseButtons(toViewPixels(event->position()), toVKeyMouse(event->buttons()),
                           toVKeyFlags(event->modifiers()), false))
        updateView();
    event->accept();
}

void OcctViewerWidget::mouseMoveEvent(QMouseEvent* event)
{
    QOpenGLWidget::mouseMoveEvent(event);
    if (!m_initialized)
        return;
    Qt::MouseButtons buttons = event->buttons();
    if (isSketchMode())
    {
        buttons &= ~Qt::LeftButton; // o esquerdo não participa da navegação no esboço
        if (m_pendingPoint)
            if (const auto p = sketchPointAt(event->position()))
                updatePreview(*p);
    }
    if (UpdateMousePosition(toViewPixels(event->position()), toVKeyMouse(buttons),
                            toVKeyFlags(event->modifiers()), false))
        updateView();
}

void OcctViewerWidget::wheelEvent(QWheelEvent* event)
{
    if (!m_initialized)
    {
        QOpenGLWidget::wheelEvent(event);
        return;
    }
    // Um "passo" da roda (120) corresponde a 15 graus; o zoom é centrado no cursor.
    const double delta = event->angleDelta().y() / 8.0;
    if (delta != 0.0 && UpdateZoom(Aspect_ScrollDelta(toViewPixels(event->position()), delta)))
        updateView();
    event->accept();
}

void OcctViewerWidget::keyPressEvent(QKeyEvent* event)
{
    if (event->key() == Qt::Key_F && (event->modifiers() & ~Qt::KeypadModifier) == Qt::NoModifier)
    {
        fitAll();
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_Escape && isSketchMode() && m_pendingPoint)
    {
        cancelSketchInput();
        event->accept();
        return;
    }
    QOpenGLWidget::keyPressEvent(event);
}

void OcctViewerWidget::focusOutEvent(QFocusEvent* event)
{
    // Evita botões "presos" se o foco sair durante um arrasto.
    ResetViewInput();
    QOpenGLWidget::focusOutEvent(event);
}

void OcctViewerWidget::updateView()
{
    update();
}

void OcctViewerWidget::redrawScene()
{
    // Objetos exibidos/ocultos com Display/Erase(..., false) só aparecem num redesenho completo
    // da camada principal; sem Invalidate() apenas a camada imediata seria redesenhada.
    if (!m_view.IsNull())
        m_view->Invalidate();
    update();
}

// --- Esboços -----------------------------------------------------------------

void OcctViewerWidget::setDocumentSketches(const std::vector<cad::sketch::Sketch>& sketches)
{
    if (m_context.IsNull())
        return;
    for (const auto& s : m_documentSketchShapes)
        m_context->Remove(s, false);
    m_documentSketchShapes.clear();
    for (const cad::sketch::Sketch& sketch : sketches)
    {
        if (sketch.entities().empty())
            continue;
        occ::handle<AIS_Shape> prs = new AIS_Shape(cad::sketch::buildSketchShape(sketch));
        prs->SetColor(kSketchColor);
        prs->SetWidth(1.5);
        m_context->Display(prs, AIS_WireFrame, -1, false); // apenas visualização
        m_context->SetZLayer(prs, Graphic3d_ZLayerId_Topmost); // visível através do sólido
        if (isSketchMode())
            m_context->Erase(prs, false); // outros esboços ocultos durante a edição
        m_documentSketchShapes.push_back(prs);
    }
    redrawScene();
}

void OcctViewerWidget::enterSketchMode(const cad::sketch::Sketch& sketch)
{
    if (m_context.IsNull())
        return;
    if (isSketchMode())
        exitSketchMode();
    m_sketchPlane = sketch.plane();
    m_sketchTool = SketchTool::Select;
    m_pendingPoint.reset();

    // Somente o esboço ativo: sólidos, outros esboços, eixos globais e ViewCube ficam ocultos
    // (sem rotação no esboço) e voltam em exitSketchMode().
    m_context->ClearSelected(false);
    if (!m_shape.IsNull())
        m_context->Erase(m_shape, false);
    for (const auto& s : m_documentSketchShapes)
        m_context->Erase(s, false);
    m_context->Erase(m_viewCube, false);
    m_context->Erase(m_originAxes, false);
    setupSketchNavigation(true);

    // Eixos locais: horizontal (vermelho) e vertical (verde) a partir da origem do plano.
    double axisLength = kEmptySceneExtent;
    if (!m_shapeData.IsNull())
    {
        const Bnd_Box box = cad::modeling::ShapeInspector::boundingBox(m_shapeData);
        if (!box.IsVoid())
            axisLength = std::max(kEmptySceneExtent, 0.6 * std::sqrt(box.SquareExtent()));
    }
    const cad::sketch::SketchPlane& plane = *m_sketchPlane;
    auto addAxis = [&](const gp_Dir& dir, const Quantity_Color& color, const char* label) {
        const gp_Pnt end = plane.origin().Translated(gp_Vec(dir) * axisLength);
        const TopoDS_Shape edge = BRepBuilderAPI_MakeEdge(plane.origin(), end).Shape();
        occ::handle<AIS_Shape> axis = new AIS_Shape(edge);
        axis->SetColor(color);
        axis->SetWidth(2.0);
        m_context->Display(axis, AIS_WireFrame, -1, false);
        m_context->SetZLayer(axis, Graphic3d_ZLayerId_Topmost);
        m_sketchDecorations.push_back(axis);

        occ::handle<AIS_TextLabel> text = new AIS_TextLabel();
        text->SetText(TCollection_ExtendedString(label));
        text->SetPosition(end.Translated(gp_Vec(dir) * (0.04 * axisLength)));
        text->SetColor(color);
        text->SetHeight(12.0);
        m_context->Display(text, 0, -1, false);
        m_context->SetZLayer(text, Graphic3d_ZLayerId_Topmost);
        m_sketchDecorations.push_back(text);
    };
    addAxis(plane.horizontalAxis(), kAxisHColor, "H");
    addAxis(plane.verticalAxis(), kAxisVColor, "V");

    updateEditingSketch(sketch, QString());

    // Câmera perpendicular ao plano, ortográfica, sem espelhamento (H para a direita, V para cima).
    AbortViewAnimation();
    m_view->SetProj(sketchOrientation(plane.type()), false);
    m_view->Camera()->SetProjectionType(Graphic3d_Camera::Projection_Orthographic);
    fitAll();
}

void OcctViewerWidget::updateEditingSketch(const cad::sketch::Sketch& sketch, const QString& selectedEntityId)
{
    if (!isSketchMode())
        return;
    clearEditShapes();
    for (const cad::sketch::SketchEntity& e : sketch.entities())
    {
        const TopoDS_Shape shape = cad::sketch::buildEntityShape(*m_sketchPlane, e);
        if (shape.IsNull())
            continue;
        const QString id = QString::fromStdString(e.id);
        occ::handle<AIS_Shape> prs = new AIS_Shape(shape);
        const bool selected = id == selectedEntityId;
        prs->SetColor(selected ? kSelectionColor : kSketchEditColor);
        prs->SetWidth(selected ? 3.0 : 2.0);
        m_context->Display(prs, AIS_WireFrame, 0, false); // selecionável no modo de esboço
        m_context->SetZLayer(prs, Graphic3d_ZLayerId_Topmost);
        m_editShapes.insert(id, prs);
    }
    redrawScene();
}

void OcctViewerWidget::clearEditShapes()
{
    for (const auto& prs : std::as_const(m_editShapes))
        m_context->Remove(prs, false);
    m_editShapes.clear();
}

void OcctViewerWidget::exitSketchMode()
{
    if (!isSketchMode() || m_context.IsNull())
        return;
    removePreview();
    clearEditShapes();
    for (const auto& d : m_sketchDecorations)
        m_context->Remove(d, false);
    m_sketchDecorations.clear();
    m_sketchPlane.reset();
    m_pendingPoint.reset();
    m_leftPressed = false;
    m_sketchTool = SketchTool::Select;

    // Restaura a visualização do documento.
    if (!m_shape.IsNull())
    {
        m_context->Display(m_shape, AIS_Shaded, AIS_Shape::SelectionMode(TopAbs_FACE), false);
        for (TopAbs_ShapeEnum type : {TopAbs_EDGE, TopAbs_VERTEX})
            m_context->Activate(m_shape, AIS_Shape::SelectionMode(type));
    }
    for (const auto& s : m_documentSketchShapes)
    {
        m_context->Display(s, AIS_WireFrame, -1, false);
        m_context->SetZLayer(s, Graphic3d_ZLayerId_Topmost);
    }
    m_context->Display(m_viewCube, 0, 0, false);
    m_context->Display(m_originAxes, 0, -1, false);
    setupSketchNavigation(false);
    ResetPreviousMoveTo();
    redrawScene();
}

void OcctViewerWidget::setSketchTool(SketchTool tool)
{
    m_sketchTool = tool;
    cancelSketchInput();
}

void OcctViewerWidget::cancelSketchInput()
{
    m_pendingPoint.reset();
    removePreview();
    redrawScene();
}

occ::handle<AIS_Shape> OcctViewerWidget::sketchEntityPresentation(const QString& entityId) const
{
    return m_editShapes.value(entityId);
}

std::optional<gp_Pnt2d> OcctViewerWidget::sketchPointAt(const QPointF& logicalPos) const
{
    if (!isSketchMode() || !m_initialized || m_view.IsNull())
        return std::nullopt;
    const NCollection_Vec2<int> px = toViewPixels(logicalPos);
    double x = 0, y = 0, z = 0, vx = 0, vy = 0, vz = 0;
    m_view->ConvertWithProj(px.x(), px.y(), x, y, z, vx, vy, vz);
    // Interseção do raio do pixel com o plano do esboço.
    const gp_Pnt origin(x, y, z);
    const gp_Vec ray(vx, vy, vz);
    const double denom = ray.Dot(gp_Vec(m_sketchPlane->normal()));
    if (std::fabs(denom) < 1e-9)
        return std::nullopt;
    const double t = -m_sketchPlane->distance(origin) / denom;
    const gp_Pnt2d local = m_sketchPlane->toLocal(origin.Translated(ray * t));
    return gp_Pnt2d(snapValue(local.X()), snapValue(local.Y()));
}

void OcctViewerWidget::removePreview()
{
    if (!m_preview.IsNull() && !m_context.IsNull())
    {
        m_context->Remove(m_preview, false);
        m_view->Invalidate();
    }
    m_preview.Nullify();
}

void OcctViewerWidget::updatePreview(const gp_Pnt2d& current)
{
    removePreview();
    if (!m_pendingPoint || !isSketchMode())
        return;
    const gp_Pnt2d& a = *m_pendingPoint;
    cad::sketch::SketchEntity e;
    switch (m_sketchTool)
    {
    case SketchTool::Line:
        e.geometry = cad::sketch::SketchLine{a.X(), a.Y(), current.X(), current.Y()};
        break;
    case SketchTool::Rectangle:
        e.geometry = cad::sketch::SketchRectangle::fromCorners(a.X(), a.Y(), current.X(), current.Y());
        break;
    case SketchTool::Circle:
        e.geometry = cad::sketch::SketchCircle{a.X(), a.Y(), a.Distance(current)};
        break;
    case SketchTool::Select:
        return;
    }
    const TopoDS_Shape shape = cad::sketch::buildEntityShape(*m_sketchPlane, e);
    if (shape.IsNull())
        return;
    m_preview = new AIS_Shape(shape);
    m_preview->SetColor(kSketchPreviewColor);
    m_preview->SetWidth(1.0);
    m_context->Display(m_preview, AIS_WireFrame, -1, false);
    m_context->SetZLayer(m_preview, Graphic3d_ZLayerId_Topmost);
    m_view->Invalidate();
}

void OcctViewerWidget::handleSketchClick(const QPointF& pos)
{
    if (m_sketchTool == SketchTool::Select)
    {
        const NCollection_Vec2<int> px = toViewPixels(pos);
        m_context->MoveTo(px.x(), px.y(), m_view, false);
        QString id;
        if (m_context->HasDetected())
        {
            const occ::handle<AIS_InteractiveObject> detected = m_context->DetectedInteractive();
            for (auto it = m_editShapes.cbegin(); it != m_editShapes.cend(); ++it)
                if (it.value().get() == detected.get())
                    id = it.key();
        }
        ResetPreviousMoveTo();
        emit sketchEntityClicked(id);
        updateView();
        return;
    }

    const auto p = sketchPointAt(pos);
    if (!p)
        return;
    if (!m_pendingPoint)
    {
        m_pendingPoint = *p;
        updateView();
        return;
    }
    const gp_Pnt2d a = *m_pendingPoint;
    cad::sketch::SketchGeometry geometry;
    switch (m_sketchTool)
    {
    case SketchTool::Line:
        geometry = cad::sketch::SketchLine{a.X(), a.Y(), p->X(), p->Y()};
        break;
    case SketchTool::Rectangle:
        geometry = cad::sketch::SketchRectangle::fromCorners(a.X(), a.Y(), p->X(), p->Y());
        break;
    case SketchTool::Circle:
        geometry = cad::sketch::SketchCircle{a.X(), a.Y(), a.Distance(*p)};
        break;
    case SketchTool::Select:
        return;
    }
    m_pendingPoint.reset();
    removePreview();
    emit sketchGeometryRequested(snapGeometry(geometry));
    updateView();
}

} // namespace cad::viewer
