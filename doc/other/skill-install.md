# Skill 安装说明（以 `scl` 技能为例）

> 分类：other（操作指南）
> 日期：2026-09-10
> 适用：VS Code + GitHub Copilot Agent Skills（`SKILL.md`）

## 1. 支持的安装位置

Skill 是**按目录发现**的：必须放在 `<某个 skill 根>/<skill-name>/SKILL.md`，
且 `SKILL.md` frontmatter 里的 `name` 必须**与文件夹同名**。

| 路径 | 作用域 |
|---|---|
| `.github/skills/<name>/` | 项目（团队共享，随仓库走） |
| `.agents/skills/<name>/` | 项目（跨工具约定） |
| `.claude/skills/<name>/` | 项目（Claude Code 兼容） |
| `~/.copilot/skills/<name>/` | **个人**（本机所有仓库可用） |
| `~/.agents/skills/<name>/` | 个人 |
| `~/.claude/skills/<name>/` | 个人 |

> ⚠️ 注意：VS Code 的**用户 prompts 目录**
> （`%APPDATA%\Code\User\prompts\`）只放 `*.prompt.md` / `*.instructions.md` /
> `*.agent.md`，**不放 skill**。个人级 skill 请用 `~/.copilot/skills/`。

本机路径换算：`~` = `C:\Users\dev04`。

## 2. 目录结构

```
.github/skills/scl/
├── SKILL.md        # 必需
├── scripts/        # 可选：可执行脚本
├── references/     # 可选：按需加载的文档
└── assets/         # 可选：模板/样板
```

## 3. 安装步骤

### 3.1 安装到某个项目（推荐，随仓库共享）

```powershell
# 在目标项目根目录执行
New-Item -ItemType Directory -Force -Path ".\.github\skills\scl" | Out-Null
Copy-Item "<c-lib 路径>\.github\skills\scl\SKILL.md" ".\.github\skills\scl\SKILL.md"
```

一份命令搞定（本例已装到 `test-ms41929`）：

```powershell
python -c "import os,shutil;s=r'C:\Users\dev04\Desktop\work\c-lib\shell-command-link\.github\skills\scl\SKILL.md';d=r'<目标仓库>\.github\skills\scl\SKILL.md';os.makedirs(os.path.dirname(d),exist_ok=True);shutil.copyfile(s,d);print('OK',d)"
```

### 3.2 安装到个人级（本机所有仓库可用）

```powershell
python -c "import os,shutil;s=r'C:\Users\dev04\Desktop\work\c-lib\shell-command-link\.github\skills\scl\SKILL.md';d=os.path.expanduser(r'~\.copilot\skills\scl\SKILL.md');os.makedirs(os.path.dirname(d),exist_ok=True);shutil.copyfile(s,d);print('OK',d)"
```

## 4. 生效与验证

1. **重新加载窗口**（`Developer: Reload Window`），让 VS Code 重新扫描 skill 目录；
2. 在 Chat 里输入 `/`，列表里应出现 `scl`（`user-invocable` 默认 `true`）；
3. 不手动调用也会生效：agent 读到 `description` 后，在相关任务上自动加载该 skill 的正文。

校验脚本（确认 frontmatter 合规、`name` 与文件夹一致）：

```powershell
python -c "import io,os;p=os.path.expanduser(r'~\.copilot\skills\scl\SKILL.md');t=io.open(p,encoding='utf-8').read();fm=t.split('---')[1];n=[l.split(':',1)[1].strip() for l in fm.splitlines() if l.startswith('name')][0];print('name=%s folder=%s lines=%d'%(n,os.path.basename(os.path.dirname(p)),t.count(chr(10))))"
```

期望输出：`name=scl folder=scl lines=192`

## 5. `scl` skill 的 frontmatter 现状（已校验）

```yaml
name: scl                      # 1-64 字符、小写字母/数字/连字符，必须与文件夹同名
description: "..."             # 157 字符（上限 1024），含触发关键词
```

约定：
- `description` 是**唯一的发现入口**，必须写清"做什么 + 何时用"，含关键词
  （Shell-Command-Link / SCL / 解释器 / 生成器 / doc/idea 等）；
- 含冒号的描述一律**加引号**，避免 YAML 静默解析失败；
- 正文保持在 500 行内（当前 192 行），更多内容拆到 `references/`。

## 6. 更新的正确姿势

skill 内容以 **c-lib 仓库为唯一源**（`.github/skills/scl/SKILL.md`），
改动后重新执行第 3 步覆盖各处副本，避免多份内容漂移。

## 7. 相关

- skill 本体：`.github/skills/scl/SKILL.md`
- 改动提案规约：`doc/idea/README.md`
- 官方参考：Agent Skills（`code.visualstudio.com/docs/copilot/customization/agent-skills`）
