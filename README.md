# 人民日报 / 新闻联播 每日抓取器（C++）

按目录内《人民日报_新闻联播_每日获取方案.txt》实现。每次运行会自动检查最近
**7 天**，把还没有保存过的日期补齐，因此不需要每天打开：隔几天点一次
`运行.bat`，中间缺的日期会自动补上。

## 一键安装（推荐）

双击仓库根目录的 **`政策解读台-安装程序.exe`**，按向导操作：

1. 选择安装位置（默认 `%LOCALAPPDATA%\Programs\政策解读台`，**不需要管理员权限**）；
2. 在安装界面直接粘贴 DeepSeek API 密钥（写入本机 `agent.ini`，也可以先跳过）；
3. 完成后可勾选「立即运行政策解读台」。

安装包已内置主界面、抓取器、解读 Agent、`skill\` 方法论和卸载程序，目标机器
不需要安装编译器、Python 或任何运行库。

> 安装包没有代码签名，首次运行时 Windows SmartScreen 可能提示“Windows 已保护你的电脑”。
> 点「更多信息」→「仍要运行」即可；程序只写入当前用户目录，不需要管理员权限。

安装后开始菜单会出现：

- **政策解读台**
- **使用说明**
- **修改 API 密钥**（用记事本打开 `agent.ini`）
- **卸载政策解读台**

卸载默认保留 `output\` 稿件和 `agent.ini`；也可以在卸载窗口选择「卸载并删除全部数据」。

静默安装（批量部署用）：

```bat
政策解读台-安装程序.exe /S /D=C:\Apps\政策解读台 /KEY=sk-你的密钥
```

可选参数：`/BASE=`（接口地址）、`/MODEL=`（模型名）、`/NODESKTOP`（不建桌面快捷方式）。
安装日志在 `%TEMP%\政策解读台-安装日志.txt`。

## 一键使用（源码目录）

如果你不使用安装包、直接在源码目录里运行：

双击 **`运行.bat`**。

- 会自动用本机 g++ 编译 `main.cpp`（编译失败时使用已有的 `fetch_news.exe`）。
- 每次扫描最近 7 天（含今天），**只抓还没有 txt 的日期，已存在的直接跳过**。
- 抓取完成后窗口会停住，按任意键关闭。
- 结果在 `output\`：

```
output\
  人民日报_YYYYMMDD.txt
  新闻联播_YYYYMMDD.txt
  logs\抓取日志_YYYYMMDD.log
```

文件名和正文开头都注明日期；文件为 UTF-8（带 BOM）+ CRLF，记事本可直接打开。

如果某次抓取有个别请求失败，程序会先把已取到的内容保存为 `*_部分.txt` 并在头部注明，
下一轮运行会重新尝试完整版；只有完整的 `*.txt` 才算“已抓取”，不会被跳过。
`--pages` 调试参数同样输出 `*_部分.txt`，不会污染正式归档。

### 自动补抓示例

- 10 月 26 日运行一次；
- 10 月 28 日再运行：窗口是 10-22～10-28，其中 10-27、10-28 缺失，会自动抓取；
  10-22～10-26 已有文件，直接跳过；
- 如果中间某天 20:30 前运行，当天的新闻联播还没有，程序会把这一天留空，
  下一次运行再补，不会拿前一天的内容冒充今天。

首次运行如果 7 天全空，会把 7 天的内容都抓下来：新闻联播较快，
人民日报每天约 2–3 分钟，整套大约 15–20 分钟。之后每天运行只抓当天，很快。

## 当天内容还没发布怎么办

- 《人民日报》电子版当天早上就有。
- 《新闻联播》19:00 播出，第三方文字稿一般 20:30 左右更新。

程序按“日期”归档：某天暂时取不到内容时，这一天不生成文件，日志记录“暂无可用内容”，
下次运行只要它还在 7 天窗口内就会自动补抓。文件永远使用**实际内容日期**命名。

如果只拿到标题、还没有正文（常见于 20:30 前的当天新闻联播），会先保存成
`新闻联播_YYYYMMDD_部分.txt`；它不是完整版，下次运行会重试，完整后才改名为
`新闻联播_YYYYMMDD.txt`。所以 20:30 后再跑一次即可把当天补全。

## 数据源与回退顺序

《新闻联播》（`xwlb_source=auto`，按顺序尝试同一天的内容）：

1. **GitHub** `DuckBurnIncense/xin-wen-lian-bo` 的当日 Markdown（全文、整理好）。
2. **cn.govopendata.com** 当日文字版（第三方，但**不需要代理**，全文完整）。
3. **央视网** `tv.cctv.com/lm/xwlb/day/YYYYMMDD.shtml`（官方源，逐条抓
   `.playingVideo .tit` 和 `#content_area`；部分条目只有摘要）。

