# CallAgent

CallAgent 是一个运行在端侧设备上的 Client Skills 执行程序。项目专注于技能加载、调用和网络通信，不提供图形界面，以命令行程序或后台服务形式运行。

## 技术栈

- **语言**：C++20
- **构建系统**：CMake 3.20+
- **网络库**：libhv v1.3.4
- **运行形态**：无 GUI 的命令行程序或后台服务
- **许可证**：MIT

## 工程约束

- 业务代码使用标准 C++20，不依赖编译器扩展。
- 网络通信统一基于 libhv 实现。
- 不引入 Qt、GTK、Electron 等 GUI 框架。
- 第三方依赖由 CMake 管理，并锁定明确版本，保证构建可复现。
- Skill 能力以独立模块组织，具体接口和生命周期将在业务需求明确后定义。

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
├── CMakeLists.txt
├── LICENSE
├── README.md
└── src/
    └── main.cpp
```
