// SPDX-License-Identifier: GPL-3.0-or-later
#include "agentloop.h"

#include <QJsonDocument>
#include <QJsonValue>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

namespace riley {

namespace {

QJsonObject prop(const char *type, const char *description)
{
    return {{QStringLiteral("type"), QLatin1String(type)}, {QStringLiteral("description"), QLatin1String(description)}};
}

QJsonObject tool(const char *name, const char *description, const QJsonObject &properties,
                 const QStringList &required)
{
    return {{QStringLiteral("type"), QStringLiteral("function")},
            {QStringLiteral("function"),
             QJsonObject{{QStringLiteral("name"), QLatin1String(name)},
                         {QStringLiteral("description"), QLatin1String(description)},
                         {QStringLiteral("parameters"),
                          QJsonObject{{QStringLiteral("type"), QStringLiteral("object")},
                                      {QStringLiteral("properties"), properties},
                                      {QStringLiteral("required"), QJsonArray::fromStringList(required)}}}}}};
}

const char *kWindow = "the window's address (0x...) from windows";

QString oneLine(QString s, int max = 160)
{
    s.replace(QLatin1Char('\n'), QLatin1Char(' ')).replace(QLatin1Char('\r'), QLatin1Char(' '));
    if (s.size() > max)
        s = s.left(max) + QStringLiteral("...");
    return s;
}

bool intArg(const QJsonObject &args, const char *name, int *out)
{
    const QJsonValue v = args.value(QLatin1String(name));
    if (v.isDouble()) {
        const double d = v.toDouble();
        if (d < -100000 || d > 100000)
            return false;
        *out = int(d);
        return true;
    }
    if (v.isString()) {
        bool ok = false;
        *out = v.toString().trimmed().toInt(&ok);
        return ok;
    }
    return false;
}

} // namespace

AgentLoop::AgentLoop(LoopOptions options, QObject *parent) : QObject(parent), m_options(std::move(options))
{
    // Never follow a redirect: the key would go to wherever it points.
    m_network.setRedirectPolicy(QNetworkRequest::ManualRedirectPolicy);
    if (m_options.endpoint.location == Location::Local)
        m_network.setProxy(QNetworkProxy::NoProxy);   // a local endpoint is reached directly, never through a proxy
    m_deadline.setSingleShot(true);
    connect(&m_deadline, &QTimer::timeout, this, [this] {
        finish(Failed, QStringLiteral("failed time-limit: the task ran %1 s").arg(m_options.maxSeconds));
    });
}

AgentLoop::~AgentLoop()
{
    m_options.key.fill('\0');
    if (m_reply)
        m_reply->abort();
}

bool AgentLoop::isSafeWord(const QString &text)
{
    return text.trimmed().compare(QLatin1String("segfault"), Qt::CaseInsensitive) == 0;
}

void AgentLoop::start(const QString &task)
{
    if (m_running)
        return;
    m_running = true;
    m_stopping = false;
    m_steps = 0;
    m_queue.clear();
    m_pictures.clear();
    m_messages = QJsonArray();
    if (isSafeWord(task)) {
        emit step(QStringLiteral("the task is the safe word: stopping everything"));
        requestStop(QStringLiteral("segfault"));
        return;
    }
    if (task.trimmed().isEmpty()) {
        finish(Failed, QStringLiteral("failed no-task: give a task"));
        return;
    }
    m_messages.append(QJsonObject{{QStringLiteral("role"), QStringLiteral("system")},
                                  {QStringLiteral("content"), systemPrompt()}});
    m_messages.append(QJsonObject{{QStringLiteral("role"), QStringLiteral("user")},
                                  {QStringLiteral("content"), task}});
    m_deadline.start(m_options.maxSeconds * 1000);
    emit step(QStringLiteral("task: %1 (endpoint %2, model %3)")
                  .arg(oneLine(task), m_options.endpoint.name, m_options.endpoint.model));
    askModel();
}

void AgentLoop::requestStop(const QString &word)
{
    if (!m_running || m_stopping)
        return;
    m_stopping = true;
    if (m_reply) {
        QNetworkReply *r = m_reply;
        m_reply = nullptr;
        r->abort();
        r->deleteLater();
    }
    emit step(QStringLiteral("%1: sent to riley-agentd").arg(word));
    m_control = new ServiceCall(m_options.socketPath, QJsonObject{{QStringLiteral("action"), word}},
                                [this, word](const ServiceReply &reply) {
        if (m_call) {
            m_call->abandon();
            m_call->deleteLater();
        }
        if (reply.kind == QLatin1String("ok"))
            finish(Stopped, QStringLiteral("stopped by %1: %2").arg(word, reply.line));
        else
            finish(Stopped, QStringLiteral("stopped by %1, but the service did not confirm it: %2").arg(word, reply.line));
    }, this);
}

void AgentLoop::askModel()
{
    if (!m_running || m_stopping)
        return;
    if (m_steps >= m_options.maxSteps) {
        finish(Failed, QStringLiteral("failed step-limit: %1 model requests without an end").arg(m_steps));
        return;
    }
    ++m_steps;
    QString base = m_options.endpoint.baseUrl;
    while (base.endsWith(QLatin1Char('/')))
        base.chop(1);
    QNetworkRequest request(QUrl(base + QStringLiteral("/chat/completions")));
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setTransferTimeout(180000);
    if (!m_options.key.isEmpty())
        request.setRawHeader("Authorization", QByteArray("Bearer ") + m_options.key);
    const QJsonObject body{{QStringLiteral("model"), m_options.endpoint.model},
                           {QStringLiteral("messages"), m_messages},
                           {QStringLiteral("tools"), toolDefinitions()},
                           {QStringLiteral("tool_choice"), QStringLiteral("auto")},
                           {QStringLiteral("stream"), false}};
    emit step(QStringLiteral("model request %1").arg(m_steps));
    QNetworkReply *reply = m_network.post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));
    m_reply = reply;
    connect(reply, &QNetworkReply::finished, this, [this, reply] { onModelReply(reply); });
}

