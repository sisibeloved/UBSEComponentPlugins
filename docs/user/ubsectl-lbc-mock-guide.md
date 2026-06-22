# ubsectl + LBC mock 集成手册

这份文档给“只想在自己的系统里调用 `ubsectl` 做测试”的集成方看。

你不需要直接运行 LBC mock 的脚本，也不需要理解 SSU Plugin 的内部实现。正确用法是：

1. 构建一个 `lbc_mock` 版本。
2. 启动一个常驻的 `ssu-mgr`。
3. 让 `ubsectl` 连接这个 `ssu-mgr`。
4. 后续只用 `ubsectl allocate/list/allocate-result-get/mount/unmount/free`。

## 一句话结论

可以做到“上游只感知 `ubsectl`，底层自动走 LBC mock”。

但不要把每条 `ubsectl` 命令都当成独立进程直接裸跑。分配、挂载、释放这些状态都在 `ssu-mgr` 进程内维护，所以完整生命周期必须走：

```text
你的系统 / 脚本
  -> ubsectl
  -> 默认本地 FIFO
  -> ssu-mgr
  -> LBC mock SSU Plugin
  -> mock/setup_mock_target.sh / mock/run_mock.sh
  -> /dev/nvmeXnY
```

## 最快验证路径

只需要准备一个环境变量：`LBC_PREFIX`。

```bash
export LBC_PREFIX=/path/to/lbc/mock/prefix

meson setup build-lbc -Dvendor=lbc_mock
meson compile -C build-lbc
```

第一个终端启动 manager：

```bash
sudo env LBC_PREFIX="$LBC_PREFIX" \
    ./build-lbc/src/user/runtime/ssu-mgr --role=manager
```

这个进程不要退出。

第二个终端只调用 `ubsectl`：

```bash
sudo ./build-lbc/tools/ubsectl list

sudo ./build-lbc/tools/ubsectl allocate \
    --size 512M \
    --tenant tenant-demo \
    --shards 1 \
    --aggregate \
    --share exclusive \
    --host local \
    --out /tmp/ssu-rid

RID=$(cat /tmp/ssu-rid)
DEV=$(sudo ./build-lbc/tools/ubsectl allocate-result-get --request-id "$RID")

sudo ./build-lbc/tools/ubsectl mount \
    --dev "$DEV" \
    --host local

sudo ./build-lbc/tools/ubsectl query --type logdev

sudo ./build-lbc/tools/ubsectl unmount --dev "$DEV"
sudo ./build-lbc/tools/ubsectl free --dev "$DEV"
```

默认值如下：

```text
manager FIFO=/tmp/ubse-ssu-mgr.fifo
dev_ip=127.0.0.1
port=4420
subnqn=nqn.2025-01.io.ssu:m0
nsze=1048576
log_file=/tmp/ubse-lbc-mock.log
```

## SSU_MGR_SOCKET 是什么

`SSU_MGR_SOCKET` 是 `ubsectl` 用来找到 `ssu-mgr` 的通信入口。快速验证时通常不用设置它。

现在代码里的实现是 Linux FIFO，也就是命名管道，不是真正的 Unix domain socket。变量名里叫 `SOCKET`，表达的是“manager 的本地通信地址”这个概念。

它为什么需要存在：

- `ssu-mgr` 是常驻进程，负责保存分配、挂载、namespace、设备路径这些运行时状态。
- `ubsectl` 是命令行工具，执行完一条命令就退出。
- 所以 `ubsectl allocate`、`ubsectl mount`、`ubsectl free` 必须发给同一个 `ssu-mgr`，才能共享同一份状态。

默认入口是：

```text
/tmp/ubse-ssu-mgr.fifo
```

如果你有多个 manager，或者不想用默认路径，可以手动指定：

```bash
sudo env LBC_PREFIX="$LBC_PREFIX" \
    ./build-lbc/src/user/runtime/ssu-mgr \
    --role=manager \
    --socket "$SSU_SOCKET"
```

调用 `ubsectl` 时再用 `SSU_MGR_SOCKET` 指向同一个入口：

```bash
sudo env SSU_MGR_SOCKET="$SSU_SOCKET" \
    ./build-lbc/tools/ubsectl list
```

如果不设置 `SSU_MGR_SOCKET`，`ubsectl` 会自动尝试默认入口 `/tmp/ubse-ssu-mgr.fifo`。如果默认入口也不存在，才会退回直接调用模式。

## 准备 LBC mock 目录

假设 LBC mock 被放在一个目录里，下面称为 `$LBC_PREFIX`。这个目录至少要有：

