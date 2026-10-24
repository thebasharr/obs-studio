#pragma once

#include <QObject>
#include <QString>

class QTcpServer;
class QTcpSocket;

namespace station {

/* Minimal read-only HTTP server for the HLS folder plus an embeddable player
   page. Serves only flat files with known extensions from one folder. */
class HlsServer : public QObject {
	Q_OBJECT

public:
	HlsServer();
	~HlsServer() override;

	bool Start(const QString &folder, quint16 port, QString &error);
	void Stop();

private:
	void HandleClient(QTcpSocket *socket);
	void Respond(QTcpSocket *socket, int status, const QByteArray &type, const QByteArray &body,
		     bool noCache = true);

	QTcpServer *server_ = nullptr;
	QString folder_;
};

} // namespace station
