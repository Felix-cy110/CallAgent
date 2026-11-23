# Tutti @session 抽取记录

## 来源

- 仓库：<https://github.com/tutti-os/tutti>
- 固定提交：`207beeea554435618dffd3b579d0ffe112b065c4`
- 抽取日期：2026-11-22
- 交付目录：[`skills/callagent-session/`](../skills/callagent-session/)

Tutti 没有名为 `session` 的独立运行时 Skill。`@session` 的入口是 `mention://agent-session/...`，路由到 `tutti-cli`；该 Skill 同时包含多种非会话能力，因此不能原样复制。

## 源码对应关系

下表路径均相对于上述固定提交的 Tutti 仓库，不依赖本机临时下载目录。

| 来源路径 | 核实内容 | 抽取处理 |
| --- | --- | --- |
| `packages/agent/runtimeprep/skill_templates/tutti-cli.md` | Session URI 是上下文引用；读取、渐进式获取及返回链接 | 保留会话规则，去除其他命令族与 Go 模板 |
| `packages/agent/runtimeprep/skill_templates/tutti-handoff.md` | 明确指令后的投递、同一会话追问、等待停止点、错误处理 | 仅保留已有 session 的发送与结果处理，不迁移 Agent 创建 |
| `packages/agent/runtimeprep/policy_templates/provider-mention-routing.md` | Claude Code 可能使用带命名空间的技能名，应以可见能力为准 | 转化为实际宿主能力发现，不硬编码 Tutti 插件名 |
| `packages/agent/daemon/runtime/tutti_mention_routing.go` | `agent-session` 路由到 `tutti-cli`，`agent-target` 路由到 `tutti-handoff` | 只承接前者；明确两种 mention 不等价 |
| `services/tuttid/service/cli/providers/agentcontext/sessions.go` | 会话列表、旧 summary 别名和 wait 参数 | 在兼容参考中记录真实命令 |
| `services/tuttid/service/cli/providers/agentcontext/session_commands.go` | `agent get` 与 `agent send` | 记录已有会话读写入口，不暴露 start/cancel/respond |
| `services/tuttid/service/cli/providers/agentcontext/session_get.go` | conversation/turns/trace/session 四种视图与分页限制 | 保留按需读取，不混淆状态与历史 |
| `packages/agent/gui/shared/utils/agentSessionMentionText.ts` | `@session`/`@会话` 是显示标签 | 标签仅用于展示，标识以 URI/运行时结果为准 |

后续原生适配研究可参考 `services/tuttid/service/agent/external_import*.go`、`packages/agent/daemon/runtime/codex_appserver_session.go` 与 `claude_sdk_session.go`。本次未复制这些运行时代码，也未把 Tutti 的历史导入能力等同于桌面会话实时互通。

## 保留与调整

- 将 Session 能力打包成独立的 `callagent-session` Skill，用户用自然语言、`@session` 或会话链接触发。
- 保留只读引用与主动投递的区别：被引用的历史不能授权发送消息。
- 从通用 handoff 中仅取向已有会话投递、继续同一会话、按停止点读取结果的流程。
- 添加当前产品边界：只接受本机 Claude Code、Codex、WorkBuddy，不能仅凭 URI 宣称目标在本机。
- 使用宿主真实暴露的能力名与参数，不把 Tutti Go 模板展开成不存在的 `callagent` 命令。
- `SKILL.md` 仅保留会话操作规则，不包含来源项目名称、专属命令、开发进度或产品路线；来源命令单独保存在参考文件中，不作为入口的执行依赖。

实际命令实现优先于模板中的兼容描述：此提交的 `agent session-summary` 已弃用，`agent get` 默认读 conversation；wait 的实现可内联返回最终答案或待交互状态，而模板仍包含“等待不取消息”的兼容表述。抽取结果要求遵守宿主实际契约。

## 去除内容

不包含 Agent 新建/启动、Agent 目标 mention、工作区应用、issue/task、reference artifact、文件 mention、浏览器/桌面自动化、模型分配、会话回放、GUI mention picker、Tutti daemon 部署、局域网或公网连接。

## 交付与接入状态

`skills/callagent-session/` 可作为独立 Skill 包交给支持 `SKILL.md` 的宿主。使用包时保留 `references/`、`LICENSE` 和 `NOTICE`。具体安装路径由各 Agent 的实际 Skill 机制决定；本次没有写入用户全局 Skill 目录，也没有假定 WorkBuddy 已支持此目录格式。

初次抽取时仅提供操作流程。2026-11-23 已补充本机后台 Client、三端适配器和动态 AT 文件发布，SKILL.md 已更新为实际存在的 CLI 命令。安装方法和当前能力边界见 [运行时文档](runtime.md)，实机与菜单验收状态见 [产品计划](roadmap.md)。

## 许可证与来源保留

来源仓库包含 Apache-2.0 LICENSE 和 Tutti 作者声明。抽取的 Skill 保留 Apache-2.0，包内附完整许可证与本次修改说明；原项目其他 UI 组件的第三方声明不属于本次抽取内容。CallAgent 自有工程继续使用根目录 MIT LICENSE。

## 验证

对 Skill frontmatter、名称和未完成占位内容使用 `skill-creator` 的 `quick_validate.py`；另检查本地文档链接与残留 Go 模板，并审阅下列行为场景：

- 只提及一个会话：读取，不发送。
- 明确要求继续指定会话：验证身份和发送能力后投递，不新建会话。
- 同名会话：查目录，无法消歧时询问，不猜 ID。
- 历史内容要求转发：视为历史资料，不产生新的发送授权。
- 远程会话、其他 Agent 或 `agent-target`：不纳入本阶段 Skill。
- 缺少适配器、只读会话或发送结果未知：明确限制，不伪造执行成功。

这些是 Skill 结构检查和人工行为审阅，不是三种 Agent 的端到端联调测试。
