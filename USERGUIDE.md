# 使用手册（USERGUIDE）

本手册面向**只想使用、不打算读源码**的集成方。读完它，你能用自己的程序或脚本，把 SSU 池化组件跑起来：申请空间 → 挂成 `/dev/ssuN` 逻辑块设备 → 读写数据 → 解挂载/释放。

两种用法都覆盖：

- **运维 / 脚本**：用 `ubsectl` 命令行（无需写代码）。
- **业务程序**：运行时 `dlopen` 加载 `libssu_api.so`，调 SDK 函数（C/C++）。

> 全文命令以 **LBC mock** 后端为例——它是给集成测试用的、能在单机跑通的 NVMe-over-UB 模拟后端。真实厂商环境的命令形态一致，只是后端换成真实 SSU + LBC INI 驱动。

---

## 1. 它能为你做什么（30 秒）

你（业务存储系统，例如 UBS IO）向本组件申请一块逻辑存储空间，本组件：

1. 在底层 SSU 上建好 namespace；
2. 在你的机器上挂出一个**逻辑块设备 `/dev/ssuN`**；
3. 你像用普通磁盘一样对 `/dev/ssuN` 读写；
4. 用完可以**解挂载**（保留数据，以后还能再挂）或**释放**（彻底擦除并归还空间）。

你**全程不碰**底层 NVMe 命令、namespace、物理盘细节。

---

## 2. 先准备好环境（一次性）

### 2.1 你需要拿到什么

| 依赖 | 说明 |
| ---- | ---- |
| 本组件构建产物 | `ssu-mgr`（常驻控制面进程）、`ubsectl`（CLI）、`libssu_api.so` + `ssu_api.h`（SDK） |
| LBC mock 后端 | 模拟 SSU 硬件的脚本套件，放在某个目录（下文记作 `$LBC_PREFIX`） |
| 内核头（可选） | 若要真正读写 `/dev/ssuN`，需要构建并加载 `ssu_reqshim.ko` 内核模块 |

`$LBC_PREFIX` 至少要包含这些文件：

```text
$LBC_PREFIX/
├── mock/
│   ├── setup_mock_target.sh
│   └── run_mock.sh
├── sample_create_attach
└── sample_detach_delete
```

本组件会自己调用这些脚本。集成方平时只调用 `ubsectl` 或 SDK，不直接调 LBC mock 脚本。

先做一次前置检查，避免把旧测试目录或空目录当成 LBC mock：

```bash
test -x "$LBC_PREFIX/mock/setup_mock_target.sh"
test -x "$LBC_PREFIX/mock/run_mock.sh"
test -e "$LBC_PREFIX/sample_create_attach"
test -e "$LBC_PREFIX/sample_detach_delete"
```

> 只在 `/tmp/ssu-lbc-mock-test-*` 下面找到的 prefix 通常是 Meson 测试生成的假目录，只适合自动化测试，不适合真实 `/dev` 数据面验证。
> blue-98 当前也是这种情况：机器上只发现了自动测试留下的 `/tmp/ssu-lbc-mock-test-*` 目录，没有发现可用于真实 `/dev` 验证的 LBC mock 部署。要按本手册跑真实数据面，需要先把 LBC mock 后端部署到固定目录，然后把 `LBC_PREFIX` 指到那里。

### 2.2 构建

```bash
# 指向 LBC mock 后端目录
export LBC_PREFIX=/path/to/lbc/mock/prefix

# 用户态（必选）
meson setup build-lbc -Dvendor=lbc_mock
meson compile -C build-lbc

# 内核模块（真实 /dev 环境下执行 mount 和数据面读写时需要）
meson setup build-lbc-kernel \
    -Dvendor=lbc_mock \
    -Dbuild_kernel=enabled \
    -Dkernel_src_dir=/lib/modules/$(uname -r)/build
meson compile -C build-lbc-kernel
sudo insmod build-lbc-kernel/src/kernel/reqshim/ssu_reqshim.ko
ls -l /dev/ssu-ctl        # 看到这个控制设备 = 模块加载成功
```

> 不加载 ReqShim 也能做控制面验收（allocate/list/allocate-result-get/free 都能跑）；只有 `mount`（建 `/dev/ssuN`）和真正的数据读写需要它。ReqShim 没加载时 `mount` 会明确报 `SSU_ERR_KERNEL (-6)`。

