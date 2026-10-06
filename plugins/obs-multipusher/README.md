# obs-multipusher

OBS Studio 插件，将 `multipusher` Fyne 桌面推流器的功能集成到 OBS 内。采集 OBS 当前场景输出，编码为 4 路 ABR 视频流（H.264/HEVC），通过 Tencent SRT 推送到腾讯云直播，并提供 Qt6 可停靠面板（Dock）进行实时控制。

- 版本：**v1.1.5**
- 作者：**Amor**
- 语言：C++17
- 许可证：与 OBS Studio 主项目一致

> **已集成进 OBS 源码树**：本插件现位于 `plugins/obs-multipusher/`，作为 OBS 内置模块随主工程一起编译（Windows）。构建时启用 `ENABLE_MULTIPUSHER`（默认 ON）即可，无需独立 SDK 配置。
> ```bash
> cmake --preset windows-x64        # 或按 OBS 官方构建流程配置
> cmake --build build_x64 --target obs-multipusher
> ```
> 产物 `obs-multipusher.dll` 安装到 `obs-plugins/64bit/`，数据文件（locale、`pusher.local.json`）安装到 `data/obs-plugins/obs-multipusher/`。
> 下文“编译/部署”章节为独立仓库构建方式的原始说明，仅作参考。

## 架构

```
obs-multipusher.dll
├── plugin-main        OBS 插件入口 & 生命周期
├── multipusher-dock   Qt6 Dock UI 面板（需 HAS_DOCK_UI=1）
├── config-manager     加载/保存/校验 pusher.local.json
├── srt-auth           腾讯云 SRT 推流签名
├── stream-ladder      5 档 ABR 流梯管理
├── output-manager     管理多路 OBS encoder + output
├── device-enumerator  枚举 DirectShow 音视频设备
├── player-link        生成播放器预览链接
├── status-reporter    向 backend 上报推流状态
└── utils              工具函数
```

## 与 Fyne multipusher 的对比

| 特性 | obs-multipusher (本插件) | multipusher/ (Fyne) |
|------|--------------------------|---------------------|
| 运行方式 | OBS 插件 DLL | 独立桌面应用 |
| UI 框架 | Qt6 Dock（嵌入 OBS） | Fyne (Go) |
| 语言 | C++17 | Go |
| 视频源 | OBS 当前场景输出 | DirectShow / .mp4 文件 |
| 配置格式 | JSON (`pusher.local.json`) | YAML (`pusher.local.yml`) |
| 配置文件查找 | CWD → `conf/` → `bin/conf/` | CWD → `conf/` → `bin/conf/` |
| 适用场景 | 嵌入 OBS 工作流 | 独立轻量推流 |

## 环境要求

### Windows（主要目标平台）

| 工具 | 版本 / 路径 |
|------|------------|
| Visual Studio 2022 | 含 MSVC v143 工具链 |
| CMake | 3.16+ |
| OBS Studio 源码 | 提供头文件（`libobs/`、`frontend/api/`） |
| OBS Studio 构建产物 | 提供 `.lib` 导入库和 `obsconfig.h` |
| Qt6 SDK | Widgets + Core（来自 obs-deps） |

> **注意**：Qt6 和 OBS SDK 通常随 OBS 源码构建一起提供。如果没有 Qt6，插件仍可编译，但 Dock UI 将被跳过（`HAS_DOCK_UI` 未定义），插件以无界面模式运行。

### Linux / macOS

理论上支持（CMakeLists.txt 中有平台分支），但当前主要开发和测试均在 Windows 上进行。

## 编译

### 方式一：使用 build.sh（推荐）

```bash
cd obs-multipusher

# 自动检测 OBS SDK（默认路径 C:/obs-studio/build）
./build.sh

# 指定 OBS SDK 构建路径
./build.sh D:/winapp/obs-studio
```

脚本内部流程：
1. `mkdir -p build && cd build`
2. `cmake .. -DCMAKE_BUILD_TYPE=RelWithDebInfo -DOBS_SDK_DIR=...`
3. `cmake --build . --config RelWithDebInfo -j$(nproc)`

### 方式二：手动 CMake

```bash
cd obs-multipusher
mkdir -p build && cd build

# 配置（根据你的环境调整路径）
cmake .. -G "Visual Studio 17 2022" -A x64 -DCMAKE_BUILD_TYPE=RelWithDebInfo -DOBS_SDK_DIR="D:/github/obs-studio-master" -DOBS_LIB_DIR="D:/winapp/obs-studio"

# 编译
cmake --build . --config RelWithDebInfo -j 8
```

### CMake 变量说明

| 变量 | 说明 | 默认值 |
|------|------|--------|
| `OBS_SDK_DIR` | OBS 源码/安装目录（提供头文件） | `C:/obs-studio/build` |
| `OBS_LIB_DIR` | OBS 构建目录（提供 `.lib` 文件） | 同 `OBS_SDK_DIR` |
| `CMAKE_BUILD_TYPE` | 构建类型 | `RelWithDebInfo` |
| `CMAKE_INSTALL_PREFIX` | 安装目标前缀 | `{OBS_SDK_DIR}/rundir/RelWithDebInfo` |