void AgentLoop::onModelReply(QNetworkReply *reply)
{
    reply->deleteLater();
    if (reply != m_reply || !m_running || m_stopping)
        return;
    m_reply = nullptr;
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QByteArray data = reply->readAll();
    if (reply->error() != QNetworkReply::NoError && status == 0) {
        finish(Failed, QStringLiteral("failed model: %1").arg(oneLine(reply->errorString())));
        return;
    }
    if (status != 200) {
        finish(Failed, QStringLiteral("failed model: HTTP %1 %2").arg(status).arg(oneLine(QString::fromUtf8(data), 200)));
        return;
    }
    const QJsonObject answer = QJsonDocument::fromJson(data).object();
    const QJsonObject message = answer.value(QStringLiteral("choices")).toArray().at(0).toObject()
                                    .value(QStringLiteral("message")).toObject();
    if (message.isEmpty()) {
        finish(Failed, QStringLiteral("failed model: a reply without a message"));
        return;
    }
    const QString text = message.value(QStringLiteral("content")).toString();
    const QJsonArray calls = message.value(QStringLiteral("tool_calls")).toArray();
    if (calls.isEmpty()) {
        finish(Done, QStringLiteral("done %1").arg(text.trimmed().isEmpty() ? QStringLiteral("(no text)")
                                                                           : oneLine(text.trimmed(), 2000)));
        return;
    }
    if (!text.trimmed().isEmpty())
        emit step(QStringLiteral("model: %1").arg(oneLine(text.trimmed(), 400)));

    QJsonArray echoed;
    m_queue.clear();
    for (int i = 0; i < calls.size(); ++i) {
        QJsonObject c = calls.at(i).toObject();
        Call call;
        call.id = c.value(QStringLiteral("id")).toString();
        if (call.id.isEmpty()) {
            call.id = QStringLiteral("call-%1-%2").arg(m_steps).arg(i);
            c.insert(QStringLiteral("id"), call.id);
        }
        if (!c.contains(QStringLiteral("type")))
            c.insert(QStringLiteral("type"), QStringLiteral("function"));
        const QJsonObject function = c.value(QStringLiteral("function")).toObject();
        call.name = function.value(QStringLiteral("name")).toString();
        const QJsonValue args = function.value(QStringLiteral("arguments"));
        if (args.isObject()) {
            call.args = args.toObject();
        } else {
            const QString s = args.toString().trimmed();
            if (!s.isEmpty()) {
                QJsonParseError error{};
                const QJsonDocument doc = QJsonDocument::fromJson(s.toUtf8(), &error);
                if (!doc.isObject())
                    call.argError = QStringLiteral("the arguments are not a JSON object");
                else
                    call.args = doc.object();
            }
        }
        echoed.append(c);
        m_queue.append(call);
    }
    QJsonObject assistant{{QStringLiteral("role"), QStringLiteral("assistant")}, {QStringLiteral("tool_calls"), echoed}};
    assistant.insert(QStringLiteral("content"), text.isEmpty() ? QJsonValue() : QJsonValue(text));
    m_messages.append(assistant);
    m_pictures.clear();
    runNext();
}

