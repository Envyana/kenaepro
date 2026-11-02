#pragma once

#include "api/ApiDispatcher.h"

#include <QByteArray>
#include <QHash>
#include <QObject>
#include <QTcpServer>

class QTcpSocket;

namespace fr {

// Local TCP front end for ApiDispatcher.
//
// Framing is ASSUMED, not observed: one JSON object per line ('\n' terminated) in each
// direction. Listens on 127.0.0.1 only. Confirm against the original application's traffic
// before relying on it for compatibility (see README).
class ApiServer : public QObject {
    Q_OBJECT
public:
    explicit ApiServer(Session& session, QObject* parent = nullptr);

    bool listen(quint16 port, QString* error = nullptr);
    quint16 port() const;

private:
    void onNewConnection();
    void onReadyRead(QTcpSocket* socket);
    QByteArray respondTo(const QByteArray& line);

    ApiDispatcher dispatcher_;
    QTcpServer server_;
    QHash<QTcpSocket*, QByteArray> buffers_;
};

}  // namespace fr
