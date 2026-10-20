// Utilitários dos testes do visualizador 3D: análise de pixels do framebuffer real.
#pragma once

#include "viewer/OcctViewerWidget.h"

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QImage>
#include <QMouseEvent>
#include <QPoint>
#include <QRect>
#include <QTest>
#include <QWheelEvent>

#include <Graphic3d_Camera.hxx>
#include <gp_Pnt.hxx>

#include <algorithm>
#include <memory>

namespace cad::test::viewer {

inline int luminance(QRgb c)
{
    return (qRed(c) * 299 + qGreen(c) * 587 + qBlue(c) * 114) / 1000;
}

inline bool differs(QRgb a, QRgb b, int tol = 12)
{
    return std::abs(qRed(a) - qRed(b)) > tol || std::abs(qGreen(a) - qGreen(b)) > tol
           || std::abs(qBlue(a) - qBlue(b)) > tol;
}

struct DiffResult
{
    int count = 0;  // pixels diferentes
    QRect box;      // retângulo envolvente dos pixels diferentes (pixels físicos)
};

// Compara duas imagens do mesmo tamanho (ex.: com e sem a peça).
inline DiffResult diff(const QImage& a, const QImage& b, int tol = 12)
{
    DiffResult r;
    if (a.size() != b.size())
        return r;
    const QImage ia = a.convertToFormat(QImage::Format_RGB32);
    const QImage ib = b.convertToFormat(QImage::Format_RGB32);
    int x0 = ia.width(), y0 = ia.height(), x1 = -1, y1 = -1;
    for (int y = 0; y < ia.height(); ++y)
    {
        const auto* la = reinterpret_cast<const QRgb*>(ia.constScanLine(y));
        const auto* lb = reinterpret_cast<const QRgb*>(ib.constScanLine(y));
        for (int x = 0; x < ia.width(); ++x)
        {
            if (differs(la[x], lb[x], tol))
            {
                ++r.count;
                x0 = std::min(x0, x);
                y0 = std::min(y0, y);
                x1 = std::max(x1, x);
                y1 = std::max(y1, y);
            }
        }
    }
    if (r.count > 0)
        r.box = QRect(QPoint(x0, y0), QPoint(x1, y1));
    return r;
}

// Projeta um ponto 3D em pixels físicos da janela OCCT (origem no canto superior esquerdo).
inline QPoint project(const cad::viewer::OcctViewerWidget* w, const gp_Pnt& p)
{
    int x = 0, y = 0;
    w->view()->Convert(p.X(), p.Y(), p.Z(), x, y);
    return QPoint(x, y);
}

inline QRgb pixelAt(const QImage& img, QPoint p)
{
    p.setX(std::clamp(p.x(), 0, img.width() - 1));
    p.setY(std::clamp(p.y(), 0, img.height() - 1));
    return img.pixel(p);
}

// Média de cor em uma vizinhança (2r+1)x(2r+1).
inline QRgb meanAround(const QImage& img, QPoint c, int r)
{
    long sr = 0, sg = 0, sb = 0, n = 0;
    for (int dy = -r; dy <= r; ++dy)
        for (int dx = -r; dx <= r; ++dx)
        {
            const QRgb p = pixelAt(img, c + QPoint(dx, dy));
            sr += qRed(p);
            sg += qGreen(p);
            sb += qBlue(p);
            ++n;
        }
    return qRgb(int(sr / n), int(sg / n), int(sb / n));
}

inline int minLuminanceAround(const QImage& img, QPoint c, int r)
{
    int m = 255;
    for (int dy = -r; dy <= r; ++dy)
        for (int dx = -r; dx <= r; ++dx)
            m = std::min(m, luminance(pixelAt(img, c + QPoint(dx, dy))));
    return m;
}

// Salva a imagem em $CADMECANICO_EVIDENCE_DIR, quando definido.
inline void saveEvidence(const QImage& img, const QString& name)
{
    const QString dir = qEnvironmentVariable("CADMECANICO_EVIDENCE_DIR");
    if (dir.isEmpty())
        return;
    QDir().mkpath(dir);
    img.save(QDir(dir).filePath(name + ".png"));
}

// Aguarda a inicialização do OpenGL e ao menos um quadro renderizado.
inline bool waitViewerReady(cad::viewer::OcctViewerWidget* w, int timeoutMs = 10000)
{
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < timeoutMs)
    {
        if (w->isViewerInitialized() && w->frameCount() > 0)
            return true;
        QTest::qWait(20);
    }
    return false;
}