void AgentLoop::runNext()
{
    if (!m_running || m_stopping)
        return;
    if (m_queue.isEmpty()) {
        addPictures();
        askModel();
        return;
    }
    const Call call = m_queue.takeFirst();
    if (call.name == QLatin1String("done")) {
        const QString summary = call.args.value(QStringLiteral("summary")).toString().trimmed();
        finish(Done, QStringLiteral("done %1").arg(summary.isEmpty() ? QStringLiteral("(no text)")
                                                                     : oneLine(summary, 2000)));
        return;
    }
    QJsonObject request;
    QString why;
    if (!serviceRequest(call, &request, &why)) {
        ServiceReply reply{QStringLiteral("failed"), QStringLiteral("failed bad-call: ") + why, {}, {}};
        emit step(QStringLiteral("%1 -> %2").arg(describe(call), reply.line));
        m_messages.append(QJsonObject{{QStringLiteral("role"), QStringLiteral("tool")},
                                      {QStringLiteral("tool_call_id"), call.id},
                                      {QStringLiteral("content"), reply.line}});
        QMetaObject::invokeMethod(this, &AgentLoop::runNext, Qt::QueuedConnection);
        return;
    }
    m_call = new ServiceCall(m_options.socketPath, request, [this, call](const ServiceReply &reply) {
        if (m_call)
            m_call->deleteLater();
        m_call = nullptr;
        onToolReply(call, reply);
    }, this);
}

void AgentLoop::onToolReply(const Call &call, const ServiceReply &reply)
{
    if (!m_running || m_stopping)
        return;
    emit step(QStringLiteral("%1 -> %2").arg(describe(call), oneLine(reply.line, 300)));
    if (reply.kind == QLatin1String("stopped")) {
        // A stop from the user (the stop key, riley-agent stop): nothing more runs, no model is asked.
        finish(Stopped, QStringLiteral("stopped: %1").arg(reply.line));
        return;
    }
    QString content = reply.line;
    for (const QString &l : reply.body)
        content += QLatin1Char('\n') + l;
    m_messages.append(QJsonObject{{QStringLiteral("role"), QStringLiteral("tool")},
                                  {QStringLiteral("tool_call_id"), call.id},
                                  {QStringLiteral("content"), content}});
    const QString window = call.args.value(QStringLiteral("window")).toString();
    if (call.name == QLatin1String("see")) {
        if (reply.kind == QLatin1String("ok") && !reply.png.isEmpty()) {
            m_pictures.append({call.id, window, reply.png});
#ifdef RILEY_FAULT_LOOP_IMAGE_LEAK
            m_lastPicture = m_pictures.last();
        } else if (!m_lastPicture.png.isEmpty()) {
            // Planted fault (gate builds only): the cached picture of another window goes out after a refused see.
            m_pictures.append({call.id, window, m_lastPicture.png});
#endif
        }
    }
    runNext();
}

void AgentLoop::addPictures()
{
    if (m_pictures.isEmpty())
        return;
    dropOldPictures();
    QJsonArray parts;
    for (const Picture &p : std::as_const(m_pictures)) {
        parts.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("text")},
                                 {QStringLiteral("text"), QStringLiteral("Picture of window %1 (from see, call %2).")
                                                              .arg(p.window, p.callId)}});
        parts.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("image_url")},
                                 {QStringLiteral("image_url"),
                                  QJsonObject{{QStringLiteral("url"), QStringLiteral("data:image/png;base64,") + p.png}}}});
    }
    m_messages.append(QJsonObject{{QStringLiteral("role"), QStringLiteral("user")}, {QStringLiteral("content"), parts}});
    emit step(QStringLiteral("%1 picture(s) attached for the model").arg(m_pictures.size()));
    m_pictures.clear();
}

