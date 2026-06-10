#include "controllers/SchedulerService.h"
#include <combaseapi.h>
#include <cctype>
#include <taskschd.h>
#include <windows.h>
#include <wrl/client.h>
#include <vector>

#pragma comment(lib, "taskschd.lib")
#pragma comment(lib, "ole32.lib")

using Microsoft::WRL::ComPtr;

namespace {

// ─── RAII helpers ─────────────────────────────────────────────────────────────

// Wraps a BSTR with automatic SysFreeString on destruction.
struct BStr {
	BSTR b;
	explicit BStr(const wchar_t *s) : b(SysAllocString(s)) {}
	~BStr() { SysFreeString(b); }
	operator BSTR() const { return b; }
};

// Wraps a VT_BSTR VARIANT with automatic VariantClear on destruction.
struct BStrVar {
	VARIANT v;
	explicit BStrVar(const wchar_t *s)
	{
		VariantInit(&v);
		v.vt			= VT_BSTR;
		v.bstrVal = SysAllocString(s);
	}
	~BStrVar() { VariantClear(&v); }
};

// Manages CoInitializeEx / CoUninitialize for a single call scope.
struct CoGuard {
	HRESULT hr_;
	bool shouldUninit_;
	explicit CoGuard(DWORD model = COINIT_APARTMENTTHREADED)
	{
		hr_						= CoInitializeEx(nullptr, model);
		shouldUninit_ = (hr_ == S_OK);
	}
	~CoGuard()
	{
		if (shouldUninit_)
			CoUninitialize();
	}
	// True if COM is usable (initialized by us, already initialized, or different model).
	bool ok() const { return SUCCEEDED(hr_) || hr_ == RPC_E_CHANGED_MODE; }
};

// ─── helpers ──────────────────────────────────────────────────────────────────

VARIANT emptyVar()
{
	VARIANT v;
	VariantInit(&v);
	return v;
}

// string → wstring (required by Task Scheduler COM APIs which only accept BSTR/wchar_t).
std::wstring toWide(const std::string &s)
{
	if (s.empty())
		return {};
	int len = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
	std::wstring ws(len, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), ws.data(), len);
	return ws;
}

// wstring → string (used when reading strings back out of Task Scheduler COM APIs).
std::string toNarrow(const std::wstring &ws)
{
	if (ws.empty())
		return {};
	int len = WideCharToMultiByte(CP_UTF8, 0, ws.data(), static_cast<int>(ws.size()), nullptr, 0, nullptr, nullptr);
	std::string s(len, '\0');
	WideCharToMultiByte(CP_UTF8, 0, ws.data(), static_cast<int>(ws.size()), s.data(), len, nullptr, nullptr);
	return s;
}

// Builds an ISO 8601 start-boundary string. A fixed past date makes the trigger immediately active.
std::wstring startBoundary(int hour, int minute)
{
	wchar_t buf[32];
	swprintf_s(buf, L"2025-01-01T%02d:%02d:00", hour, minute);
	return buf;
}

// Maps repeat_days (0=Sun ... 6=Sat) to the TASK_DAY_OF_WEEK bitmask.
// TASK_SUNDAY=0x01, TASK_MONDAY=0x02, ..., TASK_SATURDAY=0x40
short daysOfWeekMask(const std::vector<int> &repeatDays)
{
	short mask = 0;
	for (int d : repeatDays)
		mask |= static_cast<short>(1 << d);
	return mask;
}

std::string hrErr(const char *op, HRESULT hr)
{
	return std::format("{} failed (HRESULT 0x{:08X})", op, static_cast<uint32_t>(hr));
}

std::string trim(std::string_view s)
{
	const auto first = s.find_first_not_of(" \t\r\n");
	if (first == std::string_view::npos)
		return {};

	const auto last = s.find_last_not_of(" \t\r\n");
	return std::string(s.substr(first, last - first + 1));
}

std::string quoteCommandLineArgument(const std::string &arg)
{
	std::string quoted;
	quoted.reserve(arg.size() + 2);
	quoted.push_back('"');

	size_t backslashes = 0;
	for (char c : arg) {
		if (c == '\\') {
			++backslashes;
		}
		else if (c == '"') {
			quoted.append(backslashes * 2 + 1, '\\');
			quoted.push_back(c);
			backslashes = 0;
		}
		else {
			quoted.append(backslashes, '\\');
			backslashes = 0;
			quoted.push_back(c);
		}
	}

	quoted.append(backslashes * 2, '\\');
	quoted.push_back('"');
	return quoted;
}

