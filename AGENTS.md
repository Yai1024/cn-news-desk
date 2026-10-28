# 开发说明（给协作者与 Agent）

本仓库包含两个程序，共用一个 `output/` 语料目录：

| 程序 | 源码 | 作用 |
|---|---|---|
| `fetch_news.exe` | `main.cpp` | 抓取《新闻联播》《人民日报》到 `output/` |
| `news_desk.exe` | `news_desk.cpp` | 图形界面：获取新闻稿 / 发起解读 |
| `agent_host.exe` | `agent_host.cpp` | 独立解读 Agent：组材料包、载入 `skill/`、调 API |

## 构建

本机约定：无 cmake、无 make，使用 MinGW g++。

```bat
build_desk.bat          :: 编译 news_desk.exe + agent_host.exe
编译.bat                :: 只编译 fetch_news.exe
```

等价命令：

```bat
g++ -std=c++17 -O2 -static -static-libgcc -static-libstdc++ agent_host.cpp -o agent_host.exe -lwinhttp
g++ -std=c++17 -O2 -static -static-libgcc -static-libstdc++ -mwindows news_desk.cpp -o news_desk.exe -lshell32 -luser32 -lgdi32 -lgdiplus
g++ -std=c++17 -O2 -static -static-libgcc -static-libstdc++ main.cpp -o fetch_news.exe -lwinhttp -lshell32
```

编译前关闭正在运行的 `news_desk.exe`，否则链接会报 exe 被占用。

## 约定

- 源码 UTF-8 无 BOM、LF 换行；中文直接写在字符串里（GCC 默认输入/执行字符集均为 UTF-8）。
- 窗口程序用 Win32 API，不引入第三方 GUI 框架；网络统一走 WinHTTP。
- 界面自绘用系统自带的 GDI+（`-lgdiplus`，属 Windows 组件，不算第三方框架），
  配色与尺寸集中在 `news_desk.cpp` 顶部 `namespace C` 与 `computeLayout()`。
- 二进制文件以 `exe` 形式随仓库分发，便于没有编译器的机器直接运行。

## 绝不提交

- `agent.ini`：含明文 API 密钥（`.gitignore` 已屏蔽，模板见 `agent.ini.example`）
- `output/`：抓取到的新闻正文与解读结果，版权归原媒体，不授权再分发

提交前自查：

```bat
git status
git ls-files | findstr /i "agent.ini"    :: 应当没有任何输出
```

## 上游与许可

- 程序代码：MIT（见 `LICENSE.md`）。
- 抓取内容版权归中央广播电视总台、人民日报社所有，仅限个人学习研究。
- `skill/` 的解读方法论改写自 `yuwanxiaomian/china-news-analysis`（MIT），
  并针对《新闻联播》《人民日报》的文体结构做了扩展；署名保留在 `skill/SKILL.md` 末尾。

## 已知限制

- 抓取依赖第三方镜像与官网页面结构，改版后选择器可能失效（日志在 `output/logs/`）。
- 解读接口目前只验证过 DeepSeek 官方端点；其他 OpenAI 兼容端点需自行实测。
- `12152` 等传输层抖动由 `agent_host` 自动重试，配置项见 `README.md` 的 API 章节。
