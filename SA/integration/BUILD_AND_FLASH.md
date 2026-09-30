# screenparser 集成 · 编译 · 烧录指南（RK3568 / Dayu200）

> 本文是早期集成指南。当前工程已完成 SA 编译、库路径修正、开机自启和 HTTP 调试口访问验证；以仓库根目录 README 和现有源码配置为准。

本指南把 `foundation/screenparser` 组件挂进 **OpenHarmony 源码树**，编译进系统镜像并烧录到
**RK3568 / Dayu200** 开发板，最后验证 SA（SAID **65537**）是否正常起来。

- 目标产品：`rk3568`（Dayu200），OpenHarmony 4.x / 5.x
- 编译主机：**Linux**（Ubuntu 18.04 / 20.04 / 22.04，官方推荐环境）
- 前提：你已经有一份**完整的 OpenHarmony 源码树**，`foundation/screenparser` 已就位

> 组件自身的 `BUILD.gn` / `bundle.json` / SA profile / init cfg 均已写好并通过审计，
> 本文只做**源码树集成**与**编译烧录**两件事。

> **没有开发板？** 想先在 **QEMU 模拟器**（edu 芯片集 / `x86_64_virt` 标准系统）里验证
> 组件能否编译、加载、注册 SA、跑通 IPC 与远程 VLM，请看
> [`QEMU_VALIDATION.md`](./QEMU_VALIDATION.md)。组件集成部分两者通用，那份只写 QEMU 差异。

---

## 0. 组件构建目标一览（已存在，无需修改）

| GN target | 类型 | 安装路径 |
|---|---|---|
| `//foundation/screenparser:screenparser` | group | — |
| `.../interfaces/innerkits/screen_parser:screenparser_innerkits` | shared lib | `/system/lib64/libscreenparser_innerkits.z.so` |
| `.../services/screen_parser:screenparser_bin` | executable | `/system/bin/screenparser` |
| `.../services/screen_parser:screenparser_service` | shared lib | `/system/lib64/libscreenparser_service.z.so` |
| `.../services/screen_parser:screenparser_sa_profile` | prebuilt_etc | `/system/etc/systemability/screenparser.xml` |
| `.../services/screen_parser:screenparser_init_cfg` | prebuilt_etc | `/system/etc/init/screenparser.cfg` |
| `.../services/screen_parser:screenparser_service_config` | prebuilt_etc | `/system/etc/screenparser/service_config.json` |

`bundle.json` 的 `build` 段已把 `sub_component` / `inner_kits` 指向上表目标，
`subsystem` 与 `part_name` 均为 `screenparser`。

---

## 1. 前置条件

1. 完整的 OH 源码树（含 `build/`、`productdefine/` 或 `vendor/`、`base/security/selinux_adapter/`）。
2. 已按官方文档装好编译依赖（`build/prebuilts_download.sh`、Python、ccache 等），且能成功编译一次**未改动**的 `rk3568`（确保基线可编译）。
3. Dayu200 开发板 + USB-C 数据线 + 供电；烧录用 **RKDevTool**（Windows GUI）或 **upgrade_tool**（Linux CLI）。
4. 板子与主机能建立 `hdc` 连接（`hdc list targets` 能看到设备）。

> 强烈建议先跑通一次纯净 `rk3568` 全量编译 + 烧录，再叠加本组件，便于定位问题。

---

## 2. 注册子系统（build/subsystem_config.json）

把 `integration/subsystem_config.snippet.json` 里的键合并进源码树根目录的
`build/subsystem_config.json`（顶层对象里加一项）：

```jsonc
{
  // ... 其它已有子系统 ...
  "screenparser": {
    "path": "foundation/screenparser",
    "name": "screenparser"
  }
}
```

这一步让构建系统知道 `screenparser` 子系统位于 `foundation/screenparser`。

---

## 3. 挂载到产品配置（rk3568）

不同 OH 版本产品配置位置不同，二选一（两处都有就都改）：

- 新版（productdefine）：`productdefine/common/products/rk3568.json`
- 旧版（vendor）：`vendor/ohos-rk/rk3568/config.json`（或你板子对应的产品目录）

