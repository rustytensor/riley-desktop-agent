// SPDX-License-Identifier: GPL-3.0-or-later
#include "config.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>

#include <libxml/parser.h>
#include <libxml/tree.h>
#include <libxml/xmlerror.h>
#include <libxml/xmlschemas.h>

#include <mutex>

namespace riley {

namespace {

constexpr qint64 kMaxConfigBytes = 1024 * 1024;
constexpr int kParseOptions = XML_PARSE_NONET | XML_PARSE_NO_XXE | XML_PARSE_NO_SYS_CATALOG;

// Keeps the first error libxml2 reports, with its line.
struct ErrorSink {
    QString first;
    static void handler(void *data, const xmlError *error)
    {
        auto *sink = static_cast<ErrorSink *>(data);
        if (!sink->first.isEmpty() || !error)
            return;
        QString message = error->message ? QString::fromUtf8(error->message).trimmed() : QStringLiteral("error");
        sink->first = error->line > 0 ? QStringLiteral("line %1: %2").arg(error->line).arg(message) : message;
    }
};

struct CompiledSchema {
    xmlSchemaPtr schema = nullptr;
    QString error;
};

const CompiledSchema &schema()
{
    static CompiledSchema compiled;
    static std::once_flag once;
    std::call_once(once, [] {
        xmlInitParser();
        const QByteArray xsd = compiledSchema();
        if (xsd.isEmpty()) {
            compiled.error = QStringLiteral("the compiled-in schema is missing");
            return;
        }
        ErrorSink sink;
        xmlSchemaParserCtxtPtr ctxt = xmlSchemaNewMemParserCtxt(xsd.constData(), int(xsd.size()));
        if (!ctxt) {
            compiled.error = QStringLiteral("cannot create the schema parser");
            return;
        }
        xmlSchemaSetParserStructuredErrors(ctxt, &ErrorSink::handler, &sink);
        compiled.schema = xmlSchemaParse(ctxt);
        xmlSchemaFreeParserCtxt(ctxt);
        if (!compiled.schema)
            compiled.error = QStringLiteral("the compiled-in schema does not compile: ") + sink.first;
    });
    return compiled;
}

bool isElement(const xmlNode *node, const char *name)
{
    return node && node->type == XML_ELEMENT_NODE && xmlStrEqual(node->name, BAD_CAST name);
}

// A null QString when the attribute is absent, so absent and empty stay apart.
QString attr(const xmlNode *node, const char *name)
{
    xmlAttrPtr a = xmlHasNsProp(node, BAD_CAST name, nullptr);
    if (!a)
        return QString();
    xmlChar *value = xmlNodeGetContent(reinterpret_cast<xmlNodePtr>(a));
    QString result = value ? QString::fromUtf8(reinterpret_cast<const char *>(value)) : QStringLiteral("");
    xmlFree(value);
    if (result.isNull())
        result = QStringLiteral("");
    return result;
}

QString text(const xmlNode *node)
{
    xmlChar *value = xmlNodeGetContent(node);
    QString result = value ? QString::fromUtf8(reinterpret_cast<const char *>(value)).trimmed() : QString();
    xmlFree(value);
    return result;
}

Location location(const QString &value)
{
    return value == QLatin1String("local") ? Location::Local : Location::Remote;
}

WindowPattern windowPattern(const xmlNode *node)
{
    WindowPattern w;
    w.id = attr(node, "id");
    w.windowClass = attr(node, "class");
    w.title = attr(node, "title");
    w.monitor = attr(node, "monitor");
    return w;
}

template <typename F>
void forEachChild(const xmlNode *parent, const char *name, F &&f)
{
    for (const xmlNode *n = parent->children; n; n = n->next)
        if (isElement(n, name))
            f(n);
}

// The document has already passed the schema, so every required part is present.
Config readModel(const xmlNode *root)
{
    Config c;
    for (const xmlNode *section = root->children; section; section = section->next) {
        if (isElement(section, "endpoints")) {
            forEachChild(section, "endpoint", [&](const xmlNode *n) {
                Endpoint e;
                e.name = attr(n, "name");
                e.baseUrl = attr(n, "base-url");
                e.model = attr(n, "model");
                e.location = location(attr(n, "location"));
                forEachChild(n, "key-env", [&](const xmlNode *k) { e.keyEnv = text(k); });
                forEachChild(n, "key-file", [&](const xmlNode *k) { e.keyFile = text(k); });
                c.endpoints << e;
            });
        } else if (isElement(section, "clients")) {
            forEachChild(section, "client", [&](const xmlNode *n) {
                Client cl;
                cl.kind = attr(n, "kind");
                cl.endpoint = attr(n, "endpoint");
                const QString loc = attr(n, "location");
                if (!loc.isNull())
                    cl.location = location(loc);
                c.clients << cl;
            });
        } else if (isElement(section, "remote-allow")) {
            forEachChild(section, "window", [&](const xmlNode *n) { c.remoteAllow << windowPattern(n); });
        } else if (isElement(section, "permissions")) {
            forEachChild(section, "allow", [&](const xmlNode *n) {
                Permission p;
                p.id = attr(n, "id");
                p.client = attr(n, "client");
                p.actions = attr(n, "actions").simplified().split(QLatin1Char(' '), Qt::SkipEmptyParts);
                forEachChild(n, "window", [&](const xmlNode *w) { p.windows << windowPattern(w); });
                c.permissions << p;
            });
        } else if (isElement(section, "deny-always")) {
            forEachChild(section, "window", [&](const xmlNode *n) { c.denyAlways << windowPattern(n); });
        } else if (isElement(section, "named-hook-editors")) {
            forEachChild(section, "editor", [&](const xmlNode *n) { c.namedHookEditors << attr(n, "class"); });
        }
    }
    return c;
}

ConfigLoad invalid(const QString &name, const QString &why)
{
    ConfigLoad load;
    load.status = ConfigStatus::Invalid;
    load.error = name + QStringLiteral(": ") + (why.isEmpty() ? QStringLiteral("invalid") : why);
    return load;
}

} // namespace

const Client *Config::client(const QString &kind) const
{
    for (const Client &c : clients)
        if (c.kind == kind)
            return &c;
    return nullptr;
}

const Endpoint *Config::endpoint(const QString &name) const
{
    for (const Endpoint &e : endpoints)
        if (e.name == name)
            return &e;
    return nullptr;
}

QByteArray compiledSchema()
{
    QFile f(QStringLiteral(":/riley/schema/config.xsd"));
    if (!f.open(QIODevice::ReadOnly))
        return QByteArray();
    return f.readAll();
}

QString defaultConfigPath()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)
           + QStringLiteral("/riley-desktop-agent/config.xml");
}

