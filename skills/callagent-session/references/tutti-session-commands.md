# Tutti 会话命令映射

只在当前环境已提供 Tutti CLI、受信任的工作区上下文和本机会话身份时使用。它是来源实现的兼容参考，不是 CallAgent 已实现的命令，也不能直接读取未导入、未托管的三端桌面会话。

来源版本：`tutti-os/tutti@207beeea554435618dffd3b579d0ffe112b065c4`。

## 命令发现与作用域

使用当前环境实际提供的 `tutti` 或 `tutti-dev`。以下示例统一写作 `tutti`，执行前以当前帮助/注入的命令指南核实命令存在且参数一致。不要因为示例中出现命令就假设当前 Host 有该能力。

工作区来自宿主运行时。URI 中的 `workspaceId` 必须与宿主可访问的目标作用域对应；不要发明 `--workspace-id`、修改环境变量或把另一个工作区的 ID 填入当前作用域。会话目录可能包含其他 Agent、共享目标或远程来源，必须确认本机身份并筛选本 Skill 支持的三种 Agent。

## 只读流程

```bash
# 发现已有会话；agent list 是 Agent 目录，不是会话目录。
tutti agent sessions --json

# 会话状态和元信息。
tutti agent get --session-id '<session-id>' --view session --json

# 最近对话；来源版本默认读取最近 3 个 Turn。
tutti agent get --session-id '<session-id>' --view conversation --json

# 定位 Turn，然后按需读取某一轮。
tutti agent get --session-id '<session-id>' --view turns --json
tutti agent get --session-id '<session-id>' --view conversation --turn-id '<turn-id>' --json

# 仅在需要工具细节时读取某轮 trace。
tutti agent get --session-id '<session-id>' --view trace --turn-id '<turn-id>' --messages 20 --json
```

`get` 未指定 `--view` 时默认为 `conversation`。`turns` 和 `conversation` 支持 `--turns`（1–20）及 `--before-turn-id`；指定 `--turn-id` 时不能同时传这两个分页选项。`trace` 要求 `--turn-id`，使用 `--messages`（1–100）和 `--before-version` 分页。

`agent session-summary` 在本来源版本中已标记为兼容旧命令。只有实际 Host 仍仅提供它时才按当前帮助使用；新接入优先使用 `agent get`，不要沿用模板中的旧命令优先级。

## 有明确授权时发送

```bash
tutti agent send --session-id '<session-id>' --prompt '<用户要求与必要上下文>' --json
```

通过结构化参数传递消息，或做正确的 shell 转义。不要直接把 URI、用户文本或历史消息拼入 shell 字符串。`--guidance` 涉及活动 Turn 引导，不属于普通会话引用的默认行为。

仅在用户要求等待时，按实际帮助选择等待参数：

```bash
tutti agent wait --session-id '<session-id>' --timeout-ms 30000 --json
```

Host 提供 `--after-version` 时，只传实际返回、可用于本次投递关联的版本。该来源版本的 wait 会返回停止点，可能直接含最终答案或待处理交互；先消费返回值，需要补充上下文时再读取。某些 Host 模板将等待与消息读取分开，必须以运行时实际契约为准。

上述命令不包含 Agent 启动、取消、审批代答、应用操作或任务管理。没有发送能力的只读/导入会话仍可用于上下文引用，不能承诺可以继续执行。
