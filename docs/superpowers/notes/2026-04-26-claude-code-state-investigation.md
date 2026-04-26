# Claude Code 本地状态调查 — 周限流复位时刻 (Weekly Reset)

调查时间: 2026-04-26
工作机: macOS Darwin 24.6.0
Claude Code 版本: 2.1.119 (运行中)
目标: 找出和 Claude Code 自身 `/status` 一致的"周限流复位时刻"计算口径，供 `mac/push-usage.sh` 输出 `weekly.resets_at`。

---

## ~/.claude inventory

```
/Users/rock/.claude/
├── backups/                 160K   .claude.json.backup.* (5 个时间戳备份)
├── cache/                   256K   changelog.md, my-closed-issues.json
├── downloads/               0
├── file-history/            500K   按 sessionId 分目录的内容 diff 快照
├── history.jsonl            144K   全局 prompt 历史 (跨项目)
├── paste-cache/             0
├── plugins/                 45M    blocklist.json, installed_plugins.json, install-counts-cache.json, known_marketplaces.json
├── projects/                17M    按 cwd-slug 分目录, 每个 session 一个 .jsonl
│   ├── -Users-rock-code-esp32-ai-usage-display/
│   │   ├── dfe757ea-...jsonl   1.0M (当前对话)
│   │   ├── dfe757ea-.../subagents/agent-*.jsonl
│   │   └── memory/             (CLAUDE.md auto-memory)
│   ├── -Users-rock-code-kongkong/
│   └── -Users-rock-code-playground/
├── session-env/             0
├── sessions/                4K     90366.json (单条 PID->sessionId 映射)
├── settings.json            4K     model/theme/plugins
├── shell-snapshots/         184K   一份 zsh snapshot
├── statusline-command.sh    450B
├── tasks/                   84K    dfe757ea-.../{1..27}.json (TodoWrite 持久化)
└── telemetry/               328K   1p_failed_events.*.json (上传失败的遥测 batch)
```

**外加** `~/.claude.json` (29 KB, 主要全局配置/缓存/项目元数据), `~/.claude.json.backup.*` (5 份备份均在 ~/.claude/backups/)

**外加** `~/Library/Caches/claude-cli-nodejs/<cwd-slug>/mcp-logs-*/<ISO>.jsonl` (MCP 服务器调试日志)

`~/Library/Application Support/Claude*` 不存在，`~/.config/claude*` 不存在 (CLI 不使用这些路径)。

---

## Candidate state files surveyed

### 1. `~/.claude.json` (主全局配置, 29 KB)
顶层 keys (43 个，全部列出):

```
additionalModelCostsCache, additionalModelOptionsCache, anonymousId, autoUpdates,
autoUpdatesProtectedForNative, cachedExtraUsageDisabledReason, cachedGrowthBookFeatures,
changelogLastFetched, claudeCodeFirstTokenDate, clientDataCache, closedIssuesLastChecked,
effortCalloutV2Dismissed, feedbackSurveyState, firstStartTime, githubRepoPaths,
groveConfigCache, hasCompletedOnboarding, installMethod, lastOnboardingVersion,
lastPlanModeUse, lastReleaseNotesSeen, lspRecommendationIgnoredCount, migrationVersion,
numStartups, oauthAccount, officialMarketplaceAutoInstallAttempted,
officialMarketplaceAutoInstalled, opus47LaunchSeenCount, opusProMigrationComplete,
overageCreditGrantCache, penguinModeOrgEnabled, projects, promptQueueUseCount,
remoteControlUpsellSeenCount, showExpandedTodos, showSpinnerTree, skillUsage,
sonnet1m45MigrationComplete, theme, tipsHistory, userID
```

潜在相关字段已逐一检查:

