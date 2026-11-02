#include "api/ApiServer.h"

#include <QHostAddress>
#include <QJsonDocument>
#include <QTcpSocket>

namespace fr {
namespace {
constexpr qsizetype kMaxBufferedBytes = 16 * 1024 * 1024;
}

ApiServer::ApiServer(Session& session, QObject* parent) : QObject(parent), dispatcher_(session) {
    connect(&server_, &QTcpServer::newConnection, this, &ApiServer::onNewConnection);
}

bool ApiServer::listen(quint16 port, QString* error) {
    if (server_.listen(QHostAddress::LocalHost, port)) return true;
    if (error) *error = server_.errorString();
    return false;
}

quint16 ApiServer::port() const {
    return server_.serverPort();
}

void ApiServer::onNewConnection() {
    while (QTcpSocket* socket = server_.nextPendingConnection()) {
        buffers_.insert(socket, QByteArray());
        connect(socket, &QTcpSocket::readyRead, this, [this, socket] { onReadyRead(socket); });
        connect(socket, &QTcpSocket::disconnected, this, [this, socket] {
            buffers_.remove(socket);
            socket->deleteLater();
        });
    }
}

void ApiServer::onReadyRead(QTcpSocket* socket) {
    QByteArray& buffer = buffers_[socket];
    buffer += socket->readAll();
    if (buffer.size() > kMaxBufferedBytes) {
        socket->disconnectFromHost();
        return;
    }
    for (;;) {
        const qsizetype newline = buffer.indexOf('\n');
        if (newline < 0) break;
        const QByteArray line = buffer.left(newline).trimmed();
        buffer.remove(0, newline + 1);
        if (line.isEmpty()) continue;
        socket->write(respondTo(line));
    }
}

QByteArray ApiServer::respondTo(const QByteArray& line) {
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(line, &parseError);

    QJsonObject response;
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        response.insert(QStringLiteral("success"), false);
        response.insert(QStringLiteral("msg"), QStringLiteral("Request is not a valid JSON object"));
    } else {
        response = dispatcher_.handle(doc.object());
    }
    return QJsonDocument(response).toJson(QJsonDocument::Compact) + '\n';
}

}  // namespace fr
