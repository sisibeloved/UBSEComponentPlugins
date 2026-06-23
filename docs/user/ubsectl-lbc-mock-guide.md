# ubsectl + LBC mock 集成手册

这份文档给“只想在自己的系统里调用 `ubsectl` 做测试”的集成方看。需要直接接 SDK 的系统，可以参考后面的 C++ SDK 字段示例。

你不需要直接运行 LBC mock 的脚本，也不需要理解 SSU Plugin 的内部实现。正确用法是：

1. 构建一个 `lbc_mock` 版本。
2. 启动一个常驻的 `ssu-mgr`。
3. 让 `ubsectl` 连接这个 `ssu-mgr`。
4. 后续只用 `ubsectl allocate/list/allocate-result-get/mount/unmount/free`。

其中 `allocate/free` 走 LBC mock 脚本创建和删除 `/dev/nvmeXnY`；`mount/unmount` 会再通过 ReqShim 的 `/dev/ssu-ctl` 下发 `LOGDEV_CREATE/MAP_ADD/MAP_DEL/LOGDEV_DESTROY`，把 `/dev/ssuN` 逻辑块设备建出来或拆掉。

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
  -> /dev/ssu-ctl
  -> /dev/ssuN
```

## 最快验证路径

控制面 create/attach 只需要准备一个环境变量：`LBC_PREFIX`。它只表示 LBC mock 软件所在目录，用来找到 `mock/setup_mock_target.sh`、`mock/run_mock.sh` 和 sample 程序，不放 UBSE 自己的配置文件。如果要执行 `mount` 并对 `/dev/ssuN` 做数据面验证，还需要先加载 ReqShim 内核模块，让 `/dev/ssu-ctl` 存在。

```bash
export LBC_PREFIX=/path/to/lbc/mock/prefix

meson setup build-lbc -Dvendor=lbc_mock
meson compile -C build-lbc
```

如果当前机器有可用的内核构建目录，可以同时构建并加载 ReqShim：

```bash
meson setup build-lbc-kernel \
    -Dvendor=lbc_mock \
    -Dbuild_kernel=enabled \
    -Dkernel_src_dir=/lib/modules/$(uname -r)/build
meson compile -C build-lbc-kernel
sudo insmod build-lbc-kernel/src/kernel/reqshim/ssu_reqshim.ko
ls /dev/ssu-ctl
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
    --user user-demo \
    --share exclusive \
    --host local \
    --out /tmp/ssu-rid

RID=$(cat /tmp/ssu-rid)
RESULT=$(sudo ./build-lbc/tools/ubsectl allocate-result-get --request-id "$RID")
DEV=$(printf '%s\n' "$RESULT" | sed -n '1p')
printf '%s\n' "$RESULT"

sudo ./build-lbc/tools/ubsectl mount \
    --dev "$DEV" \
    --host local

ls /dev/ssu*

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
ssu_count=3
physical_disk_count=0  # allocate 默认单张物理盘
logical_disk_aggregate=on
```

如果没有加载 ReqShim，真实 `/dev` 环境下 `mount` 会失败，`ubsectl` 会返回 `mount failed: SSU_ERR_KERNEL (-6)`，并提示检查 `/dev/ssu-ctl`。`ssu-mgr` 终端和 `/tmp/ubse-lbc-mock.log` 里会看到类似 `ReqShim open failed ctl=/dev/ssu-ctl errno=2` 的信息。这种情况下 `allocate` 仍可能成功，因为 LBC mock 已经创建了 `/dev/nvmeXnY`，但还没有把它挂成 `/dev/ssuN`。

## ReqShim 模块怎么构建和加载

ReqShim 是内核模块，必须使用当前正在运行的内核对应的构建目录。先检查：

```bash
uname -r
ls /lib/modules/$(uname -r)/build
```

如果这个目录不存在，说明当前机器缺少匹配的 kernel headers/kernel-devel，模块无法在这台机器上直接构建。

构建：

```bash
meson setup build-lbc-kernel \
    -Dvendor=lbc_mock \
    -Dbuild_kernel=enabled \
    -Dkernel_src_dir=/lib/modules/$(uname -r)/build

