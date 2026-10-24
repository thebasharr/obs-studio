// Standalone tests for the Station Studio scheduling core.
// Build: g++ -std=c++17 -I../core test-station-core.cpp ../core/station-schedule.cpp ../core/scheduler-engine.cpp

#include "scheduler-engine.hpp"
#include "station-schedule.hpp"

#include <cstdio>
#include <cstdlib>

using namespace station;

static int failures = 0;

#define CHECK(cond)                                                          \
	do {                                                                 \
		if (!(cond)) {                                               \
			fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
			failures++;                                          \
		}                                                            \
	} while (0)

static Slot MakeSlot(const char *id, int startMinute, int duration, uint8_t days = kEveryDay)
{
	Slot s;
	s.id = id;
	s.title = id;
	s.startMinute = startMinute;
	s.durationMinutes = duration;
	s.weekdays = days;
	s.kind = SlotKind::Scene;
	s.target = std::string("scene-") + id;
	return s;
}

static int64_t At(int y, int m, int d, int hh, int mm, int ss = 0)
{
	return ToLocalSeconds(CivilDate{y, m, d}, hh * 3600 + mm * 60 + ss);
}

static void TestCalendar()
{
	CHECK(WeekdayOf({1970, 1, 1}) == 4); /* Thursday */
	CHECK(WeekdayOf({2026, 10, 24}) == 6); /* Saturday */
	CHECK(WeekdayOf({2000, 2, 29}) == 2); /* Tuesday */
	CHECK(AddDays({2024, 2, 28}, 1) == (CivilDate{2024, 2, 29}));
	CHECK(AddDays({2023, 12, 31}, 1) == (CivilDate{2024, 1, 1}));
	CHECK(FormatDate({2026, 3, 7}) == "2026-03-07");
	CHECK(ParseDate("2026-03-07") == (CivilDate{2026, 3, 7}));
	CHECK(!ParseDate("2026-02-31"));
	CHECK(!ParseDate("garbage"));
}

static void TestValidation()
{
	Slot s = MakeSlot("a", 0, 60);
	CHECK(ValidateSlot(s) == SlotError::None);
	s.target.clear();
	CHECK(ValidateSlot(s) == SlotError::MissingTarget);
	s = MakeSlot("a", 1440, 60);
	CHECK(ValidateSlot(s) == SlotError::BadStart);
	s = MakeSlot("a", 0, 0);
	CHECK(ValidateSlot(s) == SlotError::BadDuration);
	s = MakeSlot("a", 0, 60, 0);
	CHECK(ValidateSlot(s) == SlotError::NoWeekdays);
	s.date = CivilDate{2026, 1, 1};
	CHECK(ValidateSlot(s) == SlotError::None);
}

static void TestActiveBasic()
{
	Schedule sch;
	sch.slots.push_back(MakeSlot("morning", 6 * 60, 180));
	sch.slots.push_back(MakeSlot("noon", 12 * 60, 60));

	CHECK(!sch.ActiveAt(At(2026, 10, 24, 5, 59, 59)));
	auto a = sch.ActiveAt(At(2026, 10, 24, 6, 0));
	CHECK(a && a->slot.id == "morning");
	CHECK(a && a->Key() == "morning@2026-10-24");
	CHECK(!sch.ActiveAt(At(2026, 10, 24, 9, 0))); /* end is exclusive */
	auto b = sch.ActiveAt(At(2026, 10, 24, 12, 30));
	CHECK(b && b->slot.id == "noon");
}

static void TestWeekdays()
{
	Schedule sch;
	/* Fridays only (bit 5). */
	sch.slots.push_back(MakeSlot("friday", 10 * 60, 60, 1u << 5));
	CHECK(sch.ActiveAt(At(2026, 10, 23, 10, 30))); /* Friday */
	CHECK(!sch.ActiveAt(At(2026, 10, 24, 10, 30))); /* Saturday */
}