```text
$LBC_PREFIX/
├── mock/
│   ├── setup_mock_target.sh
│   └── run_mock.sh
├── sample_create_attach
└── sample_detach_delete
```

SSU 组件会在 `$LBC_PREFIX` 目录下执行这些命令：

```bash
bash mock/setup_mock_target.sh "$SUBNQN" 4420

bash mock/run_mock.sh ./sample_create_attach \
    --dev-ip 127.0.0.1 \
    --port 4420 \
    --sub-nqn "$SUBNQN" \
    --nsze 1048576

bash mock/run_mock.sh ./sample_detach_delete \
    --nsid 1 \
    --dev-path /dev/nvmeXnY \
    --dev-ip 127.0.0.1 \
    --port 4420 \
    --sub-nqn "$SUBNQN"
```

默认 `SUBNQN` 是：

```text
nqn.2025-01.io.ssu:m0
```

它没有超过 LBC mock 的 31 字符限制。

`--nsze` 固定传 `1048576`。按 512 字节扇区计算，系统里看到的盘大小是 512 MiB。

## 构建 lbc_mock 版本

在 UBSEComponentPlugins 仓库里执行：

```bash
meson setup build-lbc -Dvendor=lbc_mock
meson compile -C build-lbc
```

构建产物里会用到两个程序：

```text
build-lbc/src/user/runtime/ssu-mgr
build-lbc/tools/ubsectl
```

注意：默认构建是 `-Dvendor=mock`，不是 LBC mock。要走 LBC mock，必须显式传 `-Dvendor=lbc_mock`。

## 启动 ssu-mgr

真实 LBC mock 会操作 configfs 和 `/dev/nvme*`，通常需要 root 权限。建议先用一个终端专门跑 `ssu-mgr`：

```bash
export LBC_PREFIX=/path/to/lbc/mock/prefix

sudo env LBC_PREFIX="$LBC_PREFIX" \
    ./build-lbc/src/user/runtime/ssu-mgr --role=manager
```

这个进程不要退出。它就是 `ubsectl` 后面的常驻控制面。

如果启动失败，并提示类似 `pool refresh failed`，优先检查：

- `LBC_PREFIX` 是否指向 LBC mock 前缀目录。
- `$LBC_PREFIX/mock/setup_mock_target.sh` 是否存在。
- `$LBC_PREFIX/mock/run_mock.sh` 是否存在。
- 脚本是否能被 `bash` 执行。
- `/tmp/ubse-lbc-mock.log` 里的最后几行。

## 用 ubsectl 操作

另开一个终端。下面所有命令都只调用 `ubsectl`，不直接碰 LBC mock 脚本：

```bash
sudo ./build-lbc/tools/ubsectl list
```

看到类似输出就说明 `ubsectl -> ssu-mgr -> LBC mock plugin` 这条控制面通了：

```text
pool entries: 1
lbc-mock-ssu0 lbc-mock-host0 ONLINE 0/536870912
```

申请 512 MiB 空间：

```bash
sudo ./build-lbc/tools/ubsectl allocate \
    --size 512M \
    --tenant tenant-demo \
    --shards 1 \
    --aggregate \
    --share exclusive \
    --host local \
    --out /tmp/ssu-rid

RID=$(cat /tmp/ssu-rid)
```

获取这次分配对应的逻辑设备路径：

```bash
DEV=$(sudo ./build-lbc/tools/ubsectl allocate-result-get --request-id "$RID")
echo "$DEV"
```

输出类似：

```text
/dev/ssu0
```

挂载成逻辑设备：

```bash
sudo ./build-lbc/tools/ubsectl mount \
    --dev "$DEV" \
    --host local
```

查看逻辑设备到真实 NVMe namespace 的映射：

```bash
sudo ./build-lbc/tools/ubsectl query --type logdev
```

输出类似：

```text
logdev entries: 1
/dev/ssu0 local alloc-0 0 536870912 /dev/nvme1n1 1 0
```

这里 `/dev/nvme1n1` 就是 LBC mock create + attach 后出现的真实块设备。

可以用系统命令确认：

```bash
ls /dev/nvme*n*
lsblk /dev/nvme1n1
cat /sys/class/block/nvme1n1/size
ls /sys/kernel/config/nvmet/subsystems/nqn.2025-01.io.ssu:m0/namespaces/
```

期望：

- `lsblk` 看到约 512M。
- `/sys/class/block/nvme1n1/size` 是 `1048576`。
- configfs 下能看到 namespace `1`。

解挂载并释放：

```bash
sudo ./build-lbc/tools/ubsectl unmount --dev "$DEV"

sudo ./build-lbc/tools/ubsectl free --dev "$DEV"
```