meson compile -C build-lbc-kernel
```

加载：

```bash
sudo insmod build-lbc-kernel/src/kernel/reqshim/ssu_reqshim.ko
lsmod | grep ssu_reqshim
ls -l /dev/ssu-ctl
```

看到 `/dev/ssu-ctl` 后，再执行 `ubsectl mount`。如果加载失败，先看：

```bash
sudo dmesg | tail -n 80
```

卸载前要先解挂所有 `/dev/ssuN`：

```bash
sudo ./build-lbc/tools/ubsectl unmount --dev /dev/ssu0
sudo rmmod ssu_reqshim
```

当前 ReqShim 已经实现了 MVP 数据面的基础闭环：

- 注册 `/dev/ssu-ctl` 控制设备。
- 支持 `GET_VERSION`、`LOGDEV_CREATE`、`LOGDEV_DESTROY`、`MAP_ADD`、`MAP_DEL`、`MAP_QUERY` ioctl。
- `LOGDEV_CREATE` 后创建 `/dev/ssuN` 逻辑块设备。
- 对 `/dev/ssuN` 的普通 read/write 请求，根据映射表找到物理设备和 LBA。
- 当前提交到底层物理设备使用普通 block/bio 路径。

当前还不是最终完整版：

- 只支持普通 read/write，不支持 flush/discard/write zeroes 等扩展请求。
- `/dev/nvme*` 后端已经有识别边界，但还没有真正组装 NVMe 命令并直投 NVMe/LBC INI 队列。
- 还没有实现 SGL/URMA、多流、NDS、多副本/EC 等后续能力。

## 逻辑 API 参数怎么理解

`ubsectl allocate` 对应逻辑接口 `INTF_SSU_API_ALLOCATE`。快速验证时推荐这样传：

```bash
sudo ./build-lbc/tools/ubsectl allocate \
    --size 512M \
    --user user-demo \
    --share exclusive \
    --host local \
    --out /tmp/ssu-rid
```

这些参数的含义是：

- `--size`：逻辑盘大小，不是单张物理盘大小。
- `--user`：逻辑盘的用户归属标签。租户隔离、用户权限、Host 是否允许访问，都由上游业务系统判断，本组件不做业务校验。
- `--physical-disks N`：手动指定使用几张物理盘。不传时等价于默认单张物理盘。
- `--aggregate`：逻辑盘聚合开关，默认打开，通常不用显式传。
- `--no-aggregate`：当前 MVP 不支持，会返回 `SSU_ERR_UNSUPPORTED`。
- `--share exclusive|shared`：独占盘给一个 Host，共享盘给 Host 列表。
- `--host`：独占盘是使用该盘的 Host；共享盘可以传多次，表示共享范围内的 Host 列表。

老参数 `--tenant`、`--shards` 仍作为兼容别名保留，但新集成建议使用 `--user`、`--physical-disks`。

`allocate-result-get` 的输出是多行。第一行是后续 `mount/free` 使用的逻辑设备路径，后面每一行是物理盘 LBA 明细：

```text
/dev/ssu0
physical 0 lbc-mock-ssu0 1 0 536870912 lba=0
```

脚本里不要再把整个输出都当成设备路径。推荐这样取第一行：

```bash
RESULT=$(sudo ./build-lbc/tools/ubsectl allocate-result-get --request-id "$RID")
DEV=$(printf '%s\n' "$RESULT" | sed -n '1p')
printf '%s\n' "$RESULT"
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

当前 LBC mock 快速路径默认发现 3 个 mock SSU：`lbc-mock-ssu0`、`lbc-mock-ssu1`、`lbc-mock-ssu2`。不传 `--physical-disks` 时仍按单张物理盘分配；需要验证多物理盘时可以显式传 `--physical-disks 3`。

申请 512 MiB 空间：

```bash
sudo ./build-lbc/tools/ubsectl allocate \
    --size 512M \
    --user user-demo \
    --share exclusive \
    --host local \
    --out /tmp/ssu-rid

RID=$(cat /tmp/ssu-rid)
```

获取这次分配对应的逻辑设备路径和物理盘 LBA 明细：

```bash
RESULT=$(sudo ./build-lbc/tools/ubsectl allocate-result-get --request-id "$RID")
DEV=$(printf '%s\n' "$RESULT" | sed -n '1p')
printf '%s\n' "$RESULT"
```

输出类似：

```text
/dev/ssu0
physical 0 lbc-mock-ssu0 1 0 536870912 lba=0
```

`physical` 行字段依次是：序号、SSU ID、namespace ID、逻辑偏移、长度、物理起始 LBA。

挂载成逻辑设备：

```bash
sudo ./build-lbc/tools/ubsectl mount \
    --dev "$DEV" \
    --host local
```

这一步会做两件事：