数据面验证如果要用 `fio` 和 `iostat`，还需要这两个工具：

```bash
command -v fio
command -v iostat
```

在 openEuler 上通常来自 `fio` 和 `sysstat` 包；如果机器不允许安装，也可以只用后面的 `dd + cmp + blockdev` 做基础验证。

blue-98 当前默认环境未安装 `fio` 和 `iostat`。如果不能安装软件包，就跳过文档里的 fio 压测和 iostat 观察，先用 `dd + cmp + blockdev` 验证基本读写和容量。

blue-98 上已经用 openEuler 24.03 LTS-SP3 的 6.6 aarch64 内核验证过模块能编译出来：

```text
$ uname -r
6.6.0-132.0.0.111.oe2403sp3.aarch64

$ modinfo build-lbc-kernel/src/kernel/reqshim/ssu_reqshim.ko | grep vermagic
vermagic:       6.6.0-132.0.0.111.oe2403sp3.aarch64 SMP mod_unload modversions aarch64
```

构建后你会用到两个可执行程序：

```
build-lbc/src/user/runtime/ssu-mgr      # 常驻控制面进程
build-lbc/tools/ubsectl                 # 命令行工具
```

### 2.3 启动控制面（`ssu-mgr`）

`ssu-mgr` 是**常驻进程**，保存所有分配/挂载状态。开一个终端，让它一直跑：

```bash
sudo env LBC_PREFIX="$LBC_PREFIX" \
    ./build-lbc/src/user/runtime/ssu-mgr --role=manager
```

> 必须用 `sudo`：LBC mock 要操作 configfs 和 `/dev/nvme*`。**这个进程不要退出**，否则状态丢失。

后续所有 `ubsectl` 命令、SDK 调用都通过它默认的本地通道（`/tmp/ubse-ssu-mgr.fifo`）连到这个 `ssu-mgr`。

验证连通：

```bash
sudo ./build-lbc/tools/ubsectl list
```

看到类似输出就说明通了（LBC mock 默认发现 3 个 mock SSU）：

```
pool entries: 3
lbc-mock-ssu0 lbc-mock-host0 ONLINE 0/536870912
lbc-mock-ssu1 lbc-mock-host1 ONLINE 0/536870912
lbc-mock-ssu2 lbc-mock-host2 ONLINE 0/536870912
```

---

## 3. 完整使用流程（CLI 版）

**所有命令都在另一个终端执行，只调 `ubsectl`，不碰 LBC mock 脚本。**

### 第 1 步：申请空间

```bash
sudo ./build-lbc/tools/ubsectl allocate \
    --size 512M \
    --user user-demo \
    --share exclusive \
    --host local \
    --out /tmp/ssu-rid
```

- `--size`：**逻辑盘**大小（支持 `K`/`M`/`G` 后缀），不是单张物理盘大小。
- `--user`：用户/租户标签（仅作记录，**鉴权由你的业务系统负责**，本组件不校验）。
- `--share exclusive|shared`：独占给一个 host / 共享给多个 host。
- `--host`：独占盘=使用者；共享盘可传多次 `--host` 形成列表。
- `--physical-disks N`：可选，手动指定用几张物理盘；不传=默认单盘。
- `--out FILE`：把 `request_id` 写入文件，方便脚本取用。

`allocate` 是**异步**的——它立刻返回一个 `request_id`，底层正在 create+attach。上面的命令会同时在屏幕上打印类似 `alloc-0`，并把同样的内容写入 `/tmp/ssu-rid`。

### 第 2 步：取分配结果（拿到逻辑设备路径）

```bash
RID=$(cat /tmp/ssu-rid)
RESULT=$(sudo ./build-lbc/tools/ubsectl allocate-result-get --request-id "$RID")
DEV=$(printf '%s\n' "$RESULT" | sed -n '1p')      # 只取第一行作为设备路径
printf '%s\n' "$RESULT"
```

输出（**第一行是逻辑设备路径，后面是物理盘明细**）：

```
/dev/ssu0
physical 0 lbc-mock-ssu0 1 0 536870912 lba=0
```

`physical` 行字段：序号、SSU ID、namespace ID、逻辑偏移、长度、物理起始 LBA。

