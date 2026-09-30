# Hongxi ScreenGraph

面向 OpenHarmony 的屏幕语义结构解析项目，包含 Python 验证原型和 C++ 端侧 System Ability（SA）。Python 原型通过 HDC 截图，融合 OCR、OpenCV 与视觉语言模型（VLM）；C++ 实现作为系统服务在 OpenHarmony 设备端运行。

**当前进展：**项目方已将 C++ SA 跑起来，完成编译、共享库路径修正、开机自启和 HTTP 调试口访问验证。这里记录的是服务启动与接口可达状态；具体 VLM、OCR 和完整屏幕解析效果取决于设备能力及后端配置。

## 结构

```text
Screen-Parser/
├─ SA/                      OpenHarmony C++ 端侧 System Ability
│  ├─ services/screen_parser/  SA、截屏、分析、OCR、VLM 与数据结构
│  ├─ interfaces/innerkits/   IPC 接口、Proxy、Stub 与回调
│  ├─ sa_profile/             SAID 65537 注册配置
│  ├─ etc/                    init 自启和服务配置
│  ├─ integration/            OpenHarmony 源码树集成资料
│  ├─ models/                 端侧模型导出脚本
│  └─ test/                   C++ 宿主机测试源码
├─ screen/                  Python 验证原型
│  ├─ server.py             HTTP API 与网页入口
│  ├─ analyzer.py           截图、OCR／CV 提示与 VLM 结果融合
│  ├─ hdc.py                HDC 设备发现、截图与前台应用查询
│  ├─ ocr.py                PaddleOCR 文字识别
│  ├─ vision.py             OpenCV 图形候选检测
│  ├─ schemas.py            节点、坐标与单帧 ScreenGraph 数据结构
│  ├─ static/               网页、样式、交互脚本
│  └─ tests/                原型功能单元测试
├─ benchmarks/              独立评测工具与说明
│  ├─ enrico_data.py        Enrico 截图和层级标注读取
│  ├─ enrico_metrics.py     Enrico 节点与关系指标
│  ├─ smoke_vlm.py          模型抽样回放
│  ├─ cv_metrics.py         图形候选召回诊断
│  ├─ test_*.py             评测指标测试
│  ├─ data/                 本地下载的数据集，Git 忽略
├─ requirements.txt         Python 原型依赖（含 PaddleOCR 和 OpenCV）
└─ LICENSE                  Apache-2.0 许可文本
```

`.venv/` 是本地 Python 环境，`.paddlex/` 是 OCR 模型缓存；二者均被 Git 忽略，可由安装过程重新生成。`SA/test/build/` 是本地 CMake 编译产物，也不纳入版本库。

## OpenHarmony 端侧 SA

`SA/` 对应 OpenHarmony 源码树中的 `foundation/screenparser`。它提供 SAID `65537` 的 IPC 服务（`GetStatus`、`AnalyzeSync`、`AnalyzeAsync`、`RecognizeText`），由 `init` 启动 `/system/bin/screenparser`，共享库安装在系统默认库目录。端侧截屏不依赖 HDC；VLM 可选远程 OpenAI 兼容接口或 MindSpore Lite 端侧后端，OCR 由端侧能力提供。具体能力需按目标产品启用相应构建开关和依赖。

服务配置位于 [`SA/etc/screenparser/service_config.json`](SA/etc/screenparser/service_config.json)。仓库中的示例选择 `remote` 后端，地址为 `http://127.0.0.1:8000/v1`；部署时需改成**设备可访问**的 VLM 地址、模型 ID 和凭据。C++ 远程传输当前支持 `http://`。如选择 `mslite`，还需准备并配置模型资产。HTTP 调试口默认仅监听设备的 `127.0.0.1:8765`，生产客户端使用 IPC 接口。

在主机上通过 HDC 转发调试口后，可访问状态接口：

```sh
hdc fport tcp:8765 tcp:8765
curl http://127.0.0.1:8765/api/status
```

HTTP 路由还包括 `POST /api/analyze` 和 `POST /api/ocr`。状态 JSON 使用 `model_ready` 字段表示模型是否就绪；HTTP 端点可访问本身不代表分析链路已全部验证。当前 `SA/` 没有随镜像安装网页静态文件，根路径 `/` 不应作为服务验证入口。

OpenHarmony 源码树接入资料见 [`SA/integration/BUILD_AND_FLASH.md`](SA/integration/BUILD_AND_FLASH.md) 和 [`SA/integration/QEMU_VALIDATION.md`](SA/integration/QEMU_VALIDATION.md)。这些指南包含早期示例路径与配置；实际库目录、HTTP 路由和默认后端以当前 `BUILD.gn`、配置文件及上文为准。