- 调 LBC mock 已经 attach 出来的 `/dev/nvmeXnY` 作为物理块设备。
- 通过 `/dev/ssu-ctl` 给 ReqShim 下发 `LOGDEV_CREATE` 和 `MAP_ADD`，创建 `/dev/ssuN` 并写入逻辑区间到物理区间的映射。

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
ls /dev/ssu*
ls /dev/nvme*n*
lsblk "$DEV"
lsblk /dev/nvme1n1
cat /sys/class/block/nvme1n1/size
ls /sys/kernel/config/nvmet/subsystems/nqn.2025-01.io.ssu:m0/namespaces/
```

期望：

- `/dev/ssuN` 存在，`lsblk "$DEV"` 能看到逻辑块设备。
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

## C++ SDK 最小示例

用户态集成也可以直接调 SDK。下面示例只展示逻辑 API 字段怎么填，以及分配、取结果、挂载、解挂载、释放的调用顺序；LBC mock 快速联调仍建议先用前面的 `ubsectl + ssu-mgr` 路径跑通。

```cpp
#include "ssu_api.h"

#include <cstdio>

int main()
{
    const char *hosts[] = {"local"};
    ssu_api_allocate_req_t req = {};
    ssu_api_allocate_resp_t resp = {};
    ssu_api_allocate_result_info_t result = {};

    req.size_bytes = 512ULL * 1024ULL * 1024ULL;
    req.user_id = "user-demo";
    req.physical_disk_count = 0;       // 默认单张物理盘
    req.logical_disk_aggregate = 0;    // 默认打开聚合
    req.allocation_type = SSU_SHARE_EXCLUSIVE;
    req.host_ids = hosts;
    req.host_count = 1;

    ssu_err_t err = ssu_api_allocate(&req, &resp);
    if (err != SSU_OK) {
        std::printf("allocate failed: %d\n", err);
        return 1;
    }

    err = ssu_api_allocate_result_get(resp.request_id, &result);
    if (err != SSU_OK) {
        std::printf("allocate-result-get failed: %d %s\n",
                    err, result.error_message);
        return 1;
    }

    std::printf("device=%s\n", result.device_path);
    for (uint32_t i = 0; i < result.physical_disk_count; i++) {
        const ssu_api_physical_lba_t &disk = result.physical_disks[i];
        std::printf("physical %u %s %s %llu %llu lba=%llu\n",
                    i,
                    disk.ssu_id,
                    disk.ns_id,
                    (unsigned long long)disk.logical_offset,
                    (unsigned long long)disk.length,
                    (unsigned long long)disk.lba);
    }

    err = ssu_api_mount(result.device_path, "local");
    if (err != SSU_OK) {
        std::printf("mount failed: %d\n", err);
        return 1;
    }

    ssu_api_unmount(result.device_path);
    ssu_api_free(result.device_path);
    return 0;
}
```

## 多物理盘和数据面验证

多物理盘分配使用 `--physical-disks N`：

```bash
sudo ./build-lbc/tools/ubsectl allocate \
    --size 12288 \
    --user user-demo \
    --physical-disks 3 \
    --host local \
    --out /tmp/ssu-rid
