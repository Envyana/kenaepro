#pragma once

#include "core/Session.h"

#include <QHash>
#include <QJsonObject>
#include <QString>

#include <functional>

namespace fr {

// Executes Client API commands against a Session. Transport-independent: ApiServer feeds it
// parsed requests; the tests call it directly.
//
// Request:  { "cmd": "<name>", ...parameters }
// Response: { "success": true,  "result": { ... } }
//        or { "success": false, "msg": "<reason>" }
//
// Every command except "echo" and "initialize" is refused until "initialize" succeeds.
class ApiDispatcher {
public:
    static constexpr int kApiVersion = 1;

    explicit ApiDispatcher(Session& session);

    QJsonObject handle(const QJsonObject& request);
    bool initialized() const { return initialized_; }

private:
    using Handler = std::function<bool(const QJsonObject& request, QJsonObject& result, QString& error)>;

    void registerHandlers();

    Session& session_;
    bool initialized_ = false;
    QHash<QString, Handler> handlers_;
};

}  // namespace fr
