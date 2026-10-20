// Testes do visualizador 3D com contexto OpenGL real (requer sessão gráfica do Windows).
// A renderização é verificada por pixels do framebuffer, não apenas pela janela aberta.
#include "ViewerTestUtils.h"

#include "modeling/TestPart.h"
#include "viewer/OcctViewerWidget.h"

#include <QScreen>
#include <QTest>

#include <Aspect_NeutralWindow.hxx>
#include <Graphic3d_Camera.hxx>
#include <gp_Dir.hxx>

#include <cmath>
#include <memory>

using cad::viewer::OcctViewerWidget;
using cad::viewer::StandardView;
using namespace cad::test::viewer;

namespace {

bool sameDir(const gp_Dir& a, const gp_Dir& b, double tolRad = 1e-3)
{
    return a.Angle(b) < tolRad;
}

QSize physicalSize(const QWidget* w)
{
    const qreal dpr = w->devicePixelRatioF();
    return QSize(qRound(w->width() * dpr), qRound(w->height() * dpr));
}

// O FBO do Qt pode diferir em 1 pixel do arredondamento de tamanho lógico x devicePixelRatio.
bool closeTo(const QSize& a, const QSize& b)
{
    return std::abs(a.width() - b.width()) <= 1 && std::abs(a.height() - b.height()) <= 1;
}

QSize windowSize(const OcctViewerWidget* w)
{
    int width = 0, height = 0;
    w->view()->Window()->Size(width, height);
    return QSize(width, height);
}

double aspect(const QRect& r)
{
    return r.height() > 0 ? double(r.width()) / double(r.height()) : 0.0;
}

} // namespace

class TestViewer : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    // E3
    void inicializaOpenGL();
    void renderizaPeca();
    void sombreamentoEArestas();
    void fundoCad();
    void triedro();
    void projecaoOrtografica();
    void vistaIsometricaInicial();
    void vistasPadrao_data();
    void vistasPadrao();
    void ajustarJanela();
    void redimensionamento();
    void devicePixelRatio();
    void geometriaPreservada();
    void liberaRecursos();

private:
    QImage grab(const QString& evidence = QString());

    OcctViewerWidget* m_w = nullptr;
    TopoDS_Shape m_part;
};

void TestViewer::init()
{
    m_part = cad::modeling::buildTestPart();
    m_w = new OcctViewerWidget;
    m_w->resize(800, 600);
    m_w->move(40, 40);
    m_w->setShape(m_part);
    m_w->show();
    QVERIFY(QTest::qWaitForWindowExposed(m_w));
    QVERIFY2(waitViewerReady(m_w), "contexto OpenGL/OCCT não inicializado");
}

void TestViewer::cleanup()
{
    delete m_w;
    m_w = nullptr;
}

QImage TestViewer::grab(const QString& evidence)
{
    const QImage img = m_w->grabViewerImage();
    if (!evidence.isEmpty())
        saveEvidence(img, evidence);
    return img;
}

void TestViewer::inicializaOpenGL()
{
    QVERIFY(m_w->isViewerInitialized());
    QVERIFY(m_w->frameCount() > 0);
    qInfo("OpenGL: %s", qPrintable(m_w->glInfo()));
    QVERIFY(!m_w->glInfo().isEmpty());
    // Renderizador por software (GDI Generic) indicaria ausência de driver OpenGL.
    QVERIFY2(!m_w->glInfo().contains("GDI Generic"), qPrintable(m_w->glInfo()));

    QVERIFY(!m_w->view()->Window().IsNull());
    // A janela OCCT tem o tamanho físico do framebuffer do Qt.
    const QImage img = grab();
    QCOMPARE(windowSize(m_w), img.size());
    QVERIFY(closeTo(img.size(), physicalSize(m_w)));

    // Todas as linhas e colunas do framebuffer são renderizadas (bordas com a cor do fundo).
    const QRgb bottomLeft = img.pixel(img.width() / 2, img.height() - 1);
    const QRgb bottomPrev = img.pixel(img.width() / 2, img.height() - 2);
    const QRgb rightEdge = img.pixel(img.width() - 1, 2);
    const QRgb rightPrev = img.pixel(img.width() - 2, 2);
    QVERIFY(!differs(bottomLeft, bottomPrev, 6));
    QVERIFY(!differs(rightEdge, rightPrev, 6));
}