> 脚本里**只取第一行**当作 `$DEV`，别把整段输出当设备路径。

### 第 3 步：挂载成逻辑块设备

```bash
sudo ./build-lbc/tools/ubsectl mount --dev "$DEV" --host local
```

这一步把物理 namespace 挂成 `/dev/ssuN`，并向 ReqShim 下发映射。成功后：

```bash
ls /dev/ssu*                                    # 看到 /dev/ssuN
sudo ./build-lbc/tools/ubsectl query --type logdev
```

`query --type logdev` 输出（`/dev/ssuN → /dev/nvmeXnY` 的映射，真实设备名以本机输出为准）：

```
logdev entries: 1
/dev/ssu0 local alloc-0 0 536870912 /dev/nvmeXnY 1 0
```

真实 `/dev` 环境下，`mount` 成功还应能看到系统块设备：

```bash
PHYS_DEV=$(sudo ./build-lbc/tools/ubsectl query --type logdev |
    awk -v dev="$DEV" '$1 == dev { print $6; exit }')

ls /dev/ssu*
lsblk "$DEV"
lsblk "$PHYS_DEV"
blockdev --getsz "$PHYS_DEV"   # 1048576，表示 512 MiB
```

不要把 `/dev/nvme1n1` 写死。很多机器上它可能是已有系统盘或业务盘；实际物理设备名必须以 `query --type logdev` 输出为准。

blue-98 实测 `/dev/nvme1n1`、`/dev/nvme2n1`、`/dev/nvme3n1` 都是已有 3.6T 磁盘，其中 `/dev/nvme3n1` 挂着系统分区。不要直接对这些名字照抄 `dd`/`fio`。

Meson 验收测试为了不污染真实 `/dev`，会把 `dev_dir` 指到临时目录。这种测试模式会跳过 `/dev/ssu-ctl`，所以日志里可能看到：

```text
lbc_mock: ReqShim control device unavailable, skip ioctl path ctl=/dev/ssu-ctl errno=2
```

这不是给真实集成使用的模式。真实集成保持默认 `dev_dir=/dev` 时，`mount` 必须加载 ReqShim。

### 第 4 步：读写数据（标准块设备操作）

`$DEV` 指向的 `/dev/ssuN` 就是一个普通块设备，任何 `open`/`read`/`write` 程序都能用。下面命令会覆盖测试盘上的前 4 MiB 数据，只能对测试设备执行：

```bash
# 简单验证：写一段随机数据再读回比对
sudo dd if=/dev/urandom of=/tmp/data.bin bs=1M count=4
sudo dd if=/tmp/data.bin of="$DEV" bs=1M count=4 oflag=direct
sudo dd if="$DEV" of=/tmp/out.bin bs=1M count=4 iflag=direct
cmp /tmp/data.bin /tmp/out.bin && echo "data verified"
```

或用自带的 `ssu_smoke`（一条命令跑完写+读+比对）：

```bash
sudo ./build-lbc/tests/stubs/ubs_io/ssu_smoke "$DEV" --bytes 65536
```

如果机器上装了 `fio`，更适合做压力一点的黑盒验证。注意：这里的 `--size` 是 fio 的工作集大小，如果只写 `--size=256M`，fio 默认只会访问从逻辑 offset 0 开始的 256 MiB 区间。

```bash
DEV_BYTES=$(sudo blockdev --getsize64 "$DEV")

sudo fio \
    --name=ssu-dataplane \
    --filename="$DEV" \
    --rw=randrw \
    --bs=4k \
    --iodepth=16 \
    --direct=1 \
    --size="$DEV_BYTES" \
    --verify=crc32c \
    --do_verify=1 \
    --verify_fatal=1
```

### 第 5 步：解挂载 / 释放（二选一，语义不同）

| 操作 | 命令 | 数据 |
| ---- | ---- | ---- |
| **解挂载**（保留数据，以后可再挂/迁移到别的节点） | `sudo ./build-lbc/tools/ubsectl unmount --dev "$DEV"` | 保留 |
| **释放**（彻底擦除并删 namespace，归还空间） | 先 `unmount`，再 `sudo ./build-lbc/tools/ubsectl free --dev "$DEV"` | 物理擦除 |