| 字段 | 值 / 含义 | 与周限流相关? |
|------|----------|--------------|
| `firstStartTime` | `2026-03-09T14:15:34.446Z` (一次性安装时刻) | 否 |
| `claudeCodeFirstTokenDate` | `2026-03-09T14:34:27.159069Z` (账号首次成功 API 调用) | 否 — 不会随每周窗口刷新 |
| `cachedExtraUsageDisabledReason` | `"org_level_disabled"` (是否提供 overage 提示) | 否 |
| `usage_limit_notifications_enabled` (在 `cachedGrowthBookFeatures` 内) | feature flag 名，仅控制是否弹提示 | 否 |
| `overageCreditGrantCache.<orgId>` | `{available:false, eligible:false, granted:false, ..., timestamp: 1777212070079}` | 否 — 是 overage 配额，不是 weekly reset |
| `groveConfigCache.<accountId>` | `{grove_enabled: true, timestamp: 1777212069472}` | 否 |
| `clientDataCache` | `null` | 否 |
| `oauthAccount` | OAuth account 对象 keys (内容 `<redacted>`) | 否 |
| `projects[<path>]` | 含 `lastSessionId, lastCost, lastAPIDuration, lastModelUsage, lastSessionMetrics` 等 per-project 上次运行指标 | 否 — 累积/上次值，没有 weekly window |
| `tipsHistory`, `numStartups` | UI 统计 | 否 |

**结论: `~/.claude.json` 不存周限流窗口起止时间。** 唯一时间字段是一次性的 `firstStartTime` / `claudeCodeFirstTokenDate`。

### 2. `~/.claude/settings.json`
内容: `{model, statusLine, enabledPlugins, extraKnownMarketplaces, effortLevel, skipDangerousModePermissionPrompt, theme}`. 无任何使用量字段。

### 3. `~/.claude/sessions/90366.json`
单条 PID-to-session 映射:
```
{pid, sessionId, cwd, startedAt, procStart, version, peerProtocol, kind, entrypoint, status, updatedAt}
```
`startedAt: 1777212069557` (= 当前进程启动时间, 即今天 14:01)，与周窗口无关。

### 4. `~/.claude/cache/`
仅 `changelog.md` 和 `my-closed-issues.json` (内容: `{}`). 无关。

### 5. `~/.claude/telemetry/1p_failed_events.*.json`
**上传失败**的遥测事件 batch。每行一个 `ClaudeCodeInternalEvent`，含 `event_name` (如 `tengu_version_lock_acquired`, `tengu_exit`)、`session_id`、`client_timestamp`、`auth.organization_uuid` (`<redacted>`)、`auth.account_uuid` (`<redacted>`)、`device_id` (`<redacted>`)、`email` (`<redacted>`)。**不含** ratelimit 事件或 weekly window 字段。最新文件 mtime 是 2026-04-26 (今天) 之前，说明现行版本通常成功上传，不在本地累积。

### 6. `~/.claude/backups/.claude.json.backup.<ms>`
`~/.claude.json` 在每次写入前的备份。内容结构同上，没有额外字段。

### 7. `~/Library/Caches/claude-cli-nodejs/<slug>/mcp-logs-*/<ISO>.jsonl`
MCP 客户端 debug 日志 (例: `mcpsrv_01F3zBu66ZQsxAnqkvb64kKL` 的连接错误 stack)。**与 Anthropic API ratelimit 无关**。

---

## Ratelimit headers in jsonl

**无。**

`grep -i ratelimit ~/.claude/projects/**/*.jsonl` 命中 2 处，均为 **用户消息正文**（即此次对话本身在讨论 "ratelimit"）。命中行结构:

```
{"type":"user","message":{"role":"user","content":"...weekly limit reset..."}, ...}
```

**没有任何一条事件包含 `anthropic-ratelimit-*-reset`、`anthropic-ratelimit-unified-*`、`x-ratelimit-*` 字样的 HTTP 响应头。** Claude Code 不把 API 响应头落盘到 jsonl。

`grep "weekly"` 同理，全部命中都是用户消息正文（中文 "weekly 窗口"、"weekly limit reset" 等），没有结构化字段。

