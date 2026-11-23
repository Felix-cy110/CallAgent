# CallAgent

CallAgent 将 Tutti 的 `@session` 能力抽取为端侧 Client Skill，让 **Claude Code、Codex、WorkBuddy** 的已有会话相互引用上下文，并按用户明确指令向指定会话发送消息。项目无 GUI，以命令行程序或后台服务形式运行。

当前只做本机阶段；后续依次扩展到局域网不同电脑、连接同一配置公网服务的电脑。只做 session，不做其他 `@` 功能。完整范围记录在 [产品计划](docs/roadmap.md)。

## 技术栈

- **语言**：C++20
- **构建系统**：CMake 3.20+
- **网络库**：libhv v1.3.4
- **运行形态**：无 GUI 的命令行程序或后台服务
- **许可证**：自有工程使用 MIT；Tutti 抽取的 Skill 保留 Apache-2.0，见包内 LICENSE/NOTICE

## 工程约束

- 业务代码使用标准 C++20，不依赖编译器扩展。
- 网络通信统一基于 libhv 实现。
- 不引入 Qt、GTK、Electron 等 GUI 框架。
- 第三方依赖由 CMake 管理，并锁定明确版本，保证构建可复现。
- Skill 操作约定与原生会话适配器分离，能力必须以真实接口为准。

## Session Skill

目标交互：本机后台 Client 汇总三个 Desktop 的 session，为每端最活跃的 3 个生成 AT Skill 入口，最多 9 个。用户在桌面端的 `/` 菜单选择 `AT codex 手机端小小优化` 这样的条目；session 名超过 10 个字截断，实际操作仍绑定原生会话 ID。详细方案及参考实现见 [本机桌面与 AT 设计](docs/desktop-at-design.md)。

独立包位于 [`skills/callagent-session/`](skills/callagent-session/)，入口是 [SKILL.md](skills/callagent-session/SKILL.md)，包含会话定位、上下文读取、明确指令后的投递与结果处理规则。来源与抽取边界见 [抽取记录](docs/session-skill-extraction.md)。

**当前已完成 Skill 抽取与桌面接入源码调查。** 后台 Client、动态 AT 发布和三种 Agent 的原生桌面会话适配尚未实现，`callagent` 仍是最小工程入口，没有会话子命令；安装 Skill 不等于完成三端互通。

## 构建

```bash
cmake -S . -B build
cmake --build build
./build/callagent
```

首次配置时，CMake 会自动获取 libhv 源码并构建静态库。

## 目录结构

```text
CallAgent/
├── AGENTS.md
├── CMakeLists.txt
├── LICENSE
├── README.md
├── docs/
│   ├── desktop-at-design.md
│   ├── roadmap.md
│   └── session-skill-extraction.md
├── skills/
│   └── callagent-session/
│       ├── SKILL.md
│       ├── agents/openai.yaml
│       ├── references/tutti-session-commands.md
│       ├── LICENSE
│       └── NOTICE
└── src/
    └── main.cpp
```
