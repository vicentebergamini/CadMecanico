// Testes de interação do visualizador (E4) com contexto OpenGL real: eventos de mouse e
// teclado do Qt entregues ao widget, efeitos verificados na câmera, na seleção e nos pixels.
#include "ViewerTestUtils.h"

#include "modeling/SubShapeInfo.h"
#include "modeling/TestPart.h"
#include "viewer/OcctViewerWidget.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QMouseEvent>
#include <QScreen>
#include <QSignalSpy>
#include <QTest>
#include <QWheelEvent>

#include <AIS_ViewCube.hxx>
#include <NCollection_List.hxx>
#include <StdSelect_BRepOwner.hxx>
#include <Graphic3d_Camera.hxx>
#include <TopoDS.hxx>
#include <gp_Dir.hxx>

#include <cmath>

using cad::viewer::OcctViewerWidget;
using cad::viewer::StandardView;
using namespace cad::test::viewer;
using namespace cad::modeling;

namespace {

constexpr double kPi = 3.14159265358979323846;

struct CameraState
{
    gp_Dir dir;
    gp_Pnt center;
    double scale = 0.0;
};

CameraState camera(const OcctViewerWidget* w)
{
    const auto& c = w->view()->Camera();
    return {c->Direction(), c->Center(), c->Scale()};
}

// Procura, na região do ViewCube, o pixel físico que detecta a orientação pedida
// (localização do alvo apenas; o clique é feito com eventos de mouse reais).
QPoint findViewCubePixel(OcctViewerWidget* w, V3d_TypeOfOrientation orientation)
{
    const auto& ctx = w->aisContext();
    int width = 0, height = 0;
    w->view()->Window()->Size(width, height);
    const int step = std::max(2, int(std::lround(2 * w->devicePixelRatioF())));
    long sx = 0, sy = 0, n = 0;
    for (int y = 0; y < height / 3; y += step)
        for (int x = width * 2 / 3; x < width; x += step)
        {
            ctx->MoveTo(x, y, w->view(), false);
            auto owner = occ::down_cast<AIS_ViewCubeOwner>(ctx->DetectedOwner());
            if (!owner.IsNull() && owner->MainOrientation() == orientation)
            {
                sx += x;
                sy += y;
                ++n;
            }
        }
    ctx->ClearDetected(false);
    w->ResetPreviousMoveTo();
    return n > 0 ? QPoint(int(sx / n), int(sy / n)) : QPoint(-1, -1);
}

bool hasOrangeNear(const QImage& img, QPoint c, int r)
{
    for (int dy = -r; dy <= r; ++dy)
        for (int dx = -r; dx <= r; ++dx)
        {
            const QRgb p = pixelAt(img, c + QPoint(dx, dy));
            if (qRed(p) > 170 && qRed(p) - qBlue(p) > 90 && qGreen(p) > 60)
                return true;
        }
    return false;
}

} // namespace

class TestInteracao : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void rotacaoBotaoCentral();
    void botaoEsquerdoNaoNavega();
    void panCtrlBotaoCentral();
    void zoomRoda();
    void zoomCentradoNoCursor();
    void teclaF();
    void vistasPadraoAposNavegacao();
    void deteccaoDaPeca();
    void selecaoFace();
    void selecaoAresta();
    void selecaoVertice();
    void selecaoLimpaNoFundo();
    void navegacaoNaoSeleciona();
    void viewCubeCantoSuperiorDireito();
    void viewCubeClique_data();
    void viewCubeClique();
    void eventosRapidosSemTravamento();

private:
    QPointF at(const gp_Pnt& p) const { return toLogical(m_w, project(m_w, p)); }
    QPointF backgroundPoint() const { return QPointF(12.0, m_w->height() / 2.0); }
    bool waitSelection(QSignalSpy& spy) { return spy.count() > 0 || spy.wait(3000); }

    OcctViewerWidget* m_w = nullptr;
    TopoDS_Shape m_part;
};

void TestInteracao::init()
{
    m_part = buildTestPart();
    m_w = new OcctViewerWidget;
    const QSize available = QGuiApplication::primaryScreen()->availableGeometry().size() * 0.85;
    m_w->resize(QSize(900, 650).boundedTo(available));
    m_w->move(30, 30);
    m_w->setShape(m_part);
    m_w->show();
    m_w->activateWindow();
    QVERIFY(QTest::qWaitForWindowExposed(m_w));
    QVERIFY(waitViewerReady(m_w));
    m_w->setFocus();
    pauseBetweenClicks();
}

void TestInteracao::cleanup()
{
    delete m_w;
    m_w = nullptr;
}