// Aguarda novos quadros (eventos de entrada são processados no paintGL).
inline bool waitFrames(cad::viewer::OcctViewerWidget* w, int frames = 1, int timeoutMs = 5000)
{
    const int target = w->frameCount() + frames;
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < timeoutMs)
    {
        w->update(); // um update() por quadro pedido (o Qt agrupa updates pendentes)
        QTest::qWait(10);
        if (w->frameCount() >= target)
            return true;
    }
    return false;
}

// Imagem do mesmo viewer sem a peça: mesmo tamanho, mesma câmera (fundo e triedro idênticos).
inline QImage backgroundReference(cad::viewer::OcctViewerWidget* w)
{
    auto ref = std::make_unique<cad::viewer::OcctViewerWidget>();
    ref->resize(w->size());
    ref->move(w->pos());
    ref->show();
    if (!waitViewerReady(ref.get()))
        return QImage();
    ref->view()->Camera()->Copy(w->view()->Camera());
    ref->view()->Invalidate();
    return ref->grabViewerImage();
}

// --- Eventos de mouse do Qt entregues ao widget (E4) ---

// Pixel físico (OCCT) -> posição lógica do Qt.
inline QPointF toLogical(const QWidget* w, QPoint physical)
{
    const qreal dpr = w->devicePixelRatioF();
    return QPointF(physical.x() / dpr, physical.y() / dpr);
}

inline void sendMouse(QWidget* w, QEvent::Type type, QPointF pos, Qt::MouseButton button, Qt::MouseButtons buttons,
               Qt::KeyboardModifiers mods = Qt::NoModifier)
{
    QMouseEvent e(type, pos, w->mapToGlobal(pos), button, buttons, mods);
    QApplication::sendEvent(w, &e);
}

inline void sendWheel(QWidget* w, QPointF pos, int angle)
{
    QWheelEvent e(pos, w->mapToGlobal(pos), QPoint(), QPoint(0, angle), Qt::NoButton, Qt::NoModifier,
                  Qt::NoScrollPhase, false);
    QApplication::sendEvent(w, &e);
}

// Arrasto com quadros renderizados entre os eventos, como numa interação real.
inline bool drag(cad::viewer::OcctViewerWidget* w, QPointF from, QPointF to, Qt::MouseButton button,
          Qt::KeyboardModifiers mods = Qt::NoModifier, int steps = 8)
{
    sendMouse(w, QEvent::MouseMove, from, Qt::NoButton, Qt::NoButton, mods);
    sendMouse(w, QEvent::MouseButtonPress, from, button, button, mods);
    if (!waitFrames(w))
        return false;
    for (int i = 1; i <= steps; ++i)
    {
        const QPointF p = from + (to - from) * (double(i) / steps);
        sendMouse(w, QEvent::MouseMove, p, Qt::NoButton, button, mods);
        if (!waitFrames(w))
            return false;
    }
    sendMouse(w, QEvent::MouseButtonRelease, to, button, Qt::NoButton, mods);
    return waitFrames(w);
}

inline bool click(cad::viewer::OcctViewerWidget* w, QPointF pos, Qt::MouseButton button = Qt::LeftButton,
           Qt::KeyboardModifiers mods = Qt::NoModifier)
{
    sendMouse(w, QEvent::MouseMove, pos, Qt::NoButton, Qt::NoButton, mods);
    if (!waitFrames(w))
        return false;
    sendMouse(w, QEvent::MouseButtonPress, pos, button, button, mods);
    sendMouse(w, QEvent::MouseButtonRelease, pos, button, Qt::NoButton, mods);
    return waitFrames(w, 2);
}

// Espera o intervalo de duplo clique do OCCT (0,4 s) para cliques independentes.
inline void pauseBetweenClicks()
{
    QTest::qWait(450);
}

} // namespace cad::test::viewer
