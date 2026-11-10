// PenCast for Linux: home screen, scrolling reading view, presenter and projector window.
#include "deck.h"
#include "media.h"
#include "slideview.h"
#include "tooloptions.h"

#include <QtWidgets>
#include <QPdfDocument>
#include <QPdfPageNavigator>
#include <QPdfView>
#include <QScroller>


// Toolbar metrics and colours of the iPad app (SlideView.swift): 44 pt targets, 22 pt icons, system blue.
static const QColor accent(10, 132, 255), destructive(255, 69, 58), orange(255, 159, 10), green(48, 209, 88);
static constexpr int barIcon = 22, barTarget = 44;

static QColor primary() { return QApplication::palette().color(QPalette::WindowText); }
static QColor secondary() { QColor c = primary(); c.setAlphaF(0.55); return c; }

static QIcon swatchIcon(const QColor &c, bool current) {
    QPixmap pm(QSize(28, 28) * 3);
    pm.setDevicePixelRatio(3);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    QColor ring = primary();
    ring.setAlphaF(current ? 0.9 : 0.15);
    p.setPen(QPen(ring, 2.5));
    p.setBrush(c);
    p.drawEllipse(QRectF(1.25, 1.25, 25.5, 25.5));
    return QIcon(pm);
}

enum class Glyph { Close, Play, Pause, Pointer, Laser, Pen, Highlighter, Eraser, Lasso, Undo, Redo, Trash, Display, Back, Forward,
                   Share, WholeSlide, Grid };

