# screenparser QEMU 可行性验证指南（edu 芯片集 / x86_64_virt 标准系统）

> 本文保留早期 QEMU 集成过程供参考。项目当前已完成 SA 编译、库路径修正、开机自启和 HTTP 调试口访问验证；当前接口和配置以仓库根目录 README 及源码为准。

本文件把 `foundation/screenparser` 组件放进 **OpenHarmony QEMU 标准系统**里跑起来，用来在
**没有开发板**的情况下验证「真实 OHOS SDK 编译链接 + init/samgr 生命周期 + SA 注册(SAID 65537)
+ IPC + HTTP 调试口 + 远程 VLM 全链路」是否可行。

- 源码套件：`openharmony-robot/manifest` 的 **`chipsets/edu.xml`**（edu 芯片集，OpenHarmony-5.1.0-Release）
- QEMU 产品：**`x86_64_virt`**（x86_64 架构、**64 位**标准系统，自带 `qemu_run.sh`）
- 编译/运行主机：**Linux**（下文以 `ubuntu@example.com` 表示你的 SSH 主机；tap 网络需 sudo）
- 与 [`BUILD_AND_FLASH.md`](./BUILD_AND_FLASH.md) 的关系：那份是**真机 RK3568/Dayu200** 烧录指南；
  本文件是**QEMU 模拟器**验证指南。组件集成部分（子系统注册、SELinux、后端配置）两者通用，
  本文只写 QEMU 特有差异，通用细节会指回 `BUILD_AND_FLASH.md`。

> 因为 `x86_64_virt` 是 **64 位**，库路径与真机一致（`/system/lib64/...`），
> 所以组件的 `BUILD.gn` / `bundle.json` / `sa_profile` / `init cfg` **无需为 QEMU 改动**。

---

## 进度清单（勾选你的当前位置）

- [ ] **Phase 0** 备齐主机/依赖 + **获取 QEMU**（源码编译或 apt，见 §0.5）——运行阶段前完成即可
- [ ] **Phase 1a** 拉取源码（`repo init/sync` + `git lfs pull`）——上次报错已删除，按 §2.1 健壮流程重做
- [ ] **Phase 1b** 下载预编译工具链 `build/prebuilts_download.sh`——**你尚未做，下一步就是它**
- [ ] **Phase 2** 确认 QEMU 产品名（`x86_64_virt`）与运行脚本存在
- [ ] **Phase 3** 把 `foundation/screenparser` 集成进源码树
- [ ] **Phase 4** 配置后端（**先用 remote 远程 VLM**，最省事）
- [ ] **Phase 5** 编译 `./build.sh --product-name x86_64_virt`
- [ ] **Phase 6** 启动 QEMU（`qemu_run.sh`）
- [ ] **Phase 7** 验证 SA（`hdc` + HTTP 调试口 + 日志）

---

## 0. 工作准备（开工前先备齐）

### 0.1 硬件 / 主机

| 项 | 最低 | 推荐 | 说明 |
|---|---|---|---|
| CPU | 4 核 | 8 核以上 | 全量编译很吃 CPU |
| 内存 | 16 GB | 32 GB | 标准系统编译 + QEMU 运行；内存不足会 OOM |
| 磁盘 | 150 GB 空闲 | 250 GB+ | 源码树 ~100GB，`out/` 产物数十 GB |
| 主机系统 | Ubuntu 20.04 | Ubuntu 20.04/22.04 | 官方推荐编译环境 |
| 权限 | 普通用户编译 | **root / sudo** | 启动 QEMU 的 tap 网络需要 root |

> 你当前在 `/data/qemu-workspace` 下操作，确保该分区有 ≥150GB 空闲。

### 0.2 软件依赖

```bash
# 编译基础依赖（Ubuntu）
sudo apt-get update
sudo apt-get install -y git-core gnupg flex bison gperf build-essential \
  zip curl zlib1g-dev libc6-dev-i386 lib32z1-dev x11proto-core-dev \
  libx11-dev lib32z1-dev libgl1-mesa-dev libxml2-utils xsltproc unzip \
  libssl-dev libelf-dev libdwarf-dev libpixman-1-dev libglib2.0-dev \
  libsdl2-dev python3-pip ccache ninja-build

# QEMU（x86_64 目标）：源码编译 / 预编译三选一，完整步骤见 §0.5
#   最省事的预编译方式：sudo apt-get install -y qemu-system-x86 qemu-utils

# hdc（源码树自带，编译后可用；也可提前装 oh 工具链）
```

### 0.3 网络与远程 VLM 服务

- QEMU guest 需要**能出网**，才能连远程 VLM。edu 的 `qemu_run.sh` 用 **tap + dnsmasq**
  给 guest 分配 `192.168.111.49` 并 NAT 出网（见 Phase 6）。
- 远程 VLM 后端只支持 **`http://`**（`https://` 会 fail-fast）。你的推理服务
  （OpenAI 兼容 `/v1/chat/completions`）需保证 QEMU guest 经 NAT 后可达。
- 如果远程 VLM 与编译主机是同一台，Phase 4 要填设备可达的 `base_url`（如 `http://example.com:<端口>/v1`，主机和端口按实际部署填）、`model`、`api_key`。
- guest 要能连到它：VLM 服务需监听**公网可达地址**（非仅 `127.0.0.1`）。若只绑了 localhost，就改听 `0.0.0.0` 或建 SSH 隧道，否则 guest 经 NAT 也连不上。

### 0.4 组件源码位置与传输（Windows → Linux 主机）

- **源（Windows 工作机）**：本仓库的 `SA/` 目录
- **目标（SSH 主机 `ubuntu@example.com`）**：`/data/qemu-workspace/foundation/screenparser`

