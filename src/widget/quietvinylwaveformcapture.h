#pragma once

#include <QOpenGLContext>
#include <QOpenGLFramebufferObject>
#include <QOpenGLFunctions>
#include <QOpenGLWindow>
#include <QScopeGuard>
#include "waveform/widgets/allshader/waveformwidget.h"
#include "widget/wglwidget.h"

// Render at remote density without resizing the visible HDMI waveform.
inline QImage quietVinylCaptureWaveform(WGLWidget* widget, qreal pixelRatio) {
    auto* window = widget->getOpenGLWindow();
    if (!window) return {};
    auto* renderer = dynamic_cast<allshader::WaveformWidget*>(widget);
    if (!renderer || !widget->isContextValid() || renderer->getWidth() <= 0 || renderer->getHeight() <= 0) {
        return window->grabFramebuffer();
    }
    auto* previousContext = QOpenGLContext::currentContext();
    auto* previousSurface = previousContext ? previousContext->surface() : nullptr;
    widget->makeCurrentIfNeeded();
    const auto restoreContext = qScopeGuard([&] {
        widget->doneCurrent();
        if (previousContext && previousSurface) previousContext->makeCurrent(previousSurface);
    });
    auto* functions = window->context()->functions();
    GLint framebuffer, viewport[4];
    functions->glGetIntegerv(GL_FRAMEBUFFER_BINDING, &framebuffer);
    functions->glGetIntegerv(GL_VIEWPORT, viewport);
    const int width = renderer->getWidth(), height = renderer->getHeight();
    const float originalRatio = renderer->getDevicePixelRatio();
    QOpenGLFramebufferObject target(QSize(qRound(width * pixelRatio), qRound(height * pixelRatio)),
            QOpenGLFramebufferObject::CombinedDepthStencil);
    if (!target.isValid()) return window->grabFramebuffer();
    const auto restoreRenderer = qScopeGuard([&] {
        renderer->resizeRenderer(width, height, originalRatio);
        widget->resizeGL(qRound(width * originalRatio), qRound(height * originalRatio));
        functions->glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
        functions->glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
    });
    renderer->resizeRenderer(width, height, static_cast<float>(pixelRatio));
    widget->resizeGL(target.width(), target.height());
    if (!target.bind()) return {};
    functions->glViewport(0, 0, target.width(), target.height());
    functions->glClearColor(0, 0, 0, 1);
    functions->glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    widget->paintGL();
    return target.toImage();
}
