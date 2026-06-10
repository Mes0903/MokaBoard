# HanabiAlarm

A Windows desktop alarm application built with C++23, Dear ImGui, and Vulkan.

Each alarm stores a time, label, repeat days, and a YouTube URL. When triggered, Windows Task Scheduler opens the URL in a new maximized Chrome window (even if the application itself is not running). The window minimises to the system tray rather than closing.

## Building

**Requirements:** MSVC (C++23), CMake ≥ 3.20, Vulkan SDK.

```bash
cmake -S . -B build
cmake --build build --config Release
```

The build step copies `data/` (JSON storage) next to the executable automatically.

## Usage

- Launch the application.
- Open Settings and set `chrome_path` to your `chrome.exe` if needed.
- Add an alarm (time, label, repeat days, YouTube URL).
- Confirm. The application persists JSON and synchronises the corresponding Task Scheduler task immediately.

Notes:

- Alarms are executed by Task Scheduler, so they can fire even when the UI is not running.
- The UI minimises to tray instead of exiting, depending on `close_to_tray`.

## Project Structure

```
HanabiAlarm/
├── include/
│   ├── Application.h
│   ├── models/
│   │   ├── AlarmModel.h
│   │   └── SettingsModel.h
│   ├── controllers/
│   │   ├── AlarmController.h
│   │   ├── PersistenceService.h
│   │   └── SchedulerService.h
│   └── views/
│       └── AlarmView.h
├── src/
│   ├── Application.cpp
│   ├── main.cpp
│   ├── controllers/
│   │   ├── AlarmController.cpp
│   │   ├── PersistenceService.cpp
│   │   └── SchedulerService.cpp
│   └── views/
│       └── AlarmView.cpp
└── data/
    ├── alarms.json
    └── settings.json
```

## Architecture

The codebase uses a layered architecture. Dependencies only flow **downward**: View knows about Controller, Controller knows about Model, nothing flows back up.

```
┌──────────────────────────────────────────────────────┐
│                    Application                       │  Win32 + Vulkan lifecycle
│             owns Controller and View                 │
└────────────────┬───────────────────┬─────────────────┘
                 │                   │
                 ▼                   ▼
┌──────────────────────────┐  ┌──────────────────┐
│  Controller layer        │◄─│      View        │  View holds a ref to AlarmController
│                          │  │                  │  Controller does not know View exists
│  ┌────────────────────┐  │  └──────────────────┘
│  │  AlarmController   │  │
│  │  (business logic,  │  │
│  │   owns state)      │  │
│  └─────────┬──────────┘  │
│            │ calls       │
│  ┌─────────▼──────────┐  │
│  │ PersistenceService │  │  Pure I/O utility; reads/writes JSON files
│  │ (static, no state) │  │
│  └────────────────────┘  │
│  ┌────────────────────┐  │
│  │ SchedulerService   │  │  COM API wrapper; creates, updates, and
│  │ (static, no state) │  │  deletes Windows Task Scheduler tasks
│  └────────────────────┘  │
└────────────┬─────────────┘
             │ uses as value objects
             ▼
┌──────────────────────────┐
│       Model layer        │  Plain data structs; no knowledge of anything above
│  AlarmModel / Settings   │
└──────────────────────────┘
```

### Model & Controller

**Model** (`alarm::model`) is intentionally thin. `AlarmModel` and `SettingsModel` are plain structs. They carry data and know how to serialise themselves to and from JSON, but contain no business logic.

```cpp
struct AlarmModel {
    std::string id;
    std::string label;
    int hour, minute;
    bool enabled;
    std::vector<int> repeat_days;
    std::string youtube_url;

    static AlarmModel fromJson(const nlohmann::json &);
    nlohmann::json toJson() const;
};
```

`fromJson` and `toJson` live on the struct because they describe the data's own representation. This is self description, not business logic, and it keeps serialisation code close to the fields it touches.

**Controller** (`alarm::controller`) is where all business logic lives. `AlarmController` is the single source of truth for runtime state: it owns the in memory `alarms_` vector and the `settings_` value.

```
AlarmController
├── alarms(): read-only access to the alarm list
├── addAlarm(): assigns a UUID v4 id, appends, sorts, persists, syncs scheduler
├── updateAlarm(): finds by id, overwrites, sorts, persists, syncs scheduler
├── deleteAlarm(): removes scheduler task, removes by id, persists
├── setEnabled(): flips enabled flag, persists, syncs scheduler
├── cleanAll(): deletes all tasks in \HanabiAlarm\ folder, clears list, persists
├── settings(): read-only access to settings
└── saveSettings(): replaces settings, persists
```

Every mutation follows the same pattern: mutate in memory, sort (if needed), persist to disk, sync Task Scheduler. The JSON files and Task Scheduler are always kept in sync; there is no separate save step visible to the rest of the application.