```bash
sudo ./build-lbc/tools/ubsectl unmount --dev "$DEV"     # 拆 /dev/ssuN，数据留着
sudo ./build-lbc/tools/ubsectl free     --dev "$DEV"     # 删 namespace、擦数据
```

### CLI 命令速查

```text
ubsectl allocate   --size BYTES [--user U] [--physical-disks N] [--share exclusive|shared] [--host H...] [--out FILE]
ubsectl allocate-result-get --request-id RID [--out FILE]
ubsectl list                                    # 看资源池
ubsectl mount   --dev /dev/ssuN --host HOST
ubsectl unmount --dev /dev/ssuN
ubsectl free    --dev /dev/ssuN
ubsectl query   --type pool|allocation|logdev   # 查资源池/分配明细/逻辑设备映射
```

> 还有一组兼容旧命令 `alloc/mount --aid/release/query`，新集成建议用上面的 `allocate/free/query`。

---

## 4. 完整使用流程（SDK 版）

上游业务系统通常通过 `dlopen` 加载 SDK，不在编译期直接链接 `libssu_api.so`。使用顺序是：

1. `dlopen("libssu_api.so", RTLD_NOW | RTLD_LOCAL)`；
2. `dlsym` 只取一个入口函数 `ssu_api_entry`；
3. `ssu_api_entry()` 返回函数表，先调用 `api->init(NULL)`；
4. 按同样生命周期申请、查询、挂载、读写、解挂载、释放；
5. 退出前调用 `api->fini()`，再 `dlclose()`。

`ssu_api_init(NULL)` 表示使用默认 SDK 配置。需要显式传 options 时，只要把 `opts.struct_size` 填成 `sizeof(opts)`；当前版本没有其它必填字段。

最小示例：

```cpp
#include "ssu_api.h"
#include <dlfcn.h>
#include <cstdio>
#include <cstring>

using ssu_api_entry_fn = const ssu_api_ops_t *(*)(void);

int main()
{
    void *sdk = dlopen("libssu_api.so", RTLD_NOW | RTLD_LOCAL);
    if (sdk == nullptr) {
        std::printf("dlopen failed: %s\n", dlerror());
        return 1;
    }

    auto ssu_api_entry = reinterpret_cast<ssu_api_entry_fn>(
        dlsym(sdk, "ssu_api_entry"));
    if (ssu_api_entry == nullptr) {
        std::printf("dlsym failed: %s\n", dlerror());
        dlclose(sdk);
        return 1;
    }

    const ssu_api_ops_t *api = ssu_api_entry();
    if (api == nullptr || api->struct_size < sizeof(*api)) {
        std::printf("invalid ssu api table\n");
        dlclose(sdk);
        return 1;
    }

    if (api->init(NULL) != SSU_OK) {
        std::printf("api init failed\n");
        dlclose(sdk);
        return 1;
    }

    /* ---- 第 1 步：申请空间（异步，返回 request_id）---- */
    const char *hosts[] = {"local"};
    ssu_api_allocate_req_t  req  = {};
    ssu_api_allocate_resp_t resp = {};
    req.size_bytes            = 512ULL * 1024 * 1024;   // 512 MiB
    req.user_id               = "user-demo";
    req.physical_disk_count   = 0;                       // 0 = 默认单盘
    req.logical_disk_aggregate= 1;                       // 1 = 开聚合（默认）
    req.allocation_type       = SSU_SHARE_EXCLUSIVE;
    req.host_ids              = hosts;
    req.host_count            = 1;

    if (api->allocate(&req, &resp) != SSU_OK) {
        std::printf("allocate failed\n"); return 1;
    }

    /* ---- 第 2 步：取结果（拿到逻辑设备路径 + 物理盘明细）---- */
    ssu_api_allocate_result_info_t result = {};
    if (api->allocate_result_get(resp.request_id, &result) != SSU_OK) {
        std::printf("result-get failed: %s\n", result.error_message); return 1;
    }
    std::printf("device=%s\n", result.device_path);

    /* ---- 第 3 步：挂载 ---- */
    if (api->mount(result.device_path, "local") != SSU_OK) {
        std::printf("mount failed\n"); return 1;
    }

    /* ---- 第 4 步：读写 /dev/ssuN（标准 open/read/write，此处略）---- */

    /* ---- 第 5 步：解挂载 + 释放 ---- */
    api->unmount(result.device_path);   // 保留数据
    api->free   (result.device_path);   // 擦除并删 namespace

    api->fini();
    dlclose(sdk);
    return 0;
}
```