ConfigLoad loadConfigFile(const QString &path)
{
    const QFileInfo info(path);
    if (!info.exists()) {
        ConfigLoad load;
        load.status = ConfigStatus::Missing;
        load.error = path + QStringLiteral(": no such file");
        return load;
    }
    if (!info.isFile())
        return invalid(path, QStringLiteral("not a regular file"));
    if (info.size() > kMaxConfigBytes)
        return invalid(path, QStringLiteral("larger than 1 MiB"));
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return invalid(path, QStringLiteral("cannot be read: ") + f.errorString());
    const QByteArray data = f.read(kMaxConfigBytes + 1);
    if (data.size() > kMaxConfigBytes)
        return invalid(path, QStringLiteral("larger than 1 MiB"));
    return loadConfigData(data, path);
}

ConfigLoad loadConfigData(const QByteArray &data, const QString &name)
{
    const CompiledSchema &compiled = schema();
    if (!compiled.schema)
        return invalid(name, compiled.error);
    if (data.isEmpty())
        return invalid(name, QStringLiteral("empty file"));
    if (data.size() > kMaxConfigBytes)
        return invalid(name, QStringLiteral("larger than 1 MiB"));

    ErrorSink sink;
    xmlParserCtxtPtr parser = xmlNewParserCtxt();
    if (!parser)
        return invalid(name, QStringLiteral("cannot create the XML parser"));
    xmlCtxtSetErrorHandler(parser, &ErrorSink::handler, &sink);
    xmlDocPtr doc = xmlCtxtReadMemory(parser, data.constData(), int(data.size()), nullptr, nullptr, kParseOptions);
    xmlFreeParserCtxt(parser);
    if (!doc)
        return invalid(name, QStringLiteral("not well-formed XML: ") + sink.first);

    xmlSchemaValidCtxtPtr valid = xmlSchemaNewValidCtxt(compiled.schema);
    if (!valid) {
        xmlFreeDoc(doc);
        return invalid(name, QStringLiteral("cannot create the schema validator"));
    }
    xmlSchemaSetValidStructuredErrors(valid, &ErrorSink::handler, &sink);
    const int rc = xmlSchemaValidateDoc(valid, doc);
    xmlSchemaFreeValidCtxt(valid);
    if (rc != 0) {
        xmlFreeDoc(doc);
        return invalid(name, QStringLiteral("does not match the schema: ")
                                 + (sink.first.isEmpty() ? QStringLiteral("validation failed") : sink.first));
    }

    const xmlNode *root = xmlDocGetRootElement(doc);
    if (!isElement(root, "riley-desktop-agent")) {
        xmlFreeDoc(doc);
        return invalid(name, QStringLiteral("wrong root element"));
    }
    auto config = std::make_shared<Config>(readModel(root));
    xmlFreeDoc(doc);

    ConfigLoad load;
    load.status = ConfigStatus::Ok;
    load.config = std::move(config);
    return load;
}

bool isRemoteClient(const Config &config, const QString &clientKind)
{
    const Client *client = config.client(clientKind);
    if (!client)
        return true;
    if (client->location && *client->location == Location::Remote)
        return true;
    if (!client->endpoint.isEmpty()) {
        const Endpoint *endpoint = config.endpoint(client->endpoint);
        if (!endpoint || endpoint->location == Location::Remote)
            return true;
    }
    if (!client->location && client->endpoint.isEmpty())
        return true;
    return false;
}

} // namespace riley
