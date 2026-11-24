#pragma once

#include <QString>

class QThread;

namespace station {

class HlsWorker;

/* Minimal read-only HTTP server for the HLS folder plus the bundled player
   page. Serves only flat files with known names, runs on its own thread so
   file reads never block the OBS UI, and drops idle or excess connections. */
class HlsServer {
public:
	HlsServer() = default;
	~HlsServer();

	HlsServer(const HlsServer &) = delete;
	HlsServer &operator=(const HlsServer &) = delete;

	/* `lanAccess` false binds to localhost only. */
	bool Start(const QString &hlsFolder, const QString &playerFolder, quint16 port, bool lanAccess, QString &error);
	void Stop();

private:
	QThread *thread_ = nullptr;
	HlsWorker *worker_ = nullptr;
};

} // namespace station