在产品的 `subsystems` 数组里追加 `integration/product_rk3568.snippet.json` 的内容：

```jsonc
{
  "subsystems": [
    // ... 其它已有子系统 ...
    {
      "subsystem": "screenparser",
      "components": [
        { "component": "screenparser", "features": {} }
      ]
    }
  ]
}
```

如需在产品级覆盖特性开关（对应 `screenparser.gni` 的 `declare_args`），在 `features` 里写：

```jsonc
"features": {
  "screenparser_enable_capture": true,
  "screenparser_enable_mslite": true,
  "screenparser_enable_ocr": true,
  "screenparser_enable_remote_vlm": true,
  "screenparser_enable_http_debug": true
}
```

---

## 4. 安装 SELinux 策略

RK3568/Dayu200 默认 **SELinux enforcing**，init cfg 里已用标准 `secon` 字段把服务标为
`u:r:screenparser:s0`，因此必须提供对应策略，否则服务会被拒绝甚至反复重启。

把本组件的两个文件放到 selinux_adapter 的策略目录（路径按你的 OH 版本微调）：

```
base/security/selinux_adapter/sepolicy/ohos_policy/screenparser/screenparser.te
base/security/selinux_adapter/sepolicy/ohos_policy/screenparser/file_contexts
```

来源：`integration/selinux/screenparser.te`、`integration/selinux/file_contexts`。
若你的版本用的是**集中式** `file_contexts`，把其中的行合并进去即可。

**首次点亮建议先用 permissive 跑通**（`screenparser.te` 里已带 `permissive screenparser;`），
待收集完 avc 拒绝再用 `audit2allow` 收敛为 enforcing，详见第 9 节。

---

## 5. 模型资产（端侧后端才需要）

`etc/screenparser/service_config.json` 当前选择 `backend = "remote"`；切换到 `mslite` 时，默认 `model_root = "/system/etc/screenparser"`，
端侧后端需要 Qwen-VL 的 MindSpore Lite 模型与 CPU 侧资产（均由仓库 `models/` 下的导出脚本生成）：

| 文件 | 用途 | 生成者 |
|---|---|---|
| `qwen2vl_vision.ms` | 视觉编码器：`pixel_values → image_embeds` | `export_qwen2vl_onnx.py` + `convert_to_ms.sh` |
| `qwen2vl_lm.ms` | 语言解码器：`(inputs_embeds, position_ids, past_kv) → logits, present_kv`（含 lm_head，输出真 vocab logits） | 同上 |
| `qwen2vl_embed.bin` | 词嵌入矩阵（raw float32 `[vocab, hidden]`），引擎在 CPU 侧查表并把视觉特征拼进 prompt | `export_qwen2vl_onnx.py` |
| `model_meta.json` | `hidden_size`/`vocab_size`/`image_token_id`，`Load()` 用它校验嵌入矩阵尺寸 | `export_qwen2vl_onnx.py` |
| `vocab.json` / `merges.txt` | 分词器资产 | `export_tokenizer.py` |

> **多模态融合契约**：引擎用 `inputs_embeds` 方案——文本 token 查 `qwen2vl_embed.bin`，
> 图像占位 token 替换为视觉编码输出，再喂给 `qwen2vl_lm.ms`。因此 `qwen2vl_embed.bin`
> 与 `model_meta.json` **必须与 `qwen2vl_lm.ms` 同目录**（`Load()` 从语言模型路径的同级目录读取），
> 缺失会报 `无法读取词嵌入矩阵` / `model_meta.json 缺失`。拼接逻辑（`BuildPromptEmbeds`）已被宿主机单测覆盖。

两种放置方式：

- **A. 远程后端（推荐先跑通）**：把 `backend` 改为 `"remote"`，填 `base_url`/`model`/`api_key`
  （仅支持 `http://`）。无需任何模型文件，最省事，适合第一次验证链路。
