#include "controllers/AlarmController.h"
#include "controllers/PersistenceService.h"
#include "controllers/SchedulerService.h"
#include <iostream>
#include <random>

namespace alarm::controller {

// ─── load ─────────────────────────────────────────────────────────────────────
void AlarmController::load()
{
	settings_ = PersistenceService::loadSettings();

	if (std::filesystem::exists("data/alarms.json")) {
		// JSON exists → it is the source of truth; re-sync scheduler to match.
		alarms_ = PersistenceService::loadAlarms();
		for (const auto &a : alarms_) {
			if (auto res = SchedulerService::syncAlarm(a, settings_.chrome_path); !res)
				std::cerr << "[SchedulerService] load syncAlarm: " << res.error() << '\n';
		}
	}
	else {
		// JSON missing → recover from Task Scheduler and persist.
		if (auto scheduled = SchedulerService::loadAlarmsFromScheduler()) {
			alarms_ = std::move(*scheduled);
			persist_();
		}
		else {
			std::cerr << "[SchedulerService] loadAlarmsFromScheduler: " << scheduled.error() << '\n';
		}
	}

	sortAlarms_();
}

// ─── alarms ───────────────────────────────────────────────────────────────────
const std::vector<model::AlarmModel> &AlarmController::alarms() const noexcept { return alarms_; }

// ─── addAlarm ─────────────────────────────────────────────────────────────────
void AlarmController::addAlarm(model::AlarmModel alarm)
{
	alarm.id = generateId_();
	if (auto res = SchedulerService::syncAlarm(alarm, settings_.chrome_path); !res)
		std::cerr << "[SchedulerService] addAlarm: " << res.error() << '\n';
	alarms_.push_back(std::move(alarm));
	sortAlarms_();
	persist_();
}

// ─── updateAlarm ──────────────────────────────────────────────────────────────
void AlarmController::updateAlarm(const model::AlarmModel &alarm)
{
	auto it = std::ranges::find(alarms_, alarm.id, &model::AlarmModel::id);
	if (it != alarms_.end()) {
		// If the label changed the task name changes too, so delete the old task first.
		if (it->label != alarm.label) {
			if (auto res = SchedulerService::deleteTask(*it); !res)
				std::cerr << "[SchedulerService] updateAlarm(deleteOld): " << res.error() << '\n';
		}
		*it = alarm;
		if (auto res = SchedulerService::syncAlarm(alarm, settings_.chrome_path); !res)
			std::cerr << "[SchedulerService] updateAlarm: " << res.error() << '\n';
		sortAlarms_();
		persist_();
	}
}

// ─── deleteAlarm ──────────────────────────────────────────────────────────────
void AlarmController::deleteAlarm(const std::string &id)
{
	auto it = std::ranges::find(alarms_, id, &model::AlarmModel::id);
	if (it != alarms_.end()) {
		if (auto res = SchedulerService::deleteTask(*it); !res)
			std::cerr << "[SchedulerService] deleteAlarm: " << res.error() << '\n';
	}
	std::erase_if(alarms_, [&](const auto &a) { return a.id == id; });
	persist_();
}

// ─── setEnabled ───────────────────────────────────────────────────────────────
void AlarmController::setEnabled(const std::string &id, bool enabled)
{
	auto it = std::ranges::find(alarms_, id, &model::AlarmModel::id);
	if (it != alarms_.end()) {
		it->enabled = enabled;
		if (auto res = SchedulerService::syncAlarm(*it, settings_.chrome_path); !res)
			std::cerr << "[SchedulerService] setEnabled: " << res.error() << '\n';
		persist_();
	}
}

// ─── cleanAll ─────────────────────────────────────────────────────────────────
void AlarmController::cleanAll()
{
	if (auto res = SchedulerService::cleanAllTasks(); !res)
		std::cerr << "[SchedulerService] cleanAll: " << res.error() << '\n';
	alarms_.clear();
	persist_();
}

// ─── settings ─────────────────────────────────────────────────────────────────
const model::SettingsModel &AlarmController::settings() const noexcept { return settings_; }

// ─── saveSettings ─────────────────────────────────────────────────────────────
void AlarmController::saveSettings(model::SettingsModel s)
{
	settings_ = std::move(s);
	PersistenceService::saveSettings(settings_);
}

// ─── persist_ ─────────────────────────────────────────────────────────────────
void AlarmController::persist_() { PersistenceService::saveAlarms(alarms_); }

// ─── sortAlarms_ ──────────────────────────────────────────────────────────────
void AlarmController::sortAlarms_()
{
	std::ranges::sort(
			alarms_, [](const auto &a, const auto &b) { return a.hour != b.hour ? a.hour < b.hour : a.minute < b.minute; });
}

// ─── generateId_ ──────────────────────────────────────────────────────────────
std::string AlarmController::generateId_()
{
	std::random_device rd;
	std::mt19937_64 gen(rd());
	std::uniform_int_distribution<uint64_t> dis;

	uint64_t hi = dis(gen);
	uint64_t lo = dis(gen);

	// UUID v4: set version bits
	hi = (hi & 0xFFFFFFFFFFFF0FFFULL) | 0x0000000000004000ULL;
	// Set variant bits
	lo = (lo & 0x3FFFFFFFFFFFFFFFULL) | 0x8000000000000000ULL;

	std::ostringstream ss;
	ss << std::hex << std::setfill('0') << std::setw(8) << (hi >> 32) << '-' << std::setw(4) << ((hi >> 16) & 0xFFFF)
		 << '-' << std::setw(4) << (hi & 0xFFFF) << '-' << std::setw(4) << (lo >> 48) << '-' << std::setw(12)
		 << (lo & 0x0000FFFFFFFFFFFFULL);
	return ss.str();
}

} // namespace alarm::controller