`assistant` 类型事件中 `message.usage` 含 `input_tokens / cache_creation_input_tokens / cache_read_input_tokens / output_tokens / service_tier / cache_creation / iterations / speed`，**没有** `requests_remaining` / `tokens_remaining` / `*_reset` 字段。

---

## jsonl shape summary

每个 `~/.claude/projects/<slug>/<sessionId>.jsonl` 的事件结构:

**type 取值**: `user`, `assistant`, `system`, `attachment`, `file-history-snapshot`, `last-prompt`, `permission-mode`, `queue-operation`

**通用字段** (各事件 keys 的并集):
```
attachment, content, cwd, durationMs, entrypoint, gitBranch, isApiErrorMessage,
isMeta, isSidechain, isSnapshotUpdate, lastPrompt, level, message, messageCount,
messageId, operation, parentUuid, permissionMode, promptId, requestId, sessionId,
slug, snapshot, sourceToolAssistantUUID, sourceToolUseID, subtype, timestamp,
toolUseResult, type, userType, uuid, version
```

**关键字段口径**:
- `timestamp`: ISO 8601 UTC 字符串 (例 `2026-04-26T10:06:28.644Z`)
- `sessionId`: UUID v4 (一个会话/`/resume` 任务一个)
- `type: "user"` 标记一次用户输入；`type: "assistant"` 标记一次模型响应（**消耗 quota**）；`assistant` 事件含 `message.usage` 与 `requestId`。

**子代理 jsonl** (`~/.claude/projects/<slug>/<sessionId>/subagents/agent-*.jsonl`) 共享相同 schema，且 `agentId` 额外存在；它们也消耗 weekly quota，应一并扫描。

**本机 trailing 7 天数据**:
- jsonl 文件总数: 51 (含 subagents)
- trailing 7d 内有 `type:"user"` 事件的 sessionId: 3
  - `3370ac5f-...` 最早用户事件: `2026-04-25T10:38:33.947Z`
  - `55b7927f-...` 最早用户事件: `2026-04-25T10:39:40.659Z`
  - `dfe757ea-...` 最早用户事件: `2026-04-26T10:06:28.644Z`
- 全机 trailing 7d 最早 `type:"user"` 事件: **`2026-04-25T10:38:33.947Z`**
- 全机 trailing 7d 最早 `type:"assistant"` 事件 (首次实际 API 计费): `2026-04-25T10:39:43.921Z`
- 推算 weekly reset (用户事件 + 7d): `2026-05-02T10:38:33Z` = epoch `1777718313`

---

## Recommended weekly-clock strategy

**推荐方案 (fallback path 3): 扫 jsonl，取 trailing 168h 内最早的 `type:"user"` 事件 timestamp，加 7 天。**

```python
# 伪码
import json, glob, os
from datetime import datetime, timedelta, timezone

now = datetime.now(timezone.utc)
cutoff = now - timedelta(days=7)
earliest = None
for path in glob.glob(os.path.expanduser("~/.claude/projects/**/*.jsonl"), recursive=True):
    with open(path) as f:
        for line in f:
            try:
                ev = json.loads(line)
            except Exception:
                continue
            ts = ev.get("timestamp")
            if not ts or ev.get("type") != "user":
                continue
            t = datetime.fromisoformat(ts.replace("Z", "+00:00"))
            if t < cutoff:
                continue
            if earliest is None or t < earliest:
                earliest = t

if earliest is None:
    # trailing 7d 内没有任何 user 事件 — 周窗口未启动，reset 未知，先报告 None
    resets_at = None
else:
    resets_at = int((earliest + timedelta(days=7)).timestamp())
```

### 理由

1. **路径 1 (本地 state 文件) 不可行**: 已穷举 `~/.claude/`、`~/.claude.json` 及备份、`~/Library/Caches/claude-cli-nodejs`、`~/Library/Application Support`、`~/.config`，没有任何字段记录 weekly window 起点或终点。Claude Code CLI 不把 quota state 持久化到本地。

