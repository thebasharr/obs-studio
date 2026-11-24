#include "hls-server.hpp"

#include <obs-module.h>

#include <QDir>
#include <QFile>
#include <QHostAddress>
#include <QRegularExpression>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>
#include <QTimer>

namespace station {

namespace {

constexpr int kMaxClients = 64;
constexpr int kRequestTimeoutMs = 10 * 1000;  /* time to send the request line */
constexpr int kResponseTimeoutMs = 120 * 1000; /* time to download the response */
constexpr qint64 kMaxRequestLine = 8192;
const char *kServedProperty = "stationServed";

} // namespace

/* Lives on the server thread; everything below runs there. */
class HlsWorker : public QObject {
public:
	HlsWorker(const QString &hlsFolder, const QString &playerFolder)
		: hlsFolder_(hlsFolder),
		  playerFolder_(playerFolder)
	{
	}

	bool Listen(const QHostAddress &address, quint16 port, QString &error)
	{
		server_ = new QTcpServer(this);
		server_->setMaxPendingConnections(kMaxClients);
		if (!server_->listen(address, port)) {
			error = server_->errorString();
			return false;
		}
		connect(server_, &QTcpServer::newConnection, this, &HlsWorker::Accept);
		return true;
	}

private:
	void Accept()
	{
		while (server_->hasPendingConnections()) {
			QTcpSocket *socket = server_->nextPendingConnection();
			if (clients_ >= kMaxClients) {
				socket->abort();
				socket->deleteLater();
				continue;
			}

			clients_++;
			connect(socket, &QObject::destroyed, this, [this]() { clients_--; });
			connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
			connect(socket, &QTcpSocket::readyRead, this, [this, socket]() { HandleClient(socket); });

			/* Drop clients that connect but never send a request. */
			QTimer::singleShot(kRequestTimeoutMs, socket, [socket]() {
				if (!socket->property(kServedProperty).toBool()) {
					socket->abort();
					socket->deleteLater();
				}
			});
		}
	}

	void Respond(QTcpSocket *socket, int status, const QByteArray &type, const QByteArray &body,
		     bool includeBody = true, bool noCache = true)
	{
		socket->setProperty(kServedProperty, true);

		const char *reason = status == 200 ? "OK" : status == 404 ? "Not Found" : "Bad Request";
		QByteArray head = "HTTP/1.1 " + QByteArray::number(status) + " " + reason + "\r\n";
		head += "Content-Type: " + type + "\r\n";
		head += "Content-Length: " + QByteArray::number(body.size()) + "\r\n";
		head += "Access-Control-Allow-Origin: *\r\n";
		head += "X-Content-Type-Options: nosniff\r\n";
		head += noCache ? "Cache-Control: no-cache\r\n" : "Cache-Control: max-age=3600\r\n";
		head += "Connection: close\r\n\r\n";
		socket->write(head);
		if (includeBody)
			socket->write(body);
		socket->disconnectFromHost();

		/* Cap how long a slow reader may hold the connection. */
		QTimer::singleShot(kResponseTimeoutMs, socket, [socket]() {
			socket->abort();
			socket->deleteLater();
		});
	}

	void HandleClient(QTcpSocket *socket)
	{
		if (socket->property(kServedProperty).toBool())
			return;

		if (!socket->canReadLine()) {
			if (socket->bytesAvailable() > kMaxRequestLine) {
				socket->abort();
				socket->deleteLater();
			}
			return;
		}

		const QList<QByteArray> parts = socket->readLine(kMaxRequestLine).trimmed().split(' ');
		if (parts.size() < 2 || (parts[0] != "GET" && parts[0] != "HEAD")) {
			Respond(socket, 400, "text/plain", "bad request");
			return;
		}
		const bool head = parts[0] == "HEAD";

		QString path = QString::fromLatin1(parts[1]);
		const int query = path.indexOf('?');
		if (query >= 0)
			path.truncate(query);

		/* Fixed player files from the plugin data folder. */
		struct StaticFile {
			const char *path;
			const char *file;
			const char *type;
		};
		static const StaticFile kStatic[] = {
			{"/", "index.html", "text/html; charset=utf-8"},
			{"/index.html", "index.html", "text/html; charset=utf-8"},
			{"/hls.light.min.js", "hls.light.min.js", "text/javascript; charset=utf-8"},
		};
		for (const StaticFile &s : kStatic) {
			if (path != QLatin1String(s.path))
				continue;
			QFile file(QDir(playerFolder_).filePath(QString::fromLatin1(s.file)));
			if (!file.open(QIODevice::ReadOnly)) {
				Respond(socket, 404, "text/plain", "not found");
				return;
			}
			Respond(socket, 200, s.type, file.readAll(), !head, false);
			return;
		}

		/* Only flat names like live.m3u8 / live12.ts: no traversal, no subdirs. */
		static const QRegularExpression allowed("^/([A-Za-z0-9_-]+\\.(m3u8|ts))$");
		const QRegularExpressionMatch match = allowed.match(path);
		if (!match.hasMatch()) {
			Respond(socket, 404, "text/plain", "not found");
			return;
		}

		QFile file(QDir(hlsFolder_).filePath(match.captured(1)));
		if (!file.open(QIODevice::ReadOnly)) {
			Respond(socket, 404, "text/plain", "not found");
			return;
		}

		const bool playlist = match.captured(2) == "m3u8";
		Respond(socket, 200, playlist ? "application/vnd.apple.mpegurl" : "video/mp2t", file.readAll(), !head,
			playlist);
	}

	QTcpServer *server_ = nullptr;
	QString hlsFolder_;
	QString playerFolder_;
	int clients_ = 0;
};

HlsServer::~HlsServer()
{
	Stop();
}

bool HlsServer::Start(const QString &hlsFolder, const QString &playerFolder, quint16 port, bool lanAccess,
		      QString &error)
{
	Stop();

	thread_ = new QThread;
	thread_->setObjectName("station-hls-server");
	worker_ = new HlsWorker(hlsFolder, playerFolder);
	worker_->moveToThread(thread_);
	thread_->start();

	const QHostAddress address = lanAccess ? QHostAddress(QHostAddress::Any) : QHostAddress(QHostAddress::LocalHost);
	bool ok = false;
	QString reason;
	QMetaObject::invokeMethod(
		worker_, [&]() { ok = worker_->Listen(address, port, reason); }, Qt::BlockingQueuedConnection);

	if (!ok) {
		error = QString::fromUtf8(obs_module_text("Error.HttpPort")).arg(port).arg(reason);
		Stop();
		return false;
	}

	blog(LOG_INFO, "[station-studio] HLS web server listening on %s:%u", lanAccess ? "all interfaces" : "localhost",
	     port);
	return true;
}

void HlsServer::Stop()
{
	if (!thread_)
		return;

	/* Sockets and the server must be destroyed on the thread that owns them. */
	HlsWorker *worker = worker_;
	QMetaObject::invokeMethod(worker, [worker]() { delete worker; }, Qt::BlockingQueuedConnection);
	thread_->quit();
	thread_->wait();
	delete thread_;
	thread_ = nullptr;
	worker_ = nullptr;
}

} // namespace station