编译：

```bash
g++ my_app.cpp -o my_app \
    -I./include \
    -ldl

LD_LIBRARY_PATH=./build-lbc/src/user/api:./build-lbc/src/user/controller:./build-lbc/src/user/scheduler:./build-lbc/src/user/plugin/vendors/lbc_mock \
    ./my_app
```

如果已经把库安装到系统路径，可以按你的安装路径调整 `-I` 和 `LD_LIBRARY_PATH`。开发自测时也可以直接链接 `-L./build-lbc/src/user/api -lssu_api`，但上游集成推荐按上面的 `dlopen` 方式。

### SDK 函数速查

| 函数 | 作用 |
| ---- | ---- |
| `ssu_api_entry()` | `dlopen` 主入口，返回 `ssu_api_ops_t` 函数表 |
| `ssu_api_init(opts)` | 初始化 SDK；`opts` 可传 `NULL` 使用默认值，`dlopen` 用户应先调它 |
| `ssu_api_fini()` | 释放 SDK 进程内缓存；调用后再 `dlclose` |
| `ssu_api_allocate` | 申请空间，返回 `request_id`（异步） |
| `ssu_api_allocate_result_get` | 按 `request_id` 取逻辑设备路径 + 物理盘 LBA 明细 |
| `ssu_api_mount(device_path, host)` | 挂成 `/dev/ssuN` |
| `ssu_api_unmount(device_path)` | 解挂载（保留数据） |
| `ssu_api_free(device_path)` | 释放（擦除并删 namespace） |
| `ssu_api_list(out, &count)` | 列资源池 |

> 还有一组细粒度接口 `ssu_resource_alloc/mount/unmount/release/query`（`allocate_id` 语义），用于需要 extent 明细的场景；新集成建议用 `ssu_api_*`。完整字段见 `include/ssu_api.h`。

---

## 5. 多物理盘与共享

**多物理盘**（连续区间聚合）：

```bash
sudo ./build-lbc/tools/ubsectl allocate --size 768M --physical-disks 3 \
    --user user-demo --share exclusive --host local --out /tmp/ssu-rid
```

这条命令用 3 张物理盘拼出一个 768 MiB 的逻辑盘，每张物理盘分到连续的 256 MiB 逻辑区间。输出形态如下：

```text
/dev/ssu1
physical 0 lbc-mock-ssu0 1 0 268435456 lba=0
physical 1 lbc-mock-ssu1 2 268435456 268435456 lba=0
physical 2 lbc-mock-ssu2 3 536870912 268435456 lba=0
```

挂载后，`query --type logdev` 会看到同一个 `/dev/ssu1` 对应 3 条物理映射：

```text
logdev entries: 3
/dev/ssu1 local alloc-1 0 268435456 /dev/nvmeXnY 1 0
/dev/ssu1 local alloc-1 268435456 268435456 /dev/nvmeXnZ 2 0
/dev/ssu1 local alloc-1 536870912 268435456 /dev/nvmeXnW 3 0
```

这里的 `/dev/nvmeXnY` 只是占位写法，实际名称以本机输出为准。

当前 MVP 的多物理盘不是 4 KiB 轮询 RAID0 条带，而是“逻辑盘按连续区间切成多段”。所以如果 fio 只访问前 256 MiB，只会命中第一张物理盘。要观察 3 张盘都有 I/O，需要让 fio 覆盖整块逻辑盘：

```bash
PHYS=$(sudo ./build-lbc/tools/ubsectl query --type logdev |
    awk -v dev="$DEV" '$1 == dev { sub("/dev/", "", $6); print $6 }')

# 先在一个终端观察底层盘；没有 iostat 时，退回容量检查
if command -v iostat >/dev/null 2>&1; then
    iostat -dx 1 $PHYS
else
    for p in $PHYS; do
        echo "$p $(sudo blockdev --getsize64 /dev/$p)"
    done
fi

# 再在另一个终端跑覆盖整块逻辑盘的 fio
DEV_BYTES=$(sudo blockdev --getsize64 "$DEV")
sudo fio \
    --name=ssu-multiphy \
    --filename="$DEV" \
    --rw=randrw \
    --bs=4k \
    --iodepth=16 \
    --direct=1 \
    --time_based \
    --runtime=30 \
    --randrepeat=0 \
    --norandommap=1 \
    --size="$DEV_BYTES" \
    --verify=crc32c \
    --do_verify=1 \
    --verify_fatal=1
```