释放后再确认：

```bash
ls /dev/nvme*n*
ls /sys/kernel/config/nvmet/subsystems/nqn.2025-01.io.ssu:m0/namespaces/
```

期望刚才创建的 `/dev/nvmeXnY` 和 namespace `1` 已经消失。

## 失败时怎么看

`ubsectl` 现在会返回错误名和提示，不只返回数字。例如：

```text
alloc failed: SSU_ERR_NOT_FOUND (-3)
plugin: lbc_mock
hint: lbc_mock create/attach finished, but no new /dev/nvmeXnY namespace was found. Check LBC_PREFIX, mock/run_mock.sh, ls /dev/nvme*n*, and /tmp/ubse-lbc-mock.log.
```

LBC mock 插件的默认日志文件是：

```bash
sudo tail -n 50 /tmp/ubse-lbc-mock.log
```

同时也看启动 `ssu-mgr` 的那个终端。插件会把脚本执行命令、前缀目录、dev 目录、脚本退出码、找不到 NVMe namespace 等信息写到那里。

## 可选配置文件

默认情况下不需要配置文件。SSU Plugin 会使用这些默认值：

```text
dev_ip=127.0.0.1
port=4420
subnqn=nqn.2025-01.io.ssu:m0
dev_dir=/dev
configfs_dir=/sys/kernel/config/nvmet/subsystems
log_file=/tmp/ubse-lbc-mock.log
```

如果确实需要改，比如测试时想把 `/dev` 和 configfs 指到临时目录，可以在 `$LBC_PREFIX/mock/ssu_lbc_mock.conf` 写：

```ini
dev_ip=127.0.0.1
port=4420
subnqn=nqn.2025-01.io.ssu:m0
dev_dir=/dev
configfs_dir=/sys/kernel/config/nvmet/subsystems
log_file=/tmp/ubse-lbc-mock.log
```

这份配置只属于 LBC mock SSU Plugin，不是整个 SSU Manager 的全局配置。

## 你不需要做的事

集成方不需要直接运行：

```bash
sudo bash mock/setup_mock_target.sh ...
sudo bash mock/run_mock.sh ./sample_create_attach ...
sudo bash mock/run_mock.sh ./sample_detach_delete ...
```

这些是插件内部会调用的东西。你的系统只需要按上面的方式启动 `ssu-mgr`，然后调用 `ubsectl`。

## 当前边界

当前这条 LBC mock 集成路径验证的是控制面：

- `ubsectl allocate` 触发 LBC mock create + attach，并返回请求 ID。
- `ubsectl allocate-result-get` 根据请求 ID 返回 `/dev/ssuN` 逻辑设备路径。
- `ubsectl mount` 建立 `/dev/ssuN -> /dev/nvmeXnY` 的逻辑映射。
- `ubsectl unmount` 删除逻辑映射。
- `ubsectl free` 触发 LBC mock detach + delete。

也就是说，现在可以用它验证“上游只调 `ubsectl`，底层出现并删除 `/dev/nvmeXnY`”。

真正对 `/dev/ssu0` 发起数据读写，需要 ReqShim 数据面接入后一起验证。LBC mock 本身已经能产生 `/dev/nvmeXnY`，但 `/dev/ssu0` 这个逻辑块设备不是 LBC mock 脚本创建的。

## 常见问题

### 我能不能只运行 ubsectl，不启动 ssu-mgr？

不建议。单条 `list` 可能能看到资源，但完整生命周期不行。

原因很简单：不连 `ssu-mgr` 时，每条 `ubsectl` 都是一个新的短进程，上一条命令创建的分配状态不会留给下一条命令。`allocate -> mount -> free` 这种流程必须通过常驻 `ssu-mgr`。

### 为什么 ubsectl 需要 sudo？

因为真实 LBC mock 脚本会操作 configfs 和 `/dev/nvme*`。同时 `ssu-mgr` 创建的 FIFO 通常只有同一用户可访问。最简单的做法是 `ssu-mgr` 和 `ubsectl` 都用 root 跑。

### 为什么我看不到 512M？

先查这个：

```bash
sudo ./build-lbc/tools/ubsectl query --type logdev
```

找到真实设备，比如 `/dev/nvme1n1`，再查：

```bash
cat /sys/class/block/nvme1n1/size
```

如果不是 `1048576`，说明 LBC mock 的 create 路径没有按预期收到 `--nsze 1048576`，需要看 LBC mock 脚本日志。

### 我的系统应该集成 ubsectl 还是 LBC mock 脚本？

集成 `ubsectl`。LBC mock 脚本是底层适配细节，不应该暴露给上游业务系统。
