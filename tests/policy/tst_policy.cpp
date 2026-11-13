// SPDX-License-Identifier: GPL-3.0-or-later
//
// M1 gate driver: feeds every case of tests/policy/cases.xml to the permission engine and compares the
// decision with the expected one worked out by hand. Also checks that the engine's own config loader
// (libxml2 + the compiled-in schema) accepts the example and the fixtures and rejects every broken copy.
//
// Engine interface this driver is written against (src/core):
//
//   config.h
//     namespace riley {
//     enum class ConfigStatus { Ok, Missing, Invalid };
//     struct ConfigLoad {                 // copyable
//         ConfigStatus status;
//         QString error;                  // human-readable, empty when Ok
//         <optional or shared pointer> config;   // set when Ok; the driver only uses `*load.config`
//     };
//     ConfigLoad loadConfigFile(const QString &path);   // Missing if the file does not exist
//     bool isRemoteClient(const Config &config, const QString &clientKind);   // rule L of cases.xml
//     }
//
//   policy.h
//     namespace riley {
//     struct WindowInfo { QString windowClass, title, monitor, agent; bool focused = false; };
//     struct Request {
//         QString client;              // client kind as identified by the service
//         QString action;
//         WindowInfo window;
//         bool passwordField = false;
//         bool passwordUnknown = false;   // AT-SPI could not tell whether the focused control is a password
//                                         // field (spec change, lead decision: refused with password-unknown)
//         bool focusMoved = false;
//         QString text;                // type: the text; key: the key name
//     };
//     The action "resume" is decided by decide() too (rule R0 of cases.xml); only client and action matter.
//     struct State { bool stopped = false; };
//     enum class Verdict { Allow, Deny };
//     struct Decision { Verdict verdict = Verdict::Deny; QString reason; QString rule; };
//     Decision decide(const Request &request, const ConfigLoad &config, const State &state);   // pure
//     }

#include <QtTest>
#include <QDir>
#include <QFile>
#include <QXmlStreamReader>

#include "config.h"
#include "policy.h"

#ifndef RILEY_SOURCE_DIR
#error "RILEY_SOURCE_DIR must be defined by the build"
#endif

namespace {

const QString kSourceDir = QStringLiteral(RILEY_SOURCE_DIR);
const QString kPolicyDir = kSourceDir + QStringLiteral("/tests/policy");

struct Case {
    QString id, config, client, location, action;
    QString windowClass, title, monitor, agent, text;
    bool focused = false, password = false, passwordUnknown = false, stopped = false, focusMoved = false;
    QString expect, reason, rule, why;
};

bool boolAttr(const QXmlStreamAttributes &a, const char *name, QString *error)
{
    const QString v = a.value(QLatin1String(name)).toString();
    if (v.isEmpty() || v == QLatin1String("false"))
        return false;
    if (v == QLatin1String("true"))
        return true;
    *error = QStringLiteral("bad boolean %1=\"%2\"").arg(QLatin1String(name), v);
    return false;
}

// Sets Request::passwordUnknown when the engine has it. Until it does, the cases that need it fail on their own
// and every other case still runs.
template <typename R>
bool setPasswordUnknown(R &request, bool unknown)
{
    if constexpr (requires { request.passwordUnknown = unknown; }) {
        request.passwordUnknown = unknown;
        return true;
    } else {
        return !unknown;
    }
}

QString configPath(const QString &name)
{
    if (name == QLatin1String("missing"))
        return kPolicyDir + QStringLiteral("/fixtures/does-not-exist.xml");
    return kPolicyDir + QStringLiteral("/fixtures/") + name + QStringLiteral(".xml");
}

} // namespace

class tst_Policy : public QObject
{
    Q_OBJECT