### 常见编译问题

**找不到 Qt6**
```bash
cmake .. -DCMAKE_PREFIX_PATH="D:/winapp/obs-studio/.deps/obs-deps-qt6-2025-03-06-x64"
```

**找不到 obsconfig.h**
确保 `OBS_LIB_DIR` 指向已编译的 OBS 构建目录，该目录下应有 `buildx64/config/obsconfig.h`。

**找不到 obs.lib / obs-frontend-api.lib**
确保先用 VS 编译过 OBS Studio（至少 Release 配置）。

## 编译产物

```
build/RelWithDebInfo/
├── obs-multipusher.dll     # 插件主体（~608 KB）
├── obs-multipusher.lib     # 导入库
├── obs-multipusher.exp     # 导出符号
└── obs-multipusher.pdb     # 调试符号（~7.8 MB）
```

## 部署

### 自动安装

```bash
cd obs-multipusher/build
cmake --install . --config RelWithDebInfo
```

安装目标：
```
{OBS_SDK_DIR}/rundir/RelWithDebInfo/obs-plugins/64bit/obs-multipusher/
├── obs-multipusher.dll
└── data/
    └── locale/
```

### 手动部署到已安装的 OBS Studio

```powershell
# OBS Studio 插件目录（根据实际安装位置修改）
$obsPluginDir = "C:\Program Files\obs-studio\obs-plugins\64bit\obs-multipusher"

# 创建目录
New-Item -ItemType Directory -Force -Path $obsPluginDir

# 复制 DLL
Copy-Item build\RelWithDebInfo\obs-multipusher.dll $obsPluginDir\

# 复制数据文件
Copy-Item -Recurse data\* $obsPluginDir\data\

# 复制配置文件
Copy-Item conf\pusher.local.json $obsPluginDir\
```

最终部署结构：
```
obs-plugins/64bit/obs-multipusher/
├── obs-multipusher.dll      # 插件 DLL
├── data/
│   └── locale/              # 多语言文件
│       └── en-US.ini
└── pusher.local.json        # 配置文件
```

### 验证部署

启动 OBS Studio，在菜单栏 **工具** → **obs-multipusher Dock** 打开控制面板。如果 Docker 面板正常显示且能加载配置，则部署成功。

## 配置文件

### 查找顺序

插件启动时按以下顺序查找配置文件（与 Go 版 `multipusher` 一致）：

1. `conf/pusher.local.json`
2. `bin/conf/pusher.local.json`
3. `pusher.local.json`

如果全部未找到，使用内置默认值（所有流梯配置使用默认参数，将使用内置默认流梯配置）。

### 配置结构

```json
{
  "siteName": "3drush-fwh",
  "tencentSrt": {
    "host": "publish.numericgame.ph",
    "port": 9000,
    "app": "live",
    "tokenDays": 30
  },
  "input": {
    "mode": "device",
    "videoDevice": "OBS Virtual Camera",
    "audioDevice": "",
    "videoFile": "",
    "videoLayout": "landscape"
  },
  "streams": [
    {
      "level": "bottom",
      "streamName": "{siteName}_audio",
      "audioOnly": true,
      "videoCodec": "",
      "videoBitrate": "",
      "videoMaxrate": "",
      "portraitWidth": 0,
      "portraitHeight": 0,
      "landscapeWidth": 0,
      "landscapeHeight": 0,
      "audioCodec": "aac",
      "audioBitrate": "128k"
    },
    {
      "level": "economic",
      "streamName": "{siteName}_economic",
      "audioOnly": false,
      "videoCodec": "h264",
      "videoBitrate": "400k",
      "videoMaxrate": "600k",
      "portraitWidth": 360,
      "portraitHeight": 640,
      "landscapeWidth": 640,
      "landscapeHeight": 360,
      "audioCodec": "aac",
      "audioBitrate": "128k"
    },
    {
      "level": "standard_hevc",
      "streamName": "{siteName}_standard_hevc",
      "audioOnly": false,
      "videoCodec": "hevc",
      "videoBitrate": "600k",
      "videoMaxrate": "1000k",
      "portraitWidth": 720,
      "portraitHeight": 1280,
      "landscapeWidth": 1280,
      "landscapeHeight": 720,
      "audioCodec": "aac",
      "audioBitrate": "128k"
    },
    {
      "level": "standard",
      "streamName": "{siteName}_standard",
      "audioOnly": false,
      "videoCodec": "h264",
      "videoBitrate": "1000k",
      "videoMaxrate": "1500k",
      "portraitWidth": 720,
      "portraitHeight": 1280,
      "landscapeWidth": 1280,
      "landscapeHeight": 720,
      "audioCodec": "aac",
      "audioBitrate": "128k"
    },
    {
      "level": "high",
      "streamName": "{siteName}",
      "audioOnly": false,
      "videoCodec": "h264",
      "videoBitrate": "2000k",
      "videoMaxrate": "3000k",
      "portraitWidth": 1080,
      "portraitHeight": 1920,
      "landscapeWidth": 1920,
      "landscapeHeight": 1080,
      "audioCodec": "aac",
      "audioBitrate": "128k"
    }
  ],
  "publish": {
    "onReady": false,
    "reconnectMinSeconds": 3,
    "reconnectMaxSeconds": 60
  }
}
```

