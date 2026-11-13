// SPDX-License-Identifier: GPL-3.0-or-later
//
// The config file model and its loader. Every load parses the file with libxml2 and validates it against the
// schema compiled into the binary (schema/config.xsd). A missing or invalid file yields no Config at all, and the
// permission engine then denies everything.
#pragma once

#include <QList>
#include <QString>
#include <QStringList>

#include <memory>
#include <optional>

namespace riley {

enum class Location { Local, Remote };

// An OpenAI-compatible endpoint. The key is never held here: only the name of an environment variable or the
// path of a file that holds it.
struct Endpoint {
    QString name;
    QString baseUrl;
    QString model;
    Location location = Location::Remote;
    QString keyEnv;   // empty when not given
    QString keyFile;  // empty when not given
};

struct Client {
    QString kind;                       // own-loop, claude-code or pi
    QString endpoint;                   // empty when not given
    std::optional<Location> location;   // absent when not given
};

// A window pattern: class, title and monitor globs (* and ?), case-insensitive, matched against the whole value.
// A null title or monitor matches anything.
struct WindowPattern {
    QString id;
    QString windowClass;
    QString title;
    QString monitor;
};

struct Permission {
    QString id;
    QString client;
    QStringList actions;
    QList<WindowPattern> windows;
};

struct Config {
    QList<Endpoint> endpoints;
    QList<Client> clients;
    QList<WindowPattern> remoteAllow;
    QList<Permission> permissions;
    QList<WindowPattern> denyAlways;
    QStringList namedHookEditors;   // window classes

    const Client *client(const QString &kind) const;
    const Endpoint *endpoint(const QString &name) const;
};

enum class ConfigStatus { Ok, Missing, Invalid };

struct ConfigLoad {
    ConfigStatus status = ConfigStatus::Missing;
    QString error;                        // human-readable, empty when Ok
    std::shared_ptr<const Config> config; // set only when Ok
};

// Reads and validates one config file. Missing if the file does not exist; Invalid if it cannot be read, is not
// well-formed, or does not validate against the compiled-in schema.
ConfigLoad loadConfigFile(const QString &path);

// Same, from bytes already read (the name is used in error messages only).
ConfigLoad loadConfigData(const QByteArray &data, const QString &name);

// The schema compiled into the binary.
QByteArray compiledSchema();

// ~/.config/riley-desktop-agent/config.xml (honours XDG_CONFIG_HOME).
QString defaultConfigPath();

// Rule L: a client is remote if its location is remote, or the endpoint it names is remote, or it names neither a
// location nor an endpoint. A client the config does not list is treated as remote.
bool isRemoteClient(const Config &config, const QString &clientKind);

} // namespace riley