Phase 3 要把**整棵树**从 Windows 送到 Linux 源码树。下面三种方式都走你已配好的 SSH，任选其一：

#### 方式 A（Windows 原生最省事）：scp

Windows 10/11 自带 OpenSSH 客户端，**在 PowerShell 里**直接推：

```powershell
# 1) 先在远端建好组件目录
ssh ubuntu@example.com "mkdir -p /data/qemu-workspace/foundation/screenparser"

# 2) 从本仓库根目录推送 SA 目录内容
scp -r .\SA\* ubuntu@example.com:/data/qemu-workspace/foundation/screenparser/
```

> `scp -r` 可能连本地 `SA/test/build/`（CMake 中间产物）一起带上；可改用方式 B 的 rsync 排除。

#### 方式 B（可增量重传，改代码后最快）：rsync over SSH

反复同步时 rsync 只传差异。**需 Git Bash 或 WSL**（PowerShell 无原生 rsync）：

```bash
# 在仓库根目录的 Git Bash 或 WSL 中执行
rsync -avz --delete \
  --exclude 'test/build/' --exclude '.git/' --exclude '__pycache__/' \
  SA/ \
  ubuntu@example.com:/data/qemu-workspace/foundation/screenparser/
```

> - 源路径结尾的 `/` 表示「传目录内容」，与目标 `.../screenparser/` 对齐，别漏。
> - `--delete` 让远端与本地严格一致（删远端多余文件）；首次同步可先去掉。
> - `--exclude` 跳过构建产物与 `.git`，干净又快。

#### 方式 C（版本管理）：git

想留版本记录：Windows 侧 `git push` 到你的远端仓，再 SSH 登录 Linux，把仓里的
`foundation/screenparser` 检出或拷贝到 `/data/qemu-workspace/foundation/`。适合长期维护；
首次点亮不如 A/B 直接。

#### 传完自检（SSH 登录后）

```bash
ssh ubuntu@example.com
ls /data/qemu-workspace/foundation/screenparser/bundle.json   # 能看到＝传对了
ls /data/qemu-workspace/foundation/screenparser/              # BUILD.gn / sa_profile / services ... 都在
```

> **下文所有 Linux 命令（Phase 1~7）都在你的 SSH 主机上执行**：先替换示例主机名并登录。

### 0.5 获取 QEMU（源码编译 / 预编译，三选一）

edu 的 `qemu_run.sh` 本质是调用 `qemu-system-x86_64` 去加载 `out/.../images/*.img`。
所以运行前，主机上必须有一个**支持 x86_64 目标的 QEMU**在 `PATH` 里。三种方式任选其一：

**先看脚本到底要哪个 qemu**（避免版本/路径不符）：

```bash
cd /data/qemu-workspace
grep -nE 'qemu-system|QEMU|qemu_run' vendor/edu/x86_64_virt/qemu_run.sh
```

> 若脚本里写的是**绝对路径**或源码树自带的 prebuilt qemu，就以它为准，可跳过下面的安装。

#### 方式 A（推荐 · OpenHarmony 官方 device_qemu 法）：QEMU 6.2.0 源码编译

这是 OH 官方 `device_qemu` 仓给出的标准做法，版本与 OH 兼容性最好。

```bash
# 1) 安装编译依赖
sudo apt-get update
sudo apt-get install -y build-essential zlib1g-dev pkg-config libglib2.0-dev \
  binutils-dev libboost-all-dev autoconf libtool libssl-dev libpixman-1-dev \
  virtualenv flex bison ninja-build python3-venv libsdl2-dev libgtk-3-dev libfdt-dev

# 2) 拉取源码（官方 tarball）
cd ~                       # 或任意工作目录
wget https://download.qemu.org/qemu-6.2.0.tar.xz
tar -xf qemu-6.2.0.tar.xz
cd qemu-6.2.0

# 3) 配置（只编 x86_64 目标，快很多）+ 编译 + 安装
mkdir -p build && cd build
../configure --prefix=/usr/local/qemu --target-list=x86_64-softmmu
make -j$(nproc)
sudo make install

# 4) 加入 PATH（写进 ~/.bashrc 永久生效）
echo 'export PATH=$PATH:/usr/local/qemu/bin' >> ~/.bashrc
source ~/.bashrc
```

> 主机若本身是 x86_64 且有 `/dev/kvm`，可在 configure 追加 `--enable-kvm`；
> 运行 x86_64 guest 时能用硬件加速（需 `qemu_run.sh` 传 `-enable-kvm`），大幅提速。

#### 方式 B：git clone 源码编译（想自选版本时用）

```bash
sudo apt-get install -y git build-essential zlib1g-dev pkg-config libglib2.0-dev \
  libpixman-1-dev libssl-dev ninja-build python3-venv flex bison libsdl2-dev libfdt-dev

git clone https://gitlab.com/qemu-project/qemu.git
cd qemu
git checkout v8.2.0            # 稳定 tag；也可用 v6.2.0 对齐官方
mkdir -p build && cd build
../configure --prefix=/usr/local/qemu --target-list=x86_64-softmmu
make -j$(nproc)
sudo make install
echo 'export PATH=$PATH:/usr/local/qemu/bin' >> ~/.bashrc && source ~/.bashrc
```

#### 方式 C（最快）：apt 预编译二进制

不折腾源码时，直接用发行版打包的 QEMU：

```bash
sudo apt-get install -y qemu-system-x86 qemu-utils
```

> Ubuntu 22.04 的 apt 版本约为 QEMU 6.2，与官方一致；20.04 偏旧（4.x），
> 若 `qemu_run.sh` 报「不识别的参数」，改用方式 A/B 装新版。

#### 验证 QEMU 就绪