static void TestOvernight()
{
	Schedule sch;
	/* Saturdays 23:00 for 3 hours -> runs into Sunday 02:00. */
	sch.slots.push_back(MakeSlot("late", 23 * 60, 180, 1u << 6));
	auto a = sch.ActiveAt(At(2026, 10, 25, 1, 30)); /* Sunday */
	CHECK(a && a->slot.id == "late");
	CHECK(a && a->startDate == (CivilDate{2026, 10, 24}));
	CHECK(!sch.ActiveAt(At(2026, 10, 25, 2, 0)));
}

static void TestNestedInsertResumes()
{
	Schedule sch;
	sch.slots.push_back(MakeSlot("block", 8 * 60, 240)); /* 08:00-12:00 */
	sch.slots.push_back(MakeSlot("news", 10 * 60, 15)); /* 10:00-10:15 */

	auto a = sch.ActiveAt(At(2026, 10, 24, 9, 0));
	CHECK(a && a->slot.id == "block");
	auto b = sch.ActiveAt(At(2026, 10, 24, 10, 5));
	CHECK(b && b->slot.id == "news");
	auto c = sch.ActiveAt(At(2026, 10, 24, 10, 15));
	CHECK(c && c->slot.id == "block");

	auto change = sch.NextChange(At(2026, 10, 24, 9, 0));
	CHECK(change && change->atSeconds == At(2026, 10, 24, 10, 0));
	CHECK(change && change->next && change->next->slot.id == "news");

	auto resume = sch.NextChange(At(2026, 10, 24, 10, 5));
	CHECK(resume && resume->atSeconds == At(2026, 10, 24, 10, 15));
	CHECK(resume && resume->next && resume->next->slot.id == "block");

	auto gap = sch.NextChange(At(2026, 10, 24, 11, 0));
	CHECK(gap && gap->atSeconds == At(2026, 10, 24, 12, 0) && !gap->next);

	auto overlaps = sch.FindOverlaps();
	CHECK(overlaps.size() == 1 && overlaps[0].nested);
}

static void TestOneOffOverride()
{
	Schedule sch;
	sch.slots.push_back(MakeSlot("daily", 0, 1440));
	Slot special = MakeSlot("match", 20 * 60, 120);
	special.date = CivilDate{2026, 10, 24};
	sch.slots.push_back(special);

	auto a = sch.ActiveAt(At(2026, 10, 24, 20, 30));
	CHECK(a && a->slot.id == "match");
	auto b = sch.ActiveAt(At(2026, 10, 25, 20, 30));
	CHECK(b && b->slot.id == "daily");
}

static void TestPartialOverlapAndDisabled()
{
	Schedule sch;
	sch.slots.push_back(MakeSlot("a", 60, 120)); /* 01:00-03:00 */
	sch.slots.push_back(MakeSlot("b", 120, 120)); /* 02:00-04:00 */
	auto overlaps = sch.FindOverlaps();
	CHECK(overlaps.size() == 1 && !overlaps[0].nested);

	auto x = sch.ActiveAt(At(2026, 10, 24, 2, 30));
	CHECK(x && x->slot.id == "b"); /* later start wins */

	sch.slots[1].enabled = false;
	auto y = sch.ActiveAt(At(2026, 10, 24, 2, 30));
	CHECK(y && y->slot.id == "a");
	CHECK(sch.FindOverlaps().empty());
}

static void TestIdsAndRemove()
{
	Schedule sch;
	sch.slots.push_back(MakeSlot("slot-1", 0, 10));
	CHECK(sch.GenerateId() == "slot-2");
	CHECK(sch.Remove("slot-1"));
	CHECK(!sch.Remove("slot-1"));
	CHECK(sch.slots.empty());
}

static void TestEngineSwitchesOnlyOnChange()
{
	Schedule sch;
	sch.slots.push_back(MakeSlot("a", 60, 60)); /* 01:00-02:00 */
	SchedulerEngine eng;

	CHECK(eng.Tick(sch, At(2026, 10, 24, 1, 0)).type == SchedulerEngine::Decision::Type::None); /* disabled */

	eng.SetEnabled(true);
	auto d1 = eng.Tick(sch, At(2026, 10, 24, 1, 0));
	CHECK(d1.type == SchedulerEngine::Decision::Type::Play && d1.occurrence->slot.id == "a");
	CHECK(eng.Tick(sch, At(2026, 10, 24, 1, 0, 1)).type == SchedulerEngine::Decision::Type::None);

	auto d2 = eng.Tick(sch, At(2026, 10, 24, 2, 0));
	CHECK(d2.type == SchedulerEngine::Decision::Type::Fallback);
	CHECK(eng.Tick(sch, At(2026, 10, 24, 2, 0, 1)).type == SchedulerEngine::Decision::Type::None);

	eng.ForceRefresh();
	CHECK(eng.Tick(sch, At(2026, 10, 24, 2, 0, 2)).type == SchedulerEngine::Decision::Type::Fallback);
}