也可以按 `query --type logdev` 的逻辑偏移和物理 LBA，分别从 `$DEV` 与输出里的各个物理设备读取同一段数据再 `cmp`。这个方法能证明逻辑区间确实映射到了指定物理盘。

**共享盘**（多 host 共用同一 namespace）：

```bash
sudo ./build-lbc/tools/ubsectl allocate --size 1G --share shared \
    --host node-a --host node-b --out /tmp/ssu-rid
```

> 多 host 并发写的**一致性 / fencing 由你的业务系统保证**，本组件只负责访问路径的增删。

---

## 6. 出错怎么办

`ubsectl` 会返回错误名 + 提示，不只给数字：

```
alloc failed: SSU_ERR_NOT_FOUND (-3)
plugin: lbc_mock
hint: lbc_mock create/attach finished, but no new /dev/nvmeXnY namespace was found. ...
```

| 错误码 | 常见原因 | 处理 |
| ---- | ---- | ---- |
| `SSU_ERR_NO_RESOURCE` | `--physical-disks N` 超过可用盘数，或池里没 ONLINE 资源 | 先 `ubsectl list`（LBC mock 默认 3 个 SSU） |
| `SSU_ERR_KERNEL (-6)` | `mount/unmount` 调 ReqShim 失败，通常 `/dev/ssu-ctl` 不存在 | `ls -l /dev/ssu-ctl`；不存在则 `insmod ssu_reqshim.ko` |
| `SSU_ERR_UNSUPPORTED (-9)` | 用了 `--no-aggregate`，或请求了未启用能力 | 快速验证保持默认聚合；未启用能力不要当成功路径 |
| `SSU_ERR_NOT_FOUND (-3)` | request_id / 设备路径 / namespace 找不到 | 检查 `$RID`、`$DEV` 是否取对；`query --type logdev` |
| `mount/free` 参数错 | 脚本把 `allocate-result-get` 的多行输出整体当设备路径 | 用 `sed -n '1p'` 只取第一行 |
| manager 重启后 `/dev/ssuN` 还在 | 用户态状态丢了，但 ReqShim 内核模块里还有旧逻辑盘 | 先执行 `sudo ./build-lbc/tools/ubsectl unmount --dev /dev/ssuN` 清理 ReqShim 残留，再重新申请/挂载 |
| 多物理盘 fio 只有一张盘有 I/O | `query --type logdev` 只有一行，或 fio 只访问了第一段逻辑区间 | 先确认 `logdev entries: 3`；fio 用 `blockdev --getsize64 "$DEV"` 覆盖整块逻辑盘 |

查日志：

```bash
sudo tail -n 50 /tmp/ubse-lbc-mock.log      # LBC mock 插件日志
# 同时看启动 ssu-mgr 的那个终端的输出
```

---

## 7. 常见问题

**能不能不启动 `ssu-mgr`，直接跑 `ubsectl`？**
不建议。单条 `list` 可能能看到资源，但完整生命周期不行——每条 `ubsectl` 都是短进程，不连 `ssu-mgr` 时状态不保留。`allocate → mount → free` 必须走常驻 `ssu-mgr`。

**`ssu-mgr` 重启后旧的 `/dev/ssu0` 还在怎么办？**
先清理逻辑块设备：

```bash
sudo ./build-lbc/tools/ubsectl unmount --dev /dev/ssu0
```

这会按 `/dev/ssu0` 的 minor 清理 ReqShim 里的残留逻辑盘，即使新的 `ssu-mgr` 进程已经没有旧的用户态挂载记录。注意：本系统当前不持久化 manager 内存状态，重启后旧 request_id/free 状态不会自动恢复；要重新使用请重新 `allocate → allocate-result-get → mount`。