void TestInteracao::rotacaoBotaoCentral()
{
    const CameraState before = camera(m_w);
    const QPointF c(m_w->width() / 2.0, m_w->height() / 2.0);
    QVERIFY(drag(m_w, c, c + QPointF(120, 40), Qt::MiddleButton));
    const CameraState after = camera(m_w);
    const double angle = before.dir.Angle(after.dir) * 180.0 / kPi;
    qInfo("Rotacao: %.1f graus", angle);
    QVERIFY(angle > 10.0);
    QVERIFY(m_w->view()->Camera()->IsOrthographic());
    QVERIFY(m_w->selectedShapes().isEmpty());
    saveEvidence(m_w->grabViewerImage(), "E4_01_rotacao_botao_central");
}

void TestInteracao::botaoEsquerdoNaoNavega()
{
    const CameraState before = camera(m_w);
    const QPointF c(m_w->width() / 2.0, m_w->height() / 2.0);
    QVERIFY(drag(m_w, c, c + QPointF(150, 60), Qt::LeftButton));
    const CameraState after = camera(m_w);
    QVERIFY(before.dir.Angle(after.dir) < 1e-9);
    QVERIFY(before.center.Distance(after.center) < 1e-9);
    QCOMPARE(after.scale, before.scale);
    // Arrasto com o botão esquerdo não é clique: não seleciona.
    QVERIFY(m_w->selectedShapes().isEmpty());
}

void TestInteracao::panCtrlBotaoCentral()
{
    const gp_Pnt ref(25.0, 15.0, 20.0);
    const QPoint p0 = project(m_w, ref);
    const CameraState before = camera(m_w);
    const QPointF c(m_w->width() / 2.0, m_w->height() / 2.0);
    const QPointF delta(100, -50);
    QVERIFY(drag(m_w, c, c + delta, Qt::MiddleButton, Qt::ControlModifier));
    const CameraState after = camera(m_w);
    QVERIFY(before.dir.Angle(after.dir) < 1e-6); // sem rotação
    QVERIFY(before.center.Distance(after.center) > 1.0);
    QCOMPARE(after.scale, before.scale);

    // A peça acompanha o cursor (deslocamento em pixels físicos).
    const QPoint p1 = project(m_w, ref);
    const double dpr = m_w->devicePixelRatioF();
    qInfo("Pan: (%d,%d) esperado (%.0f,%.0f)", p1.x() - p0.x(), p1.y() - p0.y(), delta.x() * dpr, delta.y() * dpr);
    QVERIFY(std::abs((p1.x() - p0.x()) - delta.x() * dpr) <= 3);
    QVERIFY(std::abs((p1.y() - p0.y()) - delta.y() * dpr) <= 3);
    QVERIFY(m_w->selectedShapes().isEmpty());
    saveEvidence(m_w->grabViewerImage(), "E4_02_pan_ctrl_botao_central");
}

void TestInteracao::zoomRoda()
{
    const QPointF c(m_w->width() / 2.0, m_w->height() / 2.0);
    const double s0 = m_w->view()->Camera()->Scale();
    sendWheel(m_w, c, 120);
    QVERIFY(waitFrames(m_w));
    const double s1 = m_w->view()->Camera()->Scale();
    sendWheel(m_w, c, -240);
    QVERIFY(waitFrames(m_w));
    const double s2 = m_w->view()->Camera()->Scale();
    qInfo("Escala: %.3f -> %.3f (aproxima) -> %.3f (afasta)", s0, s1, s2);
    QVERIFY(s1 < s0 * 0.95); // aproximar = escala ortográfica menor
    QVERIFY(s2 > s1 * 1.05);
    QVERIFY(m_w->view()->Camera()->IsOrthographic());
}

void TestInteracao::zoomCentradoNoCursor()
{
    // Cursor sobre um canto da peça, longe do centro da janela.
    const gp_Pnt corner(100.0, 0.0, 20.0);
    const QPointF cursor = at(corner);
    const QPoint before = project(m_w, corner);
    const double s0 = m_w->view()->Camera()->Scale();
    for (int i = 0; i < 3; ++i)
    {
        sendWheel(m_w, cursor, 120);
        QVERIFY(waitFrames(m_w));
    }
    const QPoint after = project(m_w, corner);
    const double s1 = m_w->view()->Camera()->Scale();
    qInfo("Zoom no cursor: escala %.3f -> %.3f; ponto (%d,%d) -> (%d,%d)", s0, s1, before.x(), before.y(),
          after.x(), after.y());
    QVERIFY(s1 < s0 * 0.8);
    // O ponto sob o cursor permanece sob o cursor (zoom no centro o deslocaria).
    QVERIFY(std::abs(after.x() - before.x()) <= 3 && std::abs(after.y() - before.y()) <= 3);
    saveEvidence(m_w->grabViewerImage(), "E4_03_zoom_no_cursor");
}

