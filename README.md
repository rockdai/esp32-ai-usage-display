# ESP32 AI Desktop Buddy

A desk-mounted display that shows your **Claude Code** quota burn at a glance.

Hardware: [Waveshare ESP32-S3-RLCD-4.2](https://docs.waveshare.net/ESP32-S3-RLCD-4.2/)
— a 4.2", 300×400, reflective LCD (no backlight, sunlight-readable, low-power).

## What it shows

```
┌──────────────────────────────────────────────────┐
│ CLAUDE CODE · MAX 5X              14:32          │
├──────────────────────────────────────────────────┤
│ 5H WINDOW                              47%       │
│ ████████████████░░░░░░░░░░░░░░░░░░░░░░           │
│ 342K / 720K tokens          resets 16:45 · 2h13m │
│                                                  │
│ WEEKLY                                 18%       │
│ ███████░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░           │
│ 1.8M / 10M tokens         resets Sun 00:00 · 4d  │
├──────────────────────────────────────────────────┤
│ Today: 5.2M tok · 14 sessions     Updated 14:32  │
└──────────────────────────────────────────────────┘
```

The **5-hour rolling window** and **weekly window** quotas (Claude Pro/Max
plan limits) are the primary metrics; today's token usage is secondary.

> Codex CLI is on the roadmap for v2 — see
> [`docs/superpowers/specs/`](docs/superpowers/specs/).

## How it works

```
┌──────────── Mac ────────────────────┐
│ launchd (every 60 s)                │
│   └─> mac/push-usage.sh             │
│         ├─ npx ccusage              │
│         ├─ aggregate to JSON        │
│         └─ curl POST → ESP32        │
└─────────────────────────────────────┘
                │ HTTP/JSON over LAN, mDNS
                ▼
┌──────────── ESP32-S3-RLCD-4.2 ──────┐
│ Arduino + LovyanGFX (PlatformIO)    │
│   ├─ Wi-Fi STA                      │
│   ├─ mDNS  ai-usage-display.local   │
│   ├─ HTTP server  POST /data        │
│   └─ render loop @ 1 Hz             │
└─────────────────────────────────────┘
```

**Design constraints:**

- No calls to Anthropic's API — usage is parsed from local Claude Code logs
  (via [`ccusage`](https://github.com/ryoppippi/ccusage)).
- No listening sockets on the Mac — only outbound `curl`.
- ESP32 never originates network traffic — purely passive HTTP server.

See [`docs/superpowers/specs/`](docs/superpowers/specs/) for the full design
and [`docs/superpowers/plans/`](docs/superpowers/plans/) for the implementation
plan.

## Repo layout

```
mac/                  # launchd job + push script
firmware/             # PlatformIO project (Arduino + LovyanGFX)
docs/superpowers/
  specs/              # design specs (one per major iteration)
  plans/              # implementation plans
```

## Setup

### 1. Mac side

```bash
cd mac
cp secrets.env.example secrets.env
# edit secrets.env: HOST=ai-usage-display.local

# install launchd job (once flashed and on the network):
cp com.rock.ai-usage-push.plist ~/Library/LaunchAgents/
launchctl load ~/Library/LaunchAgents/com.rock.ai-usage-push.plist
```

Logs: `~/Library/Logs/ai-usage-push.log`.

### 2. Firmware

Requires [PlatformIO](https://platformio.org/) (CLI or VS Code extension).

```bash
cd firmware
cp src/secrets.h.example src/secrets.h
# edit src/secrets.h: WIFI_SSID, WIFI_PWD

pio run -t upload      # build + flash
pio device monitor     # watch serial output
```

The device announces itself on the LAN as `ai-usage-display.local` once it's
on Wi-Fi. The Mac job will start delivering data on the next minute boundary.

## Status

🚧 v1 in progress — see the latest plan in
[`docs/superpowers/plans/`](docs/superpowers/plans/).

## License

MIT — see [`LICENSE`](LICENSE).