void TestViewer::renderizaPeca()
{
    const QImage img = grab("E3_01_renderizacao_isometrica");
    const QImage ref = backgroundReference(m_w);
    saveEvidence(ref, "E3_01b_referencia_sem_peca");
    QVERIFY(closeTo(img.size(), physicalSize(m_w)));
    QCOMPARE(ref.size(), img.size());

    const DiffResult d = diff(img, ref);
    const double fraction = double(d.count) / (img.width() * img.height());
    qInfo("Pixels da peca: %d (%.1f%%), caixa %d,%d %dx%d", d.count, fraction * 100.0, d.box.x(), d.box.y(),
          d.box.width(), d.box.height());
    QVERIFY2(fraction > 0.15, "a peça não aparece no framebuffer");

    // A peça projetada fica dentro da janela (ajustada) e ocupa boa parte dela.
    QVERIFY(d.box.left() > 0 && d.box.top() > 0);
    QVERIFY(d.box.right() < img.width() - 1 && d.box.bottom() < img.height() - 1);
    QVERIFY(d.box.width() > 0.7 * img.width() || d.box.height() > 0.7 * img.height());

    // A posição da peça na imagem coincide com a projeção da geometria real.
    const QPoint c = project(m_w, gp_Pnt(25.0, 15.0, 20.0)); // face superior, fora do furo
    QVERIFY(d.box.contains(c));
    QVERIFY(differs(meanAround(img, c, 2), meanAround(ref, c, 2)));
}

void TestViewer::sombreamentoEArestas()
{
    const QImage img = grab();

    // Centro das três faces visíveis na isométrica: iluminações diferentes.
    const QRgb top   = meanAround(img, project(m_w, gp_Pnt(25.0, 15.0, 20.0)), 3);
    const QRgb front = meanAround(img, project(m_w, gp_Pnt(50.0, 0.0, 10.0)), 3);
    const QRgb right = meanAround(img, project(m_w, gp_Pnt(100.0, 30.0, 10.0)), 3);
    qInfo("Luminancia: superior %d, frontal %d, direita %d", luminance(top), luminance(front), luminance(right));
    QVERIFY(std::abs(luminance(top) - luminance(front)) > 8);
    QVERIFY(std::abs(luminance(top) - luminance(right)) > 8);
    QVERIFY(std::abs(luminance(front) - luminance(right)) > 8);

    // Arestas escuras sobre as faces: aresta superior frontal e borda do furo.
    const int faceLum = luminance(top);
    const int edgeFront = minLuminanceAround(img, project(m_w, gp_Pnt(50.0, 0.0, 20.0)), 2);
    const int edgeHole  = minLuminanceAround(img, project(m_w, gp_Pnt(50.0, 20.0, 20.0)), 2);
    qInfo("Arestas: frontal %d, furo %d (face %d)", edgeFront, edgeHole, faceLum);
    QVERIFY(edgeFront < 50 && edgeFront < faceLum - 40);
    QVERIFY(edgeHole < 50 && edgeHole < faceLum - 40);
}

void TestViewer::fundoCad()
{
    const QImage ref = backgroundReference(m_w);
    QVERIFY(!ref.isNull());
    // Degradê vertical: claro em cima, escuro embaixo (centro horizontal, sem triedro).
    const QRgb topColor = meanAround(ref, QPoint(ref.width() / 2, 3), 2);
    const QRgb bottomColor = meanAround(ref, QPoint(ref.width() / 2, ref.height() - 4), 2);
    qInfo("Fundo: topo (%d,%d,%d) base (%d,%d,%d)", qRed(topColor), qGreen(topColor), qBlue(topColor),
          qRed(bottomColor), qGreen(bottomColor), qBlue(bottomColor));
    QVERIFY(!differs(topColor, qRgb(209, 219, 232), 12));
    QVERIFY(!differs(bottomColor, qRgb(92, 107, 133), 12));
    QVERIFY(qBlue(topColor) > qRed(topColor)); // tom azulado
}