void TestInteracao::teclaF()
{
    const QPointF c(m_w->width() / 2.0, m_w->height() / 2.0);
    QVERIFY(drag(m_w, c, c + QPointF(200, 120), Qt::MiddleButton, Qt::ControlModifier));
    for (int i = 0; i < 4; ++i)
        sendWheel(m_w, c, -120);
    QVERIFY(waitFrames(m_w));
    const DiffResult d0 = diff(m_w->grabViewerImage(), backgroundReference(m_w));
    const QImage beforeImg = m_w->grabViewerImage();
    QVERIFY(d0.box.width() < 0.6 * beforeImg.width());

    QTest::keyClick(m_w, Qt::Key_F);
    QVERIFY(waitFrames(m_w));
    const QImage img = m_w->grabViewerImage();
    const DiffResult d1 = diff(img, backgroundReference(m_w));
    QVERIFY(d1.box.width() > 0.8 * img.width() || d1.box.height() > 0.8 * img.height());
    saveEvidence(img, "E4_04_tecla_F");
}

void TestInteracao::vistasPadraoAposNavegacao()
{
    const QPointF c(m_w->width() / 2.0, m_w->height() / 2.0);
    QVERIFY(drag(m_w, c, c + QPointF(-90, 70), Qt::MiddleButton));
    m_w->setStandardView(StandardView::Front);
    QVERIFY(m_w->view()->Camera()->Direction().Angle(gp_Dir(0, 1, 0)) < 1e-6);
    QVERIFY(m_w->view()->Camera()->OrthogonalizedUp().Angle(gp::DZ()) < 1e-6);
    m_w->setStandardView(StandardView::Isometric);
    QVERIFY(m_w->view()->Camera()->Direction().Angle(gp_Dir(-1, 1, -1)) < 1e-6);
}

void TestInteracao::deteccaoDaPeca()
{
    // Regressão: a peça precisa estar carregada no seletor (faces, arestas e vértices ativos).
    const auto& ctx = m_w->aisContext();
    NCollection_List<int> modes; // TColStd_ListOfInteger está obsoleto no OCCT 8
    ctx->ActivatedModes(m_w->shapePresentation(), modes);
    QCOMPARE(modes.Size(), 3);
    const QPoint p = project(m_w, gp_Pnt(25.0, 15.0, 20.0));
    ctx->MoveTo(p.x(), p.y(), m_w->view(), false);
    QVERIFY(ctx->HasDetected());
    QVERIFY(!occ::down_cast<StdSelect_BRepOwner>(ctx->DetectedOwner()).IsNull());
    ctx->ClearDetected(false);
    m_w->ResetPreviousMoveTo();
}

void TestInteracao::selecaoFace()
{
    QSignalSpy spy(m_w, &OcctViewerWidget::selectionChanged);
    QVERIFY(click(m_w, at(gp_Pnt(25.0, 15.0, 20.0))));
    QVERIFY(waitSelection(spy));
    const auto sel = m_w->selectedShapes();
    QCOMPARE(sel.size(), 1);
    const SubShapeInfo info = describeSubShape(m_part, sel.first());
    QVERIFY(info.kind == SubShapeKind::Face);
    QVERIFY(info.geometry == GeometryKind::Plane);
    QVERIFY(std::abs(info.area - (6000.0 - kPi * 100.0)) < 1e-6); // face superior (com furo)

    // Realce: a face fica alaranjada (cursor afastado para não haver pré-seleção).
    sendMouse(m_w, QEvent::MouseMove, backgroundPoint(), Qt::NoButton, Qt::NoButton);
    QVERIFY(waitFrames(m_w));
    const QImage img = m_w->grabViewerImage();
    const QRgb c = meanAround(img, project(m_w, gp_Pnt(25.0, 15.0, 20.0)), 2);
    qInfo("Face selecionada: cor (%d,%d,%d)", qRed(c), qGreen(c), qBlue(c));
    QVERIFY(qRed(c) - qBlue(c) > 60);
    saveEvidence(img, "E4_05_selecao_face");
}