### 流梯说明

| 逻辑档位 | 实际流名 | 编码 | 码率 | 分辨率 |
|---------|---------|------|------|--------|
| `bottom` | `{siteName}_audio` | AAC audio only | 128k | 无视频（当前不作为推流目标） |
| `economic` | `{siteName}_economic` | H.264 + AAC | 400k / max 600k | 360×640 或 640×360 |
| `standard_hevc` | `{siteName}_standard_hevc` | HEVC + AAC | 600k / max 1000k | 720×1280 或 1280×720 |
| `standard` | `{siteName}_standard` | H.264 + AAC | 1000k / max 1500k | 720×1280 或 1280×720 |
| `high` | `{siteName}` | H.264 + AAC | 2000k / max 3000k | 1080×1920 或 1920×1080 |

### SRT URL 拼接

插件自动生成 Tencent SRT 推流地址：

```
srt://{host}:{port}?streamid=#!::h={host},r={app}/{streamName},txSecret={txSecret},txTime={txTime}
```

其中 `txSecret` 和 `txTime` 由 `srt-auth.cpp` 根据配置自动生成。

## 运行行为

### 启动流程

1. OBS 加载 `obs-multipusher.dll`
2. 调用 `obs_module_load()` → 创建 `MultipusherContext`
3. 按顺序查找并加载 `pusher.local.json`
4. 如果编译了 Dock UI，注册 OBS 前端面板
5. 用户通过 Dock 面板或 OBS 菜单控制推流

### 推流控制

- **Start**：校验配置 → 保存配置 → 创建多路 OBS encoder/output → 推流
- **Stop**：停止所有 encoder/output，发送最终状态到 backend
- **状态上报**：如果配置了 `ABRPLAYER_BACKEND_URL` 环境变量，每 10 秒向 `/api/pusher/status` 上报

### 断线重连

- `reconnectMinSeconds`：最小重连间隔（默认 3 秒）
- `reconnectMaxSeconds`：最大重连间隔（默认 60 秒）
- 使用指数退避策略

## Dock UI 功能

如果以 `HAS_DOCK_UI=1` 编译（需要 Qt6 + obs-frontend-api），Dock 面板提供：

| 区域 | 功能 |
|------|------|
| 配置路径 | 显示/修改当前配置文件路径 |
| Site Name | 下拉选择 `siteName` |
| SRT 参数 | host、port、app、tokenDays |
| 输入模式 | device 模式 / file 模式 |
| 视频布局 | portrait / landscape |
| 设备选择 | 枚举 DirectShow video/audio 设备 |
| 流状态表 | 实时显示各路流的编码参数和状态 |
| 控制按钮 | Start All / Stop All |
| 预览按钮 | Local Preview / Online Player / Online Statistics |
| 日志面板 | 实时运行日志 |

## 目录结构

```
obs-multipusher/
├── CMakeLists.txt          # CMake 构建定义
├── build.sh                # 一键编译脚本
├── obs-multipusher.def     # Windows DLL 导出符号
├── src/                    # 源代码
│   ├── plugin-main.cpp     # OBS 插件入口
│   ├── plugin-main.hpp     # 核心数据结构
│   ├── multipusher-dock.cpp/hpp  # Qt6 Dock UI
│   ├── config-manager.cpp/hpp    # 配置管理
│   ├── srt-auth.cpp/hpp    # SRT 推流签名
│   ├── stream-ladder.cpp/hpp     # 流梯管理
│   ├── output-manager.cpp/hpp    # OBS 输出管理
│   ├── device-enumerator.cpp/hpp # 设备枚举
│   ├── player-link.cpp/hpp # 播放器预览链接
│   ├── status-reporter.cpp/hpp   # 状态上报
│   └── utils.cpp/hpp       # 工具函数
├── conf/
│   └── pusher.local.json   # 示例配置文件
├── data/
│   └── locale/             # 多语言文件
├── deps/                   # 预留依赖目录
└── build/                  # 构建输出目录
```

## 相关文档

- 项目根目录 [`README.md`](../README.md) — 整体项目说明
- `multipusher/` [README](../multipusher/README.md) — Fyne 桌面推流器
- `doc/abr-multipusher-fyne-design.md` — multipusher 设计文档
- `doc/multipusher-abrplayer-test-plan.md` — 测试计划