std::vector<std::string> splitCommandLineArguments(std::string_view args)
{
	std::vector<std::string> tokens;
	size_t i = 0;

	while (i < args.size()) {
		while (i < args.size() && std::isspace(static_cast<unsigned char>(args[i])))
			++i;
		if (i >= args.size())
			break;

		std::string token;
		bool inQuotes = false;
		while (i < args.size()) {
			const char c = args[i];
			if (c == '"') {
				inQuotes = !inQuotes;
				++i;
			}
			else if (!inQuotes && std::isspace(static_cast<unsigned char>(c))) {
				break;
			}
			else if (c == '\\' && i + 1 < args.size() && args[i + 1] == '"') {
				token.push_back('"');
				i += 2;
			}
			else {
				token.push_back(c);
				++i;
			}
		}

		tokens.push_back(std::move(token));
	}

	return tokens;
}

// Builds the task display name: "<label> <id>", or "unnamed <id>" when label is empty.
std::wstring taskNameFor(const std::string &label, const std::string &id)
{
	const std::string prefix = label.empty() ? "unnamed" : label;
	return toWide(prefix + " " + id);
}

// Opens the \HanabiAlarm task folder, creating it if it does not exist.
HRESULT getOrCreateAlarmFolder(ITaskService *svc, ITaskFolder **alarmFolder)
{
	ComPtr<ITaskFolder> root;
	HRESULT hr = svc->GetFolder(BStr(L"\\"), &root);
	if (FAILED(hr))
		return hr;

	hr = root->GetFolder(BStr(L"HanabiAlarm"), alarmFolder);
	if (SUCCEEDED(hr))
		return S_OK;

	VARIANT vEmpty = emptyVar();
	return root->CreateFolder(BStr(L"HanabiAlarm"), vEmpty, alarmFolder);
}

} // namespace

