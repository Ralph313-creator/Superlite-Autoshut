# Superlite Autoshut

Shuts a Windows PC down when nobody touches the keyboard or mouse for a set time.

Made for pisonet / coin-timer shops where the timer board cuts only the **monitor** when time runs out. Without this, the PC keeps running behind the dark screen until someone turns it off.

- One 24 KB exe with no C runtime, no .NET and no network use.
- Everything is set in one `config.ini`: idle time, countdown length, the action (shutdown, restart, sleep or hibernate), beep, pause, and the warning text. Changes apply as soon as you save.
- About 1 MB of memory for the service and 1.5 MB for the helper, and near-zero CPU. It sleeps until the idle time could be reached and only checks often while the warning is on screen.
- It runs as a Windows service and starts at boot.
- Before shutting down it shows a 60-second warning on top of everything, with beeps. Any mouse move or key press cancels it. A customer still watching a video can just wiggle the mouse.
- Customers can't end it in Task Manager because the helper runs as SYSTEM. If an admin kills it, the service starts it again within a second, and the full countdown runs again.

## Install (one line)

Open **PowerShell as administrator** on the PC and paste:

```powershell
irm https://raw.githubusercontent.com/Ralph313-creator/Superlite-Autoshut/main/install.ps1 | iex
```

That installs with a 10-minute idle time. To choose another time, for example 15 minutes:

```powershell
$env:AUTOSHUT_MINUTES = 15; irm https://raw.githubusercontent.com/Ralph313-creator/Superlite-Autoshut/main/install.ps1 | iex
```

Running it again updates to the newest release and keeps the saved time.

## Settings: config.ini

All settings are in **`C:\Program Files\Superlite Autoshut\config.ini`**. Open Notepad as administrator, edit the file and save it. The new settings apply within a second, with no command or restart needed. The service doesn't poll: Windows wakes it only when a file in that folder changes. Customers can't edit it because the folder is in Program Files.

```ini
[Settings]
; Minutes with no keyboard or mouse use before the warning shows (1-1440)
IdleMinutes=10
; How long the warning counts down, in seconds (10-600)
WarnSeconds=60
; What happens when the countdown ends: shutdown, restart, sleep or hibernate
Action=shutdown
; Beep during the countdown: 1 = on, 0 = off
Beep=1
; 1 = on, 0 = paused (stays installed but never acts)
Enabled=1

[Warning]
; Text on the warning screen. Write \n for a new line.
; Leave Message empty to use the default text for the Action.
Title=No activity detected
Message=
```

- If a value is missing or wrong, that setting uses its default. An unknown `Action` counts as `shutdown`, so a typo never leaves a PC running.
- If `sleep` or `hibernate` is turned off on a PC, it shuts down instead.
- The file is UTF-16, so the warning text can be in any language.
- Installing again or updating keeps your config.ini. When upgrading from 1.0.0, the old idle time moves into it.

## Commands

The program is installed to `C:\Program Files\Superlite Autoshut\SuperliteAutoshut.exe`. Run these in an administrator PowerShell:

| Command | What it does |
|---|---|
| `SuperliteAutoshut.exe install [minutes]` | Install and start it. The default is 10 minutes, from 1 to 1440. |
| `SuperliteAutoshut.exe set <minutes>` | Change `IdleMinutes` in config.ini. It applies right away. |
| `SuperliteAutoshut.exe status` | Show whether it is running and all current settings. |
| `SuperliteAutoshut.exe uninstall` | Remove the service, config.ini and files. |

## Test build

`SuperliteAutoshut-test.exe`, attached to each release, behaves the same but **can never turn the PC off**. The shutdown and sleep calls aren't compiled into it. It writes what it would do to `C:\ProgramData\Superlite Autoshut\test.log`. For quick tests it also reads `[Test]` `IdleSeconds=` from config.ini, 5 or more, in place of the minutes.

## How it works

- The **service** runs as LocalSystem in session 0. It starts the helper on the console desktop and sleeps until the helper exits, the user session changes, or Windows stops it, so it doesn't poll.
- The **helper** runs as SYSTEM in the console session. It reads the session's last keyboard or mouse input time with `GetLastInputInfo`, then sleeps until the warning would be due. While the warning shows, it checks 5 times a second so input cancels the warning quickly.
- When the countdown ends, the helper exits with a special code. The service then runs the configured action. Shutdown and restart use `InitiateSystemShutdownExW` with force, so open games can't block them. Sleep and hibernate use `SetSuspendState`.
- The service also waits on a change notification for its folder. When config.ini is saved, it reloads the settings and restarts the helper with them.
- The PC shuts down only after the full warning has been on screen, even if it was idle long before, for example after the helper was restarted.

Not counted as activity: games, video or sound still playing. If they were counted, a game left open behind a cut monitor would keep the PC on forever.

## Build

You need the Visual Studio 2022 Build Tools (C++).

```
build.cmd        runs the unit tests, then builds dist\SuperliteAutoshut.exe and dist\SuperliteAutoshut-test.exe
build.cmd test   runs the unit tests only
```

Created by Raptech Solution.
