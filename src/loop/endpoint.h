// SPDX-License-Identifier: GPL-3.0-or-later
//
// Which endpoint the loop talks to, and its key. The endpoint comes from config.xml: the one the own-loop client
// names, or another one given by name if that cannot make the loop more remote than the service thinks it is. The
// key is read from the variable or file the endpoint names, at the moment it is needed, and kept only in memory.
#pragma once

#include "config.h"

#include <QByteArray>
#include <QString>

namespace riley {

// The endpoint for own-loop. override, when not empty, names another endpoint of the config. A remote endpoint is
// refused unless the service already counts own-loop as remote (rule L), so it never gets pictures that only a
// local model may see. False with *why set when no endpoint can be used.
bool resolveLoopEndpoint(const Config &config, const QString &override, Endpoint *out, QString *why);

// The key: key-env's value, or key-file's content without surrounding whitespace; empty (and true) when the
// endpoint names neither. False with *why set (never holding the key) when it is named but cannot be read.
bool readEndpointKey(const Endpoint &endpoint, QByteArray *key, QString *why);

} // namespace riley
