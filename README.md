# Harmony Screen Parser

基于 HDC 截图和远程视觉语言模型（VLM）的鸿蒙屏幕语义解析原型。点击网页“解析”，即可获取当前模拟器／真机页面的结构化节点与可视化叠框。**不包含 Agent 循环或自动操作。**

## 结构

```text
screen/
  hdc.py             HDC 设备发现、截图、前台应用查询
  analyzer.py        发送截图给远程 VLM，解析并校验返回的 JSON
  schemas.py         页面、节点和 0–1000 归一化坐标模型
  server.py          本地 HTTP API 和网页服务
  static/            解析按钮、截图叠框与节点列表
  tests/             离线单元测试
```

## 运行（PowerShell）

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
$env:SCREEN_VLM_MODEL="qwen3.5-2b"  # 改为 /v1/models 返回的模型 ID
$env:SCREEN_VLM_API_KEY="EMPTY"    # 服务端需要鉴权时改为实际密钥
.\.venv\Scripts\python.exe -m screen.server --port 8765
```

打开 <http://127.0.0.1:8765>，点击“解析”。蓝框为视觉模型节点，灰框为低置信度／待确认节点，红框为当前选中节点。远程服务必须实际支持图像输入；仅能完成文本对话的模型不能用于此项目。

可用 `Invoke-RestMethod http://127.0.0.1:8000/v1/models` 检查 SSH 隧道及模型 ID。

## 测试

```powershell
.\.venv\Scripts\python.exe -m unittest discover -s screen/tests -v
```

## 来源与许可

本项目的 HDC 适配器提取并修改自 [zai-org/Open-AutoGLM](https://github.com/zai-org/Open-AutoGLM) 的 HDC 实现；已删去操作型 Agent 依赖，只保留读取屏幕所需功能。原项目采用 Apache License 2.0，许可文本见 [LICENSE](LICENSE)。`screen/hdc.py` 顶部记录了修改说明。其余屏幕解析与网页代码为本独立原型。
