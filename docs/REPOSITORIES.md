# KemiCAD 仓库对应关系与 GitHub 同步（开发者/agent 必读）

最后核对：2026-10-30（UTC+8）

## 1. 三个远端与对应关系

| 位置 | 地址 | 分支 | 2026-10-30 同步 tip |
|---|---|---|---|
| 本地开发仓 | TD80 `D:\KiCad` | `main` | `59787e4c4fc354742713f334a053bedafcd58abe` |
| 内部 GitLab（remote `origin`） | `ssh://git@nl.ydata.online:9527/macos/KiCad.git`（网页 `https://nl.ydata.online:9529/macos/KiCad`） | `main`（默认分支） | `59787e4c…`，与本地相同 |
| GitHub 开源（remote `github`） | `https://github.com/newlinksz/KemiCAD.git` | `main` | `9983777d5eff566a7d8bcdc95413d6b80997db21` |

- 表中 tip 为该次同步时的值；本文件自身的更新提交会再让 tip 前进一格，实时值以 `git ls-remote` 为准。
- 本地 `main` = `origin/main`：同一条完整开发历史。
- GitHub `main` 的**文件内容与本地 `main` 完全相同**（tree `4dfbf9febf73f986df818e595c02ddbd26936c22`，`git diff main github/main` 为空），但**历史不同**：GitHub 上是在原有历史（`a609266a`）后接"内容一致快照提交"（`a609266a` → `2d6505c2` → `9983777d`）。图上显示为两条线属正常，不是内容不一致。
- 原因：本地仓为浅克隆，整历史推 GitHub 会报 `did not receive expected object …`；因此 GitHub 只用快照方式同步。
- 目标仓库是 `newlinksz/KemiCAD`，**不是** `newlinksz/KEMI-EDA`（旧示例，查询失败不代表账号连不上）。
- remote `upstream` = 官方 KiCad（只读，push 已禁用）。

## 2. 开发规矩

- 日常只在本地 `main` 开发；需要临时分支时另开（建议 `temp/` 或 `codex/` 前缀），做完合回 `main` 后删掉。
- 完成一版后：先推 `origin main`（内部 GitLab），再按第 3 节同步 GitHub。
- 不在 TD80 `D:\` 根目录新建目录；worktree 放在 `D:\KiCad\.codex-worktrees\` 下，用完删除。
- push / 强推 / 删除分支须老莫批准。

## 3. GitHub 同步方法（"第一版方式"：内容一致快照）

不 unshallow、不整历史强推。在 TD80 **本机会话**（PowerShell）执行：

```powershell
cd D:\KiCad
gh auth switch --hostname github.com --user newlinksz
gh api user --jq .login                                        # 必须是 newlinksz
gh repo view newlinksz/KemiCAD --json url,viewerPermission     # 需 WRITE/ADMIN
git fetch github main
$tree = git rev-parse 'main^{tree}'
$parent = git rev-parse github/main
$c = git commit-tree $tree -p $parent -m "KemiCAD snapshot of main@$(git rev-parse --short main) (tree-identical)"
git diff --stat main $c                                        # 必须为空
git push --force-with-lease=refs/heads/main:$parent github "$($c):refs/heads/main"
git ls-remote https://github.com/newlinksz/KemiCAD.git refs/heads/main   # 核对 = $c
```

这是快进推送，不覆盖 GitHub 已有历史。

## 4. 工具与授权

- 用 GitHub CLI `gh` + HTTPS + 账号 **newlinksz**（凭据在 TD80 当前用户 Windows 凭据管理器）。
- **不要**用 motyabc 的 SSH 钥或 Deploy key 推断"连不上"；TD80 上 SSH 钥的 GitHub 身份是 motyabc，对该库无权限。
- 从云电脑经 Tailscale SSH 登录 TD80 时读不到 Windows 凭据管理器，gh 会显示 token invalid / HTTP 401；必须用 TD80 本机会话（本机通道）执行。
- 本文件不含任何授权；其他电脑或系统用户需各自本机 `gh auth login`。