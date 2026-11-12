# Superlite Autoshut

Shuts a Windows PC down when nobody touches the keyboard or mouse for a set time.

Made for pisonet / coin-timer shops where the timer board cuts only the **monitor** when time runs out. Without this, the PC keeps running behind the dark screen until someone turns it off.

- One 18 KB exe with no C runtime, no .NET and no network use.
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

## Commands

The program is installed to `C:\Program Files\Superlite Autoshut\SuperliteAutoshut.exe`. Run these in an administrator PowerShell:

| Command | What it does |
|---|---|
| `SuperliteAutoshut.exe install [minutes]` | Install and start it. The default is 10 minutes, from 1 to 1440. |
| `SuperliteAutoshut.exe set <minutes>` | Change the idle time. It takes effect immediately. |
| `SuperliteAutoshut.exe status` | Show whether it is running and the idle time. |
| `SuperliteAutoshut.exe uninstall` | Remove the service, settings and files. |

Settings are kept in `HKLM\SOFTWARE\Superlite Autoshut`: `IdleMinutes`, and optionally `WarnSeconds` from 10 to 600, default 60.

## Test build

`SuperliteAutoshut-test.exe`, attached to each release, behaves the same but **can never shut the PC down**. The shutdown call isn't compiled into it. It writes what it would do to `C:\ProgramData\Superlite Autoshut\test.log`. For quick tests it also reads the `TestIdleSeconds` registry value, 5 or more, in place of the minutes.

## How it works

- The **service** runs as LocalSystem in session 0. It starts the helper on the console desktop and sleeps until the helper exits, the user session changes, or Windows stops it, so it doesn't poll.
- The **helper** runs as SYSTEM in the console session. It reads the session's last keyboard or mouse input time with `GetLastInputInfo`, then sleeps until the warning would be due. While the warning shows, it checks 5 times a second so input cancels the warning quickly.
- When the countdown ends, the helper exits with a special code, and the service calls `InitiateSystemShutdownExW` with force so open games can't block it.
- The PC shuts down only after the full warning has been on screen, even if it was idle long before, for example after the helper was restarted.

Not counted as activity: games, video or sound still playing. If they were counted, a game left open behind a cut monitor would keep the PC on forever.

## Build

You need the Visual Studio 2022 Build Tools (C++).

```
build.cmd        runs the unit tests, then builds dist\SuperliteAutoshut.exe and dist\SuperliteAutoshut-test.exe
build.cmd test   runs the unit tests only
```

Created by Raptech Solution.