```bash
which qemu-system-x86_64
qemu-system-x86_64 --version                          # 能打印版本即可
qemu-system-x86_64 -machine help | grep -iE 'q35|pc'  # 确认有 x86 机器类型
```

---

## 1. 阶段总览

| 阶段 | 动作 | 关键命令 / 产物 |
|---|---|---|
| 0 | 装 QEMU | 源码编译 或 `apt install qemu-system-x86`（见 §0.5） |
| 1a | 拉源码 | 健壮拉取流程（防错/可重试）见 §2.1 |
| 1b | 下工具链 | `bash -x build/prebuilts_download.sh` |
| 2 | 确认产品 | `ls vendor/edu/`、`find vendor/edu -name qemu_run.sh` |
| 3 | 集成组件 | 拷贝 + 注册子系统 + 挂产品 + SELinux |
| 4 | 配后端 | 改 `service_config.json` → `backend=remote` |
| 5 | 编译 | `./build.sh --product-name x86_64_virt` |
| 6 | 跑 QEMU | `initnetwork.sh` + `qemu_run.sh` |
| 7 | 验证 | `hdc tconn 192.168.111.49:55555` + `/api/status` |

---

## 2. Phase 1 — 拉取源码

### 2.1 从零开始的健壮拉取流程（防错 · 可重试）

> 上次这一步报错、工作区已删除。下面按「装齐工具 → 配可靠性 → 断点续传拉取 → 验证」重做，
> 每步都带自检；出错先查本节末尾的**排查表**。全程在 **Linux 编译主机**执行。

**步骤 0 · 装齐前置工具**（`repo` / `git-lfs` / `python3` 缺一个都会失败）

```bash
# python3 与基础工具
sudo apt-get update
sudo apt-get install -y python3 python3-pip git git-lfs curl

# git-lfs 必须初始化一次（否则 lfs pull 报 command not found / 不下载大文件）
git lfs install

# 安装 repo 启动器到 ~/bin（国内用清华镜像，快且稳）
mkdir -p ~/bin
curl -s https://mirrors.tuna.tsinghua.edu.cn/git/git-repo -o ~/bin/repo
chmod a+x ~/bin/repo
export PATH=~/bin:$PATH                 # 建议同时写进 ~/.bashrc
export REPO_URL=https://mirrors.tuna.tsinghua.edu.cn/git/git-repo

# 自检：三者都能打印版本/路径
which repo && repo --version | head -1
git lfs version
python3 --version
```

> `repo --version` 若报 Python 相关错，多半是 `python3` 不在 PATH 或版本过旧（需 ≥ 3.6）。

**步骤 1 · 配置 git 可靠性 + 重建工作区**（大仓必备，减少中途断流）

```bash
# 放大缓冲、放宽低速断开阈值，缓解 RPC failed / early EOF
git config --global http.postBuffer 524288000
git config --global http.lowSpeedLimit 0
git config --global http.lowSpeedTime 999999
# repo 校验/提交需要身份（缺失会报 "Please tell me who you are"）
git config --global user.email "you@example.com"
git config --global user.name  "Your Name"

# 干净的工作区（你已删除旧的，这里直接重建）
cd /data
mkdir -p qemu-workspace && cd qemu-workspace
df -h /data                      # 确认 /data 剩余 ≥ 150GB
```

**步骤 2 · repo init**（主源＝你原来的 robot/edu）

```bash
repo init -u https://gitcode.com/openharmony-robot/manifest.git \
  -b OpenHarmony-5.1.0-Release \
  -m chipsets/edu.xml \
  --no-repo-verify
```

若 init 就失败（连不上 / 超时 / 证书），先验证 manifest 源是否可达：

```bash
curl -I https://gitcode.com/openharmony-robot/manifest.git
```

**备选源**（主源实在拉不动时）——同属 edu 体系的 gitee 镜像，通常更稳：

```bash
repo init -u https://gitee.com/open-harmony-edu-dist/manifest.git \
  -b OpenHarmony-5.0.2-Release \
  --no-repo-verify
# edu-dist 用默认 manifest（不需要 -m），同样含 vendor/edu/x86_64_virt
```

**步骤 3 · repo sync（断点续传 + 自动重试）**

`repo sync` 支持断点续传——**中途断了直接重跑即可**，不必从头。用循环自动重试：

```bash
cd /data/qemu-workspace
for i in $(seq 1 8); do
  repo sync -c -j4 --no-tags --no-clone-bundle && break
  echo ">>> sync 中断，第 $i 次重试（断点续传）..."; sleep 10
done
```

> - `-c` 只拉当前分支、`--no-tags` 少拉标签、`--no-clone-bundle` 规避 bundle 失效，都更稳更省流量。
> - 网络很差时把 `-j4` 降到 `-j2` 甚至 `-j1`。
> - 若报某子项目 "is not a git repository" / 路径冲突，加 `--force-sync` 重跑：
>   `repo sync -c -j4 --force-sync`。

**步骤 4 · git lfs pull**（拉取大文件资产）

```bash
git lfs install                       # 再确认一次已初始化
repo forall -c 'git lfs pull' -j4
```

> 个别仓不含 LFS 时可能打印无害提示；只要整体不中断即可。LFS 断了同样重跑续传。

**步骤 5 · 验证拉取完整**

```bash
cd /data/qemu-workspace
repo list | wc -l                                     # 项目数应为数百
ls -d build productdefine vendor/edu foundation 2>/dev/null   # 关键目录都在
find vendor/edu -name qemu_run.sh                     # 应命中 x86_64_virt/qemu_run.sh
du -sh .                                              # 体积应有数十~上百 GB
```

四条都有正常输出＝源码树完整，可进入 **Phase 1b**（下载工具链，见 2.2）。