《人民日报》：

- 官方电子版 `paper.people.com.cn`，解析 `node_01.html` 到 `node_N.html`，
  逐篇抓 `<h1>` 与 `#ozoom` 正文；
- 自动跳过“本版责编”和整版“广告”条目；
- 全程直连，不需要代理。

## 无代理 / 换机器使用

程序不依赖代理：

- GitHub 请求**总是先直连**，只有直连失败才使用 `config.ini` 里的 `proxy`；
- 没有代理的机器，把 `config.ini` 里的 `proxy=` 留空即可；
- 即使 GitHub 完全不可达，新闻联播会自动回退到 GovOpenData 或央视网，
  人民日报本来就直连，因此仍能拿到当天内容。

`fetch_news.exe` 是静态链接的 64 位 Windows 程序；换机器时即使没有 g++，
也可以直接运行它（`运行.bat` 检测不到编译器时会使用已有 exe）。

## 常用参数

```text
fetch_news.exe                          补抓最近 7 天里缺失的日期
fetch_news.exe --days 3                 只补最近 3 天
fetch_news.exe --days 1                 只抓今天（等价于 --strict）
fetch_news.exe --date 2026-10-25        以指定日期为终点，默认仍往前补 7 天
fetch_news.exe --date 2026-10-25 --days 1  只抓指定日期
fetch_news.exe --only xwlb              只抓新闻联播
fetch_news.exe --only rmrb              只抓人民日报
fetch_news.exe --pages 1-3              人民日报只抓第 1–3 版（调试用，输出 *_部分.txt）
fetch_news.exe --force                  已有的也重抓（会重抓整个窗口，慎用）
fetch_news.exe --delay-ms 1200          放慢请求间隔
fetch_news.exe --proxy 127.0.0.1:7897   临时指定 GitHub 代理
fetch_news.exe --no-github-proxy        访问 GitHub 不走代理
fetch_news.exe --xwlb-source govopen    强制使用某个新闻联播来源
fetch_news.exe --help                   查看全部参数
```

`运行.bat` 支持透传参数，例如：`运行.bat --days 3`。

## 配置 `config.ini`

| 键 | 说明 |
|---|---|
| `proxy` | GitHub 代理，`host:port`；留空表示不用代理 |
| `github_proxy` | `1` 允许为 GitHub 使用代理，`0` 永远直连 |
| `catchup_days` | 每次自动补抓最近多少天（含今天），默认 7 |
| `delay_ms` | 请求间隔（毫秒），默认 1000，建议不要低于 500 |
| `output_dir` | 输出目录，相对程序所在目录 |
| `xwlb_source` | `auto` / `github` / `govopen` / `cctv` |

## 自己编译

```bat
编译.bat
```

等价命令：

```bat
g++ -std=c++17 -O2 -static -static-libgcc -static-libstdc++ main.cpp -o fetch_news.exe -lwinhttp -lshell32
```

实现上用 WinHTTP 发 HTTPS 请求（自动跟随跳转、带浏览器 UA、失败重试与指数退避），
用轻量 HTML 提取器处理 `<h1>`、`#ozoom`、`#content_area` 等节点，
并按页面声明的 charset 处理 GBK/UTF-8。

## 想完全无人值守

