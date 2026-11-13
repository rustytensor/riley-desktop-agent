// SPDX-License-Identifier: GPL-3.0-or-later
//
// riley-test-app: the window the desktop gate (tests/run-desktop.sh) drives. Nothing else may act on it, and it
// acts on nothing: it only shows fixed controls and reports what it received.
//
//   riley-test-app --report FILE            the main test window (class org.riley.testapp, 480x360 logical px)
//   riley-test-app --report FILE --decoy    the small window the gate moves focus to (class org.riley.testdecoy,
//                                           240x120 logical px)
//
// The layout is fixed, in logical pixels inside the window, so the gate can work out every expected value by hand
// (tests/desktop/gate.py, LAYOUT):
//
//   colour block   x 20  y 20   80 x 60    solid rgb(18,164,92), no border
//   background     everywhere else not covered by a control: solid rgb(240,240,240)
//   button         x 20  y 100  140 x 40   text and accessible name "Count click"; counts clicks
//   click label    x 180 y 100  140 x 40   "Clicks: N"
//   text field     x 20  y 160  300 x 32   QLineEdit, accessible name "Plain field"
//   password field x 20  y 210  300 x 32   QLineEdit in Password mode, accessible name "Secret field"
//   list           x 340 y 100  120 x 140  QListWidget, accessible name "Fruit list": apple, banana, cherry
//
// The decoy has one QLineEdit (accessible name "Decoy field") filling it.
//
// Report: one JSON object per line, appended and flushed at once, times in ms since the epoch (UTC wall clock, the
// same clock the gate reads):
//   {"ms":T,"event":"ready","window":"main"|"decoy","pid":P}
//   {"ms":T,"event":"click","count":N}
//   {"ms":T,"event":"key","widget":"plain"|"secret"|"list"|"button"|"decoy"|"window","text":"a"}
//   {"ms":T,"event":"field","widget":"plain"|"decoy","value":"..."}       (every change of the text field)
//   {"ms":T,"event":"field","widget":"secret","length":N}                (the password field: length only)
// Key events at the password field carry no text, only "length": the length of the event's text.
//
// Start it with QT_LINUX_ACCESSIBILITY_ALWAYS_ON=1 so it is on the AT-SPI bus, and with
// QT_WAYLAND_DISABLE_WINDOWDECORATION=1 so the window is exactly its fixed size.

#include <QApplication>
#include <QDateTime>
#include <QFile>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPalette>
#include <QPushButton>
#include <QWidget>

#include <cstdio>
#include <unistd.h>

namespace {

QFile *reportFile = nullptr;

void report(QJsonObject object)
{
    object.insert(QStringLiteral("ms"), QDateTime::currentMSecsSinceEpoch());
    if (!reportFile)
        return;
    reportFile->write(QJsonDocument(object).toJson(QJsonDocument::Compact));
    reportFile->write("\n");
    reportFile->flush();
}

QWidget *solid(QWidget *parent, const QColor &colour, const QRect &rect)
{
    auto *w = new QWidget(parent);
    QPalette p = w->palette();
    p.setColor(QPalette::Window, colour);
    w->setPalette(p);
    w->setAutoFillBackground(true);
    w->setGeometry(rect);
    return w;
}

// Records every key press at the widget that has focus (once per event, not again as it propagates).
class KeyReporter : public QObject
{
public:
    explicit KeyReporter(QHash<QObject *, QString> names, QObject *secret)
        : m_names(std::move(names)), m_secret(secret)
    {
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (event->type() != QEvent::KeyPress)
            return false;
        QWidget *focus = QApplication::focusWidget();
        const bool atFocus = focus ? watched == focus : watched->isWidgetType()
                                         && static_cast<QWidget *>(watched)->isWindow();
        if (!atFocus)
            return false;
        auto *key = static_cast<QKeyEvent *>(event);
        QJsonObject o{{QStringLiteral("event"), QStringLiteral("key")},
                      {QStringLiteral("widget"), m_names.value(watched, QStringLiteral("window"))}};
        if (watched == m_secret)
            o.insert(QStringLiteral("length"), key->text().size());
        else
            o.insert(QStringLiteral("text"), key->text());
        report(o);
        return false;
    }

private:
    QHash<QObject *, QString> m_names;
    QObject *m_secret;
};

} // namespace