`PersistenceService` is a pure I/O utility for JSON reads and writes. `SchedulerService` is a pure COM API wrapper for Task Scheduler operations. Both are static and stateless, deliberately separated from `AlarmController` so that each concern can change independently.

```
PersistenceService (static methods, no state)
├── loadAlarms() / saveAlarms()
└── loadSettings() / saveSettings()
```

Alarm IDs are UUID v4 strings generated entirely in process using `<random>`. The 128 bit random value has version bits (4) and variant bits (RFC 4122) forced into the appropriate positions before being formatted as a hex string.

### View & Controller

**View** (`alarm::view`) holds a non owning reference to `AlarmController` and is the only layer that directly calls its mutation methods. The relationship is one directional: View reads from and writes to Controller; Controller never calls back into View.

```cpp
class AlarmView {
    controller::AlarmController &ctrl_;   // reference, not pointer; always valid
    ...
};
```

Because this application uses **Dear ImGui** (immediate mode), there is no observer pattern, no data binding, and no "model changed then notify view" mechanism. Instead, every frame the View reads the current state directly from the Controller and rebuilds the entire UI from scratch:

```
Every frame:
  ctrl_.alarms()        -> iterate and render each row
  ctrl_.settings()      -> populate the settings dialog fields
  ctrl_.setEnabled()    -> called immediately when the ON/OFF button is clicked
  ctrl_.addAlarm()      -> called when the add dialog is confirmed
  ctrl_.updateAlarm()   -> called when the edit dialog is confirmed
  ctrl_.deleteAlarm()   -> called when the delete confirm dialog is accepted
  ctrl_.cleanAll()      -> called when the Clean All confirm dialog is accepted
```

**Communicating upward to Application**: the View needs to signal two things to `Application` without holding a reference to it: window resize requests (from Settings) and close decisions (minimise vs. exit). Rather than callbacks or events, it exposes two poll methods:

```cpp
bool pollWindowResize(int &w, int &h);   // true once after a resize is requested
CloseDecision pollCloseDecision();        // None / Minimize / Exit
```

`Application::mainLoop_()` calls these every frame. This keeps the dependency direction clean: View still has no knowledge of Application.

**Dialog state** is owned entirely by the View as private member variables (`editHour_`, `editLabel_`, `pendingDeleteId_`, etc.). The Controller is only touched on confirmed actions, never during intermediate editing. This means a user can open the edit dialog, change values, and cancel. The Controller state is never dirtied.

## Data Files

Stored in `data/` next to the executable. Created with default values on first run if absent.

**`data/alarms.json`**
```json
{
  "alarms": [
    {
      "id": "550e8400-e29b-4000-8000-446655440000",
      "label": "Morning",
      "hour": 7,
      "minute": 30,
      "enabled": true,
      "repeat_days": [1, 2, 3, 4, 5],
      "youtube_url": ""
    }
  ]
}
```

**`data/settings.json`**
```json
{
  "chrome_path": "C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe",
  "close_to_tray": true,
  "suppress_minimize_hint": false,
  "window_width": 1280,
  "window_height": 800
}
```

`repeat_days` uses `0 = Sunday … 6 = Saturday`. An empty array means the alarm fires every day.

## Task Scheduler Details

This project uses the Windows Task Scheduler COM API (`taskschd.h`) to create and manage tasks. Documentation:

- Task Scheduler start page: https://learn.microsoft.com/windows/win32/taskschd/task-scheduler-start-page
- Task Scheduler objects and interfaces: https://learn.microsoft.com/windows/win32/taskschd/task-scheduler-objects

Operational expectations (high level):

- Each alarm corresponds to a scheduled task that launches Chrome with `--new-window --start-maximized` and the configured URL.
- Enabling or disabling an alarm is expected to keep the scheduled task state in sync.
- Deleting an alarm is expected to remove the corresponding scheduled task.
- `cleanAll()` is expected to remove all tasks managed by this application and clear local JSON state.

## Troubleshooting

- Alarm triggers but nothing opens
  - Verify `chrome_path` points to a valid `chrome.exe`.
  - Verify the URL is non empty and valid.
  - Open Task Scheduler and check the task history and last run result.
  - Task Scheduler UI is covered by the same documentation entry point: https://learn.microsoft.com/windows/win32/taskschd/task-scheduler-start-page

- Tasks are not created or updated
  - Verify the Task Scheduler service is running.
  - Verify the application has sufficient permissions to register tasks.
  - Verify Windows policies are not blocking task registration in your environment.
  - COM API reference entry point: https://learn.microsoft.com/windows/win32/taskschd/task-scheduler-start-page

- Data does not persist
  - Verify `data/` is present next to the executable.
  - Verify `alarms.json` and `settings.json` are writable (not read only, not blocked by controlled folder access).