可以在 Windows“任务计划程序”里新建一个每天 20:45 运行 `fetch_news.exe` 的任务，
“起始于”填程序目录。程序本身不弹窗，日志写在 `output\logs\`。不注册计划任务也能用，
隔几天手动运行一次即可自动补齐。

## 已知限制

- 只补最近 `catchup_days` 天（默认 7 天）；更早的日期不会自动补。
- 官网或第三方站点改版后，选择器可能失效；程序会在日志里记录失败原因。
- 新闻联播的 GitHub 镜像和 GovOpenData 都是第三方，可能延迟或停更；
  央视官方源作为最后回退，但部分条目只有摘要而非全文。
- 只抓文字，不下载 PDF、视频。
- 请求是串行的并带间隔，抓完整份人民日报约 2–3 分钟。

## 许可

- **程序代码：MIT License**（全文见 [LICENSE.md](LICENSE.md) 附录 A）。
- **抓取到的新闻内容：不授权**，仅限个人阅读、学习研究；不得转载、商用、镜像或公开再分发。
- 第三方来源、使用规范、内容范围与变动记录统一记录在 [LICENSE.md](LICENSE.md)
  （当前版本 1.0，2026-10-26）。

## 图形界面：政策解读台（news_desk.exe）

双击 `news_desk.exe` 打开。两个按钮对应两条链路，互不阻塞：

界面为纯 Win32 + GDI+ 自绘：编辑室浅色配色（米白底 + 党政红强调色）、圆角卡片、
自绘按钮与细滚动条；标题区右侧的状态胶囊随「就绪 / 抓取中 / 解读中 / 完成 / 失败」变色。
所有尺寸按系统 DPI 缩放（125%、150% 等缩放下排版一致），不依赖第三方 GUI 库或图片资源。

| 按钮 | 做什么 |
|---|---|
| **获取新闻稿** | 在后台隐藏运行同目录的 `运行.bat`（即 fetch_news.exe），输出实时回显；结束后自动盘点 `output\` |
| **开始解读** | 以子进程拉起 `agent_host.exe`，由它读取本机稿件、载入 `skill\` 方法论、调用 API，结果逐字流式显示 |
| **停止** | 连同子进程树一起结束（Job Object，不会留下孤儿进程） |
| **打开稿件目录** | 打开 `output\` |

### 组成

```
news_desk.exe     主界面（Win32，无第三方依赖）
agent_host.exe    独立封装的解读 Agent（命令行 / 子进程均可调用）
agent.ini         API 与材料窗口配置
skill\            解读方法论（SKILL.md + references），供 agent_host 载入
build_desk.bat    一键编译上面两个 exe
```

### 配置 agent.ini

首次使用先把模板复制成正式配置（`agent.ini` 已被 `.gitignore` 屏蔽，不会进仓库）：

```bat
copy agent.ini.example agent.ini
```

```ini
[api]
base_url=https://api.deepseek.com
model=deepseek-chat
api_key=              # 填你自己的；也可用环境变量 DEEPSEEK_API_KEY
extra_json=           # 厂商特有的联网参数，原样拼进请求体
max_tokens=8000
temperature=0.8

[agent]
days=7                # 近几日窗口
extend_history=1      # 允许读取 7 天以前稿件（只取标题做长期趋势）
max_input_chars=60000 # 材料包上限
skill_dir=skill
```

密钥只从 `agent.ini` 或环境变量读取，不写日志、不回显。**未填 key 时点「解读」只会提示，不会发任何请求。**
环境变量 `DEEPSEEK_API_KEY` 的优先级高于 `agent.ini` 里的 `api_key`。

### 调用 API 的标准流程（当前仅支持 DeepSeek）

#### 1. 准备密钥

1. 打开 <https://platform.deepseek.com>，注册并充值（解读按 token 计费）。
2. 进入 **API keys** 页面，创建密钥，形如 `sk-xxxxxxxx`，只显示一次，请立即保存。
3. 不要把它写进任何会被提交的文件。本项目的 `agent.ini` 已被 `.gitignore` 屏蔽。

#### 2. 配置

```bat
copy agent.ini.example agent.ini
```

编辑 `agent.ini`：

```ini
[api]
base_url=https://api.deepseek.com
model=deepseek-chat
api_key=sk-你的密钥
```

或者保持 `api_key=` 为空，改用环境变量（优先级更高，适合临时切换）：

```bat
set DEEPSEEK_API_KEY=sk-你的密钥
```

#### 3. 请求是怎么发出去的

`agent_host.exe` 走的是 OpenAI 兼容的 **Chat Completions** 协议，具体如下。

**端点**：`{base_url}/chat/completions`

- `base_url` 末尾多余斜杠会自动去掉；
- 若 `base_url` 里已经带了 `chat/completions`，不再重复拼接（便于直接填中转站的完整地址）。

**请求头**：

```text
Content-Type: application/json; charset=utf-8
Accept: text/event-stream
Authorization: Bearer <api_key>
```

**请求体**（结构示意，`system`/`user` 内容已省略）：

```json
{
  "model": "deepseek-chat",
  "stream": true,
  "max_tokens": 8000,
  "temperature": 0.8,
  "messages": [
    { "role": "system", "content": "（skill/SKILL.md + references + 本次任务要求）" },
    { "role": "user",   "content": "（材料清单 + 近七日稿件 + 更早材料标题）" }
  ]
}
```

若配置了 `extra_json`，其内容会被原样拼进这个顶层对象。

**响应**：SSE 流，逐条下发增量。

```text
data: {"choices":[{"delta":{"content":"## 一页结论"}}]}
data: {"choices":[{"delta":{"content":"\n\n【事实】…"}}]}
data: [DONE]
```

程序只取 `delta.content` 逐字回显；`data: [DONE]` 结束。若用到 `deepseek-reasoner`，
它返回的 `reasoning_content`（思维链）会被忽略，只显示最终答案，避免刷屏。

#### 4. 实际发出去的内容量

| 部分 | 内容 | 实测规模 |
|---|---|---|
| system | `skill/SKILL.md` + 5 个 references + 任务要求 | 约 11,100 字符 |
| user | 材料清单 + 7 天联播摘要/重点正文 + 人民日报版面标题/头版正文 | 约 59,700 字符（上限 `max_input_chars`） |

材料按优先级填充：**联播摘要 → 人民日报版面标题 → 联播重点正文 → 人民日报重点正文 → 更早材料标题**。
超预算时截断并在 prompt 里声明"缺失部分按无法取证处理"，不会静默丢材料。

#### 5. 配置项说明

| 键 | 含义 | 建议 |
|---|---|---|
| `base_url` | API 根地址 | DeepSeek 填 `https://api.deepseek.com` |
| `model` | 模型名 | `deepseek-chat`（已实测）；`deepseek-reasoner` 未验证 |
| `api_key` | 密钥 | 本地填；或改用环境变量 |
| `extra_json` | 追加到请求体的原始 JSON | DeepSeek 留空 |
| `max_tokens` | 单次输出上限 | 8000；DeepSeek 上限 8192 |
| `temperature` | 采样温度 | 0.8；要更严守规则可降到 0.4 |
| `timeout_seconds` | 读超时 | 300 |
| `retries` | 传输层失败的重试轮数 | 3 |
| `proxy` | 代理地址 `host:port` | 留空即不用；本机有 Clash 可填 `127.0.0.1:7897` |
| `proxy_mode` | `auto` 先直连再走代理 / `direct` 只直连 / `always` 只走代理 | `auto` |
| `days` | 近几日窗口 | 7 |
| `extend_history` | 是否读更早稿件（仅标题） | 1 |
| `max_input_chars` | 材料包上限 | 60000；报上下文超限时降到 40000 |