void TestInteracao::selecaoAresta()
{
    QSignalSpy spy(m_w, &OcctViewerWidget::selectionChanged);
    const gp_Pnt mid(50.0, 0.0, 20.0); // aresta superior frontal (100 mm)
    QVERIFY(click(m_w, at(mid)));
    QVERIFY(waitSelection(spy));
    const auto sel = m_w->selectedShapes();
    QCOMPARE(sel.size(), 1);
    const SubShapeInfo info = describeSubShape(m_part, sel.first());
    QVERIFY(info.kind == SubShapeKind::Edge);
    QVERIFY(info.geometry == GeometryKind::Line);
    QVERIFY(std::abs(info.length - 100.0) < 1e-6);

    sendMouse(m_w, QEvent::MouseMove, backgroundPoint(), Qt::NoButton, Qt::NoButton);
    QVERIFY(waitFrames(m_w));
    const QImage img = m_w->grabViewerImage();
    QVERIFY(hasOrangeNear(img, project(m_w, mid), int(3 * m_w->devicePixelRatioF())));
    saveEvidence(img, "E4_06_selecao_aresta");

    // Aresta circular do furo.
    pauseBetweenClicks();
    QVERIFY(click(m_w, at(gp_Pnt(50.0, 20.0, 20.0))));
    QTRY_COMPARE(m_w->selectedShapes().size(), 1);
    const SubShapeInfo circle = describeSubShape(m_part, m_w->selectedShapes().first());
    QVERIFY(circle.kind == SubShapeKind::Edge && circle.geometry == GeometryKind::Circle);
    QVERIFY(std::abs(circle.radius - 10.0) < 1e-6);
}

void TestInteracao::selecaoVertice()
{
    QSignalSpy spy(m_w, &OcctViewerWidget::selectionChanged);
    const gp_Pnt corner(100.0, 0.0, 20.0);
    QVERIFY(click(m_w, at(corner)));
    QVERIFY(waitSelection(spy));
    const auto sel = m_w->selectedShapes();
    QCOMPARE(sel.size(), 1);
    const SubShapeInfo info = describeSubShape(m_part, sel.first());
    QVERIFY(info.kind == SubShapeKind::Vertex);
    QVERIFY(info.point.Distance(corner) < 1e-6);

    sendMouse(m_w, QEvent::MouseMove, backgroundPoint(), Qt::NoButton, Qt::NoButton);
    QVERIFY(waitFrames(m_w));
    const QImage img = m_w->grabViewerImage();
    QVERIFY(hasOrangeNear(img, project(m_w, corner), int(4 * m_w->devicePixelRatioF())));
    saveEvidence(img, "E4_07_selecao_vertice");
}

void TestInteracao::selecaoLimpaNoFundo()
{
    QVERIFY(click(m_w, at(gp_Pnt(25.0, 15.0, 20.0))));
    QTRY_COMPARE(m_w->selectedShapes().size(), 1);
    pauseBetweenClicks();
    QSignalSpy spy(m_w, &OcctViewerWidget::selectionChanged);
    QVERIFY(click(m_w, backgroundPoint()));
    QVERIFY(waitSelection(spy));
    QVERIFY(m_w->selectedShapes().isEmpty());
}

void TestInteracao::navegacaoNaoSeleciona()
{
    QVERIFY(click(m_w, at(gp_Pnt(25.0, 15.0, 20.0))));
    QTRY_COMPARE(m_w->selectedShapes().size(), 1);
    const TopoDS_Shape face = m_w->selectedShapes().first();
    pauseBetweenClicks();

    // Rotação, pan, zoom, clique do botão central e arrasto do esquerdo sobre outra entidade.
    const QPointF c(m_w->width() / 2.0, m_w->height() / 2.0);
    QVERIFY(drag(m_w, c, c + QPointF(60, 30), Qt::MiddleButton));
    QVERIFY(drag(m_w, c, c + QPointF(-40, 20), Qt::MiddleButton, Qt::ControlModifier));
    sendWheel(m_w, c, 120);
    QVERIFY(click(m_w, at(gp_Pnt(50.0, 0.0, 10.0)), Qt::MiddleButton));
    pauseBetweenClicks();
    const QPointF frontFace = at(gp_Pnt(50.0, 0.0, 10.0));
    QVERIFY(drag(m_w, frontFace, frontFace + QPointF(40, 0), Qt::LeftButton));

    const auto sel = m_w->selectedShapes();
    QCOMPARE(sel.size(), 1);
    QVERIFY(sel.first().IsSame(face));
}