> `chipsets/edu.xml` 会 include `ohos/ohos.xml` + `chipsets/edu/edu.xml` + `robot/robot.xml`，
> 其中 `chipsets/edu/edu.xml` 拉取 `vendor/edu/*`（含 QEMU 产品 `x86_64_virt`）。

**拉取报错排查表**

| 报错关键字 | 根因 | 解决 |
|---|---|---|
| `repo: command not found` | 没装 repo 启动器 | 步骤 0 装 `~/bin/repo` 并把 `~/bin` 加进 PATH |
| `git-lfs: command not found` / LFS 不下载 | 没装或没初始化 git-lfs | `sudo apt-get install git-lfs` + `git lfs install` |
| 报 `python3` / `Python` 相关错 | 缺 python3 或版本旧 | `sudo apt-get install python3`；确保 ≥3.6 且在 PATH |
| `RPC failed` / `early EOF` / `timeout` / `Connection reset` | 网络不稳、缓冲太小 | 重跑 sync（续传）；配 `http.postBuffer`；降 `-j2`；用重试循环 |
| `cannot checkout` / `is not a git repository` / 路径已存在 | 上次残留冲突 | `repo sync --force-sync`；或删除该子目录后重 sync |
| `Please tell me who you are` | 没配 git 身份 | 配 `user.email`/`user.name`（步骤 1） |
| manifest `branch`/`-m` 不存在、`not found` | 分支名或 manifest 路径错 | 核对 `-b OpenHarmony-5.1.0-Release`、`-m chipsets/edu.xml`；或改用**备选源** |
| `curl: (60)` / SSL 证书错 | 证书链问题 | 更新 `ca-certificates`；必要时换 gitee **备选源** |
| `No space left on device` | 磁盘不足 | `df -h /data` 确认 ≥150GB，清理后重来 |
| init/sync 一直卡在 gitcode | gitcode 限流/慢 | 换 gitee **备选源**（步骤 2） |

### 2.2 你还差的一步：下载预编译工具链（**必须先做**）

标准系统编译依赖预置的 clang / musl / cmake / ninja 等工具链，`repo sync` **不会**自动下载：

```bash
cd /data/qemu-workspace
bash -x build/prebuilts_download.sh
```

完成后确认工具链就位：

```bash
ls prebuilts/clang/ohos/linux-x86*/llvm/bin/clang   # Linux 主机编译用的主 clang（在 ohos/ 下第 6 层）
ls prebuilts/cmake        # 应有 linux-x86
```

> 若 `prebuilts_download.sh` 因网络失败，可多次重试；或按官方文档配置代理/镜像。
> **没跑通这一步，后面 `build.sh` 会直接报找不到编译器。**

---

## 3. Phase 2 — 确认 QEMU 产品名与运行脚本

edu 芯片集的 QEMU 产品**预期名为 `x86_64_virt`**。不同版本可能微调，用下面的命令在
**你自己的源码树**里确认真名，别硬记：

```bash
cd /data/qemu-workspace

# a) edu vendor 下有哪些产品目录
ls -1 vendor/edu/

LICENSE
README.md
virt
x86_64_virt

# b) 找 QEMU 运行脚本（最可靠的定位方式）
find vendor/edu -name 'qemu_run.sh'

vendor/edu/x86_64_virt/qemu_run.sh

# c) 直接看产品定义里的 product_name
find vendor/edu -name config.json | xargs grep -l product_name 2>/dev/null
cat vendor/edu/x86_64_virt/config.json 2>/dev/null | grep -i product_name
```

**判定标准**：只要 `find vendor/edu -name qemu_run.sh` 命中类似
`vendor/edu/x86_64_virt/qemu_run.sh`，那个目录名（`x86_64_virt`）就是本文件后续所有
`--product-name` 要用的值。若你的树里叫别的名字，把下文所有 `x86_64_virt` 替换成实际名即可。

---

## 4. Phase 3 — 把 screenparser 集成进源码树

> 组件自身的构建文件（`BUILD.gn`/`bundle.json`/`screenparser.gni`/`sa_profile`/`init cfg`）
> **已写好且通过审计，无需修改**。本阶段只做「源码树挂载」。

### 4.1 拷贝组件整棵树

把 Windows 侧的 `foundation/screenparser` **完整**放到源码树同名位置（用 0.4 选定的传输方式）：

```bash
# 目标：/data/qemu-workspace/foundation/screenparser
ls /data/qemu-workspace/foundation/screenparser/bundle.json

 # 拷对了应能看到
```

拷完自检关键文件都在：

```bash
cd /data/qemu-workspace/foundation/screenparser
ls BUILD.gn bundle.json screenparser.gni
ls sa_profile/screenparser.xml
ls etc/init/screenparser.cfg etc/screenparser/service_config.json
ls interfaces/innerkits/screen_parser/include/i_screen_parser.h
ls services/screen_parser/sa/src/screen_parser_sa.cpp
```

> **不要**手动新建 `include/ src/ sa_profile/` 之类的扁平骨架——组件是**分模块**布局
> （`interfaces/`、`services/screen_parser/<模块>/{include,src}`），照原样拷即可。

### 4.2 注册子系统（build/subsystem_config.json）

把 [`subsystem_config.snippet.json`](./subsystem_config.snippet.json) 的键合并进源码树根目录的
`build/subsystem_config.json`（顶层对象里加一项）。**在 Linux 源码树上操作**——该文件属 OHOS 树，
Windows 副本里没有。用下面的幂等脚本（比手改大 JSON 稳，重复跑安全，自动备份+校验）：

