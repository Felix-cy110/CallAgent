# 本机 Client 使用与验证

记录日期：2026-11-23。业务运行时使用 C++20、CMake、libhv。当前构建和协议测试平台为 macOS ARM64；Windows 传输未实现，CMake 明确拒绝 Windows 构建。Linux 的公共 POSIX 路径尚未实测，WorkBuddy 进程验证暂限 macOS。

## 启动与发布

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
cmake --install build --prefix "$HOME/.local"
"$HOME/.local/bin/callagent" init
```

构建需要 OpenSSL、SQLite3、ICU 的开发库；ICU 用于 Unicode grapheme 截断，包含组合字符和 emoji。CMake 获取固定版本 libhv v1.3.4，JSON 使用该版本自带的 nlohmann JSON。依赖不在默认查找路径时指定 `CMAKE_PREFIX_PATH`。测试还使用 Python 3，Claude 插件测试使用 Node.js。

配置默认为 `~/.callagent/config.json`。`init` 不覆盖已有文件。配置内路径必须是绝对路径；Codex 默认使用 `CODEX_HOME` 或 `~/.codex`，Claude 使用 `CLAUDE_CONFIG_DIR` 或 `~/.claude`，WorkBuddy 默认 `~/.workbuddy`。所有命令支持 `--config`。

先检查配置中的桌面安装路径。Claude 的接收插件可以显式安装：

```sh
callagent install-claude-bridge
```

插件安装到配置中 Claude 根目录的 `skills/callagent-desktop-bridge`，控制目录为其根目录下的 `callagent-desktop-bridge`。它没有 `SKILL.md`，不会额外生成第十个 AT。插件通过原生 Mods hook 接收经过校验的帧，再调用当前桌面 session 的 `prompt.submit`；不修改应用安装包、全局权限或历史。当前引擎版本门槛为参考协议的 `2.1.309+`，限定 `2.1.x`。安装文件不代表已有桌面会话已加载插件；需要确认当前会话产生有效心跳。CallAgent 不自动重启 Claude Desktop。

默认只在状态目录 `generated/codex`、`generated/claude`、`generated/workbuddy` 生成三份相同目标列表，方便检查。要交给宿主发现，用其实际扫描目录配置发布，例如：

```sh
callagent publish --host codex --directory "$HOME/.agents/skills"
callagent publish --host claude --directory "$HOME/.claude/skills"
# WorkBuddy 必须填写该版本桌面实际扫描的 Skill 目录，不套用其他产品的目录。
callagent publish --host workbuddy --directory /absolute/path/to/workbuddy/skill-scan-directory
```

配置变动后重启 CallAgent Client 生效。启动服务：

```sh
callagent serve
# 或交给系统进程管理器；最小后台启动示例：
nohup callagent serve > "$HOME/.callagent/client.log" 2>&1 &
```

后台只有一个目录维护者，同一状态目录用进程锁防止重复运行。`SIGTERM` / `Ctrl+C` 关闭 Client，只回收自身连接和只读子进程。服务只监听 `127.0.0.1`，使用状态目录内私有随机 token；浏览器带 Origin 的请求被拒绝。当前没有 MCP server，Skill 通过 CLI 调用同一个后台 Client。

## WorkBuddy 自动接管（macOS）

`providers.workbuddy.autoEnableDebugPort` 默认为 `true`，已有配置缺少该字段时也采用此默认值。`enabled: false` 会关闭整个 WorkBuddy 适配；只把 `autoEnableDebugPort` 设为 `false` 则保留 RPC 连接，要求桌面已有可用调试入口。配置改变后重启 Client 生效。

新生成的配置不固定 WorkBuddy 安装路径。没有 `executable` 或其值为空时，每轮依次检查 `~/Applications/WorkBuddy.app`、`/Applications/WorkBuddy.app`；显式填写的绝对路径保持不变。自动重启仅接受 `WorkBuddy.app/Contents/MacOS/Electron`，不会管理任意其他可执行程序。当前账号来自配置数据目录的 `storage/skeleton/account-snapshot.json`，未安装、未登录或桌面未运行时等待，后续约每 5 秒重新检查。

接管规则：

- 检查同一用户、安装路径、PID 与进程启动时间；多个主进程归属不明时等待。按安装路径持有跨 Client 的本机互斥锁，避免不同状态目录同时接管。
- 已有 loopback 调试入口且 renderer 匹配安装路径时直接复用。入口存在但 renderer 尚未就绪时等待，不重启该实例。调试端口默认 `50641`，可显式配置。
- 已运行的普通实例缺少端口时，重新核对账号、进程身份和端口，发送 `SIGTERM` 请求退出，最多等待 12 秒。进程拒绝退出时不强杀，也不启动替代实例；同一进程不重复请求退出。
- 只有本轮实际关闭旧实例，确认其进程及子进程退出，且没有用户并发启动的新实例或端口冲突后，才尝试启动一次替代实例。只为新进程设置 `WORKBUDDY_REMOTE_DEBUGGING_PORT`，不修改全局环境、应用安装包或账号配置。
- 用户主动退出、替代实例退出或启动失败、Client 重启后发现桌面缺失，都不会自动拉起。Client 停止时取消尚未执行的重启；已运行的桌面保持运行。新进程隔离 Client 的监听句柄，避免阻塞 Client 之后重启。

**缺少端口时会重启 WorkBuddy 窗口，当前无法在这一步确认其内部任务或未发送草稿状态。** 自动接管的进程测试已完成，真实 WorkBuddy 的重启体验尚未验收。首次在有任务的桌面使用时可先关闭自动接管。

`callagent status` 返回 `workbuddySupervisor`，包含 `state`、`detail`、`pid`、`executable`、`debugPort` 和 `restartCount`。常见状态为 `waiting_installation`、`waiting_account`、`waiting_desktop`、`port_conflict`、`ambiguous_desktop`、`managed_elsewhere`、`restarting`、`waiting_ready`、`restart_not_completed`、`ready` 和 `disabled`。`ready` 只表示调试入口已就绪；账号鉴权、会话目录与投递仍由适配器分别验证。进入 `ready` 后自动安排一次目录刷新。

## 使用

```sh
callagent status
callagent list
callagent refresh
callagent read --target FULL_KEY --limit 20
callagent send --target FULL_KEY --request-id operation-001 --text-file /absolute/message.txt --authorize
callagent receipt --target FULL_KEY --request-id operation-001
```

`refresh` 只安排后台扫描；用 `status` 查看结果。每端独立按活动时间取前三，总数最多九个。Codex 与 WorkBuddy 使用原生更新时间，Claude 使用 transcript 修改时间；读取和心跳不会更新时间。未命名回退为“未命名会话”，显示标题按可见字符截到十个。

`key` 包含 provider、配置实例/账号和原生 session ID。标题或排名变化不影响身份。旧入口内容携带完整命令和 key，即使其目录后来退出 Top 3，仍通过已持久化的绑定定位原会话并再次验证；不会换成新排第三的会话。已观察身份保留，上限 10000。单端暂时失败时保留最后成功目录并标为 stale。

生成 Skill 内部 `name` 是唯一 ASCII 标识，完整 `AT <Agent> <十字标题>` 同时写入描述和 Codex `agents/openai.yaml` 的 `display_name`。文件包用原子目录交换更新；只删除带有效所有权清单且内容未被修改的生成包，保留其他 Skill 和用户改动。**三个 Desktop 的 `/` 菜单是否采用 display_name、是否能热刷新，尚未实机验收。** 尤其 Claude 原生命令名可能显示唯一标识，当前不能声称三个菜单都已显示目标格式。

读结果只投影用户/助手文本，明确 `completeHistory: false`。Codex 的 limit 单位为 turn，Claude 为 message，WorkBuddy 为 request；目前只支持最近 1–100 个单位，不支持全文导出、任意页游标和附件。目录扫描不加载会话正文。

投递前持久化 request ID 和正文摘要。相同目标、相同 request ID、相同正文重复调用只返回已有记录；相同 ID 携带不同正文会拒绝。崩溃或断连导致未知结果时不自动重发。回执确认与任务完成分别报告；Claude 当前只提供提交/接受回执，不提供执行完成关联。

## 三端适配

| 适配器 | 目录与上下文 | 投递与要求 |
| --- | --- | --- |
| Codex Desktop | 只读 app-server：`thread/list`、`thread/read`、`thread/turns/list`；目录最多 500 个候选，按时间向桌面 IPC 核实 owner，取前三 | 本地 length-prefix IPC、`thread-follower-start-turn`，继承桌面会话设置；没有 owner 就报错，不回退到 CLI 执行 |
| Claude Desktop Code | 活跃桌面注册（entrypoint、hostSessionId、PID、cwd），按需读取 transcript 的有效父链并排除 sidechain | 原生 peer IPC 加本地插件，校验凭据、会话、challenge、心跳、版本；忙碌时拒绝 |
| WorkBuddy Desktop | 当前账号快照及只读 SQLite；认证 Local RPC 读取当前会话 requests | 校验 loopback 监听者、桌面进程树和账号，经过 v2 双向 HMAC 握手；单条复用连接；macOS 默认自动发现安装并接管已有普通实例，带端口重启一次，退出语义见上节 |

Codex owner 探测每轮有 45 秒预算；不足以完成候选确认时保留上次目录，状态明确报错。WorkBuddy 历史校验限定最近 500 条原生目录，超出时报告不可访问，不猜测路由。配置支持每端一个实例，身份结构保留以后扩展多实例的空间。

## 验证记录

- `callagent_core`：每端配额、稳定排序、Unicode 截断、跨配置身份、发布保护、旧入口继续路由、读取不触发发送、授权检查、摘要冲突、回执、断连不重发、重启恢复和失败保留目录。
- `desktop_protocols`：真实 Client/CLI 进程连接模拟的三端原生 IPC，覆盖 Codex 只读子进程与桌面 owner、Claude 注册/peer socket、WorkBuddy renderer discovery/v2 HMAC/账号匹配、九个发布包和可执行 helper。还验证无 token 与浏览器 Origin 请求拒绝、重复 Client 锁、错账号和未知投递结果。
- `workbuddy_supervisor`：真实 Client 管理临时目录中的 C++ 桌面替身，覆盖缺失等待、带端口重启、健康复用、用户退出、Client 重启与句柄隔离、端口冲突、自动发现与登录、禁用接管、退出拒绝、替代进程失败、停止取消、多 Client 互斥和安装范围。测试不操作已安装的桌面。
- `claude_plugin`：用模拟 Mods 环境验证 hook 注册、心跳、身份拒绝、原生提交、忙碌拒绝和持久化回执防重。

这些测试不调用模型、不向真实用户会话发消息。实机只读检查结果单独记录在产品计划中；模拟协议成功不等于真实版本兼容，更不等于菜单刷新验收。

本次只读实机检查在 macOS 26.6 ARM64 上完成：Codex 返回 3 个不同的 Desktop-owned 会话；WorkBuddy 5.11.3 通过认证读取 9 条当前账号本地目录，发布其中 3 条；Claude 无可用桌面注册。实际生成 6 个 AT 包并通过 Skill 格式校验。没有向真实会话投递消息，也没有安装这些生成包到宿主扫描目录；原生菜单尚未验证。探测 Client 正常退出。