void TestInteracao::viewCubeCantoSuperiorDireito()
{
    const QImage with = m_w->grabViewerImage();
    m_w->aisContext()->Erase(m_w->viewCube(), false);
    const QImage without = m_w->grabViewerImage();
    m_w->aisContext()->Display(m_w->viewCube(), 0, 0, false);
    saveEvidence(with, "E4_08_viewcube");

    // O que muda ao ocultar o ViewCube fica todo no canto superior direito.
    const DiffResult d = diff(with, without);
    qInfo("ViewCube: caixa %d,%d %dx%d em %dx%d", d.box.x(), d.box.y(), d.box.width(), d.box.height(),
          with.width(), with.height());
    QVERIFY(d.count > 500);
    QVERIFY(d.box.left() > with.width() * 0.7);
    QVERIFY(d.box.bottom() < with.height() * 0.4);
    QVERIFY(d.box.right() < with.width() - 1 && d.box.top() > 0);
    // Tamanho proporcional à escala da tela.
    const double expected = 60.0 * m_w->devicePixelRatioF();
    QVERIFY(d.box.width() > expected * 0.8 && d.box.width() < expected * 2.5);
}

void TestInteracao::viewCubeClique_data()
{
    QTest::addColumn<int>("orientation");
    QTest::addColumn<double>("dx");
    QTest::addColumn<double>("dy");
    QTest::addColumn<double>("dz");
    QTest::newRow("superior") << int(V3d_Zpos) << 0.0 << 0.0 << -1.0;
    QTest::newRow("frente") << int(V3d_Yneg) << 0.0 << 1.0 << 0.0;
    QTest::newRow("direita") << int(V3d_Xpos) << -1.0 << 0.0 << 0.0;
}

void TestInteracao::viewCubeClique()
{
    QFETCH(int, orientation);
    QFETCH(double, dx);
    QFETCH(double, dy);
    QFETCH(double, dz);

    // Seleção existente não deve ser alterada pelo clique no ViewCube.
    QVERIFY(click(m_w, at(gp_Pnt(25.0, 15.0, 20.0))));
    QTRY_COMPARE(m_w->selectedShapes().size(), 1);
    const TopoDS_Shape face = m_w->selectedShapes().first();
    pauseBetweenClicks();

    const QPoint target = findViewCubePixel(m_w, V3d_TypeOfOrientation(orientation));
    QVERIFY2(target.x() >= 0, "face do ViewCube não encontrada");
    QVERIFY(click(m_w, toLogical(m_w, target)));

    // Animação curta até a orientação padrão.
    const gp_Dir expected(dx, dy, dz);
    QTRY_VERIFY_WITH_TIMEOUT(m_w->view()->Camera()->Direction().Angle(expected) < 1e-3, 5000);
    QVERIFY(m_w->view()->Camera()->IsOrthographic());
    QTest::qWait(100);
    QVERIFY(waitFrames(m_w));
    QCOMPARE(m_w->selectedShapes().size(), 1);
    QVERIFY(m_w->selectedShapes().first().IsSame(face));
    saveEvidence(m_w->grabViewerImage(), QStringLiteral("E4_09_viewcube_%1").arg(QTest::currentDataTag()));
}

void TestInteracao::eventosRapidosSemTravamento()
{
    QElapsedTimer t;
    t.start();
    const QPointF c(m_w->width() / 2.0, m_w->height() / 2.0);
    sendMouse(m_w, QEvent::MouseButtonPress, c, Qt::MiddleButton, Qt::MiddleButton);
    for (int i = 0; i < 500; ++i)
    {
        const QPointF p = c + QPointF(100.0 * std::sin(i * 0.05), 60.0 * std::cos(i * 0.07));
        sendMouse(m_w, QEvent::MouseMove, p, Qt::NoButton, Qt::MiddleButton);
        if (i % 25 == 0)
            QCoreApplication::processEvents();
    }
    sendMouse(m_w, QEvent::MouseButtonRelease, c, Qt::MiddleButton, Qt::NoButton);
    for (int i = 0; i < 50; ++i)
        sendWheel(m_w, c, (i % 2) ? 120 : -120);
    QVERIFY(waitFrames(m_w, 2));
    qInfo("500 movimentos + 50 eventos de roda em %lld ms", t.elapsed());
    QVERIFY(t.elapsed() < 15000);
    QVERIFY(m_w->selectedShapes().isEmpty());
    // Continua respondendo: ajuste à janela funciona.
    m_w->fitAll();
    const QImage img = m_w->grabViewerImage();
    const DiffResult d = diff(img, backgroundReference(m_w));
    QVERIFY(d.box.width() > 0.8 * img.width() || d.box.height() > 0.8 * img.height());
}

QTEST_MAIN(TestInteracao)
#include "test_interacao.moc"
