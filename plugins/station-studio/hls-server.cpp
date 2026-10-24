#include "hls-server.hpp"

#include <obs-module.h>

#include <QDir>
#include <QFile>
#include <QRegularExpression>
#include <QTcpServer>
#include <QTcpSocket>

namespace station {

static const char *kPlayerPage = R"HTML(<!doctype html>
<html lang="ar" dir="rtl">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>البث المباشر</title>
<style>
html,body{margin:0;height:100%;background:#000;color:#fff;font-family:system-ui,sans-serif}
video{width:100%;height:100%;object-fit:contain;background:#000}
#msg{position:absolute;inset:auto 0 1rem 0;text-align:center;opacity:.8}
</style>
</head>
<body>
<video id="v" controls autoplay muted playsinline></video>
<div id="msg"></div>
<script src="https://cdn.jsdelivr.net/npm/hls.js@1/dist/hls.min.js"></script>
<script>
const v = document.getElementById('v');
const msg = document.getElementById('msg');
const src = 'live.m3u8';
function start() {
  if (v.canPlayType('application/vnd.apple.mpegurl')) { v.src = src; return; }
  if (window.Hls && Hls.isSupported()) {
    const hls = new Hls({ liveSyncDurationCount: 3 });
    hls.loadSource(src);
    hls.attachMedia(v);
    hls.on(Hls.Events.ERROR, (_, d) => {
      if (d.fatal) { msg.textContent = 'جارٍ إعادة الاتصال...'; setTimeout(() => { hls.destroy(); start(); }, 3000); }
    });
    hls.on(Hls.Events.MANIFEST_PARSED, () => { msg.textContent = ''; });
    return;
  }
  msg.textContent = 'المتصفح لا يدعم تشغيل البث';
}
start();
</script>
</body>
</html>
)HTML";

HlsServer::HlsServer() = default;

HlsServer::~HlsServer()
{
	Stop();
}

bool HlsServer::Start(const QString &folder, quint16 port, QString &error)
{
	Stop();
	folder_ = folder;
	server_ = new QTcpServer(this);
	if (!server_->listen(QHostAddress::Any, port)) {
		error = QString::fromUtf8(obs_module_text("Error.HttpPort")).arg(port).arg(server_->errorString());
		delete server_;
		server_ = nullptr;
		return false;
	}
	connect(server_, &QTcpServer::newConnection, this, [this]() {
		while (server_ && server_->hasPendingConnections()) {
			QTcpSocket *socket = server_->nextPendingConnection();
			connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
			connect(socket, &QTcpSocket::readyRead, this, [this, socket]() { HandleClient(socket); });
		}
	});
	blog(LOG_INFO, "[station-studio] HLS web server listening on port %u", port);
	return true;
}

void HlsServer::Stop()
{
	if (server_) {
		server_->close();
		delete server_;
		server_ = nullptr;
	}
}

void HlsServer::Respond(QTcpSocket *socket, int status, const QByteArray &type, const QByteArray &body,
			bool noCache)
{
	const char *reason = status == 200 ? "OK" : status == 404 ? "Not Found" : "Bad Request";
	QByteArray head = "HTTP/1.1 " + QByteArray::number(status) + " " + reason + "\r\n";
	head += "Content-Type: " + type + "\r\n";
	head += "Content-Length: " + QByteArray::number(body.size()) + "\r\n";
	head += "Access-Control-Allow-Origin: *\r\n";
	head += "X-Content-Type-Options: nosniff\r\n";
	head += noCache ? "Cache-Control: no-cache\r\n" : "Cache-Control: max-age=60\r\n";
	head += "Connection: close\r\n\r\n";
	socket->write(head);
	socket->write(body);
	socket->disconnectFromHost();
}

void HlsServer::HandleClient(QTcpSocket *socket)
{
	if (!socket->canReadLine()) {
		if (socket->bytesAvailable() > 8192)
			socket->abort();
		return;
	}

	const QList<QByteArray> parts = socket->readLine().trimmed().split(' ');
	if (parts.size() < 2 || (parts[0] != "GET" && parts[0] != "HEAD")) {
		Respond(socket, 400, "text/plain", "bad request");
		return;
	}

	QString path = QString::fromLatin1(parts[1]);
	const int query = path.indexOf('?');
	if (query >= 0)
		path.truncate(query);

	if (path == "/" || path == "/index.html") {
		Respond(socket, 200, "text/html; charset=utf-8", QByteArray(kPlayerPage));
		return;
	}

	/* Only flat names like live.m3u8 / live12.ts: no traversal, no subdirs. */
	static const QRegularExpression allowed("^/([A-Za-z0-9_-]+\\.(m3u8|ts))$");
	const QRegularExpressionMatch match = allowed.match(path);
	if (!match.hasMatch()) {
		Respond(socket, 404, "text/plain", "not found");
		return;
	}

	QFile file(QDir(folder_).filePath(match.captured(1)));
	if (!file.open(QIODevice::ReadOnly)) {
		Respond(socket, 404, "text/plain", "not found");
		return;
	}

	const bool playlist = match.captured(2) == "m3u8";
	Respond(socket, 200, playlist ? "application/vnd.apple.mpegurl" : "video/mp2t",
		parts[0] == "HEAD" ? QByteArray() : file.readAll(), playlist);
}

} // namespace station
