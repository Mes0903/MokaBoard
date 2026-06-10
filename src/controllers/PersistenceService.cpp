#include "controllers/PersistenceService.h"
#include <fstream>

namespace alarm::controller {

// ─── loadAlarms ──────────────────────────────────────────────────────────────
std::vector<model::AlarmModel> PersistenceService::loadAlarms()
{
	std::ifstream f(ALARMS_PATH);
	if (!f.is_open())
		return {};

	try {
		auto j = nlohmann::json::parse(f);
		std::vector<model::AlarmModel> result;
		for (const auto &item : j.at("alarms"))
			result.push_back(model::AlarmModel::fromJson(item));
		return result;
	} catch (...) {
		return {};
	}
}

// ─── saveAlarms ──────────────────────────────────────────────────────────────
void PersistenceService::saveAlarms(const std::vector<model::AlarmModel> &alarms)
{
	nlohmann::json j;
	j["alarms"] = nlohmann::json::array();
	for (const auto &a : alarms)
		j["alarms"].push_back(a.toJson());
	std::ofstream f(ALARMS_PATH);
	f << j.dump(2);
}

// ─── loadSettings ────────────────────────────────────────────────────────────
model::SettingsModel PersistenceService::loadSettings()
{
	std::ifstream f(SETTINGS_PATH);
	if (!f.is_open())
		return {};

	try {
		return model::SettingsModel::fromJson(nlohmann::json::parse(f));
	} catch (...) {
		return {};
	}
}

// ─── saveSettings ────────────────────────────────────────────────────────────
void PersistenceService::saveSettings(const model::SettingsModel &settings)
{
	std::ofstream f(SETTINGS_PATH);
	f << settings.toJson().dump(2);
}

} // namespace alarm::controller