static void TestEngineManualHold()
{
	Schedule sch;
	sch.slots.push_back(MakeSlot("a", 60, 60));
	sch.slots.push_back(MakeSlot("b", 120, 60));
	SchedulerEngine eng;
	eng.SetEnabled(true);
	eng.Tick(sch, At(2026, 10, 24, 1, 0));

	eng.EnterManual(sch, At(2026, 10, 24, 1, 10));
	CHECK(eng.Manual());
	/* Hold indefinitely, even across slot boundaries. */
	CHECK(eng.Tick(sch, At(2026, 10, 24, 2, 0)).type == SchedulerEngine::Decision::Type::None);

	eng.ResumeAutomatic();
	auto d = eng.Tick(sch, At(2026, 10, 24, 2, 5));
	CHECK(d.type == SchedulerEngine::Decision::Type::Play && d.occurrence->slot.id == "b");
}

static void TestEngineManualReleaseAtNextSlot()
{
	Schedule sch;
	sch.slots.push_back(MakeSlot("a", 60, 60));
	sch.slots.push_back(MakeSlot("b", 120, 60));
	SchedulerEngine eng;
	eng.SetEnabled(true);
	eng.SetReleaseManualAtNextSlot(true);
	eng.Tick(sch, At(2026, 10, 24, 1, 0));

	eng.EnterManual(sch, At(2026, 10, 24, 1, 10));
	CHECK(eng.Tick(sch, At(2026, 10, 24, 1, 59)).type == SchedulerEngine::Decision::Type::None);
	auto d = eng.Tick(sch, At(2026, 10, 24, 2, 0));
	CHECK(d.type == SchedulerEngine::Decision::Type::Play && d.occurrence->slot.id == "b");
	CHECK(!eng.Manual());
}

static void TestEngineFailureRetry()
{
	Schedule sch;
	sch.slots.push_back(MakeSlot("a", 60, 60));
	SchedulerEngine eng(30);
	eng.SetEnabled(true);
	auto d = eng.Tick(sch, At(2026, 10, 24, 1, 0));
	CHECK(d.type == SchedulerEngine::Decision::Type::Play);

	eng.ReportFailure(d.occurrence->Key(), At(2026, 10, 24, 1, 0), true);
	CHECK(eng.Tick(sch, At(2026, 10, 24, 1, 0, 29)).type == SchedulerEngine::Decision::Type::None);
	CHECK(eng.Tick(sch, At(2026, 10, 24, 1, 0, 30)).type == SchedulerEngine::Decision::Type::Play);

	eng.ReportFailure(d.occurrence->Key(), At(2026, 10, 24, 1, 1), false);
	CHECK(eng.Tick(sch, At(2026, 10, 24, 1, 30)).type == SchedulerEngine::Decision::Type::None);
	/* New slot boundary clears the failure. */
	CHECK(eng.Tick(sch, At(2026, 10, 24, 2, 0)).type == SchedulerEngine::Decision::Type::Fallback);
}

int main()
{
	TestCalendar();
	TestValidation();
	TestActiveBasic();
	TestWeekdays();
	TestOvernight();
	TestNestedInsertResumes();
	TestOneOffOverride();
	TestPartialOverlapAndDisabled();
	TestIdsAndRemove();
	TestEngineSwitchesOnlyOnChange();
	TestEngineManualHold();
	TestEngineManualReleaseAtNextSlot();
	TestEngineFailureRetry();

	if (failures) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return EXIT_FAILURE;
	}
	printf("All station-studio core tests passed\n");
	return EXIT_SUCCESS;
}
