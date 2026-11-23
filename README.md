# CallAgent

在用户同一台电脑上，让 **Codex Desktop、Claude Desktop Code、WorkBuddy Desktop** 的已有会话相互引用，并在用户明确要求时向指定会话投递消息。技术栈固定为 **C++20、CMake、libhv**，无 GUI。

后台 Client 为每种 Agent 选取最近活跃的 3 个会话，生成最多 9 个 AT Skill 包。目标显示格式为 `AT codex 手机端小小优化`；标题按 Unicode 可见字符截取前 10 个，目标身份与标题、排名无关。

当前已实现后台服务、CLI、三个原生适配器、动态 Skill 文件发布、投递防重与回执查询，并通过模拟三端原生协议的端到端测试。**当前不宣称三个桌面的 `/` 菜单显示和热刷新已完成验收，也不宣称真实桌面之间的发送已完成联调。** 平台首先落地 macOS；实际验证状态见 [产品计划](docs/roadmap.md)。

## 构建与启动

需要 CMake 3.20+、C++20 编译器，以及 OpenSSL、SQLite3、ICU 开发库。libhv 由 CMake 固定版本获取；测试使用 Python 3 和 Node.js。

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
./build/callagent init
./build/callagent serve
```

另一个终端中：

```sh
./build/callagent list
./build/callagent read --target FULL_KEY --limit 20
```

默认配置在 `~/.callagent/config.json`。后台只监听本机环回地址，通过私有 token 验证 CLI。每次读取/发送均重新核实原生目标，不会另起 CLI 会话。只读 app-server 子进程只用于 Codex 目录与历史。

默认生成包位于 `~/.callagent/runtime/generated/<host>/`，供检查后发布到宿主实际扫描目录。Claude 投递需要加载随项目提供的桌面桥接插件。macOS 上，Client 默认自动发现已登录的 WorkBuddy；普通运行实例缺少调试入口时，会请求退出并仅为替代进程设置端口后启动一次。用户主动退出后保持关闭，正常入口直接复用。该过程会重启窗口，尚不判断原生任务是否忙碌；可用 `providers.workbuddy.autoEnableDebugPort: false` 关闭自动接管。安装、发布、后台运行、命令和限制见 **[运行时使用文档](docs/runtime.md)**。

独立操作规则保留在 [`skills/callagent-session/SKILL.md`](skills/callagent-session/SKILL.md)。实际 AT 入口由后台为每个稳定目标生成；安装静态 Skill 本身不会启动 Client。

## 代码组织

| 目录 | 职责 |
| --- | --- |
| `src/application` | 配置、目录刷新、身份绑定、读取/投递编排 |
| `src/domain` | 每端 Top 3、稳定身份、Unicode 标题处理 |
| `src/providers` | Codex、Claude、WorkBuddy 原生协议适配 |
| `src/transport` | 本地 HTTP、只读子进程、Unix IPC、认证 WebSocket |
| `src/persistence` | 原子文件、进程锁、WorkBuddy 只读 Repository |
| `src/skills` | AT 包生成、所有权校验、原子替换与回收 |
| `resources/claude-desktop` | 原生 Mods 接收插件；只做本机会话文本投递 |
| `tests` | 业务不变量、独立 Client/CLI 及三端原生协议 fixtures |

分层和原生接入协议参考 ObeingWorkspace，迁移依据见 [本机桌面与 AT 设计](docs/desktop-at-design.md)。只实现产品计划第一阶段；局域网、公网连接仍是后续阶段。

项目自有代码使用 MIT。抽取包保留 Apache-2.0 LICENSE 与 NOTICE；来源说明保留在 [抽取记录](docs/session-skill-extraction.md)，不写入操作用 SKILL.md。
