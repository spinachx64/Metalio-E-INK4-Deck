# Metalio-E-INK4-Deck (`espdeck`)

Metalio E-INK4 墨水屏桌面应用底座与固件编排工具。

## 核心能力

- **装机清单驱动（个人应用市场）**：在 `profiles/eink4.json` 中增删应用、调整首页顺序，无需手动修改上游 `CMakeLists.txt` 或首页启动器代码。
- **一键屏蔽官方应用**：通过 `keep_official_apps` 隐藏不需要的官方应用（默认仅保留 `setting`），同时自动裁剪代码释放约 1MB+ Flash 空间。
- **底座省电补丁抽离**：底层过度轮询降频与显示优化收口在 `base/eink4/`，官方 `Metalio-E-INK4` 仓库保持只读。
- **外部应用仓库挂载**：支持直接引用独立的外部应用仓库（如 `usagi-pines`、`codex-monitor`），增量同步源码，互不覆盖。
- **在线 HTTPS OTA 升级**：支持公网无线升级（`https://ota-esp.cookup.cn`），内置 SHA-256 完整性校验与双分区安全回滚。
- **MAC 校验防刷错**：可在本地未提交的 `profiles/eink4.local.json` 中配置私有 `board_mac` 与 `ota.remote_ssh`，烧录前自动校验并默认使用 `app-flash`。

## 快速使用

```bash
# 查看设备配置、启用应用与屏蔽的官方应用
./espdeck list

# 增量同步底座补丁与应用源码到 .build/eink4/
./espdeck prepare eink4

# 编译 E-INK4 固件（自动激活 ESP-IDF 环境）
./espdeck build eink4

# 校验 MAC 地址并安全烧录（app-flash）
./espdeck flash eink4

# 线上 HTTPS OTA 发布（OTA 域名：https://ota-esp.cookup.cn）
./espdeck ota publish eink4 --build

# 快速脚手架新建一个自研 E-INK4 插件应用
./espdeck new-app eink4 my-pomodoro
```

## 上游官方固件基线

为方便后续追踪官方改动与维护 `base/eink4/` 补丁，当前对应的官方基线版本如下：

| 设备 | 上游仓库 | 基线 Commit | 说明 |
| --- | --- | --- | --- |
| `eink4` | `Metalio-E-INK4` | `d12f481f4a42420634bb56a928f3959a977367d4` | `metalio-e-ink4 v2.0.63版本` (2026-10-13) |

## 目录结构

```text
espdeck/
├── espdeck                        # 统一 CLI 入口
├── base/eink4/                    # 底座公共省电/显示补丁与 sdkconfig 片段
├── profiles/                      # 设备装机清单（私有覆盖放 *.local.json，已 gitignore）
├── apps/                          # 插件应用目录（支持本地源码或挂载外部仓库）
├── espdeck_core/                  # 编排引擎与平台适配器
└── .build/                        # 隔离编译目录（已 gitignore）
```