```bash
cd /data/qemu-workspace
cp build/subsystem_config.json build/subsystem_config.json.bak     # 备份
python3 - <<'PY'
import json
f = "build/subsystem_config.json"
data = json.load(open(f, encoding="utf-8"))
data["screenparser"] = {"path": "foundation/screenparser", "name": "screenparser"}
json.dump(data, open(f, "w", encoding="utf-8"), ensure_ascii=False, indent=2)
open(f, "a", encoding="utf-8").write("\n")
print("OK 已注册 screenparser；子系统总数:", len(data))
PY
# 验证
grep -A2 '"screenparser"' build/subsystem_config.json
python3 -c "import json;json.load(open('build/subsystem_config.json'));print('JSON 合法')"
```

合并后 `build/subsystem_config.json` 顶层应多出这一项：

```jsonc
{
  // ... 其它已有子系统 ...
  "screenparser": {
    "path": "foundation/screenparser",
    "name": "screenparser"
  }
}
```

> 改坏回滚：`cp build/subsystem_config.json.bak build/subsystem_config.json`。

### 4.3 挂载到 x86_64_virt 产品配置

找到产品配置（`vendor/edu/x86_64_virt/config.json`，或新版 `productdefine/common/products/x86_64_virt.json`），
在其 `subsystems` 数组里追加 screenparser 一项（**subsystem/component 名与产品无关，
[`product_rk3568.snippet.json`](./product_rk3568.snippet.json) 同款可直接复用**）。**同样在 Linux 源码树上操作**。

**第一步 · 定位产品配置文件**（确认哪个文件带 `subsystems` 数组）：

```bash
cd /data/qemu-workspace
ls -l vendor/edu/x86_64_virt/config.json 2>/dev/null
grep -rl '"subsystems"' vendor/edu/x86_64_virt/ productdefine/ 2>/dev/null | head
```

**第二步 · 幂等合并**（路径已确认＝`vendor/edu/x86_64_virt/config.json`，下面 `PROD` 无需替换，直接整段复制运行）：

```bash
cd /data/qemu-workspace
PROD=vendor/edu/x86_64_virt/config.json
cp "$PROD" "$PROD.bak"                                            # 备份
python3 - "$PROD" <<'PY'
import json, sys
f = sys.argv[1]
data = json.load(open(f, encoding="utf-8"))
subs = data.setdefault("subsystems", [])
subs[:] = [s for s in subs if s.get("subsystem") != "screenparser"]   # 幂等：先删旧项
subs.append({
    "subsystem": "screenparser",
    "components": [{
        "component": "screenparser",
        # ⚠ features 必须是「键=值」字符串列表（值小写 true/false）：product_util.py 的
        #   get_features 用 feat.index("=") 拆分；写成 JSON 字典会报 ValueError: substring not found
        "features": [
            "screenparser_enable_remote_vlm=true",   # 远程 VLM：QEMU 出网即可验证
            "screenparser_enable_http_debug=true",   # HTTP 调试口：最快的验证入口
            "screenparser_enable_capture=true",      # 截屏：能编译，但抓的是虚拟屏内容
            "screenparser_enable_ocr=false",         # 先关：core_vision 在 QEMU 常缺失
            "screenparser_enable_mslite=false"       # 先关：端侧推理在模拟环境极慢/易 OOM
        ]
    }]
})
json.dump(data, open(f, "w", encoding="utf-8"), ensure_ascii=False, indent=2)
open(f, "a", encoding="utf-8").write("\n")
print("OK 已挂载 screenparser 到", f, "；subsystems 数:", len(subs))
PY
# 验证
grep -n 'screenparser' "$PROD"
python3 -c "import json;json.load(open('$PROD'));print('JSON 合法')"
```

合并后产品配置的 `subsystems` 数组里应多出：

```jsonc
{
  "subsystems": [
    // ... 其它已有子系统 ...
    {
      "subsystem": "screenparser",
      "components": [
        { "component": "screenparser", "features": [ "screenparser_enable_remote_vlm=true", "…(键=值字符串列表，见上脚本)" ] }
      ]
    }
  ]
}
```

上面脚本已按**QEMU 首次点亮收窄特性开关**写好 `features`（对应 `screenparser.gni` 的 `declare_args`）：
remote_vlm + http_debug + capture 开，ocr + mslite 关，先跑通链路再逐个打开。

> **格式铁律（踩过坑）**：`features` 必须是「`键=值`」字符串列表（值用小写 `true`/`false`），**不能写成 JSON 字典**。
> `build/hb/util/product_util.py` 的 `get_features` 靠 `feat.index("=")` 拆分键值；若写成字典
> （`{"screenparser_enable_ocr": false}`），遍历拿到的是无 `=` 的键，必报 `ValueError: substring not found`
> （在走 `--no-prebuilt-sdk` 或产品镜像构建解析 parts 时触发）。

> - 关掉 `mslite`/`ocr` 后，引擎走 remote 后端 + OCR 降级为「不可用」，**不影响验证 SA/IPC/HTTP/远程 VLM**。
>   待链路通了，再按需打开 `ocr`（验降级）与 `mslite`（配 tiny 模型验管线，见第 9 节）。
> - 万一编译时 ocr/mslite 仍被打开（features 未覆盖 gni 默认值），兜底：直接改 `screenparser.gni`
>   把这两个默认值设为 `false`（会影响所有产品，且改的是 Windows 副本，需重新 scp 传上来）。
> - 改坏回滚：`cp "$PROD.bak" "$PROD"`。

### 4.4 安装 SELinux 策略

把组件自带的两个策略文件拷进 selinux_adapter 策略目录（同 `BUILD_AND_FLASH.md` 第 4 节）。
**在 Linux 源码树上操作**，来源是组件里的 [`selinux/`](./selinux/) 目录（`screenparser.te` + `file_contexts`）。

**第一步 · 拷到每组件策略目录**（主路径，直接整段复制运行）：

