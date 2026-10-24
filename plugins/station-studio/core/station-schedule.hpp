/******************************************************************************
    Station Studio - broadcast schedule model
    Pure C++17, no Qt/libobs dependency so it can be unit tested standalone.
******************************************************************************/

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace station {

constexpr int kMinutesPerDay = 24 * 60;
constexpr int64_t kSecondsPerDay = 24 * 60 * 60;

/* Bit i set => slot repeats on weekday i (0 = Sunday ... 6 = Saturday). */
constexpr uint8_t kEveryDay = 0x7F;

struct CivilDate {
	int year = 1970;
	int month = 1;
	int day = 1;

	bool operator==(const CivilDate &o) const { return year == o.year && month == o.month && day == o.day; }
	bool operator!=(const CivilDate &o) const { return !(*this == o); }
};

int64_t DaysFromCivil(const CivilDate &date);
CivilDate CivilFromDays(int64_t days);
int WeekdayOf(const CivilDate &date); /* 0 = Sunday */
CivilDate AddDays(const CivilDate &date, int days);
std::string FormatDate(const CivilDate &date); /* YYYY-MM-DD */
std::optional<CivilDate> ParseDate(const std::string &text);

/* Local wall-clock time expressed as seconds since 1970-01-01 00:00 local. */
int64_t ToLocalSeconds(const CivilDate &date, int secondOfDay);

enum class SlotKind : int {
	Scene = 0,         /* switch to an existing OBS scene (cameras, HDMI, layouts...) */
	Source = 1,        /* show an existing OBS input source full screen */
	MediaFile = 2,     /* local audio/video file */
	NetworkStream = 3, /* rtmp/srt/hls/http stream readable by FFmpeg */
	WebPage = 4,       /* browser source, e.g. YouTube embed URL */
};

constexpr int kSlotKindCount = 5;

struct Slot {
	std::string id;
	std::string title;
	int startMinute = 0;      /* minute of day, 0..1439 */
	int durationMinutes = 60; /* 1..1440, may cross midnight */
	uint8_t weekdays = kEveryDay;
	std::optional<CivilDate> date; /* one-off slot; when set, weekdays is ignored */
	SlotKind kind = SlotKind::Scene;
	std::string target; /* scene name, source name, file path or URL */
	bool loop = true;
	bool enabled = true;
};

enum class SlotError {
	None,
	MissingTarget,
	BadStart,
	BadDuration,
	NoWeekdays,
};

SlotError ValidateSlot(const Slot &slot);

struct Occurrence {
	Slot slot; /* copy, so it stays valid after the schedule is edited */
	CivilDate startDate;
	int64_t startSeconds = 0; /* local seconds */
	int64_t endSeconds = 0;

	/* Unique for one airing of one slot, e.g. "news@2026-10-24". */
	std::string Key() const;
};

struct Overlap {
	std::string firstId;
	std::string secondId;
	bool nested = false; /* one slot fully inside the other: intentional insert */
};

class Schedule {
public:
	std::vector<Slot> slots;

	std::vector<Occurrence> OccurrencesStartingOn(const CivilDate &date) const;

	/* The occurrence that should be on air at `nowSeconds`.
	   Rules when several slots cover the same instant:
	     1. a one-off (dated) slot beats a recurring slot
	     2. the most recently started slot wins (lets a short insert
	        interrupt a long block; the block resumes afterwards)
	     3. shorter slot wins, then lowest id (deterministic) */
	std::optional<Occurrence> ActiveAt(int64_t nowSeconds) const;

	/* First instant after `nowSeconds` (within `horizonDays`) where the active
	   occurrence changes. `next` is empty if the change leads to a gap. */
	struct Change {
		int64_t atSeconds = 0;
		std::optional<Occurrence> next;
	};
	std::optional<Change> NextChange(int64_t nowSeconds, int horizonDays = 8) const;

	/* Overlaps between recurring slots across one reference week. */
	std::vector<Overlap> FindOverlaps() const;

	const Slot *Find(const std::string &id) const;
	bool Remove(const std::string &id);
	std::string GenerateId() const;
};

} // namespace station