- **B. 端侧后端**：模型较大且 `/system` 只读，建议放到可写分区并改 `model_root`：
  ```bash
  hdc shell "mkdir -p /data/screenparser"
  hdc file send qwen2vl_vision.ms  /data/screenparser/
  hdc file send qwen2vl_lm.ms      /data/screenparser/
  hdc file send qwen2vl_embed.bin  /data/screenparser/   # 必须与 lm.ms 同目录
  hdc file send model_meta.json    /data/screenparser/
  hdc file send vocab.json         /data/screenparser/
  hdc file send merges.txt         /data/screenparser/
  ```
  然后把 `service_config.json` 的 `model_root` 改成 `/data/screenparser`（或分别填 `*_path`）。
  若要烧进镜像，需在 BUILD.gn 里为这些 `.ms`/`.bin` 增加 `ohos_prebuilt_etc`（体积大，一般不这么做）。

> 无论哪种后端，**OCR 始终是端侧**且独立于 VLM；`RecognizeText` 在 VLM 未就绪时也能用。

---

## 6. 编译

在源码树根目录：

```bash
source build/envsetup.sh
# 全量编译 rk3568（首次或依赖变化时）
./build.sh --product-name rk3568 --ccache
```

只改了本组件时，单独编译目标更快：

```bash
./build.sh --product-name rk3568 --build-target screenparser
```

编译产物：

```
out/rk3568/packages/phone/images/     # system.img / vendor.img / ... 或 update.img
out/rk3568/packages/phone/            # 各分区镜像与烧录所需文件
```

本组件安装产物落在 system 镜像：`/system/bin/screenparser`、`/system/lib64/libscreenparser_service.z.so`、
`/system/etc/{init,systemability,screenparser}/`。

---

## 7. 烧录 Dayu200（RK3568）

**进入 Loader/烧录模式**：断电 → 按住板上 **BOOT / MRU / REC** 键 → 上电（或插 USB）→ 松开；
若系统可正常启动，也可 `hdc shell reboot loader`（部分版本支持）。主机侧 RKDevTool 应显示"发现一个 LOADER 设备"。

**方式一：整包 update.img**
- Windows：打开 **RKDevTool** → "升级固件"页 → 选择 `update.img` → 升级。
- Linux：
  ```bash
  sudo upgrade_tool uf out/rk3568/packages/phone/update.img
  ```

**方式二：分区镜像**
- RKDevTool "下载镜像"页，按 `parameter.txt` / 分区表逐个指定 `*.img` 后执行。

烧录完成自动重启。首次启动较慢，耐心等桌面出现。

> 板子型号/固件版本不同，烧录工具与分区名可能不同，以你手里 Dayu200 的官方烧录文档为准。

---

## 8. 验证 SA 是否起来

连上设备：

```bash
hdc list targets
hdc shell
```

**a) 进程与文件**
```bash
ps -ef | grep screenparser          # 应看到 /system/bin/screenparser 常驻
ls -l /system/bin/screenparser
ls -l /system/lib64/libscreenparser_service.z.so
ls -l /system/etc/systemability/screenparser.xml
ls -l /system/etc/init/screenparser.cfg
ls -l /system/etc/screenparser/service_config.json
```

**b) 日志**
```bash
hilog | grep -i screenparser        # 看 OnStart / Publish / 引擎加载
dmesg | grep "avc: *denied"         # 看是否被 SELinux 拒绝（permissive 下只告警不拦截）
```

**c) 用 HTTP 调试口最快验证（无需写 IPC 客户端）**
组件默认 `http.enable=true`，监听 `127.0.0.1:8765`：

```bash
# 在设备内直接访问：
hdc shell "curl -s http://127.0.0.1:8765/api/status"
# 或端口转发到主机后浏览器/ curl 访问：
hdc fport tcp:8765 tcp:8765
curl -s http://127.0.0.1:8765/api/status     # 返回 backend/model/model_ready 等状态 JSON
```

`/api/status` 里 `backend` 应为 `remote` 或 `mslite`；`model_ready` 表示 VLM 引擎已加载完成。当前构建未安装网页静态文件，根路径 `/` 不作为验证入口。

**d) 通过 IPC（innerkits）验证**
用 `screenparser_innerkits` 写一个测试客户端，`GetSystemAbility(65537)` 拿到代理后调用
`GetStatus` / `AnalyzeSync` / `RecognizeText`。首次可先用上面的 HTTP 口确认服务活着。