2. **路径 2 (ratelimit 响应头) 不可行**: jsonl 里 `type:"assistant"` 事件的 `message.usage` 只有 token 计数，**没有** `anthropic-ratelimit-unified-*-reset` 头。Claude Code 在收到 API 响应时仅把 `usage` 摘要写入 jsonl，原始响应头被丢弃。telemetry 里也没有 ratelimit batched event。

3. **路径 3 是唯一可行口径**，且与 Anthropic 官方策略 ("Both limits reset seven days after your session starts") 在大多数情况下完全一致：用户与 Claude Code 的**第一条对话** = 周窗口起点。**`type:"user"` 比 `type:"assistant"` 更准确**，因为：
   - 用户键入 prompt 后才会触发 API 调用，user event 在前，assistant event 在后；窗口起点应为前者。
   - 偶发 API 失败 (assistant 事件不存在) 时，user event 仍然标记了窗口启动。

### 已知限制 (DONE_WITH_CONCERNS)

- **`/compact`、子代理 (subagents)、resume 也会消耗 quota**。`subagents/agent-*.jsonl` 的 user 事件应一并扫描 (上面伪码用 `**/*.jsonl` 已覆盖)。
- **如果 trailing 7d 内最早 user event 距今 > 7d，那它已超出当前窗口**，本机方案需丢弃；当前实现用 `cutoff = now - 7d` 已满足。
- **如果用户在最近 7d 内**没有**任何**对话，说明上一个窗口已自动复位、新窗口未开启，`resets_at` 应报 `None` (未来窗口未知)。
- **多机分裂**: weekly window 是按 Anthropic 账号，不是按机器。用户在另一台 Mac/手机/web Claude.ai 上发起的会话不会出现在本机 jsonl，**会让本机推算的窗口起点偏晚**。`mac/push-usage.sh` 必须接受这个偏差，或在 README 注明 "本机视角"。
- **5h session window** 与 weekly window 是不同口径：weekly = 7d，session = 5h 滚动。本调查仅解决 weekly reset。如要算 5h，需要按"最近一次 user event 起 5h 内的所有 assistant token 总和"重新统计 (与 ccusage 的 5h 算法一致)。
- 推算精度: 秒级 (取决于 jsonl `timestamp` 的毫秒精度)，远好于 ccusage 的 calendar-week (24h 误差)。

---

## Open questions

1. **官方 `/status` 输出从哪里来?** Claude Code CLI 的 `/status` slash command 应该会展示真实的 5h/weekly 剩余，且数据来自 API 响应头 (实时 fetch)。**值得抓一次 `/status` 调用看它的网络请求**：如果它每次都 hit 一个 quota endpoint (例如 `/v1/quota` 之类)，那 `mac/push-usage.sh` 也可以走相同 endpoint —— 但这违反"避免使用官方 Claude Usage API"的用户偏好，需要确认是否算 "official API"。
2. **跨机同步**: 用户是否在多台机器上用同一 Claude Max 账号？如果是，需要在 ESP32 上加一个 "本机视角，不一定准确" 的注释，或考虑让多台 Mac 都跑 `push-usage.sh` 并由 ESP32 取 `min(resets_at)` (= 最早开始的窗口)。
3. **session window 5h 该如何算?** 目前需求是 weekly，但如果之后要加 5h 一并算，是否复用同一段 jsonl 扫描代码？建议在 `mac/push-usage.sh` 一次性计算两个窗口。
4. **空闲超过 7 天的回滚行为**: Anthropic 的实际行为是 "上次窗口结束后下次首条 prompt 立刻开新窗口" 还是 "默认从 calendar week 起算"？若用户连续 8 天没说话，本机算出 `resets_at = None` 是否合理？建议在文档里注明此情形。