/// Line icons drawn in a 24-unit box, after the SF Symbols the iPad app uses (which can't ship on Linux).
static void drawGlyph(QPainter &p, Glyph g, const QColor &c) {
    p.setPen(QPen(c, 1.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.setBrush(Qt::NoBrush);
    auto fill = [&](const QPolygonF &poly) { p.save(); p.setBrush(c); p.setPen(Qt::NoPen); p.drawPolygon(poly); p.restore(); };
    switch (g) {
    case Glyph::Close:
        p.drawEllipse(QRectF(3, 3, 18, 18));
        p.drawLine(QPointF(9, 9), QPointF(15, 15));
        p.drawLine(QPointF(15, 9), QPointF(9, 15));
        break;
    case Glyph::Play: fill({QPointF(7, 4.5), QPointF(19.5, 12), QPointF(7, 19.5)}); break;
    case Glyph::Pause:
        p.setPen(Qt::NoPen);
        p.setBrush(c);
        p.drawRoundedRect(QRectF(6, 4.5, 4.2, 15), 1, 1);
        p.drawRoundedRect(QRectF(13.8, 4.5, 4.2, 15), 1, 1);
        break;
    case Glyph::Pointer:
        fill({QPointF(7, 2.5), QPointF(7, 19.5), QPointF(11, 15.6), QPointF(13.9, 21.6), QPointF(16.6, 20.4),
              QPointF(13.7, 14.4), QPointF(19.2, 14.4)});
        break;
    case Glyph::Laser:
        p.setPen(QPen(c, 1.5, Qt::SolidLine, Qt::RoundCap));
        for (int i = 0; i < 16; ++i) {
            const double a = i * M_PI / 8, r = i % 2 ? 6.5 : 10;
            p.drawLine(QPointF(12 + 2.5 * std::cos(a), 12 + 2.5 * std::sin(a)), QPointF(12 + r * std::cos(a), 12 + r * std::sin(a)));
        }
        break;
    case Glyph::Pen:
        p.drawLine(QPointF(12, 3), QPointF(6.5, 21));
        p.drawLine(QPointF(12, 3), QPointF(17.5, 21));
        fill({QPointF(12, 2.5), QPointF(10.1, 8.6), QPointF(13.9, 8.6)});
        break;
    case Glyph::Highlighter:
        p.drawLine(QPointF(4, 21), QPointF(20, 21));
        p.save();
        p.translate(13, 10.5);
        p.rotate(40);
        p.drawRoundedRect(QRectF(-3.5, -9, 7, 11), 1.5, 1.5);
        p.drawPolyline(QPolygonF({QPointF(-3.5, 2), QPointF(-1.8, 6.5), QPointF(1.8, 6.5), QPointF(3.5, 2)}));
        p.restore();
        break;
    case Glyph::Eraser:
        p.save();
        p.translate(12, 12);
        p.rotate(45);
        p.drawRoundedRect(QRectF(-4.5, -9.5, 9, 19), 2.5, 2.5);
        p.drawLine(QPointF(-4.5, 1.5), QPointF(4.5, 1.5));
        p.restore();
        break;
    case Glyph::Lasso: {
        QPen dashed(c, 1.8, Qt::CustomDashLine, Qt::RoundCap, Qt::RoundJoin);
        dashed.setDashPattern({2.2, 2.2});
        p.setPen(dashed);
        p.drawEllipse(QRectF(3, 3.5, 18, 13));
        p.setPen(QPen(c, 1.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        QPainterPath tail(QPointF(8, 15.5));
        tail.cubicTo(6, 18, 9, 21, 12, 21.5);
        p.drawPath(tail);
        break;
    }
    case Glyph::Share:
        p.drawPolyline(QPolygonF({QPointF(8.5, 9), QPointF(5.5, 9), QPointF(5.5, 21), QPointF(18.5, 21), QPointF(18.5, 9), QPointF(15.5, 9)}));
        p.drawLine(QPointF(12, 2.5), QPointF(12, 14.5));
        p.drawPolyline(QPolygonF({QPointF(8.5, 6), QPointF(12, 2.5), QPointF(15.5, 6)}));
        break;
    case Glyph::WholeSlide:
        p.drawEllipse(QRectF(3.5, 3.5, 13, 13));
        p.drawLine(QPointF(14.6, 14.6), QPointF(20.5, 20.5));
        p.drawLine(QPointF(7, 10), QPointF(13, 10));
        break;
    case Glyph::Grid:
        for (int r = 0; r < 2; ++r)
            for (int c = 0; c < 3; ++c) p.drawRoundedRect(QRectF(2.5 + c * 7, 5 + r * 7.5, 5.2, 5.6), 1, 1);
        break;
    case Glyph::Undo:
    case Glyph::Redo: {
        p.save();
        if (g == Glyph::Redo) { p.translate(24, 0); p.scale(-1, 1); }
        QPainterPath path(QPointF(5.5, 9));
        path.lineTo(13.5, 9);
        path.cubicTo(17.5, 9, 19.5, 11.5, 19.5, 14.5);
        path.cubicTo(19.5, 17.5, 17.5, 20, 13.5, 20);
        path.lineTo(10.5, 20);
        p.drawPath(path);
        p.drawPolyline(QPolygonF({QPointF(9.5, 5), QPointF(5.5, 9), QPointF(9.5, 13)}));
        p.restore();
        break;
    }
    case Glyph::Trash:
        p.drawLine(QPointF(4, 6.5), QPointF(20, 6.5));
        p.drawPolyline(QPolygonF({QPointF(9, 6.5), QPointF(9, 3.5), QPointF(15, 3.5), QPointF(15, 6.5)}));
        p.drawPolyline(QPolygonF({QPointF(6, 6.5), QPointF(7, 20.5), QPointF(17, 20.5), QPointF(18, 6.5)}));
        p.drawLine(QPointF(10, 10), QPointF(10.3, 17));
        p.drawLine(QPointF(14, 10), QPointF(13.7, 17));
        break;
    case Glyph::Display:
        p.drawRoundedRect(QRectF(2.5, 4, 19, 13.5), 2, 2);
        p.drawLine(QPointF(8, 20.5), QPointF(16, 20.5));
        break;
    case Glyph::Back:
    case Glyph::Forward:
        p.setPen(QPen(c, 2.2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        if (g == Glyph::Back) p.drawPolyline(QPolygonF({QPointF(15, 4.5), QPointF(8, 12), QPointF(15, 19.5)}));
        else p.drawPolyline(QPolygonF({QPointF(9, 4.5), QPointF(16, 12), QPointF(9, 19.5)}));
        break;
    }
}

static QIcon glyph(Glyph g, const QColor &c) {
    QIcon icon;
    for (const auto &[mode, color] : {std::pair{QIcon::Normal, c}, std::pair{QIcon::Disabled, QColor(128, 128, 128, 110)}}) {
        QPixmap pm(QSize(barIcon, barIcon) * 3);
        pm.setDevicePixelRatio(3);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing);
        p.scale(barIcon / 24.0, barIcon / 24.0);
        drawGlyph(p, g, color);
        p.end();
        icon.addPixmap(pm, mode);
    }
    return icon;
}

/// A borderless toolbar button: a 44 pt target around a 22 pt icon, as in the iPad app.
static QToolButton *barButton(QAction *a) {
    auto *b = new QToolButton;
    b->setDefaultAction(a);
    b->setIconSize(QSize(barIcon, barIcon));
    b->setToolButtonStyle(Qt::ToolButtonIconOnly);
    b->setFocusPolicy(Qt::NoFocus);   // keys stay with the slide: arrows turn it
    return b;
}

static QString barStyle() {
    return QStringLiteral(
        "#bar { background: palette(window); }"
        "#bar QToolButton { border: none; border-radius: 8px; padding: 0; min-width: %1px; min-height: %1px; }"
        "#bar QToolButton:hover { background: rgba(128, 128, 128, 38); }"
        "#bar QToolButton::menu-indicator { image: none; width: 0; }"
        "#tools { background: rgba(128, 128, 128, 31); border-radius: 9px; }"
        "#tools QToolButton { min-width: 50px; max-width: 50px; min-height: 42px; max-height: 42px; }"
        "#tools QToolButton:checked { background: rgba(10, 132, 255, 64); }"
        "#bar QToolButton#swatch { min-width: 34px; max-width: 34px; border-radius: 17px; }"
        "#bar QToolButton#swatch:hover { background: transparent; }").arg(barTarget);
}

/// The bar, scrolling sideways when the window is too narrow, like the iPad's fallback: nothing gets squeezed.
static QScrollArea *scrollingBar(QWidget *bar) {
    auto *area = new QScrollArea;
    area->setWidget(bar);
    area->setWidgetResizable(true);
    area->setFrameShape(QFrame::NoFrame);
    area->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    area->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    area->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    area->setFixedHeight(bar->sizeHint().height());
    QScroller::grabGesture(area->viewport(), QScroller::TouchGesture);
    return area;
}

/// The audience's window: full screen on the other monitor, or a window to drag there.
class ProjectorWindow : public QWidget {
public:
    explicit ProjectorWindow(Deck *deck) : m_deck(deck) {
        setWindowTitle(tr("PenCast Projector"));
        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        view = new SlideView(deck, SlideView::Role::Projector);
        layout->addWidget(view);
        resize(960, 540);
    }
    SlideView *view;

protected:
    void keyPressEvent(QKeyEvent *e) override {
        switch (e->key()) {
        case Qt::Key_Escape: isFullScreen() ? showNormal() : hide(); break;
        case Qt::Key_F11: isFullScreen() ? showNormal() : showFullScreen(); break;
        case Qt::Key_Right: case Qt::Key_Down: case Qt::Key_Space: case Qt::Key_PageDown: m_deck->go(m_deck->page() + 1); break;
        case Qt::Key_Left: case Qt::Key_Up: case Qt::Key_PageUp: m_deck->go(m_deck->page() - 1); break;
        default: QWidget::keyPressEvent(e);
        }
    }
    void mouseDoubleClickEvent(QMouseEvent *) override { isFullScreen() ? showNormal() : showFullScreen(); }

private:
    Deck *m_deck;
};

/// Every page as a thumbnail; click one to go there. Thumbnails render a few at a time so it opens at once.
class SlideGrid : public QDialog {
public:
    SlideGrid(Deck *deck, QWidget *parent) : QDialog(parent), m_deck(deck) {
        setWindowTitle(tr("Slides"));
        auto *layout = new QVBoxLayout(this);
        m_list = new QListWidget;
        m_list->setViewMode(QListView::IconMode);
        m_list->setResizeMode(QListView::Adjust);
        m_list->setMovement(QListView::Static);
        m_list->setUniformItemSizes(true);
        m_list->setSpacing(10);
        const QSizeF slide = deck->slideBox(0).size();
        m_icon = QSize(220, int(220 * slide.height() / std::max(slide.width(), 1.0)));
        m_list->setIconSize(m_icon);
        QPixmap blank(m_icon);
        blank.fill(QColor(128, 128, 128, 40));
        for (int i = 0; i < deck->pageCount(); ++i) {
            auto *item = new QListWidgetItem(QIcon(blank), QString::number(i + 1));
            item->setTextAlignment(Qt::AlignHCenter);
            m_list->addItem(item);
        }
        m_list->setCurrentRow(deck->page());
        connect(m_list, &QListWidget::itemClicked, this, [this](QListWidgetItem *item) {
            m_deck->go(m_list->row(item));
            accept();
        });
        connect(m_list, &QListWidget::itemActivated, this, [this](QListWidgetItem *item) {
            m_deck->go(m_list->row(item));
            accept();
        });
        layout->addWidget(m_list);
        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close);
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
        layout->addWidget(buttons);
        resize(parent->size() * 0.85);
        connect(&m_timer, &QTimer::timeout, this, &SlideGrid::renderSome);
        m_timer.start(0);
        QTimer::singleShot(0, this, [this] { m_list->scrollToItem(m_list->currentItem(), QAbstractItemView::PositionAtCenter); });
    }

private:
    // ponytail: renders every thumbnail in page order, current page first; render only visible ones if 500-page decks drag
    void renderSome() {
        const int n = m_deck->pageCount();
        for (int k = 0; k < 4 && m_next < n; ++k, ++m_next) {
            const int i = (m_deck->page() + m_next) % n;
            QPdfDocument *doc = m_deck->doc();
            const QSizeF page = doc->pagePointSize(i);
            const QRectF part = m_deck->slideBox(i);
            const double scale = m_icon.width() * devicePixelRatioF() / std::max(part.width(), 1.0);
            QPdfDocumentRenderOptions options;
            options.setScaledSize((page * scale).toSize());
            options.setScaledClipRect(QRectF(part.topLeft() * scale, part.size() * scale).toRect());
            QImage img = doc->render(i, (part.size() * scale).toSize(), options);
            img.setDevicePixelRatio(devicePixelRatioF());
            m_list->item(i)->setIcon(QIcon(QPixmap::fromImage(img)));
        }
        if (m_next >= n) m_timer.stop();
    }

    Deck *m_deck;
    QListWidget *m_list;
    QSize m_icon;
    QTimer m_timer;
    int m_next = 0;
};

class MainWindow : public QMainWindow {
public:
    explicit MainWindow(Deck *deck) : m_deck(deck) {
        loadSettings();
        m_shownTool = m_tools.tool;
        m_stack = new QStackedWidget;
        m_stack->addWidget(buildHome());
        m_stack->addWidget(buildReader());
        m_stack->addWidget(buildPresenter());
        setCentralWidget(m_stack);
        setAcceptDrops(true);
        resize(1100, 760);
        connect(deck, &Deck::changed, this, &MainWindow::refresh);
        connect(deck, &Deck::reloaded, this, &MainWindow::documentReloaded);
        m_clock.setInterval(1000);
        connect(&m_clock, &QTimer::timeout, this, &MainWindow::updateTimer);
        refresh();
    }

    ~MainWindow() override { delete m_projector; }

    void openPath(const QString &path) {
        const QString error = m_deck->open(path);
        m_openError->setText(error);
        m_openError->setVisible(!error.isEmpty());
        if (!error.isEmpty()) return;
        QStringList recent = QSettings().value("recent").toStringList();
        recent.removeAll(m_deck->path());
        recent.prepend(m_deck->path());
        QSettings().setValue("recent", recent.mid(0, 12));
        fillRecent();
    }

    void present() {
        if (!m_deck->isOpen()) return;
        m_deck->go(m_pdf->pageNavigator()->currentPage());
        m_deck->setPresenting(true);
        m_banked = 0;
        m_startedAt = QDateTime::currentMSecsSinceEpoch();
        m_clock.start();
        updateTimer();
        if (QScreen *other = otherScreen(); other && !(m_projector && m_projector->isVisible())) openProjector();
        m_slide->setFocus();
    }

protected:
    void dragEnterEvent(QDragEnterEvent *e) override {
        if (!droppedPdf(e->mimeData()).isEmpty()) e->acceptProposedAction();
    }
    void dropEvent(QDropEvent *e) override { openPath(droppedPdf(e->mimeData())); }
    void closeEvent(QCloseEvent *e) override {
        if (m_projector) m_projector->close();
        QMainWindow::closeEvent(e);
    }

private:
    static QString droppedPdf(const QMimeData *m) {
        for (const QUrl &u : m->urls())
            if (u.isLocalFile() && u.toLocalFile().endsWith(".pdf", Qt::CaseInsensitive)) return u.toLocalFile();
        return {};
    }

    QScreen *otherScreen() const {
        for (QScreen *s : QGuiApplication::screens()) if (s != screen()) return s;
        return nullptr;
    }

    // MARK: Home

    QWidget *buildHome() {
        auto *page = new QWidget;
        auto *layout = new QVBoxLayout(page);
        layout->setContentsMargins(32, 28, 32, 28);
        auto *title = new QLabel(tr("Presentations"));
        QFont f = title->font();
        f.setPointSizeF(f.pointSizeF() * 1.8);
        f.setBold(true);
        title->setFont(f);
        auto *openButton = new QPushButton(tr("Open PDF…"));
        openButton->setDefault(true);
        connect(openButton, &QPushButton::clicked, this, [this] {
            const QString path = QFileDialog::getOpenFileName(this, tr("Open Presentation"), {}, tr("PDF (*.pdf)"));
            if (!path.isEmpty()) openPath(path);
        });
        auto *top = new QHBoxLayout;
        top->addWidget(title);
        top->addStretch();
        top->addWidget(openButton);
        layout->addLayout(top);
        m_openError = new QLabel;
        m_openError->setStyleSheet("color: #d33");
        m_openError->hide();
        layout->addWidget(m_openError);
        m_recent = new QListWidget;
        m_recent->setIconSize(QSize(32, 32));
        connect(m_recent, &QListWidget::itemActivated, this, [this](QListWidgetItem *item) {
            openPath(item->data(Qt::UserRole).toString());
        });
        layout->addWidget(m_recent, 1);
        auto *hint = new QLabel(tr("Drop a PDF here, or open one from a terminal or your LaTeX editor: "
                                   "<code>pencast deck.pdf</code>. An open deck reloads itself each time it is recompiled."));
        hint->setWordWrap(true);
        hint->setEnabled(false);
        layout->addWidget(hint);
        fillRecent();
        return page;
    }

    void fillRecent() {
        m_recent->clear();
        for (const QString &path : QSettings().value("recent").toStringList()) {
            if (!QFileInfo::exists(path)) continue;
            auto *item = new QListWidgetItem(QFileInfo(path).completeBaseName() + "\n" + QDir::toNativeSeparators(path));
            item->setData(Qt::UserRole, path);
            m_recent->addItem(item);
        }
    }

    // MARK: Reading view

    QWidget *buildReader() {
        auto *page = new QWidget;
        auto *layout = new QVBoxLayout(page);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(0);
        auto *bar = new QWidget;
        bar->setObjectName("bar");
        bar->setStyleSheet(barStyle() +
            "QPushButton#present { background: rgb(10, 132, 255); color: white; border: none; border-radius: 8px;"
            " padding: 7px 14px; font-weight: 600; }"
            "QPushButton#present:pressed { background: rgb(0, 100, 220); }");
        auto *row = new QHBoxLayout(bar);
        row->setContentsMargins(10, 4, 10, 4);
        row->setSpacing(4);
        auto *closeAction = new QAction(glyph(Glyph::Close, accent), tr("Close"), page);
        closeAction->setToolTip(tr("Close presentation"));
        connect(closeAction, &QAction::triggered, m_deck, &Deck::close);
        row->addWidget(barButton(closeAction));
        m_readerTitle = new QLabel;
        m_readerTitle->setTextFormat(Qt::PlainText);
        m_readerTitle->setStyleSheet("font-weight: 600");
        row->addWidget(m_readerTitle);
        m_readerPage = new QLabel;
        m_readerPage->setStyleSheet(QStringLiteral("color: %1").arg(secondary().name(QColor::HexArgb)));
        row->addSpacing(8);
        row->addWidget(m_readerPage);
        row->addStretch(1);
        row->addWidget(exportButton(page));
        auto *presentButton = new QPushButton(glyph(Glyph::Play, Qt::white), tr("Present"));
        presentButton->setObjectName("present");
        presentButton->setIconSize(QSize(14, 14));
        presentButton->setToolTip(tr("Present from this page (F5)"));
        presentButton->setFocusPolicy(Qt::NoFocus);
        connect(presentButton, &QPushButton::clicked, this, &MainWindow::present);
        row->addWidget(presentButton);
        layout->addWidget(scrollingBar(bar));

        m_pdf = new QPdfView;
        m_pdf->setPageMode(QPdfView::PageMode::MultiPage);
        m_pdf->setZoomMode(QPdfView::ZoomMode::FitToWidth);
        m_pdf->setPageSpacing(12);
        QScroller::grabGesture(m_pdf->viewport(), QScroller::LeftMouseButtonGesture);   // drag to scroll: finger, pen or mouse
        connect(m_pdf->pageNavigator(), &QPdfPageNavigator::currentPageChanged, this, &MainWindow::refresh);
        layout->addWidget(m_pdf, 1);

        for (const auto &key : {QKeySequence(Qt::Key_F5), QKeySequence(Qt::CTRL | Qt::Key_Return)}) {
            auto *s = new QShortcut(key, page, this, &MainWindow::present);
            s->setContext(Qt::WidgetWithChildrenShortcut);
        }
        return page;
    }

    /// The deck was opened or recompiled: show the new document where the reader was.
    void documentReloaded() {
        const int scroll = m_pdf->verticalScrollBar()->value();
        const bool same = m_pdfPath == m_deck->path() && m_deck->isOpen();
        m_pdf->setDocument(m_deck->doc());
        m_pdfPath = m_deck->path();
        if (same) QTimer::singleShot(0, m_pdf, [this, scroll] { m_pdf->verticalScrollBar()->setValue(scroll); });
        m_notesAction->setVisible(m_deck->hasNotes());
        refresh();
    }

    // MARK: Presenter

    QWidget *buildPresenter() {
        auto *page = new QWidget;
        auto *layout = new QVBoxLayout(page);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(0);
        auto *bar = new QWidget;
        bar->setObjectName("bar");
        bar->setStyleSheet(barStyle());
        auto *row = new QHBoxLayout(bar);
        row->setContentsMargins(10, 4, 10, 4);
        row->setSpacing(4);
        const auto button = [&](Glyph g, const QColor &color, const QString &name, auto fn) {
            QAction *a = new QAction(glyph(g, color), name, page);
            connect(a, &QAction::triggered, this, fn);
            page->addAction(a);   // its shortcut works wherever the focus is
            row->addWidget(barButton(a));
            return a;
        };

        button(Glyph::Close, accent, tr("Stop Presenting"), [this] { stopPresenting(); })->setToolTip(tr("Stop presenting (Esc)"));
        QAction *grid = button(Glyph::Grid, accent, tr("Slides"), [this] { SlideGrid(m_deck, this).exec(); });
        grid->setShortcut(QKeySequence("G"));
        grid->setToolTip(tr("All slides (G)"));
        m_pageLabel = new QLabel;
        QFont big = m_pageLabel->font();
        big.setPixelSize(20);
        m_pageLabel->setFont(big);
        m_pageLabel->setContentsMargins(4, 0, 4, 0);
        row->addWidget(m_pageLabel);
        m_pauseAction = button(Glyph::Pause, accent, tr("Pause Timer"), [this] { toggleTimer(); });
        m_timerButton = new QToolButton;
        m_timerButton->setToolTip(tr("Talk timer"));
        m_timerButton->setPopupMode(QToolButton::InstantPopup);
        m_timerButton->setFocusPolicy(Qt::NoFocus);
        QFont mid = m_timerButton->font();
        mid.setPixelSize(17);
        m_timerButton->setFont(mid);
        auto *timerMenu = new QMenu(m_timerButton);
        timerMenu->addAction(m_pauseAction);
        timerMenu->addAction(tr("Reset Timer"), this, [this] {
            m_banked = 0;
            if (m_startedAt >= 0) m_startedAt = QDateTime::currentMSecsSinceEpoch();
            updateTimer();
        });
        m_timerButton->setMenu(timerMenu);
        row->addWidget(m_timerButton);
        row->addStretch(1);

        // Tools: icons in a rounded group; the selected one is tinted. Tap the selected pen or highlighter
        // again for its line widths, as on the iPad.
        auto *tools = new QWidget;
        tools->setObjectName("tools");
        tools->setAttribute(Qt::WA_StyledBackground);
        auto *toolRow = new QHBoxLayout(tools);
        toolRow->setContentsMargins(3, 3, 3, 3);
        toolRow->setSpacing(2);
        m_toolGroup = new QActionGroup(this);
        const struct { Tool tool; Glyph glyph; const char *name; const char *key; } toolList[] = {
            {Tool::Pointer, Glyph::Pointer, QT_TR_NOOP("Pointer"), "P"}, {Tool::Laser, Glyph::Laser, QT_TR_NOOP("Laser"), "L"},
            {Tool::Pen, Glyph::Pen, QT_TR_NOOP("Pen"), "D"}, {Tool::Highlighter, Glyph::Highlighter, QT_TR_NOOP("Highlighter"), "H"},
            {Tool::Eraser, Glyph::Eraser, QT_TR_NOOP("Eraser"), "E"}, {Tool::Lasso, Glyph::Lasso, QT_TR_NOOP("Lasso"), "S"}};
        for (const auto &t : toolList) {
            QAction *a = new QAction(glyph(t.glyph, primary()), tr(t.name), page);
            a->setCheckable(true);
            a->setChecked(m_tools.tool == t.tool);
            a->setShortcut(QKeySequence(t.key));
            a->setToolTip(QStringLiteral("%1 (%2)").arg(tr(t.name), t.key));
            a->setData(int(t.tool));
            m_toolGroup->addAction(a);
            page->addAction(a);
            QToolButton *b = barButton(a);
            toolRow->addWidget(b);
            m_toolButtons[t.tool] = b;
        }
        connect(m_toolGroup, &QActionGroup::triggered, this, [this](QAction *a) {
            const Tool tool = Tool(a->data().toInt());
            if (tool == m_shownTool && tool != Tool::Pointer && tool != Tool::Lasso) showOptions(tool);
            setTool(tool);
        });
        row->addWidget(tools);

        auto *swatchRow = new QHBoxLayout;   // swatch cells sit edge to edge, as on the iPad
        swatchRow->setSpacing(0);
        for (int i = 0; i < 4; ++i) {
            QAction *a = new QAction(page);
            connect(a, &QAction::triggered, this, [this, i] {
                if (m_tools.tool == Tool::Highlighter) m_tools.highlighterColor = highlighterSwatches[i];
                else { m_tools.penColor = penSwatches[i]; setTool(Tool::Pen); }
                saveSettings();
            });
            QToolButton *b = barButton(a);
            b->setObjectName("swatch");
            b->setIconSize(QSize(28, 28));
            swatchRow->addWidget(b);
            m_swatches << a;
        }
        row->addLayout(swatchRow);

        m_undoAction = button(Glyph::Undo, accent, tr("Undo"), [this] { m_deck->undo(); });
        m_undoAction->setShortcut(QKeySequence::Undo);
        m_redoAction = button(Glyph::Redo, accent, tr("Redo"), [this] { m_deck->redo(); });
        m_redoAction->setShortcuts({QKeySequence::Redo, QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Z)});
        button(Glyph::Trash, destructive, tr("Clear Slide"), [this] { m_deck->clearSlide(); })
            ->setToolTip(tr("Remove the ink on this slide"));
        m_wholeSlide = button(Glyph::WholeSlide, accent, tr("Whole Slide"), [this] { m_deck->resetZoom(); });
        m_wholeSlide->setShortcuts({QKeySequence(Qt::CTRL | Qt::Key_0)});
        m_wholeSlide->setToolTip(tr("Zoom out to the whole slide (Ctrl+0)"));
        QAction *zoomIn = new QAction(tr("Zoom In"), page);
        zoomIn->setShortcuts({QKeySequence::ZoomIn, QKeySequence(Qt::CTRL | Qt::Key_Equal)});
        connect(zoomIn, &QAction::triggered, this, [this] { zoomStep(1.25); });
        QAction *zoomOut = new QAction(tr("Zoom Out"), page);
        zoomOut->setShortcuts({QKeySequence::ZoomOut, QKeySequence(Qt::CTRL | Qt::Key_Minus)});
        connect(zoomOut, &QAction::triggered, this, [this] { zoomStep(0.8); });
        page->addActions({zoomIn, zoomOut});
        // Lasso selection: Delete, and the clipboard keys.
        const struct { QKeySequence key; void (Deck::*fn)(); } editKeys[] = {
            {QKeySequence::Delete, &Deck::deleteSelection}, {QKeySequence::Cut, &Deck::cutSelection},
            {QKeySequence::Copy, &Deck::copySelection}, {QKeySequence(Qt::CTRL | Qt::Key_D), &Deck::duplicateSelection}};
        for (const auto &k : editKeys) {
            QAction *a = new QAction(page);
            a->setShortcut(k.key);
            connect(a, &QAction::triggered, m_deck, k.fn);
            page->addAction(a);
        }
        QAction *pasteAction = new QAction(page);
        pasteAction->setShortcut(QKeySequence::Paste);
        connect(pasteAction, &QAction::triggered, this, [this] {
            const QRectF full(QPointF(0, 0), m_deck->slideBox(m_deck->page()).size());
            m_deck->paste(m_deck->zoom().value_or(full).center());
        });
        page->addAction(pasteAction);
        row->addStretch(1);
        row->addWidget(exportButton(page));

        m_projectorButton = new QToolButton;
        m_projectorButton->setIconSize(QSize(barIcon, barIcon));
        m_projectorButton->setToolTip(tr("Projector"));
        m_projectorButton->setPopupMode(QToolButton::InstantPopup);
        m_projectorButton->setFocusPolicy(Qt::NoFocus);
        auto *projectorMenu = new QMenu(m_projectorButton);
        projectorMenu->addAction(tr("Show Projector Window"), this, &MainWindow::openProjector);
        projectorMenu->addSeparator();
        auto overrideAction = [&](const QString &name, const char *key, Deck::Projector mode) {
            QAction *a = projectorMenu->addAction(name, this, [this, mode] {
                m_deck->setProjector(m_deck->projector() == mode ? Deck::Projector::Slides : mode);
            });
            a->setShortcut(QKeySequence(key));
            page->addAction(a);   // the shortcut works with the menu closed
        };
        overrideAction(tr("Black Screen"), "B", Deck::Projector::Black);
        overrideAction(tr("White Screen"), "W", Deck::Projector::White);
        QAction *freeze = projectorMenu->addAction(tr("Freeze Projector"), this, &MainWindow::toggleFreeze);
        freeze->setShortcut(QKeySequence("F"));
        page->addAction(freeze);
        projectorMenu->addAction(tr("Show Slides"), this, [this] { m_deck->setProjector(Deck::Projector::Slides); });
        projectorMenu->addSeparator();
        m_followZoomAction = projectorMenu->addAction(tr("Projector Follows Zoom"));
        m_followZoomAction->setCheckable(true);
        m_followZoomAction->setChecked(m_deck->projectorFollowsZoom());
        connect(m_followZoomAction, &QAction::toggled, m_deck, &Deck::setProjectorFollowsZoom);
        m_notesAction = projectorMenu->addAction(tr("Speaker Notes"));
        m_notesAction->setCheckable(true);
        m_notesAction->setChecked(true);
        m_notesAction->setShortcut(QKeySequence("N"));
        page->addAction(m_notesAction);
        connect(m_notesAction, &QAction::toggled, this, &MainWindow::refresh);
        connect(projectorMenu, &QMenu::aboutToHide, this, &MainWindow::refresh);
        m_projectorButton->setMenu(projectorMenu);
        row->addWidget(m_projectorButton);

        button(Glyph::Back, accent, tr("Back"), [this] { m_deck->go(m_deck->page() - 1); });
        button(Glyph::Forward, accent, tr("Forward"), [this] { m_deck->go(m_deck->page() + 1); });
        layout->addWidget(scrollingBar(bar));

        m_banner = new QPushButton;
        m_banner->setStyleSheet("background: #f08c00; color: white; font-weight: bold; border: none; padding: 6px");
        m_banner->setCursor(Qt::PointingHandCursor);
        connect(m_banner, &QPushButton::clicked, this, [this] { m_deck->setProjector(Deck::Projector::Slides); });
        layout->addWidget(m_banner);

        auto *split = new QSplitter;
        split->setStyleSheet("background: black");
        m_slide = new SlideView(m_deck, SlideView::Role::Presenter, &m_tools);
        m_notes = new SlideView(m_deck, SlideView::Role::Notes);
        split->addWidget(m_slide);
        split->addWidget(m_notes);
        split->setStretchFactor(0, 3);
        split->setStretchFactor(1, 2);
        layout->addWidget(split, 1);

        // Bluetooth clickers send arrows, Page Up/Down or space.
        const auto shortcut = [&](QKeySequence key, auto fn) {
            auto *s = new QShortcut(key, page, this, fn);
            s->setContext(Qt::WidgetWithChildrenShortcut);
        };
        for (int key : {Qt::Key_Right, Qt::Key_Down, Qt::Key_Space, Qt::Key_PageDown})
            shortcut(QKeySequence(key), [this] { m_deck->go(m_deck->page() + 1); });
        for (int key : {Qt::Key_Left, Qt::Key_Up, Qt::Key_PageUp, Qt::Key_Backspace})
            shortcut(QKeySequence(key), [this] { m_deck->go(m_deck->page() - 1); });
        shortcut(QKeySequence(Qt::Key_Home), [this] { m_deck->go(0); });
        shortcut(QKeySequence(Qt::Key_End), [this] { m_deck->go(m_deck->pageCount() - 1); });
        shortcut(QKeySequence(Qt::Key_Escape), [this] { stopPresenting(); });
        updateSwatches();
        return page;
    }

    /// Tap the selected tool again for its settings, as on the iPad.
    void showOptions(Tool tool) {
        auto *options = new ToolOptions(tool, &m_tools, m_deck, [this] {
            m_slide->updateCursor();
            saveSettings();
            m_slide->update();
        }, this);
        options->setAttribute(Qt::WA_DeleteOnClose);
        QToolButton *b = m_toolButtons[tool];
        options->move(b->mapToGlobal(QPoint(0, b->height())));
        options->show();
    }

    void setTool(Tool tool) {
        m_tools.tool = tool;
        m_shownTool = tool;
        for (QAction *a : m_toolGroup->actions()) a->setChecked(Tool(a->data().toInt()) == tool);
        m_slide->updateCursor();
        updateSwatches();
        saveSettings();
    }

    void updateSwatches() {
        const bool hl = m_tools.tool == Tool::Highlighter;
        for (int i = 0; i < m_swatches.size(); ++i) {
            const QColor c = hl ? highlighterSwatches[i] : penSwatches[i];
            m_swatches[i]->setIcon(swatchIcon(c, c == (hl ? m_tools.highlighterColor : m_tools.penColor)));
            m_swatches[i]->setToolTip(hl ? tr("Highlighter colour") : tr("Pen colour"));
        }
    }

    void zoomStep(double factor) {
        if (!m_deck->presenting()) return;
        const QRectF full(QPointF(0, 0), m_deck->slideBox(m_deck->page()).size());
        m_deck->zoomBy(factor, m_deck->zoom().value_or(full).center());
    }

    /// Share icon: the deck with your ink as a PDF, one page per slide (notes) or every overlay step.
    QToolButton *exportButton(QWidget *page) {
        auto *b = new QToolButton;
        b->setIcon(glyph(Glyph::Share, accent));
        b->setIconSize(QSize(barIcon, barIcon));
        b->setToolTip(tr("Export with ink"));
        b->setPopupMode(QToolButton::InstantPopup);
        b->setFocusPolicy(Qt::NoFocus);
        auto *menu = new QMenu(b);
        menu->addAction(tr("Export Notes…"), this, [this] { exportPdf(true); })
            ->setToolTip(tr("One page per slide, fully uncovered, with your ink"));
        menu->addAction(tr("Export Every Page…"), this, [this] { exportPdf(false); });
        menu->setToolTipsVisible(true);
        b->setMenu(menu);
        Q_UNUSED(page);
        return b;
    }

    void exportPdf(bool notesOnly) {
        const Pdf::Document *raw = m_deck->raw();
        if (!raw) return;
        const QString suggested = QFileInfo(m_deck->path()).dir().filePath(
            m_deck->name() + (notesOnly ? tr(" (notes).pdf") : tr(" (annotated).pdf")));
        const QString path = QFileDialog::getSaveFileName(this, notesOnly ? tr("Export Notes") : tr("Export Every Page"),
                                                          suggested, tr("PDF (*.pdf)"));
        if (path.isEmpty()) return;
        QVector<Media::ExportPage> pages;
        QVector<int> indices = m_deck->lastSteps();
        if (!notesOnly) { indices.clear(); for (int i = 0; i < m_deck->pageCount(); ++i) indices << i; }
        for (int i : indices) pages << Media::ExportPage{i, m_deck->slideBox(i), m_deck->inkOf(i)};
        QGuiApplication::setOverrideCursor(Qt::WaitCursor);
        const QByteArray pdf = Media::exportPdf(*raw, pages);
        QSaveFile f(path);
        const bool ok = f.open(QIODevice::WriteOnly) && f.write(pdf) == pdf.size() && f.commit();
        QGuiApplication::restoreOverrideCursor();
        if (!ok) QMessageBox::warning(this, tr("Export failed"), tr("Could not write %1:\n%2").arg(path, f.errorString()));
    }

    void stopPresenting() {
        m_deck->setPresenting(false);
        m_clock.stop();
        m_pdf->pageNavigator()->jump(m_deck->page(), QPointF());
        m_pdf->setFocus();
    }

    void openProjector() {
        if (!m_projector) m_projector = new ProjectorWindow(m_deck);
        if (QScreen *other = otherScreen()) {
            m_projector->setScreen(other);
            m_projector->setGeometry(other->geometry());
            m_projector->showFullScreen();
        } else {
            m_projector->showNormal();   // one screen: a window to drag to the projector, double-click for full screen
        }
        m_projector->raise();
        activateWindow();
        refresh();   // keyboard and clicker stay with the presenter
    }

    void toggleFreeze() {
        if (m_deck->projector() == Deck::Projector::Frozen) m_deck->setProjector(Deck::Projector::Slides);
        else if (m_projector && m_projector->isVisible())
            m_deck->setProjector(Deck::Projector::Frozen, m_projector->view->grab().toImage());
    }

    void toggleTimer() {
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (m_startedAt >= 0) { m_banked += now - m_startedAt; m_startedAt = -1; }
        else m_startedAt = now;
        updateTimer();
    }

    void updateTimer() {
        const qint64 ms = m_banked + (m_startedAt >= 0 ? QDateTime::currentMSecsSinceEpoch() - m_startedAt : 0);
        const qint64 t = ms / 1000;
        m_timerButton->setText(t >= 3600 ? QString::asprintf("%lld:%02lld:%02lld", t / 3600, t / 60 % 60, t % 60)
                                         : QString::asprintf("%lld:%02lld", t / 60, t % 60));
        const bool running = m_startedAt >= 0;
        m_timerButton->setStyleSheet(QStringLiteral("color: %1").arg(running ? primary().name() : orange.name()));
        m_pauseAction->setIcon(glyph(running ? Glyph::Pause : Glyph::Play, accent));
        m_pauseAction->setText(running ? tr("Pause Timer") : tr("Resume Timer"));
    }

    // MARK: State

    void refresh() {
        const bool open = m_deck->isOpen();
        m_stack->setCurrentIndex(!open ? 0 : m_deck->presenting() ? 2 : 1);
        setWindowTitle(open ? m_deck->name() + " — PenCast" : "PenCast");
        if (!open) return;
        const int n = m_deck->pageCount();
        m_readerTitle->setText(m_deck->name());
        m_readerPage->setText(QStringLiteral("%1 / %2").arg(m_pdf->pageNavigator()->currentPage() + 1).arg(n));
        m_pageLabel->setText(QStringLiteral("%1 / %2").arg(m_deck->page() + 1).arg(n));
        m_wholeSlide->setVisible(m_deck->zoom().has_value());
        m_followZoomAction->setChecked(m_deck->projectorFollowsZoom());
        m_undoAction->setEnabled(m_deck->canUndo());
        m_redoAction->setEnabled(m_deck->canRedo());
        m_notes->setVisible(m_deck->hasNotes() && m_notesAction->isChecked());
        const QString banner = m_deck->projector() == Deck::Projector::Black ? tr("Projector is black")
                             : m_deck->projector() == Deck::Projector::White ? tr("Projector is white")
                             : m_deck->projector() == Deck::Projector::Frozen ? tr("Projector is frozen") : QString();
        const bool shown = m_projector && m_projector->isVisible();
        m_projectorButton->setIcon(glyph(Glyph::Display, !banner.isEmpty() ? orange : shown ? green : secondary()));
        m_banner->setText(banner + tr(" · click to show slides"));
        m_banner->setVisible(!banner.isEmpty());
    }

    void loadSettings() {
        QSettings s;
        m_tools.tool = Tool(s.value("tool", int(Tool::Pen)).toInt());
        m_tools.penColor = s.value("penColor", m_tools.penColor).value<QColor>();
        m_tools.highlighterColor = s.value("highlighterColor", m_tools.highlighterColor).value<QColor>();
        m_tools.penWidth = s.value("penWidth", m_tools.penWidth).toDouble();
        m_tools.highlighterWidth = s.value("highlighterWidth", m_tools.highlighterWidth).toDouble();
        m_tools.partialEraser = s.value("partialEraser", m_tools.partialEraser).toBool();
        m_tools.eraserSize = s.value("eraserSize", m_tools.eraserSize).toDouble();
        m_tools.penStyle = s.value("penStyle", "fountain").toString() == "ball" ? ToolSettings::Ball : ToolSettings::Fountain;
        m_tools.fine = s.value("pressureFine", m_tools.fine).toDouble();
        m_tools.firm = s.value("pressureFirm", m_tools.firm).toDouble();
        LaserStyle &laser = m_deck->laserStyle;
        laser.trail = s.value("laserTrail", laser.trail).toBool();
        laser.dot = s.value("laserDotSize", laser.dot).toDouble();
        laser.width = s.value("laserTrailWidth", laser.width).toDouble();
    }

    void saveSettings() {
        QSettings s;
        s.setValue("tool", int(m_tools.tool));
        s.setValue("penColor", m_tools.penColor);
        s.setValue("highlighterColor", m_tools.highlighterColor);
        s.setValue("penWidth", m_tools.penWidth);
        s.setValue("highlighterWidth", m_tools.highlighterWidth);
        s.setValue("partialEraser", m_tools.partialEraser);
        s.setValue("eraserSize", m_tools.eraserSize);
        s.setValue("penStyle", m_tools.penStyle == ToolSettings::Ball ? "ball" : "fountain");
        s.setValue("pressureFine", m_tools.fine);
        s.setValue("pressureFirm", m_tools.firm);
        s.setValue("laserTrail", m_deck->laserStyle.trail);
        s.setValue("laserDotSize", m_deck->laserStyle.dot);
        s.setValue("laserTrailWidth", m_deck->laserStyle.width);
        updateSwatches();
    }

    Deck *m_deck;
    ToolSettings m_tools;
    QStackedWidget *m_stack;
    QListWidget *m_recent;
    QLabel *m_openError;
    QPdfView *m_pdf;
    QString m_pdfPath;
    QLabel *m_readerTitle, *m_readerPage, *m_pageLabel;
    QToolButton *m_timerButton, *m_projectorButton;
    QAction *m_pauseAction;
    QActionGroup *m_toolGroup;
    QHash<Tool, QToolButton *> m_toolButtons;
    Tool m_shownTool = Tool::Pen;
    QList<QAction *> m_swatches;
    QAction *m_undoAction, *m_redoAction, *m_notesAction, *m_wholeSlide, *m_followZoomAction;
    QPushButton *m_banner;
    SlideView *m_slide, *m_notes;
    ProjectorWindow *m_projector = nullptr;
    QTimer m_clock;
    qint64 m_banked = 0, m_startedAt = -1;
};

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    QApplication::setApplicationName("PenCast");
    QApplication::setApplicationVersion(PENCAST_VERSION);
    QGuiApplication::setDesktopFileName("pencast");   // Wayland: the window gets the launcher's icon
    QApplication::setWindowIcon(QIcon::fromTheme("pencast"));

    QCommandLineParser args;
    args.setApplicationDescription("Present LaTeX Beamer PDFs with a laser pointer and pen.");
    args.addHelpOption();
    args.addVersionOption();
    args.addPositionalArgument("file", "PDF to open.");
    const QCommandLineOption presentOption("present", "Start presenting right away.");
    args.addOption(presentOption);
    args.process(app);

    Deck deck;
    MainWindow window(&deck);
    window.show();
    if (!args.positionalArguments().isEmpty()) {
        window.openPath(args.positionalArguments().first());
        if (args.isSet(presentOption)) window.present();
    }
    return app.exec();
}