void TestViewer::triedro()
{
    const QImage ref = backgroundReference(m_w);
    // Canto inferior esquerdo: eixos X (vermelho), Y (verde) e Z (azul).
    const QRect corner(0, ref.height() * 3 / 4, ref.width() / 4, ref.height() / 4);
    bool red = false, green = false, blue = false;
    for (int y = corner.top(); y <= corner.bottom(); ++y)
        for (int x = corner.left(); x <= corner.right(); ++x)
        {
            const QRgb p = ref.pixel(x, y);
            red   |= qRed(p) > 150 && qGreen(p) < 90 && qBlue(p) < 90;
            green |= qGreen(p) > 120 && qRed(p) < 90 && qBlue(p) < 90;
            blue  |= qBlue(p) > 150 && qRed(p) < 80 && qGreen(p) < 80;
        }
    QVERIFY2(red && green && blue, "triedro não encontrado no canto inferior esquerdo");
}

void TestViewer::projecaoOrtografica()
{
    QVERIFY(m_w->view()->Camera()->IsOrthographic());
    m_w->setStandardView(StandardView::Front);
    QVERIFY(m_w->view()->Camera()->IsOrthographic());
}

void TestViewer::vistaIsometricaInicial()
{
    const auto& cam = m_w->view()->Camera();
    QVERIFY(sameDir(cam->Direction(), gp_Dir(-1.0, 1.0, -1.0)));
    QVERIFY(cam->OrthogonalizedUp().Dot(gp::DZ()) > 0.5); // Z para cima
}

void TestViewer::vistasPadrao_data()
{
    QTest::addColumn<int>("view");
    QTest::addColumn<double>("dx");
    QTest::addColumn<double>("dy");
    QTest::addColumn<double>("dz");
    QTest::addColumn<double>("expectedAspect"); // largura/altura da peça projetada
    QTest::addColumn<QString>("evidence");

    QTest::newRow("frente")    << int(StandardView::Front) << 0.0 << 1.0 << 0.0 << 100.0 / 20.0 << "E3_02_vista_frente";
    QTest::newRow("superior")  << int(StandardView::Top) << 0.0 << 0.0 << -1.0 << 100.0 / 60.0 << "E3_03_vista_superior";
    QTest::newRow("direita")   << int(StandardView::Right) << -1.0 << 0.0 << 0.0 << 60.0 / 20.0 << "E3_04_vista_direita";
    QTest::newRow("isometrica") << int(StandardView::Isometric) << -1.0 << 1.0 << -1.0 << 0.0 << "E3_05_vista_isometrica";
}

void TestViewer::vistasPadrao()
{
    QFETCH(int, view);
    QFETCH(double, dx);
    QFETCH(double, dy);
    QFETCH(double, dz);
    QFETCH(double, expectedAspect);
    QFETCH(QString, evidence);

    m_w->setStandardView(StandardView(view));
    const QImage img = grab(evidence);
    QVERIFY(sameDir(m_w->view()->Camera()->Direction(), gp_Dir(dx, dy, dz)));
    // Z para cima (projetado no plano da tela); na vista superior, Y para cima.
    const gp_Dir dir = m_w->view()->Camera()->Direction();
    const gp_Vec ref = view == int(StandardView::Top) ? gp_Vec(gp::DY()) : gp_Vec(gp::DZ());
    const gp_Dir expectedUp(ref - gp_Vec(dir) * ref.Dot(gp_Vec(dir)));
    QVERIFY(m_w->view()->Camera()->OrthogonalizedUp().Angle(expectedUp) < 1e-3);

    const DiffResult d = diff(img, backgroundReference(m_w));
    QVERIFY(d.count > 0);
    // Peça enquadrada em cada vista.
    QVERIFY(d.box.width() > 0.7 * img.width() || d.box.height() > 0.7 * img.height());
    if (expectedAspect > 0.0)
    {
        qInfo("Proporcao projetada %.3f (esperada %.3f)", aspect(d.box), expectedAspect);
        QVERIFY(std::abs(aspect(d.box) - expectedAspect) / expectedAspect < 0.05);
    }
}