#### 6. 失败时怎么判断

程序退出码：

| 退出码 | 含义 | 处理 |
|---:|---|---|
| 0 | 成功 | — |
| 2 | 参数错误 | 看 `--help` |
| 3 | 找不到 `skill/SKILL.md` | 确认 `skill\` 目录完整 |
| 4 | 未配置密钥 | 填 `agent.ini` 或设环境变量 |
| 5 | 调用失败 | 看输出里的 `HTTP <状态码>` 与返回体 |

常见 HTTP 状态：

| 状态 | 原因 |
|---|---|
| 401 | 密钥无效或已删除 |
| 402 | 余额不足 |
| 400 | 参数问题（含上下文超限、`extra_json` 不被支持） |
| 429 | 触发限流，稍后重试 |

`400` 且配置了 `extra_json` 时，程序会自动**去掉该参数重试一次**，并在输出里注明"本次未联网"。

#### 6.1 常见故障：错误码 12152

这是 WinHTTP 的 `ERROR_WINHTTP_INVALID_SERVER_RESPONSE`，含义是**服务器返回了无法识别的响应**，
通常不是配置错误，而是连接被中间环节重置：

- DeepSeek 边缘节点瞬时抖动（最常见，重试即可）
- 本地代理 / TLS 中间人软件干扰
- 请求发出后连接被 RST，服务端没来得及回标准响应

`agent_host` 的处理：把它归为**瞬时故障**，按 `retries` 自动重试（默认 3 轮）。
配置了 `proxy` 且 `proxy_mode=auto` 时，每轮会**先直连、失败再走代理**，
所以本地开着 Clash 时通常第二次尝试就能成功。

排查顺序：

1. 直接重跑一次，多数情况会自愈；
2. 仍然失败，用第 7 节的 curl 自检——curl 通说明网络正常，问题在 WinHTTP 的 TLS/代理路径；
3. curl 也失败，换网络或调整代理规则（把 `api.deepseek.com` 设为直连或全局）；
4. 想强制走代理：`proxy_mode=always`；想排除代理干扰：`proxy_mode=direct`。

重试过的调用会在输出里留下"第 N 轮·直连/代理失败…稍后重试"的记录，便于事后判断链路质量。

#### 7. 手工自检（不经过程序）

```bash
curl https://api.deepseek.com/chat/completions \
  -H "Content-Type: application/json" \
  -H "Authorization: Bearer $DEEPSEEK_API_KEY" \
  -d '{"model":"deepseek-chat","messages":[{"role":"user","content":"ping"}],"max_tokens":16}'