namespace alarm::controller {

std::string SchedulerService::buildChromeLaunchArguments(const std::string &youtubeUrl)
{
	return youtubeUrl;
}

std::string SchedulerService::extractYoutubeUrlFromChromeArguments(const std::string &arguments)
{
	constexpr std::string_view incognitoFlag = "--incognito";
	constexpr std::string_view newWindowFlag = "--new-window";
	constexpr std::string_view startMaximizedFlag = "--start-maximized";

	const std::string trimmed = trim(arguments);
	const std::vector<std::string> tokens = splitCommandLineArguments(trimmed);
	if (tokens.empty())
		return {};

	if (tokens.size() >= 3 &&
			std::filesystem::path(tokens[0]).filename().string() == "launch_chrome_incognito.vbs")
		return tokens[2];

	if (!tokens.empty() && tokens[0] != incognitoFlag && tokens[0] != newWindowFlag)
		return trimmed;

	const size_t urlIndex = (tokens.size() >= 3 && tokens[1] == startMaximizedFlag) ? 2 : 1;
	return tokens.size() > urlIndex ? tokens[urlIndex] : std::string{};
}

// ─── syncAlarm ────────────────────────────────────────────────────────────────
std::expected<void, std::string> SchedulerService::syncAlarm(const model::AlarmModel &alarm,
																														 const std::string &chromePath)
{
	CoGuard co;
	if (!co.ok())
		return std::unexpected(hrErr("CoInitializeEx", co.hr_));

	ComPtr<ITaskService> svc;
	HRESULT hr = CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&svc));
	if (FAILED(hr))
		return std::unexpected(hrErr("CoCreateInstance(TaskScheduler)", hr));

	VARIANT v = emptyVar();
	hr				= svc->Connect(v, v, v, v);
	if (FAILED(hr))
		return std::unexpected(hrErr("ITaskService::Connect", hr));

	ComPtr<ITaskFolder> folder;
	hr = getOrCreateAlarmFolder(svc.Get(), &folder);
	if (FAILED(hr))
		return std::unexpected(hrErr("GetOrCreateAlarmFolder", hr));

	ComPtr<ITaskDefinition> taskDef;
	hr = svc->NewTask(0, &taskDef);
	if (FAILED(hr))
		return std::unexpected(hrErr("ITaskService::NewTask", hr));

	// ── Principal: run as current interactive user ──────────────────────────────
	ComPtr<IPrincipal> principal;
	if (SUCCEEDED(taskDef->get_Principal(&principal))) {
		principal->put_LogonType(TASK_LOGON_INTERACTIVE_TOKEN);
		principal->put_RunLevel(TASK_RUNLEVEL_LUA);
	}

	// ── Task settings ───────────────────────────────────────────────────────────
	ComPtr<ITaskSettings> settings;
	if (SUCCEEDED(taskDef->get_Settings(&settings))) {
		settings->put_StartWhenAvailable(VARIANT_FALSE);
		settings->put_Enabled(alarm.enabled ? VARIANT_TRUE : VARIANT_FALSE);
		settings->put_DisallowStartIfOnBatteries(VARIANT_FALSE);
		settings->put_StopIfGoingOnBatteries(VARIANT_FALSE);
	}

	// ── Weekly trigger ──────────────────────────────────────────────────────────
	ComPtr<ITriggerCollection> triggers;
	hr = taskDef->get_Triggers(&triggers);
	if (FAILED(hr))
		return std::unexpected(hrErr("get_Triggers", hr));

	ComPtr<ITrigger> trigger;
	hr = triggers->Create(TASK_TRIGGER_WEEKLY, &trigger);
	if (FAILED(hr))
		return std::unexpected(hrErr("ITriggerCollection::Create(weekly)", hr));

	ComPtr<IWeeklyTrigger> weekly;
	hr = trigger->QueryInterface(IID_PPV_ARGS(&weekly));
	if (FAILED(hr))
		return std::unexpected(hrErr("QueryInterface(IWeeklyTrigger)", hr));

	const std::wstring sb = startBoundary(alarm.hour, alarm.minute);
	weekly->put_StartBoundary(BStr(sb.c_str()));
	weekly->put_DaysOfWeek(daysOfWeekMask(alarm.repeat_days));
	weekly->put_WeeksInterval(1);

	// ── Exec action: open YouTube URL with Chrome ───────────────────────────────
	ComPtr<IActionCollection> actions;
	hr = taskDef->get_Actions(&actions);
	if (FAILED(hr))
		return std::unexpected(hrErr("get_Actions", hr));

	ComPtr<IAction> action;
	hr = actions->Create(TASK_ACTION_EXEC, &action);
	if (FAILED(hr))
		return std::unexpected(hrErr("IActionCollection::Create(exec)", hr));

	ComPtr<IExecAction> exec;
	hr = action->QueryInterface(IID_PPV_ARGS(&exec));
	if (FAILED(hr))
		return std::unexpected(hrErr("QueryInterface(IExecAction)", hr));

	const std::string launchArguments = buildChromeLaunchArguments(alarm.youtube_url);
	exec->put_Path(BStr(toWide(chromePath).c_str()));
	exec->put_Arguments(BStr(toWide(launchArguments).c_str()));

	// ── Register (create or replace) the task ──────────────────────────────────
	BStrVar sddl(L"");
	const std::wstring taskName = taskNameFor(alarm.label, alarm.id);
	ComPtr<IRegisteredTask> registered;
	hr = folder->RegisterTaskDefinition(BStr(taskName.c_str()),
																			taskDef.Get(),
																			TASK_CREATE_OR_UPDATE,
																			emptyVar(),
																			emptyVar(),
																			TASK_LOGON_INTERACTIVE_TOKEN,
																			sddl.v,
																			&registered);
	if (FAILED(hr))
		return std::unexpected(hrErr("RegisterTaskDefinition", hr));

	return {};
}

// ─── deleteTask ───────────────────────────────────────────────────────────────
std::expected<void, std::string> SchedulerService::deleteTask(const model::AlarmModel &alarm)
{
	CoGuard co;
	if (!co.ok())
		return std::unexpected(hrErr("CoInitializeEx", co.hr_));

	ComPtr<ITaskService> svc;
	HRESULT hr = CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&svc));
	if (FAILED(hr))
		return std::unexpected(hrErr("CoCreateInstance(TaskScheduler)", hr));

	VARIANT v = emptyVar();
	hr				= svc->Connect(v, v, v, v);
	if (FAILED(hr))
		return std::unexpected(hrErr("ITaskService::Connect", hr));

	ComPtr<ITaskFolder> root;
	hr = svc->GetFolder(BStr(L"\\"), &root);
	if (FAILED(hr))
		return std::unexpected(hrErr("GetFolder(root)", hr));

	ComPtr<ITaskFolder> folder;
	hr = root->GetFolder(BStr(L"HanabiAlarm"), &folder);
	if (FAILED(hr))
		return {}; // Folder (and task) doesn't exist, nothing to do.

	const std::wstring taskName = taskNameFor(alarm.label, alarm.id);
	hr													= folder->DeleteTask(BStr(taskName.c_str()), 0);
	if (FAILED(hr) && hr != HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND))
		return std::unexpected(hrErr("DeleteTask", hr));

	return {};
}