void TestViewer::ajustarJanela()
{
    // Afasta e desloca a câmera: a peça fica pequena e fora do centro.
    const auto& cam = m_w->view()->Camera();
    cam->SetScale(cam->Scale() * 5.0);
    cam->SetCenter(cam->Center().Translated(gp_Vec(150.0, 0.0, 0.0)));
    m_w->view()->Invalidate();
    const QImage before = grab("E3_06a_antes_ajustar");
    const DiffResult d0 = diff(before, backgroundReference(m_w));
    QVERIFY(d0.box.width() < 0.4 * before.width() && d0.box.height() < 0.4 * before.height());

    m_w->fitAll();
    const QImage after = grab("E3_06b_depois_ajustar");
    const DiffResult d1 = diff(after, backgroundReference(m_w));
    QVERIFY(d1.box.width() > 0.8 * after.width() || d1.box.height() > 0.8 * after.height());
    QVERIFY(d1.box.left() > 0 && d1.box.top() > 0 && d1.box.right() < after.width() - 1
            && d1.box.bottom() < after.height() - 1);
    // Centralizada.
    const QPointF delta = QRectF(d1.box).center() - QPointF(after.width() / 2.0, after.height() / 2.0);
    QVERIFY(std::abs(delta.x()) < 0.05 * after.width() && std::abs(delta.y()) < 0.05 * after.height());
}

void TestViewer::redimensionamento()
{
    // Tamanhos limitados à área útil da tela (com escala alta a tela lógica é menor).
    const QSize available = m_w->screen()->availableGeometry().size() * 0.85;
    for (QSize s : {QSize(640, 420), QSize(1000, 700), QSize(420, 320)})
    {
        s = s.boundedTo(available);
        m_w->resize(s);
        QTRY_COMPARE(m_w->size(), s);
        m_w->fitAll();
        const QImage img = grab(QStringLiteral("E3_07_redimensionado_%1x%2").arg(s.width()).arg(s.height()));
        QVERIFY(closeTo(img.size(), physicalSize(m_w)));
        QCOMPARE(windowSize(m_w), img.size());

        const DiffResult d = diff(img, backgroundReference(m_w));
        QVERIFY(d.count > 0);
        QVERIFY(d.box.right() < img.width() - 1 && d.box.bottom() < img.height() - 1);
    }
}

void TestViewer::devicePixelRatio()
{
    const qreal dpr = m_w->devicePixelRatioF();
    qInfo("devicePixelRatio = %.3f (QT_SCALE_FACTOR=%s)", dpr, qgetenv("QT_SCALE_FACTOR").constData());
    QCOMPARE(dpr, m_w->screen()->devicePixelRatio());
    if (qEnvironmentVariableIsSet("QT_SCALE_FACTOR"))
        QVERIFY(dpr >= qEnvironmentVariable("QT_SCALE_FACTOR").toDouble() - 1e-6);

    const QImage img = grab(QStringLiteral("E3_08_dpr_%1").arg(int(std::round(dpr * 100))));
    QVERIFY(closeTo(img.size(), physicalSize(m_w)));
    QCOMPARE(windowSize(m_w), img.size());
    QCOMPARE(m_w->view()->RenderingParams().Resolution, static_cast<unsigned int>(96.0 * dpr + 0.5));

    // A projeção usa pixels físicos: o ponto projetado cai sobre a peça na imagem física.
    const QPoint c = project(m_w, gp_Pnt(25.0, 15.0, 20.0));
    QVERIFY(QRect(QPoint(0, 0), img.size()).contains(c));
    QVERIFY(differs(meanAround(img, c, 2), meanAround(backgroundReference(m_w), c, 2)));
}

void TestViewer::geometriaPreservada()
{
    QVERIFY(m_w->shape().IsSame(m_part));
    QVERIFY(m_w->shapePresentation()->Shape().IsSame(m_part));
}

void TestViewer::liberaRecursos()
{
    // Criação e destruição repetidas, inclusive sem exibir, não devem falhar nem vazar o contexto.
    for (int i = 0; i < 3; ++i)
    {
        auto w = std::make_unique<OcctViewerWidget>();
        w->resize(320, 240);
        w->setShape(m_part);
        w->show();
        QVERIFY(waitViewerReady(w.get()));
        QVERIFY(!w->grabViewerImage().isNull());
    }
    {
        OcctViewerWidget hidden; // nunca exibido: sem contexto OpenGL
        hidden.setShape(m_part);
        hidden.fitAll();
        hidden.setStandardView(StandardView::Top);
        QVERIFY(!hidden.isViewerInitialized());
    }
    // O widget principal continua renderizando após as destruições.
    const DiffResult d = diff(grab(), backgroundReference(m_w));
    QVERIFY(d.count > 0);
}

QTEST_MAIN(TestViewer)
#include "test_viewer.moc"