```bash
cd /data/qemu-workspace
SRC=foundation/screenparser/integration/selinux
DST=base/security/selinux_adapter/sepolicy/ohos_policy/screenparser
mkdir -p "$DST"
cp "$SRC/screenparser.te" "$DST/screenparser.te"
cp "$SRC/file_contexts" "$DST/file_contexts"
# 验证
ls -l "$DST"
grep -n 'permissive screenparser;' "$DST/screenparser.te"   # 首次点亮应看到此行（只告警不拦截）
```

**第二步 · 兼容「集中式 file_contexts」版本**（部分 OH 版本只读一个总的 file_contexts；不存在则自动跳过）：

```bash
cd /data/qemu-workspace
SRC=foundation/screenparser/integration/selinux
CENTRAL=base/security/selinux_adapter/sepolicy/ohos_policy/file_contexts
if [ -f "$CENTRAL" ]; then
  cp "$CENTRAL" "$CENTRAL.bak"                                                  # 备份
  grep -q 'screenparser' "$CENTRAL" || cat "$SRC/file_contexts" >> "$CENTRAL"   # 幂等追加
  echo "已把 screenparser 的 file_contexts 行合并进集中式文件："
  grep -n 'screenparser' "$CENTRAL"
else
  echo "无集中式 file_contexts，第一步的每组件目录方式即生效，无需处理"
fi
```

> - **首次验证保持 permissive**：`screenparser.te` 里已带 `permissive screenparser;`，SELinux 只告警不拦截，组件更容易先跑起来；跑通后再按 `BUILD_AND_FLASH.md` 第 9 节用 `audit2allow` 收敛为 enforcing。
> - 若 QEMU 镜像本身以 permissive 启动（常见），本步即使策略不全也不会拦住服务，可先继续 Phase 5 编译。
> - 改坏回滚：集中式文件用 `cp "$CENTRAL.bak" "$CENTRAL"`；每组件目录用 `rm -f "$DST/screenparser.te" "$DST/file_contexts"`。

---

## 5. Phase 4 — 配置后端（先用 remote）

组件默认配置在 `etc/screenparser/service_config.json`（默认 `backend="mslite"`）。
**QEMU 首次验证改成 remote 远程 VLM 最省事**（无需任何模型文件）：

```jsonc
{
  "backend": "remote",
  "base_url": "http://example.com:<端口>/v1",   // 替换为设备可达地址；只支持 http://
  "model": "<模型名，如 Qwen2-VL-7B-Instruct>",
  "api_key": "<你的key>",
  "timeout_ms": 30000,
  "enable_ocr": false,          // 与 4.3 关闭 ocr 保持一致
  "enable_node_tree": true,
  "http": { "enable": true, "host": "127.0.0.1", "port": 8765, "static_dir": "" }
}
```

改法二选一：
- **改源码再编译**（推荐，随镜像一起进去）：直接编辑
  `foundation/screenparser/etc/screenparser/service_config.json`，然后 Phase 5 编译。
- **运行时改**（QEMU 起来后）：`hdc shell` 里 remount `/system` 再改
  `/system/etc/screenparser/service_config.json`，重启服务。

> 端侧 mslite 后端的模型资产准备见 `BUILD_AND_FLASH.md` 第 5 节；QEMU 阶段**先不碰模型**。

---

## 6. Phase 5 — 编译

> 前提：Phase 1b（`prebuilts_download.sh`）、Phase 3（传组件+注册+挂载）、Phase 4（后端配置）均已完成。
> 首次全量编译很吃时间与内存（数十分钟起，建议 ≥16GB 内存，不足则加 swap 并降并发）。

### 6.1 编译前自检（强烈建议：30 秒挡住大多数编译失败）

```bash
cd /data/qemu-workspace
# a) 工具链就位？（Phase 1b）Linux 主机用的是 prebuilts/clang/ohos/linux-x86_64/llvm/bin/clang（第 6 层，勿用 maxdepth 5）
CLANG=$(find prebuilts/clang -path '*linux-x86*/llvm/bin/clang' 2>/dev/null | head -1)
[ -x "$CLANG" ] && echo "OK clang: $CLANG ($($CLANG --version 2>/dev/null | head -1))" || echo "WARN 未见可用 clang，先执行：bash build/prebuilts_download.sh"
# b) 组件文件传齐？（Phase 3.1）
ls foundation/screenparser/BUILD.gn foundation/screenparser/bundle.json foundation/screenparser/screenparser.gni
# c) §4.2/§4.3 改的两个 JSON 合法？
python3 -c "import json;json.load(open('build/subsystem_config.json'));json.load(open('vendor/edu/x86_64_virt/config.json'));print('OK 两个配置 JSON 合法')"
# d) 子系统/组件确实挂上了？（两个文件计数都应 ≥1）
grep -c screenparser build/subsystem_config.json vendor/edu/x86_64_virt/config.json
```

### 6.2 首次全量编译

```bash
cd /data/qemu-workspace
source build/envsetup.sh
./build.sh --product-name x86_64_virt --ccache
```

> - `source build/envsetup.sh` 每新开一个终端都要重新执行一次。
> - `--ccache` 加速二次编译，首次仍较久。
> - 内存吃紧就限并发：`./build.sh --product-name x86_64_virt --ccache -j4`（甚至 `-j2`）。

### 6.3 编译产物与组件落位验证

```bash
cd /data/qemu-workspace
# a) 镜像是否生成
ls -lh out/x86_64_virt/packages/phone/images/
# b) 组件二进制/库是否进了 system 暂存目录（find 兜底，路径随版本可能略异）
find out/x86_64_virt/packages/phone -type f \( -name 'screenparser' -o -name 'libscreenparser*.so' \) 2>/dev/null
# c) 配置/SA profile 是否就位
ls out/x86_64_virt/packages/phone/system/etc/screenparser/ 2>/dev/null
```