// ─── cleanAllTasks ────────────────────────────────────────────────────────────
std::expected<void, std::string> SchedulerService::cleanAllTasks()
{
	CoGuard co;
	if (!co.ok())
		return std::unexpected(hrErr("CoInitializeEx", co.hr_));

	ComPtr<ITaskService> svc;
	HRESULT hr = CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&svc));
	if (FAILED(hr))
		return std::unexpected(hrErr("CoCreateInstance(TaskScheduler)", hr));

	VARIANT v = emptyVar();
	hr				= svc->Connect(v, v, v, v);
	if (FAILED(hr))
		return std::unexpected(hrErr("ITaskService::Connect", hr));

	ComPtr<ITaskFolder> root;
	hr = svc->GetFolder(BStr(L"\\"), &root);
	if (FAILED(hr))
		return std::unexpected(hrErr("GetFolder(root)", hr));

	ComPtr<ITaskFolder> folder;
	hr = root->GetFolder(BStr(L"HanabiAlarm"), &folder);
	if (FAILED(hr))
		return {}; // Folder doesn't exist, nothing to do.

	// Enumerate all tasks, including disabled/hidden ones.
	ComPtr<IRegisteredTaskCollection> tasks;
	hr = folder->GetTasks(TASK_ENUM_HIDDEN, &tasks);
	if (FAILED(hr))
		return std::unexpected(hrErr("ITaskFolder::GetTasks", hr));

	LONG count = 0;
	hr				 = tasks->get_Count(&count);
	if (FAILED(hr))
		return std::unexpected(hrErr("IRegisteredTaskCollection::get_Count", hr));

	// Collect names first, avoid mutating the folder while iterating the collection.
	std::vector<std::wstring> names;
	names.reserve(static_cast<size_t>(count));
	for (LONG i = 1; i <= count; ++i) {
		VARIANT idx;
		VariantInit(&idx);
		idx.vt	 = VT_I4;
		idx.lVal = i;
		ComPtr<IRegisteredTask> task;
		if (FAILED(tasks->get_Item(idx, &task)))
			continue;
		BSTR name = nullptr;
		if (SUCCEEDED(task->get_Name(&name))) {
			names.emplace_back(name);
			SysFreeString(name);
		}
	}

	for (const auto &name : names) {
		hr = folder->DeleteTask(BStr(name.c_str()), 0);
		if (FAILED(hr) && hr != HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND))
			return std::unexpected(hrErr("DeleteTask", hr));
	}

	return {};
}

