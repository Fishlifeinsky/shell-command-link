# SCL Syntax Highlight（.s2c / .chain）

为 [shell-command-link](..) 库的两种脚本提供 VS Code 语法高亮：

| 语言 id | 扩展名 | 内容 |
|---------|--------|------|
| `scl-s2c`  | `.s2c` | 现代源（`var/if/while/do/when/fn/ret/const`、注释 `# // /* */`、`${var}`、运算符/箭头 `->`） |
| `scl-chain` | `.chain` | 运行时指令链（`label/jump/var/free/help`、内置运算 `iadd/…/seq`、`#` 注释、`${var}`、`-a/-b`） |

纯静态 TextMate 语法（无 TS/JS、无依赖、无激活脚本），无需构建。

## 高亮范围

- 注释（行/块）· 字符串（单/双引号，内含 `${var}` 上色）· 数字（含 `0x`/`0b`）
- 关键字/控制字、类型 `bool/int/flag/string`、flag 字面量 `-x`
- `s2c`：运算符与 `->`；`chain`：跳转模式 `-a/-b` 与全部内置运算指令

## 安装 / 调试

**方式 A：扩展开发宿主（调试）**
1. 在 VS Code 打开本目录（`scl-vscode/`）作为独立窗口；
2. 按 `F5`（选择 VS Code Extension Development Host）；
3. 在新窗口打开任意 `.s2c` / `.chain` 文件即可看到着色。

**方式 B：装入本地扩展（免打包，最直接）**
把本目录拷贝/软链到用户扩展目录（随 VS Code 启动即生效）：

- Windows：`%USERPROFILE%\.vscode\extensions\scl-syntax`
  ```powershell
  code --install-extension scl-vscode/   # 或直接拷贝目录到上面路径
  ```
- Linux/macOS：`~/.vscode/extensions/scl-syntax`
  ```bash
  code --install-extension scl-vscode/
  ```

> 注：`code --install-extension <目录>` 会安装该文件夹扩展（VS Code ≥1.65 支持目录安装）。

**方式 C：打包 vsix 分发**
```bash
npm i -g @vscode/vsce
cd scl-vscode && vsce package   # 生成 scl-syntax-0.1.0.vsix
code --install-extension scl-syntax-0.1.0.vsix
```

## 主题配色

着色跟随当前颜色主题对 TextMate scope 的映射（`keyword.control.*`、`storage.type.*`、
`string.quoted.*`、`comment.*`、`constant.numeric.*`、`variable.other.*`…）。如觉得某类太淡，
可在 `settings.json` 里按 scope 覆盖，例如：

```jsonc
"editor.tokenColorCustomizations": {
  "textMateRules": [
    { "scope": "keyword.control.scl-s2c", "settings": { "foreground": "#C586C0", "fontStyle": "bold" } },
    { "scope": "storage.type.scl-s2c",    "settings": { "foreground": "#4EC9B0" } }
  ]
}
```

## 参考

- SCL 语法：`doc/arc/script2chain-design.md`、`README.md`
- 现代脚本示例：`example/s2c/*.s2c`；链示例：`build/*.chain`（由 s2c 工具生成）
