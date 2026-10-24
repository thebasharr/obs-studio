/******************************************************************************
    Station Studio - automation state machine
    Decides *when* to switch. It never touches OBS directly; the caller turns
    decisions into scene switches. Pure C++17 so it can be unit tested.
******************************************************************************/

#pragma once

#include "station-schedule.hpp"

#include <optional>
#include <string>

namespace station {

class SchedulerEngine {
public:
	struct Decision {
		enum class Type {
			None,     /* keep whatever is on air */
			Play,     /* put `occurrence` on air */
			Fallback, /* nothing scheduled: put the fallback (filler) on air */
		};
		Type type = Type::None;
		std::optional<Occurrence> occurrence;
	};

	explicit SchedulerEngine(int retrySeconds = 30) : retrySeconds_(retrySeconds) {}

	/* Call periodically (e.g. every second). Emits a decision only when the
	   scheduled content changes, so it never fights an operator who manually
	   switched scenes in OBS; it takes over again at the next slot boundary. */
	Decision Tick(const Schedule &schedule, int64_t nowSeconds);

	void SetEnabled(bool enabled);
	bool Enabled() const { return enabled_; }

	/* Explicit manual override: automation stops switching until
	   ResumeAutomatic(), or until the next slot if release-at-next-slot is on. */
	void EnterManual(const Schedule &schedule, int64_t nowSeconds);
	void ResumeAutomatic();
	bool Manual() const { return manual_; }

	void SetReleaseManualAtNextSlot(bool release) { releaseAtNextSlot_ = release; }
	bool ReleaseManualAtNextSlot() const { return releaseAtNextSlot_; }

	/* Playback of `key` failed (missing file, dead URL, media ended early).
	   The caller has already put the fallback on air. With `retry` the same
	   occurrence is attempted again after retrySeconds. */
	void ReportFailure(const std::string &key, int64_t nowSeconds, bool retry);

	/* Re-evaluate on next tick even if the same occurrence is still active
	   (e.g. its target was edited, or the scene collection changed). */
	void ForceRefresh() { lastKey_.reset(); }

	/* Key on air according to automation; empty string = fallback/gap. */
	const std::optional<std::string> &CurrentKey() const { return lastKey_; }

private:
	int retrySeconds_;
	bool enabled_ = false;
	bool manual_ = false;
	bool releaseAtNextSlot_ = false;
	std::string manualBaselineKey_;
	std::optional<std::string> lastKey_;
	std::string failedKey_;
	int64_t retryAt_ = 0; /* 0 = never retry */
};

} // namespace station