// Only the newest pictures stay in the history: each one costs the model a lot of context.
void AgentLoop::dropOldPictures()
{
    for (qsizetype i = 0; i < m_messages.size(); ++i) {
        QJsonObject m = m_messages.at(i).toObject();
        if (m.value(QStringLiteral("role")).toString() != QLatin1String("user") || !m.value(QStringLiteral("content")).isArray())
            continue;
        QJsonArray kept;
        for (const QJsonValue &part : m.value(QStringLiteral("content")).toArray()) {
            if (part.toObject().value(QStringLiteral("type")).toString() == QLatin1String("image_url"))
                kept.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("text")},
                                        {QStringLiteral("text"), QStringLiteral("(an earlier picture, left out)")}});
            else
                kept.append(part);
        }
        m.insert(QStringLiteral("content"), kept);
        m_messages.replace(i, m);
    }
}

void AgentLoop::finish(int code, const QString &line)
{
    if (!m_running)
        return;
    m_running = false;
    m_deadline.stop();
    if (m_reply) {
        QNetworkReply *r = m_reply;
        m_reply = nullptr;
        r->abort();
        r->deleteLater();
    }
    if (m_call) {
        m_call->abandon();
        m_call->deleteLater();
    }
    m_queue.clear();
    m_pictures.clear();
    m_messages = QJsonArray();
    emit finished(code, line);
}

bool AgentLoop::serviceRequest(const Call &call, QJsonObject *request, QString *why)
{
    static const QStringList windowed{QStringLiteral("see"), QStringLiteral("tree"), QStringLiteral("point"),
                                      QStringLiteral("click"), QStringLiteral("type"), QStringLiteral("key"),
                                      QStringLiteral("close")};
    static const QStringList known = windowed + QStringList{QStringLiteral("windows"), QStringLiteral("launch")};
    if (!call.argError.isEmpty()) {
        *why = call.argError;
        return false;
    }
    if (!known.contains(call.name)) {
        *why = QStringLiteral("no tool named %1").arg(call.name);
        return false;
    }
    QJsonObject r{{QStringLiteral("action"), call.name}};
    if (windowed.contains(call.name)) {
        const QString window = call.args.value(QStringLiteral("window")).toString().trimmed();
        if (window.isEmpty()) {
            *why = QStringLiteral("%1 needs window (an address from windows)").arg(call.name);
            return false;
        }
        r.insert(QStringLiteral("window"), window);
    }
    if (call.name == QLatin1String("click") || call.name == QLatin1String("point")) {
        int x = 0, y = 0;
        if (!intArg(call.args, "x", &x) || !intArg(call.args, "y", &y)) {
            *why = QStringLiteral("%1 needs integer x and y").arg(call.name);
            return false;
        }
        r.insert(QStringLiteral("x"), x);
        r.insert(QStringLiteral("y"), y);
        const QString button = call.args.value(QStringLiteral("button")).toString();
        if (call.name == QLatin1String("click") && !button.isEmpty())
            r.insert(QStringLiteral("button"), button);
    } else if (call.name == QLatin1String("type")) {
        const QJsonValue text = call.args.value(QStringLiteral("text"));
        if (!text.isString() || text.toString().isEmpty()) {
            *why = QStringLiteral("type needs text");
            return false;
        }
        r.insert(QStringLiteral("text"), text.toString());
        if (call.args.contains(QStringLiteral("delay_ms"))) {
            int ms = 0;
            if (!intArg(call.args, "delay_ms", &ms) || ms < 0 || ms > 5000) {
                *why = QStringLiteral("delay_ms must be 0 to 5000");
                return false;
            }
            r.insert(QStringLiteral("delay-ms"), ms);
        }
    } else if (call.name == QLatin1String("key")) {
        const QString key = call.args.value(QStringLiteral("key")).toString().trimmed();
        if (key.isEmpty()) {
            *why = QStringLiteral("key needs key (a key name)");
            return false;
        }
        r.insert(QStringLiteral("key"), key);
    } else if (call.name == QLatin1String("launch")) {
        const QString app = call.args.value(QStringLiteral("app")).toString().trimmed();
        if (app.isEmpty()) {
            *why = QStringLiteral("launch needs app");
            return false;
        }
        r.insert(QStringLiteral("app"), app);
    }
    *request = r;
    return true;
}

