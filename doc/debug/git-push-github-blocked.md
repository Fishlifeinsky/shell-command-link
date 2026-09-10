# Debug：无法 push 到 GitHub（直连被阻断 + 本地代理未启动）

> 分类：debug（现象 / 原因 / 解决办法）
> 日期：2026-09-10
> 现象：`git push` 提交到 `origin`（github.com）失败

---

## 1. 现象

本仓库 `master` 领先 `origin/master` 若干提交，推送失败：

```
fatal: unable to access 'https://github.com/Fishlifeinsky/shell-command-link.git/':
TLS connect error: error:0A000126:SSL routines::unexpected eof while reading
```

注意：**不是**认证失败、也不是历史分叉（`rev-list --left-right --count` 显示 `behind=0`）。

## 2. 诊断过程与结果

| 检查项 | 命令 | 结果 |
|---|---|---|
| 是否分叉 | `git rev-list --left-right --count '@{u}...HEAD'` | `0  14` → 纯领先，无冲突 |
| git 代理配置 | `git config --show-origin --list \| findstr proxy` | `.git/config: http.proxy=http://127.0.0.1:7892` |
| 代理端口是否在监听 | `TcpClient` 连 `127.0.0.1:7890/7891/7892/7893/7897/10808/...` | **全部拒绝连接（无代理在跑）** |
| 代理进程 | `Get-Process` 过滤 clash/v2ray/xray/mihomo/… | **没有** |
| HTTPS 直连 | `git ls-remote` / `git push` | TCP 通，但 **TLS 握手被中断**（unexpected eof） |
| SSH 直连 | `ssh -T git@github.com`（BatchMode） | **Connection timed out during banner exchange** |
| 系统代理 | `netsh winhttp show proxy` / IE 设置 | 直接访问（未设代理） |

对照：内网 GitLab（`gitlab-srv.lan:22`）与 `github.com:22` 的 **TCP** 均可达，但 GitHub 的
**TLS/SSH 协议层被阻断**，只有走本地代理才能完成握手。

## 3. 原因

1. 该网络下 **直连 GitHub 被阻断**（HTTPS 的 TLS 被重置、SSH 卡在 banner 交换），
   因此仓库特意配置了本地代理 `http.proxy=http://127.0.0.1:7892`；
2. **代理客户端当前没有运行**（端口未监听、无代理进程），git 于是既连不上代理、
   也走不通直连，表现为上述 TLS/SSH 报错。

## 4. 解决办法

### 4.1 推荐：启动代理客户端后推送

1. 启动本机代理客户端（Clash/Mihomo 类，监听 7892；若端口不同按下条改配置）；
2. 确认端口已监听：

   ```powershell
   (New-Object Net.Sockets.TcpClient).Connect('127.0.0.1', 7892)   # 不报错即已监听
   ```

3. 推送：

   ```powershell
   git push origin master
   ```

若代理端口不是 7892，改成本机实际端口：

```powershell
git config http.proxy http://127.0.0.1:<端口>
```

或临时（不改配置）：

```powershell
git -c http.proxy=http://127.0.0.1:<端口> push origin master
```

### 4.2 备用：换网络 / 用其它可达通道

- 手机热点等不受限网络下直接 `git push`；
- 或在能访问 GitHub 的机器上 `git fetch <本地路径/打包文件>` 后推送。

### 4.3 应急：打包搬运

在受限环境导出、在可达环境导入：

```powershell
git bundle create scl.bundle origin/master..master    # 仅导出领先的提交
# 在可达环境： git fetch <scl.bundle路径> master:master && git push
```

## 5. 排查用到的命令（可复用）

```powershell
# 领先/落后
git rev-list --left-right --count '@{u}...HEAD'
# 代理配置来源
git config --show-origin --list | Select-String proxy
# 端口探测（不依赖外部工具）
$c=New-Object Net.Sockets.TcpClient; $c.BeginConnect('127.0.0.1',7892,$null,$null).AsyncWaitHandle.WaitOne(500)
# 协议层验证（TCP 通不等于能用）
git ls-remote --heads origin
ssh -o BatchMode=yes -T git@github.com
```

> 结论：**TCP 可达 ≠ 协议可用**。遇到 `TLS connect error ... unexpected eof` 或
> `timed out during banner exchange`，优先怀疑直连被阻断 + 代理未启动，而不是认证或分叉问题。