int main(int argc, char **argv)
{
    QString reportPath;
    bool decoy = false;
    for (int i = 1; i < argc; ++i) {
        const QByteArray arg(argv[i]);
        if (arg == "--report" && i + 1 < argc) {
            reportPath = QString::fromLocal8Bit(argv[++i]);
        } else if (arg == "--decoy") {
            decoy = true;
        } else {
            std::fprintf(stderr, "usage: riley-test-app --report FILE [--decoy]\n");
            return 2;
        }
    }
    if (reportPath.isEmpty()) {
        std::fprintf(stderr, "riley-test-app: --report FILE is required\n");
        return 2;
    }

    QApplication::setApplicationName(decoy ? QStringLiteral("riley-test-decoy") : QStringLiteral("riley-test-app"));
    QGuiApplication::setDesktopFileName(decoy ? QStringLiteral("org.riley.testdecoy")
                                              : QStringLiteral("org.riley.testapp"));
    QApplication app(argc, argv);
    app.setStyle(QStringLiteral("Fusion"));

    QFile file(reportPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append)) {
        std::fprintf(stderr, "riley-test-app: cannot open the report file\n");
        return 1;
    }
    reportFile = &file;

    QWidget window;
    {
        QPalette p = window.palette();
        p.setColor(QPalette::Window, QColor(240, 240, 240));
        window.setPalette(p);
        window.setAutoFillBackground(true);
    }
    QHash<QObject *, QString> names;
    QObject *secretObject = nullptr;

    if (decoy) {
        window.setWindowTitle(QStringLiteral("Riley test decoy"));
        window.setFixedSize(240, 120);
        auto *field = new QLineEdit(&window);
        field->setAccessibleName(QStringLiteral("Decoy field"));
        field->setGeometry(10, 10, 220, 100);
        names.insert(field, QStringLiteral("decoy"));
        QObject::connect(field, &QLineEdit::textChanged, [](const QString &value) {
            report({{QStringLiteral("event"), QStringLiteral("field")}, {QStringLiteral("widget"), QStringLiteral("decoy")},
                    {QStringLiteral("value"), value}});
        });
        field->setFocus();
    } else {
        window.setWindowTitle(QStringLiteral("Riley test app"));
        window.setFixedSize(480, 360);
        solid(&window, QColor(18, 164, 92), QRect(20, 20, 80, 60));

        auto *button = new QPushButton(QStringLiteral("Count click"), &window);
        button->setAccessibleName(QStringLiteral("Count click"));
        button->setGeometry(20, 100, 140, 40);
        auto *label = new QLabel(QStringLiteral("Clicks: 0"), &window);
        label->setGeometry(180, 100, 140, 40);
        int clicks = 0;
        QObject::connect(button, &QPushButton::clicked, [&clicks, label] {
            ++clicks;
            label->setText(QStringLiteral("Clicks: %1").arg(clicks));
            report({{QStringLiteral("event"), QStringLiteral("click")}, {QStringLiteral("count"), clicks}});
        });

        auto *plain = new QLineEdit(&window);
        plain->setAccessibleName(QStringLiteral("Plain field"));
        plain->setGeometry(20, 160, 300, 32);
        QObject::connect(plain, &QLineEdit::textChanged, [](const QString &value) {
            report({{QStringLiteral("event"), QStringLiteral("field")}, {QStringLiteral("widget"), QStringLiteral("plain")},
                    {QStringLiteral("value"), value}});
        });

        auto *secret = new QLineEdit(&window);
        secret->setEchoMode(QLineEdit::Password);
        secret->setAccessibleName(QStringLiteral("Secret field"));
        secret->setGeometry(20, 210, 300, 32);
        QObject::connect(secret, &QLineEdit::textChanged, [](const QString &value) {
            report({{QStringLiteral("event"), QStringLiteral("field")}, {QStringLiteral("widget"), QStringLiteral("secret")},
                    {QStringLiteral("length"), value.size()}});
        });
        secretObject = secret;

        auto *list = new QListWidget(&window);
        list->setAccessibleName(QStringLiteral("Fruit list"));
        list->setGeometry(340, 100, 120, 140);
        list->addItems({QStringLiteral("apple"), QStringLiteral("banana"), QStringLiteral("cherry")});

        names.insert(plain, QStringLiteral("plain"));
        names.insert(secret, QStringLiteral("secret"));
        names.insert(list, QStringLiteral("list"));
        names.insert(button, QStringLiteral("button"));
    }

    KeyReporter reporter(names, secretObject);
    app.installEventFilter(&reporter);

    window.show();
    report({{QStringLiteral("event"), QStringLiteral("ready")},
            {QStringLiteral("window"), decoy ? QStringLiteral("decoy") : QStringLiteral("main")},
            {QStringLiteral("pid"), qint64(getpid())}});
    return app.exec();
}