---

## 9. SELinux 收敛（permissive → enforcing）

1. 保持 `screenparser.te` 的 `permissive screenparser;`，完整走一遍：
   `GetStatus` / `AnalyzeSync` / `RecognizeText`，远程后端再触发一次 HTTP 调用，
   开了调试口就再访问一次 8765。
2. 收集拒绝：
   ```bash
   hdc shell "dmesg | grep 'avc: *denied'"
   ```
3. 用 `audit2allow` 生成规则并追加到 `screenparser.te`（去掉与已有规则重复的项）。
4. 删除 `permissive screenparser;` 一行，重新编译烧录，确认 enforcing 下无拒绝、功能正常。

---

## 10. 增量迭代（不整包重烧）

改完 C++ 代码后，只编译组件并把产物推到设备（需 userdebug、可 remount）：

```bash
./build.sh --product-name rk3568 --build-target screenparser

hdc shell "mount -o rw,remount /"     # 让 /system 可写（userdebug）
hdc file send out/rk3568/screenparser/screenparser                 /system/bin/
hdc file send out/rk3568/lib64/libscreenparser_service.z.so /system/lib64/
# 如改了配置/ profile / init cfg，一并推送对应 /system/etc/... 文件
hdc shell "chmod 755 /system/bin/screenparser"
hdc shell "restorecon -R /system/bin/screenparser /system/lib64/libscreenparser_service.z.so"
hdc shell "stop screenparser; start screenparser"    # 或直接 hdc shell reboot
```

> 产物路径以你的 `out/rk3568/` 实际结构为准；remount/stop-start 依赖构建类型与 init 支持，
> 失败时改用整包烧录或 `hdc shell reboot`。

---

## 11. 常见问题排查

| 现象 | 可能原因 / 处理 |
|---|---|
| 编译报 `screenparser` 找不到部件 | 第 2 步子系统未注册，或第 3 步产品配置未加组件 |
| 编译报 `external_deps` 找不到（如 `mindspore_lite`） | 产品未包含该部件；确认 `bundle.json` deps 对应组件在产品配置里，或临时关掉对应特性开关 |
| 进程反复重启 | SELinux enforcing 缺策略（第 4/9 节），或 init cfg 字段错误；先看 `dmesg` avc 与 `hilog` |
| 服务起来但 `model_ready=false` | 端侧模型缺失/路径错（第 5 节），或远程 `base_url` 不可达；远程仅支持 `http://` |
| 远程后端连接失败 | 设备与推理服务网络不通；`https://` 会 fail-fast，改用 `http://` 或 SSH 隧道 |
| 截图/窗口能力报权限 | 抓取走 `foundation/window`，需相应系统权限/APL；确认签名 profile 的 APL（system_basic）与权限 |
| samgr 报加载 lib 失败 | 确认 `libscreenparser_service.z.so` 已安装到 `/system/lib64/`，并检查动态链接器与 samgr 日志 |
| SAID 冲突 | 已用 vendor 区间 `65537`（`VENDOR_SYS_ABILITY_ID_BEGIN`+1），避开官方 `DISTRIBUTED_HARDWARE_SA_ID=4801`；若厂商另有分配约定，改 `i_screen_parser.h` 的 `kScreenParserSaId` 并同步 `sa_profile/screenparser.xml` 的 `<name>` |
| 推送后文件"消失"/被拒 | `/system` 未 remount 成功，或 SELinux 标签错；`restorecon` 后重试 |

---

## 12. 宿主机单元测试（可选，快速回归）

组件核心逻辑与 OH 解耦，可在本机用 CMake + g++ 跑离线单元测试（不依赖开发板）：

```bash
cd foundation/screenparser/test
cmake -S . -B build -G Ninja
cmake --build build
./build/screenparser_tests      # 92 个用例
```

宿主机构建通过 gtest shim + 宏开关（关闭 capture/mslite/ocr/http_debug/remote_vlm 的 OH 依赖）
只编译纯逻辑，用于在改代码后快速回归。