    QStringList m_reasons;
    QList<Case> m_cases;
    QMap<QString, riley::ConfigLoad> m_loads;

private slots:
    void initTestCase()
    {
        QFile f(kPolicyDir + QStringLiteral("/cases.xml"));
        QVERIFY2(f.open(QIODevice::ReadOnly), "cannot open tests/policy/cases.xml");
        QXmlStreamReader xml(&f);
        QString error;
        while (!xml.atEnd() && error.isEmpty()) {
            xml.readNext();
            if (!xml.isStartElement())
                continue;
            const auto a = xml.attributes();
            if (xml.name() == QLatin1String("reason")) {
                m_reasons << a.value(QLatin1String("code")).toString();
            } else if (xml.name() == QLatin1String("case")) {
                Case c;
                c.id = a.value(QLatin1String("id")).toString();
                c.config = a.value(QLatin1String("config")).toString();
                c.client = a.value(QLatin1String("client")).toString();
                c.location = a.value(QLatin1String("location")).toString();
                c.action = a.value(QLatin1String("action")).toString();
                c.windowClass = a.value(QLatin1String("class")).toString();
                c.title = a.value(QLatin1String("title")).toString();
                c.monitor = a.hasAttribute(QLatin1String("monitor")) ? a.value(QLatin1String("monitor")).toString()
                                                                     : QStringLiteral("DP-2");
                c.agent = a.value(QLatin1String("agent")).toString();
                c.text = a.value(QLatin1String("text")).toString();
                c.focused = boolAttr(a, "focused", &error);
                if (a.value(QLatin1String("password")) == QLatin1String("unknown"))
                    c.passwordUnknown = true;
                else
                    c.password = boolAttr(a, "password", &error);
                c.stopped = boolAttr(a, "stopped", &error);
                c.focusMoved = boolAttr(a, "focus-moved", &error);
                c.expect = a.value(QLatin1String("expect")).toString();
                c.reason = a.value(QLatin1String("reason")).toString();
                c.rule = a.value(QLatin1String("rule")).toString();
                // Children: <text><part/>...</text> and <why/>.
                while (xml.readNextStartElement()) {
                    if (xml.name() == QLatin1String("text")) {
                        while (xml.readNextStartElement()) {
                            if (xml.name() == QLatin1String("part"))
                                c.text += xml.readElementText();
                            else
                                xml.skipCurrentElement();
                        }
                    } else if (xml.name() == QLatin1String("why")) {
                        c.why = xml.readElementText(QXmlStreamReader::IncludeChildElements).simplified();
                    } else {
                        xml.skipCurrentElement();
                    }
                }
                if (c.id.isEmpty() || c.config.isEmpty() || c.action.isEmpty())
                    error = QStringLiteral("case %1: id, config and action are required").arg(c.id);
                else if (c.expect != QLatin1String("allow") && c.expect != QLatin1String("deny"))
                    error = QStringLiteral("case %1: expect must be allow or deny").arg(c.id);
                else if (!m_reasons.contains(c.reason))
                    error = QStringLiteral("case %1: reason %2 is not declared").arg(c.id, c.reason);
                else if ((c.expect == QLatin1String("allow")) != (c.reason == QLatin1String("allowed")))
                    error = QStringLiteral("case %1: allow goes with reason allowed and only with it").arg(c.id);
                else if (c.location != QLatin1String("local") && c.location != QLatin1String("remote")
                         && c.location != QLatin1String("n/a"))
                    error = QStringLiteral("case %1: location must be local, remote or n/a").arg(c.id);
                m_cases << c;
            }
        }
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QVERIFY2(!xml.hasError(), qPrintable(xml.errorString()));
        QVERIFY2(m_cases.size() >= 40, "the decision table needs at least 40 cases");

        QSet<QString> ids;
        for (const Case &c : std::as_const(m_cases)) {
            QVERIFY2(!ids.contains(c.id), qPrintable(QStringLiteral("duplicate case id ") + c.id));
            ids.insert(c.id);
            if (!m_loads.contains(c.config))
                m_loads.insert(c.config, riley::loadConfigFile(configPath(c.config)));
        }
    }