镜像清单（`out/x86_64_virt/packages/phone/images/`）：`system.img`（组件装在这里）、
`vendor.img`、`userdata.img`、`ramdisk.img`、`updater.img`、`sys_prod.img` / `chip_prod.img` / `eng_system.img`。

组件在 system 镜像内的落位：`/system/bin/screenparser`、
`/system/lib64/libscreenparser_service.z.so`、`/system/lib64/libscreenparser_innerkits.z.so`、
`/system/etc/{init,systemability,screenparser}/`。

### 6.4 只改本组件时的增量编译（更快）

```bash
cd /data/qemu-workspace
source build/envsetup.sh
./build.sh --product-name x86_64_virt --build-target screenparser
```

> 增量只重编 screenparser 并重打镜像；若改了 §4.3 的 `features` 开关，需重新全量（6.2）让 GN 重新配置。

### 6.5 编译报错速查

| 报错 | 处理 |
|---|---|
| 找不到 `screenparser` 部件 | §4.2 子系统未注册 / §4.3 产品未挂组件；重跑那两步末尾的校验行 |
| `external_deps` 找不到（如 `mindspore_lite`/`core_vision`） | 该产品未含此部件；按 §4.3 关掉 `mslite`/`ocr` 开关，或确认产品配置包含对应组件 |
| 找不到编译器/clang | Phase 1b `prebuilts_download.sh` 没做或失败；重跑 6.1-a 检测 |
| `subsystem_config.json`/`config.json` 解析错 | JSON 被改坏；跑 6.1-c 校验，或用 §4.2/§4.3 的 `.bak` 回滚 |
| OOM / 编译被杀（`Killed`） | 降并发 `-j4`→`-j2`，加 swap，关掉其它占内存进程 |
| 卡在 ninja 某 target | 记下该 target 名，单独 `--build-target <name>` 复现看完整错误 |

---

## 7. Phase 6 — 启动 QEMU

> 前提：主机已按 **§0.5** 装好 `qemu-system-x86_64`（`qemu-system-x86_64 --version` 能打印）。

edu 的 `qemu_run.sh` 用 **tap 网络**，需 root 且要先初始化网络。**在源码树根目录**执行：

```bash
cd /data/qemu-workspace

# 1) 初始化 tap + dnsmasq（需要 sudo）
sudo ./initnetwork.sh
#   输出会提示两个环境变量（NET_OPTS / OHOS_IMG_DIR），照抄。

# 2) 设置网络参数（示例，按 initnetwork.sh 实际输出为准）
export NET_OPTS="-netdev tap,id=net0,ifname=ohostap0,script=no,downscript=no -device virtio-net-pci,netdev=net0,mac=70:30:10:02:18:06"

# 3) 指向镜像目录（qemu_run.sh 从这里找 *.img）
export OHOS_IMG_DIR=/data/qemu-workspace/out/x86_64_virt/packages/phone/images/

# 4) 启动（脚本在 vendor/edu/x86_64_virt/ 下；也可能顶层有软链）
./vendor/edu/x86_64_virt/qemu_run.sh
```

> - 若 `initnetwork.sh` 不在根目录，用 `find . -name initnetwork.sh` 定位。
> - `qemu_run.sh` 若找不到，用 Phase 2 的 `find vendor/edu -name qemu_run.sh` 结果。
> - 启动后会弹出 QEMU 图形窗口 / 串口终端，等系统起到桌面或 shell。
> - 内存偏小可在 `qemu_run.sh` 里或环境变量调大（如 `-m 4096`）；标准系统建议 ≥2~4GB。

---

## 8. Phase 7 — 验证 SA 是否起来

QEMU 起来后，guest 默认 IP `192.168.111.49`（tap 网络），**另开一个终端**：

```bash
# 1) 连接 guest 的 hdc
hdc tconn 192.168.111.49:55555
hdc list targets -v          # 应看到 Connected

# 若 IP 不同，在 QEMU 终端里 ifconfig 查实际地址
hdc shell

# 2) 进程与文件
ps -ef | grep screenparser                   # 应常驻 /system/bin/screenparser
ls -l /system/bin/screenparser
ls -l /system/lib64/libscreenparser_service.z.so
ls -l /system/etc/systemability/screenparser.xml
ls -l /system/etc/init/screenparser.cfg

# 3) 日志：看 OnStart / Publish / 引擎加载 / SAID 65537 注册
hilog | grep -i screenparser
dmesg | grep "avc: *denied"                  # permissive 下只告警
```

### 8.1 用 HTTP 调试口最快验证（强烈推荐）

组件默认 `http.enable=true`，监听 guest 内 `127.0.0.1:8765`。因是 QEMU，用 **hdc 端口转发**
把 guest 的 8765 映射到主机，再从主机浏览器/curl 访问：

```bash
# 在 guest 内直接验证：
hdc shell "curl -s http://127.0.0.1:8765/api/status"

# 或转发到主机后访问（前端网页也能用）：
hdc fport tcp:8765 tcp:8765
curl -s http://127.0.0.1:8765/api/status     # 返回 backend/model/model_ready 状态 JSON
```

**判读 `/api/status`**：
- `backend` = `remote` → 配置生效
- `model_ready` = `true` → VLM 引擎已就绪
- 触发一次分析（前端点按或 IPC `AnalyzeSync`），能返回结构化结果 = **全链路打通**

### 8.2 用 IPC（innerkits）验证

用 `screenparser_innerkits` 写个测试客户端：`GetSystemAbility(65537)` 拿代理，
调 `GetStatus` / `AnalyzeSync` / `RecognizeText`。首次先用 8.1 的 HTTP 口确认服务活着，再上 IPC。

---

## 9. QEMU 能验证什么 / 不能验证什么

