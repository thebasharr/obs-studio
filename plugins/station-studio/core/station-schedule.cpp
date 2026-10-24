#include "station-schedule.hpp"

#include <algorithm>
#include <cstdio>
#include <set>
#include <tuple>

namespace station {

/* Civil date algorithms from Howard Hinnant (public domain). */
int64_t DaysFromCivil(const CivilDate &date)
{
	int64_t y = date.year;
	const unsigned m = (unsigned)date.month;
	const unsigned d = (unsigned)date.day;
	y -= m <= 2;
	const int64_t era = (y >= 0 ? y : y - 399) / 400;
	const unsigned yoe = (unsigned)(y - era * 400);
	const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
	const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	return era * 146097 + (int64_t)doe - 719468;
}

CivilDate CivilFromDays(int64_t z)
{
	z += 719468;
	const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
	const unsigned doe = (unsigned)(z - era * 146097);
	const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
	const int64_t y = (int64_t)yoe + era * 400;
	const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
	const unsigned mp = (5 * doy + 2) / 153;
	const unsigned d = doy - (153 * mp + 2) / 5 + 1;
	const unsigned m = mp < 10 ? mp + 3 : mp - 9;
	return CivilDate{(int)(y + (m <= 2)), (int)m, (int)d};
}

int WeekdayOf(const CivilDate &date)
{
	const int64_t z = DaysFromCivil(date);
	return (int)(z >= -4 ? (z + 4) % 7 : (z + 5) % 7 + 6);
}

CivilDate AddDays(const CivilDate &date, int days)
{
	return CivilFromDays(DaysFromCivil(date) + days);
}

std::string FormatDate(const CivilDate &date)
{
	char buf[16];
	snprintf(buf, sizeof(buf), "%04d-%02d-%02d", date.year, date.month, date.day);
	return buf;
}

std::optional<CivilDate> ParseDate(const std::string &text)
{
	CivilDate d;
	char extra;
	if (sscanf(text.c_str(), "%d-%d-%d%c", &d.year, &d.month, &d.day, &extra) != 3)
		return std::nullopt;
	if (d.month < 1 || d.month > 12 || d.day < 1 || d.day > 31)
		return std::nullopt;
	/* Reject dates like 2026-02-31 by round-tripping. */
	if (CivilFromDays(DaysFromCivil(d)) != d)
		return std::nullopt;
	return d;
}

int64_t ToLocalSeconds(const CivilDate &date, int secondOfDay)
{
	return DaysFromCivil(date) * kSecondsPerDay + secondOfDay;
}

SlotError ValidateSlot(const Slot &slot)
{
	if (slot.target.empty())
		return SlotError::MissingTarget;
	if (slot.startMinute < 0 || slot.startMinute >= kMinutesPerDay)
		return SlotError::BadStart;
	if (slot.durationMinutes < 1 || slot.durationMinutes > kMinutesPerDay)
		return SlotError::BadDuration;
	if (!slot.date && (slot.weekdays & kEveryDay) == 0)
		return SlotError::NoWeekdays;
	return SlotError::None;
}

std::string Occurrence::Key() const
{
	return slot.id + "@" + FormatDate(startDate);
}

std::vector<Occurrence> Schedule::OccurrencesStartingOn(const CivilDate &date) const
{
	std::vector<Occurrence> out;
	const int weekday = WeekdayOf(date);

	for (const Slot &slot : slots) {
		if (!slot.enabled || ValidateSlot(slot) != SlotError::None)
			continue;

		const bool airs = slot.date ? *slot.date == date : (slot.weekdays & (1u << weekday)) != 0;
		if (!airs)
			continue;

		Occurrence occ;
		occ.slot = slot;
		occ.startDate = date;
		occ.startSeconds = ToLocalSeconds(date, slot.startMinute * 60);
		occ.endSeconds = occ.startSeconds + (int64_t)slot.durationMinutes * 60;
		out.push_back(std::move(occ));
	}

	std::sort(out.begin(), out.end(),
		  [](const Occurrence &a, const Occurrence &b) { return a.startSeconds < b.startSeconds; });
	return out;
}

static bool Beats(const Occurrence &a, const Occurrence &b)
{
	const bool aDated = a.slot.date.has_value();
	const bool bDated = b.slot.date.has_value();
	if (aDated != bDated)
		return aDated;
	if (a.startSeconds != b.startSeconds)
		return a.startSeconds > b.startSeconds;
	const int64_t aLen = a.endSeconds - a.startSeconds;
	const int64_t bLen = b.endSeconds - b.startSeconds;
	if (aLen != bLen)
		return aLen < bLen;
	return a.slot.id < b.slot.id;
}

std::optional<Occurrence> Schedule::ActiveAt(int64_t nowSeconds) const
{
	int64_t day = nowSeconds >= 0 ? nowSeconds / kSecondsPerDay : (nowSeconds - kSecondsPerDay + 1) / kSecondsPerDay;
	const CivilDate today = CivilFromDays(day);

	std::optional<Occurrence> best;
	/* Slots are at most 24h long, so only today's and yesterday's can be live. */
	for (int offset = -1; offset <= 0; offset++) {
		for (Occurrence &occ : OccurrencesStartingOn(AddDays(today, offset))) {
			if (occ.startSeconds <= nowSeconds && nowSeconds < occ.endSeconds) {
				if (!best || Beats(occ, *best))
					best = std::move(occ);
			}
		}
	}
	return best;
}

std::optional<Schedule::Change> Schedule::NextChange(int64_t nowSeconds, int horizonDays) const
{
	const std::optional<Occurrence> current = ActiveAt(nowSeconds);
	const std::string currentKey = current ? current->Key() : std::string();

	const int64_t day = nowSeconds / kSecondsPerDay;
	const CivilDate today = CivilFromDays(day);

	std::set<int64_t> boundaries;
	for (int offset = -1; offset <= horizonDays; offset++) {
		for (const Occurrence &occ : OccurrencesStartingOn(AddDays(today, offset))) {
			if (occ.startSeconds > nowSeconds)
				boundaries.insert(occ.startSeconds);
			if (occ.endSeconds > nowSeconds)
				boundaries.insert(occ.endSeconds);
		}
	}

	for (int64_t t : boundaries) {
		std::optional<Occurrence> next = ActiveAt(t);
		const std::string nextKey = next ? next->Key() : std::string();
		if (nextKey != currentKey)
			return Change{t, std::move(next)};
	}
	return std::nullopt;
}

std::vector<Overlap> Schedule::FindOverlaps() const
{
	/* Any Sunday works as a reference week; 2023-01-01 is a Sunday. */
	const CivilDate sunday{2023, 1, 1};

	std::vector<Occurrence> week;
	for (int i = 0; i < 7; i++) {
		for (Occurrence &occ : OccurrencesStartingOn(AddDays(sunday, i))) {
			if (!occ.slot.date)
				week.push_back(std::move(occ));
		}
	}

	/* Wrap Saturday night slots into the following Sunday morning. */
	const int64_t weekSeconds = 7 * kSecondsPerDay;
	const size_t base = week.size();
	for (size_t i = 0; i < base; i++) {
		if (week[i].endSeconds > ToLocalSeconds(sunday, 0) + weekSeconds) {
			Occurrence wrapped = week[i];
			wrapped.startSeconds -= weekSeconds;
			wrapped.endSeconds -= weekSeconds;
			week.push_back(std::move(wrapped));
		}
	}

	std::set<std::tuple<std::string, std::string>> seen;
	std::vector<Overlap> out;
	for (size_t i = 0; i < week.size(); i++) {
		for (size_t j = i + 1; j < week.size(); j++) {
			const Occurrence &a = week[i];
			const Occurrence &b = week[j];
			if (a.slot.id == b.slot.id)
				continue;
			if (a.startSeconds >= b.endSeconds || b.startSeconds >= a.endSeconds)
				continue;

			std::string first = std::min(a.slot.id, b.slot.id);
			std::string second = std::max(a.slot.id, b.slot.id);
			if (!seen.insert({first, second}).second)
				continue;

			const bool aInB = a.startSeconds >= b.startSeconds && a.endSeconds <= b.endSeconds;
			const bool bInA = b.startSeconds >= a.startSeconds && b.endSeconds <= a.endSeconds;
			out.push_back(Overlap{first, second, aInB || bInA});
		}
	}
	return out;
}

const Slot *Schedule::Find(const std::string &id) const
{
	for (const Slot &slot : slots) {
		if (slot.id == id)
			return &slot;
	}
	return nullptr;
}

bool Schedule::Remove(const std::string &id)
{
	auto it = std::remove_if(slots.begin(), slots.end(), [&](const Slot &s) { return s.id == id; });
	if (it == slots.end())
		return false;
	slots.erase(it, slots.end());
	return true;
}

std::string Schedule::GenerateId() const
{
	for (int n = (int)slots.size() + 1;; n++) {
		std::string id = "slot-" + std::to_string(n);
		if (!Find(id))
			return id;
	}
}

} // namespace station