## Python 原型：实时设备解析（PowerShell）

要求 Python 3.10+、已安装并加入 `PATH` 的 `hdc`、已连接的鸿蒙模拟器或真机，以及提供 OpenAI 兼容 Chat Completions 接口的视觉模型。

```powershell
py -3 -m venv .venv
.\.venv\Scripts\python.exe -m pip install -r requirements.txt
hdc list targets
```

如果模型运行在远程 GPU 服务器，可另开终端建立 SSH 隧道，并保持该终端运行；将主机、端口和远端 API 端口替换为自己的值：

```powershell
ssh -p <SSH端口> -L 8000:127.0.0.1:<远端API端口> <用户>@<主机>
```

在项目目录设置模型参数并启动：

```powershell
$env:SCREEN_VLM_BASE_URL="http://127.0.0.1:8000/v1"
$env:SCREEN_VLM_MODEL="Qwen3.5-2B"  # 改为 /v1/models 返回的模型 ID
$env:SCREEN_VLM_API_KEY="EMPTY"    # 服务端需要鉴权时改为实际密钥
.\.venv\Scripts\python.exe -m screen.server --port 8765
```

打开 <http://127.0.0.1:8765>，点击“开始解析”。OCR 和 OpenCV 默认启用；仅在诊断时可分别设置 `SCREEN_OCR_ENABLED=0` 或 `SCREEN_CV_ENABLED=0` 关闭。首次 OCR 推理可能需要下载模型。蓝框为视觉模型节点，灰框为低置信度／待确认节点，红框为当前选中节点。远程服务必须实际支持图像输入；仅能完成文本对话的模型不能用于此项目。

## Python 原型输出结构

Python 原型的 `/api/analyze` 返回 `schema_version: "4.0"` 的单帧 ScreenGraph 快照，包含 `page`、`nodes`、`edges`、`current_app`、`screen_width`、`screen_height` 和 `screenshot` 等字段。模型内部仍使用紧凑 JSON；Python 服务端转换为下列公开节点格式。C++ SA 使用自身的 JSON 数据结构，不能假定与此示例完全相同。

```json
{
  "id": "node-1",
  "type": "BUTTON",
  "text": "设置",
  "description": "",
  "bounds_px": {"x1": 48, "y1": 267, "x2": 240, "y2": 401},
  "bounds_norm": {"x1": 40, "y1": 100, "x2": 200, "y2": 150},
  "window_id": null,
  "parent_id": null,
  "relations": [],
  "visible": true,
  "occluded": null,
  "interactive_inferred": true,
  "actions_verified": [],
  "confidence_calibrated": null,
  "sources": ["vlm"],
  "evidence_refs": ["screenshot"],
  "timestamp": null,
  "schema_version": "4.0"
}
```

`type` 使用 `TEXT`、`PICTOGRAM`、`IMAGE`、`BUTTON`、`TEXT_FIELD`、`LIST_ITEM`、`SWITCH`、`TAB`、`NAVIGATION_BAR`、`TOOLBAR`、`DIALOG`、`GROUP`、`OTHER`。`text` 只记录画面上实际可见的文字；图标或图片的含义写在 `description`。`bounds_norm` 为 0–1000 归一化坐标，`bounds_px` 为截图像素坐标。`parent_id` 只引用本次结果中的节点；`edges` 和 `relations` 目前只包含已知的 `contains` 关系。`sources` 为 `["vlm"]` 或 `["ocr"]`。

`interactive_inferred` 是视觉推断，不代表已通过系统接口确认可操作。`actions_verified` 目前为空；没有系统窗口与遮挡信息时，`window_id`、`occluded` 为 `null`。尚未做概率校准，`confidence_calibrated` 为 `null`。`timestamp` 使用解析完成时间。`id` 仅在单次快照内有效，尚未实现跨帧稳定 ID。提示词要求覆盖整屏可见元素，但当前没有正式覆盖率验证。

鸿蒙 Accessibility Kit 的角色、状态与动作适合作为将来的独立输入来源，再与视觉节点融合；Python 截图原型不输出未经系统验证的无障碍属性。目前仍是单帧快照，尚无差分事件。

## 来源与许可

Python 原型的 HDC 适配器提取并修改自 [zai-org/Open-AutoGLM](https://github.com/zai-org/Open-AutoGLM) 的 HDC 实现。原项目采用 Apache License 2.0，许可文本见 [LICENSE](LICENSE)。`screen/hdc.py` 顶部记录了修改说明。
