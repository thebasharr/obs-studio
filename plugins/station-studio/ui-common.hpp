#pragma once

#include <obs-module.h>

#include <QString>

#include <cstring>

namespace station {

/* Translated text from the plugin's locale files. */
inline QString T(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

/* Saturday-first week, as used by most Arabic-speaking broadcasters.
   Values are weekday numbers (0 = Sunday). */
inline constexpr int kDisplayOrder[7] = {6, 0, 1, 2, 3, 4, 5};

/* Name prefix of sources the plugin creates and owns. */
inline constexpr const char kManagedSourcePrefix[] = "station-playout::";

inline bool IsManagedSourceName(const char *name)
{
	return name && strncmp(name, kManagedSourcePrefix, sizeof(kManagedSourcePrefix) - 1) == 0;
}

} // namespace station