对照组件 5 个特性开关，明确 QEMU 的验证边界（**管理预期，别在 QEMU 上求真机精度**）：

| 能力（开关） | QEMU 能否验证 | 说明 |
|---|---|---|
| 真实 SDK 编译/链接 | ✅ 完全 | 这是 QEMU 最大价值：`#if SCREENPARSER_ENABLE_*` 设备分支对真 SDK 编译链接 |
| SA 生命周期 / 注册 65537 | ✅ 完全 | 真实 init + samgr，`OnStart`/`Publish`/`OnStop` 全跑 |
| IPC（Proxy/Stub/Callback） | ✅ 完全 | 真实 binder/IPC，`GetStatus`/`AnalyzeSync`/`RecognizeText` |
| HTTP 调试口 + 前端 | ✅ 完全 | 8765 转发后浏览器可用 |
| 远程 VLM 全链路（remote_vlm） | ✅ 完全 | guest 经 NAT 出网连你的推理服务，端到端跑通 |
| 截屏（capture） | ⚠️ 部分 | 能编译、能调用，但抓的是**虚拟屏**内容，非真实屏幕 |
| OCR（ocr / core_vision） | ⚠️ 降级 | QEMU 常缺 `core_vision`；建议先关，验证「优雅降级为不可用」 |
| 端侧推理（mslite） | ❌ 基本不行 | x86 软模拟无 GPU/KVM 加速，Qwen2-VL 会极慢/OOM；仅可用 **tiny 模型**验证「管线跑通」，不验证真实推理 |

**结论**：QEMU 足以证明「组件在真实 OHOS 运行时里能被正确编译、加载、注册、通信、并打通远程 VLM」，
这正是**可行性验证**的核心；真机相关的截屏内容、OCR 精度、端侧大模型推理留给 RK3568/RK3588 阶段。

---

## 10. QEMU 专属注意事项 / 坑

1. **64 位路径**：`x86_64_virt` 是 64 位，库在 `/system/lib64/`（不是 `lib/`），与真机一致。
2. **root 网络**：`initnetwork.sh` / tap 需要 `sudo` 或容器 `--privileged=true`，否则 guest 无网、remote 后端连不上。
3. **远程仅 http**：`base_url` 用 `https://` 会 fail-fast；必要时用 SSH 隧道把远程服务转成 guest 可达的 `http://`。
4. **内存**：标准系统 + 编译都很吃内存；QEMU 起不来或很卡时调大 `-m`（≥4096）。
5. **虚拟屏截屏**：`capture` 抓到的是 QEMU 虚拟显示内容，验证「能截、能返回 PixelMap」即可，别纠结画面。
6. **mslite 别硬上**：想在 QEMU 验端侧管线，请换**极小模型**（tiny）验证 `Load()`/`Analyze()` 流程，不要塞真实 Qwen2-VL。
7. **产品名以树为准**：一切 `x86_64_virt` 都用 Phase 2 的发现命令核对过再用。

---

## 11. 分阶段验证策略（建议照此推进，逐级加码）

- **阶段 A（最小链路）**：remote 后端 + http_debug，关 capture/ocr/mslite
  → 目标：SA 起来、65537 注册、`/api/status` 返回、远程 VLM 分析出结果。
- **阶段 B（加截屏）**：打开 capture
  → 目标：`AnalyzeSync` 内部截屏成功（虚拟屏）、能走完整 analyze 流程。
- **阶段 C（加 OCR 降级）**：打开 ocr
  → 目标：若 `core_vision` 缺失，确认组件**优雅降级**（`RecognizeText` 报不可用而非崩溃）。
- **阶段 D（端侧管线，可选）**：打开 mslite + tiny 模型
  → 目标：验证 `Load()`/推理管线在 OHOS 运行时里能装配、能跑（不追真实精度/速度）。

每个阶段跑通再进下一个，出问题好定位。

---

## 12. 常见问题排查

| 现象 | 可能原因 / 处理 |
|---|---|
| `build.sh` 报找不到编译器 | Phase 1b `prebuilts_download.sh` 未做/失败 |
| 编译报 `screenparser` 找不到部件 | 子系统未注册（4.2）或产品未挂组件（4.3） |
| 编译报 `mindspore_lite`/`core_vision` 找不到 | 产品未含该部件；按 4.3 关掉 `mslite`/`ocr` |
| `qemu_run.sh` 找不到 | 用 `find vendor/edu -name qemu_run.sh` 定位真实路径 |
| QEMU 起来但无网 / remote 连不上 | `initnetwork.sh` 未用 sudo、`NET_OPTS` 未设、guest 出网被挡 |
| `hdc tconn` 连不上 | guest IP 非 `192.168.111.49`（在 QEMU 终端 `ifconfig` 查真实 IP） |
| 进程反复重启 | SELinux enforcing 缺策略（保持 permissive）或 init cfg 字段错；看 `dmesg` avc + `hilog` |
| `model_ready=false`（remote） | `base_url` 不可达 / 用了 `https://` / key 或 model 名错 |
| 截屏报错或黑屏 | QEMU 虚拟屏正常现象；确认能力可调用即可 |
| mslite 卡死 / OOM | 模拟环境无加速，属预期；换 tiny 模型或本阶段跳过 |

---

## 附：与你已给命令的差异提醒

- 你用的是 `openharmony-robot/manifest`（gitcode，5.1.0，`chipsets/edu.xml`）；
  社区另一路是 `open-harmony-edu-dist/manifest`（gitee，5.0.2）。两者同属 **edu** 体系，
  QEMU 产品都指向 `vendor/edu/x86_64_virt`。**以你树里 Phase 2 的发现结果为准。**
- 你的拉码步骤**没有** `build/prebuilts_download.sh`——这是 Phase 1b，**编译前必须补**。
