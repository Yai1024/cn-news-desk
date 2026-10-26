# 许可与使用记录（LICENSE）

| 项目 | 内容 |
|---|---|
| 项目名称 | 人民日报 / 新闻联播每日抓取器 |
| 目录 | `project_basic_cpp1` |
| 文档版本 | **1.0** |
| 生效日期 | 2026-10-26 |
| 维护者 | Yai1024 |
| 适用范围 | 本目录内的程序、脚本、配置、文档及抓取产物 |
| 权威性 | 本文件是许可与使用范围的唯一权威记录；与 README 不一致时以本文件为准 |

> **一句话：代码采用 MIT；抓取到的新闻内容不授权，仅限个人学习研究。**

---

## 一、许可方法（怎么授权）

本项目采用**“代码与内容分离”**的授权方式，避免把别人的新闻作品随代码一起再许可。

### 1. 程序代码：MIT License

MIT 全文见[附录 A](#附录-amit-license)，覆盖：

- `main.cpp`
- `运行.bat`、`编译.bat`
- `config.ini`
- `README.md`、`LICENSE.md` 中的原创文字
- 由上述源码编译得到的 `fetch_news.exe`

### 2. 抓取内容：不授予任何许可

- `output/*.txt`、`output/logs/*.log` 中由《人民日报》《新闻联播》等产生的标题、正文、链接，
  **版权归原权利人**，不随 MIT 授权。
- 本项目只做“个人学习研究用的本地缓存”，不主张对这些内容享有任何权利。
- 任何人都不得因为拿到本项目的代码，就认为可以转载、商用或镜像这些内容。

### 3. 其他文件

- 目录内的 `人民日报_新闻联播_每日获取方案.txt` 为用户提供/第三方整理的资料，
  **不在 MIT 范围内**，权利归其原作者。
- 第三方网页、接口数据、Markdown 稿件等，按其各自条款使用，见第四节。

### 4. 为什么这样分

MIT 只覆盖我们自己写的程序逻辑。新闻正文、标题、图片等是媒体的作品，
本项目无权再许可；程序可以复制、修改，但抓下来的内容不能因此获得转载权。

---

## 二、内容范围与权利归属

| 对象 | 权利 / 许可 | 说明 |
|---|---|---|
| `main.cpp`、`运行.bat`、`编译.bat`、`config.ini`、`README.md`、`LICENSE.md` | **MIT** | 本项目原创 |
| `fetch_news.exe` | **MIT** | 由上述源码编译 |
| `output/新闻联播_YYYYMMDD.txt`、`output/新闻联播_YYYYMMDD_部分.txt` | 中央广播电视总台 | 不授权；仅个人学习研究 |
| `output/人民日报_YYYYMMDD.txt`、`output/人民日报_YYYYMMDD_部分.txt` | 人民日报社 / 人民网 | 不授权；仅个人学习研究 |
| `output/logs/*.log` | 本项目 + 原媒体 | 日志中的标题、链接归原媒体 |
| `人民日报_新闻联播_每日获取方案.txt` | 用户 / 第三方 | 不在 MIT 范围 |
| 第三方网页、Markdown、接口返回数据 | 各自权利人 | 仅作为运行时数据源，不随本项目分发 |

---

## 三、使用规范（必须遵守）

### 允许

- 个人阅读、学习、研究、资料归档。
- 在保留来源、日期和版权说明的前提下，本地保存抓取结果。
- 阅读、修改程序代码（遵守 MIT 的保留版权声明要求）。

### 禁止

- 转载、复制、发行、镜像、公开再分发抓取到的新闻内容。
- 将抓取内容用于商业产品、付费内容站、训练数据售卖等商业目的。
- 绕过验证码、登录、付费墙或其他访问控制。
- 大规模、高频、并发抓取，或对来源站点造成压力、攻击。
- 移除、遮挡或伪造文件头部的来源、日期、版权标识。
- 以本项目名义宣称对《人民日报》《新闻联播》等内容享有权利。

### 技术要求

- 默认请求间隔 1000 毫秒、串行抓取；不要随意调低到 500 毫秒以下。
- 遵守来源网站的 `robots.txt`、服务条款和版权声明。
- 将抓取内容用于商用、机构使用或对外发布前，自行向权利人取得授权。
  （MIT 允许对**代码**进行商业使用，但不代表可以商业使用新闻内容。）

---

## 四、第三方来源与条款

| 来源 | 在本项目中的用途 | 许可 / 条款 |
|---|---|---|
| `paper.people.com.cn`（人民日报电子版） | 人民日报版面与文章正文 | 官方电子版版权声明：仅供阅读、学习研究；未经授权不得商用、转载、复制、发行或镜像 |
| `tv.cctv.com`（央视网《新闻联播》） | 新闻联播日页、单条新闻正文 | 内容版权归中央广播电视总台；按其页面条款使用 |
| `raw.githubusercontent.com/DuckBurnIncense/xin-wen-lian-bo` | 新闻联播每日 Markdown（首选源） | 仓库代码为 **MIT**；其整理的新闻文字版权归央视，本项目仅作数据源，不复制其代码 |
| `cn.govopendata.com` | 新闻联播文字版（无代理回退源） | 第三方站点；本项目未对其内容声明许可，使用以其页面条款为准 |
| WinHTTP / Windows | HTTPS 请求 | Microsoft 系统 API，不随本项目分发 |
| MinGW-w64 / GCC 运行时 | 编译与静态链接 | GPLv3 + GCC Runtime Library Exception；生成的二进制可再分发 |

> 第三方站点可能改版、停更或变更条款。条款变化时，以该站点最新声明为准，
> 并按第七节更新本记录。

---

## 五、免责声明

1. 本程序按“现状”提供，不保证任何来源的可用性、完整性、及时性或准确性。
2. 使用本程序及抓取内容产生的风险与责任，由使用者自行承担。
3. 本记录不构成法律意见；涉及商用、出版、机构使用等场景，请咨询专业法律人士。
4. 若权利人认为本项目或其产物侵犯其权利，请停止使用并删除相关抓取文件。

---

## 六、变动记录

### 6.1 许可文档版本

| 版本 | 日期 | 类型 | 变更内容 | 影响 |
|---|---|---|---|---|
| 1.0 | 2026-10-26 | 初始 | 建立许可记录；代码采用 MIT；抓取内容不授权、仅限个人学习研究；列明第三方来源；确立变更规则 | 初始版本 |

### 6.2 项目功能变更与许可影响

| 日期 | 项目变更 | 对许可的影响 |
|---|---|---|
| 2026-10-26 | 初版：抓取当日人民日报、新闻联播，按日期保存 txt | 确立“代码 MIT / 内容不授权”的结构 |
| 2026-10-26 | 增加最近 7 天自动补抓、跳过已有文件、部分文件重试与升级 | 无许可变更；仍为 1.0 |
| 2026-10-26 | 增加无代理回退源（GovOpenData、央视网） | 无许可变更；新增来源已列入第四节 |

---

## 七、变更规则（以后怎么改）

### 版本号

采用 `MAJOR.MINOR.PATCH`：

- **MAJOR**：改变代码许可类型，或改变抓取内容的使用权限（例如允许再分发）。
- **MINOR**：新增/删除数据源、调整使用规范、第三方条款变化。
- **PATCH**：文字修正、链接更新、排版调整，不改变权利与义务。

### 每次变更的步骤

1. 修改本文件对应章节。
2. 更新文首的“文档版本”和“生效日期”。
3. 在 6.1 增加一行，写清变更原因与影响；**不删除历史行**。
4. 若代码许可变化：替换附录 A，并同步更新 `README.md` 与 `main.cpp` 头部说明。
5. 若内容范围或使用规范变化：同步更新 `README.md` 的“合规”部分。
6. 若新增/移除第三方来源：同步更新第四节的表。
7. 变更后重新检查：`README.md`、`main.cpp` 头部、本文件三处表述一致。

### 原则

- 历史版本只增不删，保证可以追溯。
- 任何放松内容使用限制的变更都必须升级 MAJOR，并说明授权依据。
- 权利人要求停止使用时，应立即停止分发相关内容并记录处理结果。

---

## 附录 A：MIT License

```text
MIT License

Copyright (c) 2026 Yai1024

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

> 说明：MIT 仅覆盖上表中的程序代码与原创文档文字；抓取到的新闻内容不在 MIT 范围内。

---

## 附录 B：参考依据

- 目录内《人民日报_新闻联播_每日获取方案.txt》第七节“注意事项与合规”。
- 人民日报电子版：<https://paper.people.com.cn/>
- 央视网《新闻联播》：<https://tv.cctv.com/lm/xwlb/>
- DuckBurnIncense/xin-wen-lian-bo 仓库（MIT）：<https://github.com/DuckBurnIncense/xin-wen-lian-bo>
- cn.govopendata：<https://cn.govopendata.com/xinwenlianbo/>
- 本项目说明：[README.md](README.md)

---

*本记录自 2026-10-26 起生效。*