// For the transcript: the call without its typed text (only its length).
QString AgentLoop::describe(const Call &call)
{
    QString s = call.name;
    const QString window = call.args.value(QStringLiteral("window")).toString();
    if (!window.isEmpty())
        s += QLatin1Char(' ') + oneLine(window, 40);
    if (call.args.contains(QStringLiteral("x")) || call.args.contains(QStringLiteral("y"))) {
        int x = 0, y = 0;
        intArg(call.args, "x", &x);
        intArg(call.args, "y", &y);
        s += QStringLiteral(" %1,%2").arg(x).arg(y);
    }
    if (call.args.value(QStringLiteral("text")).isString())
        s += QStringLiteral(" (%1 characters)").arg(call.args.value(QStringLiteral("text")).toString().size());
    if (call.args.value(QStringLiteral("key")).isString())
        s += QLatin1Char(' ') + oneLine(call.args.value(QStringLiteral("key")).toString(), 40);
    if (call.args.value(QStringLiteral("app")).isString())
        s += QLatin1Char(' ') + oneLine(call.args.value(QStringLiteral("app")).toString(), 60);
    return s;
}

QJsonArray AgentLoop::toolDefinitions()
{
    const QJsonObject window{{QStringLiteral("window"), prop("string", kWindow)}};
    auto with = [&window](std::initializer_list<std::pair<QString, QJsonObject>> extra) {
        QJsonObject o = window;
        for (const auto &e : extra)
            o.insert(e.first, e.second);
        return o;
    };
    const QJsonObject x = prop("integer", "logical pixels from the window's left edge");
    const QJsonObject y = prop("integer", "logical pixels from the window's top edge");
    return {
        tool("windows", "List the windows you may see: ADDR class monitor x,y wxh \"title\", one per line.", {}, {}),
        tool("see", "A picture of one window, attached after the tool results.", window, {QStringLiteral("window")}),
        tool("tree", "The window's controls from the accessibility tree, \"role: name\", indented per level.", window,
             {QStringLiteral("window")}),
        tool("point", "Move the pointer to x,y inside the window.", with({{QStringLiteral("x"), x}, {QStringLiteral("y"), y}}),
             {QStringLiteral("window"), QStringLiteral("x"), QStringLiteral("y")}),
        tool("click", "Click at x,y inside the window.",
             with({{QStringLiteral("x"), x}, {QStringLiteral("y"), y},
                   {QStringLiteral("button"), prop("string", "left (default), right or middle")}}),
             {QStringLiteral("window"), QStringLiteral("x"), QStringLiteral("y")}),
        tool("type", "Type text into the window's focused control (click into it first). Never passwords or keys.",
             with({{QStringLiteral("text"), prop("string", "the text to type")},
                   {QStringLiteral("delay_ms"), prop("integer", "optional milliseconds between key events")}}),
             {QStringLiteral("window"), QStringLiteral("text")}),
        tool("key", "Press one key in the window: Return, Tab, Escape, BackSpace, ctrl+a, ctrl+shift+t.",
             with({{QStringLiteral("key"), prop("string", "the key name")}}),
             {QStringLiteral("window"), QStringLiteral("key")}),
        tool("launch", "Start a program from /usr/bin, in the home folder with a clean environment.",
             {{QStringLiteral("app"), prop("string", "the program's name")}}, {QStringLiteral("app")}),
        tool("close", "Ask the window to close.", window, {QStringLiteral("window")}),
        tool("done", "The task is finished: say what was done.",
             {{QStringLiteral("summary"), prop("string", "one or two sentences")}}, {QStringLiteral("summary")}),
    };
}

QString AgentLoop::systemPrompt()
{
    return QStringLiteral(
        "You work a Hyprland desktop through riley-agentd, which checks every action against the user's "
        "permissions and logs it. Call windows first and use a window's address (0x...) as window. Coordinates are "
        "logical pixels inside the window: 0,0 is its top-left corner. Typing goes to the focused control: click "
        "into it first. Each tool result starts with ok, refused REASON (rule R), stopped or failed. A refusal is "
        "final: do not retry it or work around it; carry on with what you may do, or finish and say what was "
        "refused. Never type passwords, keys or card numbers; never buy, pay or move money. When the task is "
        "finished, call done or answer without a tool call.");
}

} // namespace riley