```

成功后 `allocate-result-get` 会返回多条 `physical` 行；`mount` 后 `query --type logdev` 也会看到同一个 `/dev/ssuN` 对应多条物理映射。

LBC mock 快速路径默认暴露 3 个 mock SSU，所以 `--physical-disks 3` 可以用于控制面验收。默认 mock 也会覆盖同样的 3 盘路径：

```bash
meson test -C build mvp2_check --print-errorlogs
meson test -C build mvp3_check --print-errorlogs
meson test -C build mvp4_check --print-errorlogs
```

其中：

- `mvp2_check` 验证 `--physical-disks 3`、三条物理 LBA、三条 `logdev` 映射。
- `mvp3_check` 验证挂载后通过 `ssu_smoke` 做用户态数据面读写。
- `mvp4_check` 验证 SDK 驱动的分配、挂载、读写、解挂载、释放闭环。

这三项使用 mock 后端文件模拟物理设备，不等同于 LBC mock 的真实 `/dev/nvmeXnY -> /dev/ssuN` 内核数据面闭环。LBC mock 快速路径当前主要验证控制面：create/attach 后出现 `/dev/nvmeXnY`，mount 后有逻辑映射，free 后 detach/delete。

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

常见错误可以先按下面判断：

| 错误 | 常见原因 | 处理方式 |
| ---- | ---- | ---- |
| `SSU_ERR_NO_RESOURCE` | `--physical-disks N` 超过当前可用物理盘，或池里没有 ONLINE 资源 | 先 `ubsectl list` 看资源数；LBC mock 默认有 3 个 SSU |
| `SSU_ERR_KERNEL` | `mount/unmount` 调 ReqShim 失败，常见是 `/dev/ssu-ctl` 不存在 | 先 `ls -l /dev/ssu-ctl`；如果不存在，加载 `ssu_reqshim.ko`；再看 `ssu-mgr` 终端和 `/tmp/ubse-lbc-mock.log` 里的 errno |
| `SSU_ERR_UNSUPPORTED` | 使用了 `--no-aggregate`，或请求了当前阶段未启用能力 | 快速验证保持默认聚合；未启用能力不要当成成功路径 |
| `SSU_ERR_NOT_FOUND` | request id、设备路径、namespace 或 LBC mock 生成的 `/dev/nvmeXnY` 找不到 | 检查 `RID`、`DEV` 是否取对，查看 `query --type logdev` 和 `/tmp/ubse-lbc-mock.log` |
| `mount/free` 参数异常 | 老脚本把 `allocate-result-get` 的多行输出整体当成设备路径 | 用 `sed -n '1p'` 只取第一行作为 `DEV` |

## 可选配置文件

默认情况下不需要配置文件。SSU Plugin 会使用这些默认值：

```text
dev_ip=127.0.0.1
port=4420
subnqn=nqn.2025-01.io.ssu:m0
dev_dir=/dev
configfs_dir=/sys/kernel/config/nvmet/subsystems
log_file=/tmp/ubse-lbc-mock.log
ssu_count=3
```

如果确实需要改，比如测试时想把 `/dev` 和 configfs 指到临时目录，可以写 UBSE 自己的配置文件。默认路径是：

```text
/etc/ubse/ssu_lbc_mock.conf
```

临时测试也可以用 `SSU_LBC_MOCK_CONFIG` 指到任意路径：

```bash
sudo env LBC_PREFIX="$LBC_PREFIX" \
    SSU_LBC_MOCK_CONFIG=/tmp/ssu_lbc_mock.conf \
    ./build-lbc/src/user/runtime/ssu-mgr --role=manager
```

配置内容示例：

```ini
dev_ip=127.0.0.1
port=4420
subnqn=nqn.2025-01.io.ssu:m0
dev_dir=/dev
configfs_dir=/sys/kernel/config/nvmet/subsystems
log_file=/tmp/ubse-lbc-mock.log
ssu_count=3
```

这份配置只属于 LBC mock SSU Plugin，不是整个 SSU Manager 的全局配置，也不属于 LBC mock 软件目录。不要把它放到 `$LBC_PREFIX` 下。

## 你不需要做的事

集成方不需要直接运行：

```bash
sudo bash mock/setup_mock_target.sh ...
sudo bash mock/run_mock.sh ./sample_create_attach ...
sudo bash mock/run_mock.sh ./sample_detach_delete ...
```

这些是插件内部会调用的东西。你的系统只需要按上面的方式启动 `ssu-mgr`，然后调用 `ubsectl`。

## 当前边界

当前这条 LBC mock 集成路径已经覆盖到 ReqShim 控制面：

- `ubsectl allocate` 触发 LBC mock create + attach，并返回请求 ID。
- `ubsectl allocate-result-get` 根据请求 ID 返回 `/dev/ssuN` 逻辑设备路径，并返回每张物理盘对应的 `ssu_id/ns_id/逻辑偏移/长度/LBA`。
- `ubsectl mount` 调 ReqShim `LOGDEV_CREATE/MAP_ADD`，建立 `/dev/ssuN -> /dev/nvmeXnY` 的逻辑映射。
- `ubsectl unmount` 调 ReqShim `MAP_DEL/LOGDEV_DESTROY`，删除逻辑映射并拆掉 `/dev/ssuN`。
- `ubsectl free` 触发 LBC mock detach + delete。

也就是说，现在可以用它验证“上游只调 `ubsectl`，底层出现并删除 `/dev/nvmeXnY`，并由 ReqShim 创建和拆除 `/dev/ssuN`”。

真正对 `/dev/ssuN` 发起读写，需要在有可加载内核模块的真实 Linux 环境里验收。当前仓库内的普通 Meson 测试会用临时目录模拟 LBC mock 脚本，因此会跳过真实 `/dev/ssu-ctl`；真实 `/dev` 环境不会跳过，ReqShim 没加载时 `mount` 会明确失败。

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