// ─── loadAlarmsFromScheduler ──────────────────────────────────────────────────
std::expected<std::vector<model::AlarmModel>, std::string> SchedulerService::loadAlarmsFromScheduler()
{
	CoGuard co;
	if (!co.ok())
		return std::unexpected(hrErr("CoInitializeEx", co.hr_));

	ComPtr<ITaskService> svc;
	HRESULT hr = CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&svc));
	if (FAILED(hr))
		return std::unexpected(hrErr("CoCreateInstance(TaskScheduler)", hr));

	VARIANT v = emptyVar();
	hr				= svc->Connect(v, v, v, v);
	if (FAILED(hr))
		return std::unexpected(hrErr("ITaskService::Connect", hr));

	ComPtr<ITaskFolder> root;
	hr = svc->GetFolder(BStr(L"\\"), &root);
	if (FAILED(hr))
		return std::unexpected(hrErr("GetFolder(root)", hr));

	ComPtr<ITaskFolder> folder;
	hr = root->GetFolder(BStr(L"HanabiAlarm"), &folder);
	if (FAILED(hr))
		return std::vector<model::AlarmModel>{}; // Folder doesn't exist yet.

	ComPtr<IRegisteredTaskCollection> tasks;
	hr = folder->GetTasks(TASK_ENUM_HIDDEN, &tasks);
	if (FAILED(hr))
		return std::unexpected(hrErr("ITaskFolder::GetTasks", hr));

	LONG count = 0;
	hr				 = tasks->get_Count(&count);
	if (FAILED(hr))
		return std::unexpected(hrErr("get_Count", hr));

	std::vector<model::AlarmModel> result;
	for (LONG i = 1; i <= count; ++i) {
		VARIANT idx;
		VariantInit(&idx);
		idx.vt	 = VT_I4;
		idx.lVal = i;
		ComPtr<IRegisteredTask> task;
		if (FAILED(tasks->get_Item(idx, &task)))
			continue;

		// ── Parse task name → label + id ──────────────────────────────────────
		// Task name format: "<label> <uuid>" or "unnamed <uuid>"
		// UUID is always 36 chars: xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx
		BSTR bName = nullptr;
		if (FAILED(task->get_Name(&bName)) || !bName)
			continue;
		std::wstring wName(bName);
		SysFreeString(bName);

		constexpr size_t kUuidLen = 36;
		if (wName.size() < kUuidLen + 1 || wName[wName.size() - kUuidLen - 1] != L' ')
			continue; // Not our naming format.
		std::wstring wId = wName.substr(wName.size() - kUuidLen);
		if (wId[8] != L'-' || wId[13] != L'-' || wId[18] != L'-' || wId[23] != L'-')
			continue; // UUID sanity check failed.
		std::wstring wLabel = wName.substr(0, wName.size() - kUuidLen - 1);

		model::AlarmModel alarm;
		alarm.id		= toNarrow(wId);
		alarm.label = (wLabel == L"unnamed") ? "" : toNarrow(wLabel);

		// ── Get task definition ────────────────────────────────────────────────
		ComPtr<ITaskDefinition> def;
		if (FAILED(task->get_Definition(&def)))
			continue;

		// Enabled state
		ComPtr<ITaskSettings> taskSettings;
		if (SUCCEEDED(def->get_Settings(&taskSettings))) {
			VARIANT_BOOL en = VARIANT_TRUE;
			taskSettings->get_Enabled(&en);
			alarm.enabled = (en == VARIANT_TRUE);
		}

		// Trigger → hour, minute, repeat_days
		ComPtr<ITriggerCollection> triggers;
		if (SUCCEEDED(def->get_Triggers(&triggers))) {
			LONG tc = 0;
			triggers->get_Count(&tc);
			if (tc >= 1) {
				ComPtr<ITrigger> trigger;
				if (SUCCEEDED(triggers->get_Item(1, &trigger))) {
					BSTR bSb = nullptr;
					if (SUCCEEDED(trigger->get_StartBoundary(&bSb)) && bSb) {
						std::wstring sb(bSb);
						SysFreeString(bSb);
						// Format: "2025-01-01T09:00:00"
						const auto tPos = sb.find(L'T');
						if (tPos != std::wstring::npos && sb.size() >= tPos + 6) {
							alarm.hour	 = std::stoi(sb.substr(tPos + 1, 2));
							alarm.minute = std::stoi(sb.substr(tPos + 4, 2));
						}
					}
					ComPtr<IWeeklyTrigger> weekly;
					if (SUCCEEDED(trigger->QueryInterface(IID_PPV_ARGS(&weekly)))) {
						short dow = 0;
						weekly->get_DaysOfWeek(&dow);
						for (int d = 0; d < 7; ++d)
							if (dow & (1 << d))
								alarm.repeat_days.push_back(d);
					}
				}
			}
		}

		// Action → youtube_url
		ComPtr<IActionCollection> actions;
		if (SUCCEEDED(def->get_Actions(&actions))) {
			LONG ac = 0;
			actions->get_Count(&ac);
			if (ac >= 1) {
				ComPtr<IAction> action;
				if (SUCCEEDED(actions->get_Item(1, &action))) {
					ComPtr<IExecAction> exec;
					if (SUCCEEDED(action->QueryInterface(IID_PPV_ARGS(&exec)))) {
						BSTR bArgs = nullptr;
						if (SUCCEEDED(exec->get_Arguments(&bArgs)) && bArgs) {
							alarm.youtube_url = extractYoutubeUrlFromChromeArguments(toNarrow(std::wstring(bArgs)));
							SysFreeString(bArgs);
						}
					}
				}
			}
		}

		result.push_back(std::move(alarm));
	}

	return result;
}

} // namespace alarm::controller