    // The engine's loader agrees with the schema: the example and fixtures load, broken copies do not.
    void schema_data()
    {
        QTest::addColumn<QString>("path");
        QTest::addColumn<int>("expected");
        const int ok = int(riley::ConfigStatus::Ok);
        const int invalid = int(riley::ConfigStatus::Invalid);
        QTest::newRow("examples/config.xml") << kSourceDir + QStringLiteral("/examples/config.xml") << ok;
        QTest::newRow("fixtures/main.xml") << configPath(QStringLiteral("main")) << ok;
        QTest::newRow("fixtures/alt.xml") << configPath(QStringLiteral("alt")) << ok;
        QTest::newRow("fixtures/invalid.xml") << configPath(QStringLiteral("invalid")) << invalid;
        QTest::newRow("fixtures/does-not-exist.xml") << configPath(QStringLiteral("missing"))
                                                     << int(riley::ConfigStatus::Missing);
        const QDir broken(kPolicyDir + QStringLiteral("/broken"));
        const QStringList files = broken.entryList({QStringLiteral("*.xml")}, QDir::Files, QDir::Name);
        QVERIFY2(files.size() >= 6, "at least 6 broken copies expected in tests/policy/broken");
        for (const QString &name : files)
            QTest::newRow(qPrintable(QStringLiteral("broken/") + name)) << broken.filePath(name) << invalid;
    }

    void schema()
    {
        QFETCH(QString, path);
        QFETCH(int, expected);
        const riley::ConfigLoad load = riley::loadConfigFile(path);
        QCOMPARE(int(load.status), expected);
        if (load.status == riley::ConfigStatus::Ok)
            QVERIFY(bool(load.config));
        else
            QVERIFY2(!load.error.isEmpty(), "a failed load must say why");
    }

    void decide_data()
    {
        QTest::addColumn<int>("index");
        for (int i = 0; i < m_cases.size(); ++i)
            QTest::newRow(qPrintable(m_cases.at(i).id)) << i;
    }

    void decide()
    {
        QFETCH(int, index);
        const Case &c = m_cases.at(index);
        const auto found = m_loads.constFind(c.config);
        QVERIFY(found != m_loads.constEnd());
        const riley::ConfigLoad &load = *found;

        riley::Request request;
        request.client = c.client;
        request.action = c.action;
        request.window.windowClass = c.windowClass;
        request.window.title = c.title;
        request.window.monitor = c.monitor;
        request.window.agent = c.agent;
        request.window.focused = c.focused;
        request.passwordField = c.password;
        QVERIFY2(setPasswordUnknown(request, c.passwordUnknown),
                 "the engine's Request has no passwordUnknown field (spec change: password field unknown)");
        request.focusMoved = c.focusMoved;
        request.text = c.text;
        riley::State state;
        state.stopped = c.stopped;

        const riley::Decision d = riley::decide(request, load, state);
        const QString actual = (d.verdict == riley::Verdict::Allow ? QStringLiteral("allow/") : QStringLiteral("deny/"))
                               + d.reason;
        const QString expected = c.expect + QLatin1Char('/') + c.reason;
        if (actual != expected)
            qInfo("%s why: %s", qPrintable(c.id), qPrintable(c.why));
        QCOMPARE(actual, expected);
        if (!c.rule.isEmpty())
            QCOMPARE(d.rule, c.rule);

        if (c.location != QLatin1String("n/a")) {
            QVERIFY2(load.status == riley::ConfigStatus::Ok, "a case with a location needs a valid config");
            const QString loc = riley::isRemoteClient(*load.config, c.client) ? QStringLiteral("remote")
                                                                              : QStringLiteral("local");
            QCOMPARE(loc, c.location);
        }
    }
};

QTEST_GUILESS_MAIN(tst_Policy)
#include "tst_policy.moc"