```

能返回 `choices` 说明网络与密钥都正常；此时若程序仍失败，问题就在 `agent.ini` 的字段或 `skill\` 目录。

#### 8. 兼容范围

目前**只验证过 DeepSeek 官方端点**。其他 OpenAI 兼容服务（通义、Kimi、中转站等）
理论上只需改 `base_url` 和 `model`，但未做实测；厂商特有的联网/检索参数从 `extra_json` 注入。
换端点前建议先按第 7 节用 curl 确认协议一致。

### 关于"联网搜索外部新闻"

DeepSeek 官方端点（`api.deepseek.com`）没有内置联网检索参数。若你用的是支持联网的中转端点，
把该端点要求的参数原样填进 `extra_json`，例如：

```ini
extra_json={"enable_search":true}
```

如果接口因为这个参数返回 400，`agent_host` 会自动去掉它重试一次，并在输出里说明本次未联网。
未启用联网时，模型只会基于本地材料与自身知识作答，证据不足处按 skill 规则标"无法取证"。

### 命令行用法

```bat
agent_host.exe --dry-run                仅组装材料包并打印统计（不调接口，排查用）
agent_host.exe --days 3                 改窗口天数
agent_host.exe --input-dir D:\news\out  指定稿件目录
agent_host.exe --dump-bundle b.txt      导出材料包原文，便于核对抽取结果
agent_host.exe --help
```

### 重新编译

```bat
build_desk.bat
```

> 编译前请先关闭正在运行的 `news_desk.exe`，否则链接阶段会报
> `cannot open output file news_desk.exe: Permission denied`（exe 被占用）。
> `agent_host.exe` 不依赖界面，可以随时单独重编。

等价命令：

```bat
g++ -std=c++17 -O2 -static -static-libgcc -static-libstdc++ agent_host.cpp -o agent_host.exe -lwinhttp
g++ -std=c++17 -O2 -static -static-libgcc -static-libstdc++ -mwindows news_desk.cpp -o news_desk.exe -lshell32 -luser32 -lgdi32 -lgdiplus
```

生成安装包：

```bat
build_installer.bat
```

它会先把现有的三个 exe、`skill\`、配置模板和卸载程序打成一个载荷，再用 MinGW
（含 `windres`）编译自绘安装界面，输出 `政策解读台-安装程序.exe`。
安装程序源码在 `installer\`，载荷清单在 `installer\payload.txt`，中间产物在
`installer_build\`（已忽略，不入库）。

### 已知限制

- 源码目录里的「获取新闻稿」走 `运行.bat`，每次都会先用 g++ 重编 `main.cpp`（约 10 秒）；
  机器上没有 g++ 时该脚本会回退到已有的 `fetch_news.exe`。安装包内附带的是只运行
  `fetch_news.exe` 的简化版 `运行.bat`，不会在用户机器上尝试编译。
- 材料包按 `max_input_chars` 截断，超出部分标为"无法取证"而非丢弃后静默分析。
- 10-26 这类"仅标题、无正文"的稿件会被识别为材料不全，不计入出现率统计。
- 解读接口**仅验证过 DeepSeek 官方端点**；其他 OpenAI 兼容服务需自行实测（见上文第 8 节）。
- `agent.ini` 含明文密钥且被 `.gitignore` 屏蔽；上传仓库前请确认 `git status` 里没有它。

### 解读方法论的来源

`skill/` 的解读方法论改写自 [`yuwanxiaomian/china-news-analysis`](https://github.com/yuwanxiaomian/china-news-analysis)（MIT）：
逐句差分、缺位四闸门、证据分级均出自该项目；针对《新闻联播》《人民日报》的
**结构信号**（栏目内排序、版面、文体层级、同源通稿不叠加）是本仓库新增的扩展。
原项目的联网取数、预算数据交叉验证部分已移除——本工具只处理本地材料。

署名与许可声明保留在 `skill/SKILL.md` 末尾。

## 合规

- 《人民日报》电子版声明：内容仅供阅读、学习研究使用；未经授权，不得用于商业目的，
  不得转载、复制、发行或镜像。
- 《新闻联播》内容版权归中央广播电视总台所有。
- 本程序仅用于个人学习、研究、资料归档；请勿公开再分发或做镜像站。
