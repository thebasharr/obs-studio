#include "scheduler-engine.hpp"

namespace station {

static std::string KeyOf(const std::optional<Occurrence> &occ)
{
	return occ ? occ->Key() : std::string();
}

SchedulerEngine::Decision SchedulerEngine::Tick(const Schedule &schedule, int64_t nowSeconds)
{
	Decision decision;
	if (!enabled_)
		return decision;

	std::optional<Occurrence> active = schedule.ActiveAt(nowSeconds);
	const std::string key = KeyOf(active);

	if (manual_) {
		if (!releaseAtNextSlot_ || key == manualBaselineKey_)
			return decision;
		manual_ = false;
		lastKey_.reset();
	}

	if (!failedKey_.empty() && key == failedKey_) {
		if (retryAt_ == 0 || nowSeconds < retryAt_)
			return decision;
		failedKey_.clear();
		lastKey_.reset();
	}

	if (lastKey_ && *lastKey_ == key)
		return decision;

	lastKey_ = key;
	if (active) {
		decision.type = Decision::Type::Play;
		decision.occurrence = std::move(active);
	} else {
		decision.type = Decision::Type::Fallback;
	}
	return decision;
}

void SchedulerEngine::SetEnabled(bool enabled)
{
	if (enabled_ == enabled)
		return;
	enabled_ = enabled;
	manual_ = false;
	failedKey_.clear();
	lastKey_.reset();
}

void SchedulerEngine::EnterManual(const Schedule &schedule, int64_t nowSeconds)
{
	manual_ = true;
	manualBaselineKey_ = KeyOf(schedule.ActiveAt(nowSeconds));
}

void SchedulerEngine::ResumeAutomatic()
{
	manual_ = false;
	failedKey_.clear();
	lastKey_.reset();
}

void SchedulerEngine::ReportFailure(const std::string &key, int64_t nowSeconds, bool retry)
{
	failedKey_ = key;
	retryAt_ = retry ? nowSeconds + retrySeconds_ : 0;
}

} // namespace station