**为什么需要 sudo？**
LBC mock 要操作 configfs 和 `/dev/nvme*`；`ssu-mgr` 建的通信通道默认同用户访问。最简单：`ssu-mgr` 和 `ubsectl` 都用 root。

**已经 `export LBC_PREFIX` 了，为什么启动 `ssu-mgr` 还要写 `sudo env LBC_PREFIX="$LBC_PREFIX"`？**
因为 `sudo` 默认可能清理普通用户环境变量。前面的 `export` 是给当前 shell 用的；`sudo env LBC_PREFIX="$LBC_PREFIX"` 是明确把这个变量传进 root 启动的 `ssu-mgr`。如果你的 sudoers 已经保留 `LBC_PREFIX`，也可以不写这一段，但手册里保留它更稳。

**为什么看不到 512M？**
先用 `sudo ./build-lbc/tools/ubsectl query --type logdev` 找到真实物理设备，再查容量：

```bash
PHYS_DEV=$(sudo ./build-lbc/tools/ubsectl query --type logdev |
    awk -v dev="$DEV" '$1 == dev { print $6; exit }')
sudo blockdev --getsz "$PHYS_DEV"
```

512 MiB 对应输出 `1048576`（按 512B 扇区）。不对就看 LBC mock 日志。

**`SSU_MGR_SOCKET` 是什么？**
`ubsectl` 找 `ssu-mgr` 的本地通信入口。默认 `/tmp/ubse-ssu-mgr.fifo`（命名管道）。只有想换路径时才需要设它：

```bash
# 启动时指定
ssu-mgr --role=manager --socket /custom/path.fifo
# 调用时指向同一个
sudo env SSU_MGR_SOCKET=/custom/path.fifo ./build-lbc/tools/ubsectl list
```

**集成 `ubsectl` 还是 LBC mock 脚本？**
集成 `ubsectl`（或 SDK）。LBC mock 脚本是底层适配细节，不应暴露给业务系统。

**可选的 LBC mock 配置文件？**
默认不需要。需要改 `/dev`、configfs、端口等时，写配置（默认路径 `/etc/ubse/ssu_lbc_mock.conf`，或用 `SSU_LBC_MOCK_CONFIG` 指向）：

```ini
dev_ip=127.0.0.1
port=4420
subnqn=nqn.2025-01.io.ssu:m0
dev_dir=/dev
configfs_dir=/sys/kernel/config/nvmet/subsystems
log_file=/tmp/ubse-lbc-mock.log
ssu_count=3
```

> 这份配置**只属于 LBC mock 插件**，别放进 `$LBC_PREFIX`，也不是 Manager 全局配置。

真实快速验证一般不用配置文件，只设置 `LBC_PREFIX`。只有需要把 `/dev`、configfs、日志路径等改成测试目录时，才设置 `SSU_LBC_MOCK_CONFIG`。`subnqn` 不能超过 31 个字符，默认值 `nqn.2025-01.io.ssu:m0` 已经满足限制。

---

## 8. 当前能力边界

已经覆盖：

- 控制面全闭环：`allocate`（create+attach）→ `allocate-result-get` → `mount`（建 `/dev/ssuN`）→ `unmount`/`free`。
- 数据面 MVP：对 `/dev/ssuN` 的普通 `read/write`，经 ReqShim 映射到底层物理设备。当前下发到底层设备走普通 block/bio 路径。

尚未支持（请求这些会返回 `SSU_ERR_UNSUPPORTED`）：

- `flush`/`discard`/`write zeroes` 等扩展 IO。
- `--no-aggregate`（非聚合模式）。
- 多副本（REPLICA）、纠删码（EC）、NDS 近数据访问、多流（stream）。

需要真实读写 `/dev/ssuN` 必须在有内核模块加载环境的 Linux 上验收；普通 Meson 测试用临时目录模拟，会跳过真实 `/dev/ssu-ctl`。blue-98 上按本手册的构建目录跑过的自动验收结果是：

```text
build-lbc:                   Ok: 13, Fail: 0
build-lbc-kernel:            Ok: 13, Fail: 0
```

其中 `build-lbc-kernel` 已验证 `.ko` 能在 6.6 aarch64 内核上构建并生成，`vermagic` 与当前内核一致，但没有在 blue-98 上自动执行 `insmod`；加载内核模块会影响运行内核，真实环境需要人工确认后再执行。
