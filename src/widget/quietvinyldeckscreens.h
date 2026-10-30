#pragma once

#include <QApplication>
#include <QCloseEvent>
#include <QDebug>
#include <QScreen>
#include <QStackedWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QWindow>
#include <array>
#include <functional>
#include "widget/quietvinyldisplaybridge.h"

// Two presentations of the same engine. The single shared browser moves only
// when its explicit destination changes; the other deck keeps displaying audio.
class QuietVinylDeckScreens : public QWidget {
  public:
    using Read = std::function<double(const QString&, const QString&)>;
    using Pages = std::array<std::array<QWidget*, 2>, 2>; // deck, full/minimal
    QuietVinylDeckScreens(QWidget* skin, const Pages& pages, QWidget* browser,
            Read read, QWidget* parent = nullptr,
            QuietVinylDisplayBridge::NativeImage nativeImage = {})
            : QWidget(parent), m_browser(browser), m_read(std::move(read)) {
        setObjectName("QuietVinylRoot");
        setProperty("quietvinylDualPresentation", true);
        setStyleSheet(skin->styleSheet());
        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        m_secondWindow = new QWidget(this, Qt::Window);
        m_secondWindow->setObjectName("QuietVinylRoot");
        m_secondWindow->setWindowTitle("QuietVinyl · Deck 2");
        m_secondWindow->setStyleSheet(styleSheet());
        m_secondWindow->installEventFilter(this);
        auto* secondLayout = new QVBoxLayout(m_secondWindow);
        secondLayout->setContentsMargins(0, 0, 0, 0);
        for (int deck = 0; deck < 2; ++deck) {
            m_hosts[deck] = new QStackedWidget;
            m_hosts[deck]->setObjectName(QStringLiteral("QuietVinylDeckSurface%1").arg(deck + 1));
            m_playback[deck] = new QStackedWidget;
            m_hosts[deck]->addWidget(m_playback[deck]);
            for (auto* page : pages[deck]) {
                page->setProperty("quietvinylFixedDeck", true);
                m_playback[deck]->addWidget(page);
            }
        }
        layout->addWidget(m_hosts[0]);
        secondLayout->addWidget(m_hosts[1]);
        // Preserve the library bindings and original skin-owned controls, even
        // though their old deck/page stacks no longer own the visible widgets.
        m_browser->setParent(this);
        m_browser->hide();
        skin->setParent(this);
        skin->hide();
        m_timer.setInterval(20);
        connect(&m_timer, &QTimer::timeout, this, [this] { refresh(); });
        m_timer.start();
        refresh();
        const auto displaySocket = qEnvironmentVariable("QUIETVINYL_DENON_SOCKET");
        if (!displaySocket.isEmpty()) {
            new QuietVinylDisplayBridge(displaySocket, {m_hosts[0], m_hosts[1]},
                    std::move(nativeImage), this, [this] { refresh(); });
        }
    }

    void refresh() {
        const int sharedView = m_read("[QuietVinyl]", "minimal_mode") > 0 ? 1 : 0;
        for (int deck = 0; deck < 2; ++deck) {
            const double choice = m_read("[QuietVinyl]",
                    QStringLiteral("deck%1_view_mode").arg(deck + 1));
            const int view = choice == 1 ? 0 : choice == 2 ? 1 : sharedView;
            m_playback[deck]->setCurrentIndex(view);
            if (view != m_view[deck]) { m_view[deck] = view; invalidateDisplay(deck); }
        }
        const bool browsing = m_read("[Skin]", "show_maximized_library") > 0;
        const int target = m_read("[QuietVinyl]", "deck_page") > 0 ? 1 : 0;
        const int destination = browsing ? target : -1;
        if (destination == m_browserDeck) return;
        if (m_browserDeck >= 0) {
            m_hosts[m_browserDeck]->setCurrentWidget(m_playback[m_browserDeck]);
            m_hosts[m_browserDeck]->removeWidget(m_browser);
        }
        m_browserDeck = destination;
        if (destination >= 0) {
            m_hosts[destination]->addWidget(m_browser);
            m_hosts[destination]->setCurrentWidget(m_browser);
        } else {
            m_browser->setParent(this);
            m_browser->hide();
        }
        invalidateDisplay();
    }

    QWidget* secondaryWindow() const { return m_secondWindow; }

    void present(QScreen* first, QScreen* second) {
        auto* main = window();
        main->winId();
        main->showNormal();
        // Wayland acknowledges leaving fullscreen asynchronously. Issuing the
        // new fullscreen request in the same turn can leave the old output or
        // the normal state in effect. Let the first configure settle first.
        QTimer::singleShot(200, this, [this, main, first, second] {
            main->setScreen(first);
            main->setGeometry(first->geometry());
            main->showFullScreen();
            m_secondWindow->winId();
            m_secondWindow->setScreen(second);
            m_secondWindow->setGeometry(second->geometry());
            m_secondWindow->showFullScreen();
            qInfo() << "QuietVinyl deck screens:" << first->name() << second->name();
        });
    }

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (watched == m_secondWindow && event->type() == QEvent::Close) {
            // Qt's application Quit also closes every top-level window. An
            // unconditional ignore here would keep the engine running after
            // the main window disappeared. Respect its exit confirmation.
            if (window()->close()) event->accept();
            else event->ignore();
            return true;
        }
        return QWidget::eventFilter(watched, event);
    }

  private:
    QWidget* m_browser;
    QWidget* m_secondWindow;
    Read m_read;
    std::array<QStackedWidget*, 2> m_hosts;
    std::array<QStackedWidget*, 2> m_playback;
    QTimer m_timer;
    int m_browserDeck = -1;
    std::array<int, 2> m_view{{-1, -1}};
    quint64 m_displayEpoch = 0;
    void invalidateDisplay(int deck = -1) {
        for (int i = 0; i < 2; ++i) {
            if (deck < 0 || deck == i)
                m_hosts[i]->setProperty("quietvinylDisplayEpoch", QString::number(++m_displayEpoch));
        }
    }
};
