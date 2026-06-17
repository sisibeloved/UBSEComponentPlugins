# MVP 阶段实现计划

> 本计划依据 `docs/design/implementation-design.md`（review 修改后版本）制定。
> 顶层目标：交付 MVP-0~5（§12.1），闭环"控制面（发现纳管/分配/2 步释放）+ 普通读写数据面"。
> **核心约束：MVP 必须能脱离 UBSE 独立验证**——不依赖上游 ubse daemon、真实 UBS IO、真实 LBC INI 驱动、真实 SSU 硬件，全部用自带 mock/stub 替代，可在一台（或少量）开发机上自验。

---

## 0. 总体策略：可独立验证的桩化边界

设计文档定义了三个外部邻居（§2.1）：UBS IO（上层数据面 + 控制面触发）、LBC INI（下层 NVMe-over-UB initiator）、OS（用户态/内核态）。MVP 的"独立验证"等价于把这三个邻居全部桩化：

| 外部邻居 | MVP 桩（替代真实依赖） | 实现位置 | 让真实依赖"可插拔"的手段 |
| ---- | ---- | ---- | ---- |
| **SSU 硬件 + LBC INI** | `null_blk` 内核模块 + 自带 mock LBC INI adapter（或 mock-plugin 文件后端） | `src/user/plugin/vendors/mock/`、`tests/stubs/lbc_ini/` | Plugin 的 `ssu_plugin_ops` 接口：真实厂商 plugin 与 mock plugin 同接口，构建时选 |
| **UBS IO（上层）** | 自带 `ssu_smoke` 测试程序，直接对 `/dev/ssuN` 做 `read/write`（程序化调 `libubse` + 块 I/O） | `tests/stubs/ubs_io/` | 数据面是标准块设备，任何 `open("/dev/ssuN")` 程序都是合法上层，UBS IO 只是其中一个 |
| **OS 内核态** | ReqShim `.ko` 直接加载到开发机内核（或 QEMU）；用户态 OS 接口（udev/epoll/genetlink）用 `loop`/`null_blk` 设备触发 | `ssu_reqshim.ko` + dev 机 | 不需要桩——OS 接口是真实的，只是触发源用 mock 设备 |
| **上游 ubse daemon** | MVP 不接 daemon：用户态三库 + 自带 `ssu-mgr`/`ssu-agent` 可执行入口直接跑；plugin 进程内 `dlopen`（受信，§11） | `src/user/runtime/` | 通过 `ssu.conf` 角色装配（§4.5.5），先自跑，M10 再接上游 daemon |

**关键设计决策（贯穿计划）：**

1. **接口先冻结**。`ssu_plugin_ops`（§4.4.2）、`ssu_*` API（§3.3）、ReqShim UAPI（§6.1.2e）三套契约在 MVP-0 就定稿并加测试，真实依赖与 mock 都遵守同一契约，确保"换桩即可联调"。
2. **每阶段都留独立验收脚本**，不依赖下一阶段。验收脚本放进 `tests/`，CI 可重复跑。
3. **能力门控前置**（§3.3 末尾）：MVP 只接受 `SSU_RELIABILITY_STRIPE` + 普通 SGL/NVMe；REPLICA/EC/NDS/多流一律返回 `SSU_ERR_UNSUPPORTED`，从 MVP-0 的 API 层就挡掉。

---

## 1. 阶段分解（MVP-0 ~ MVP-5）

每阶段给出：**交付物 / 任务清单 / 独立验收（可脚本化）/ 依赖**。

### MVP-0 骨架（冻结三套契约 + 可加载 .ko）

**目标**：用户态三库 + libubse/ubsectl + ReqShim `.ko` 能编译加载；三套外部接口契约冻结并有契约测试。

**交付物**
- `meson.build` 顶层 + 子目录（§8.2/8.3），`meson_options.txt`（`combine_core`、`kernel_src_dir`、`vendor=mock`）。
- `include/ssu_api.h`、`include/ssu_plugin.h`（§3.3、§4.4.2 的数据结构/枚举/函数签名全文落地，含 `SSU_ERR_UNSUPPORTED`）。
- `src/kernel/reqshim/` 骨架：`reqshim_main.c`（`module_init/exit`、`register_chrdev` 生成 `/dev/ssu-ctrl`）+ `reqshim_uapi.h`（§6.1.2e 全部 ioctl 编号与结构体）+ `Kbuild`（§8.4）。
- `libssu_api.so`、`libssu_controller.so`、`libssu_scheduler.so`、`libssu_plugin_mock.so`：函数体可为 stub（返回 `SSU_OK`/`SSU_ERR_UNSUPPORTED`），但符号齐、能链接。
- `ubsectl` CLI 骨架（子命令 `alloc/mount/unmount/release/query`，先解析参数调 SDK）。
- **契约测试**：API 层能力门控测试（REPLICA/EC/NDS/多流 → `SSU_ERR_UNSUPPORTED`）；ReqShim UAPI round-trip（用户态 `ioctl(SSU_IOC_GET_VERSION)`、空映射增删）。

**任务清单**
1. 落地 Meson 工程结构与三库 stub 构建；CI 镜像（meson + ninja + kernel-devel + libudev + protobuf）。
2. 落地三套头文件（API / plugin ops / ReqShim UAPI），逐字段对齐 §3.3/§4.4.2/§6.1.2e。
3. 写 ReqShim `module_init` + `/dev/ssu-ctrl` 字符设备 + ioctl 分发骨架（`SSU_IOC_GET_VERSION` 真返回，其余先 `-ENOTTY`/桩）。
4. 写 `libssu_plugin_mock.so` 最小实现（`name`/`discover` 返回 0 个 SSU，其余桩）。
5. CI：`meson test` 跑契约测试 + `modprobe` 加载 `.ko` 检查 `/dev/ssu-ctrl` 存在。

**独立验收（脚本 `tests/mvp0/check.sh`）**
```bash
meson setup build && ninja -C build
sudo modprobe null_blk                    # 数据面 mock 设备
sudo insmod build/src/kernel/reqshim/ssu_reqshim.ko
test -c /dev/ssu-ctrl                     # 控制设备存在
./build/tests/contract/api_gating         # REPLICA/EC/NDS/多流→UNSUPPORTED
./build/tests/contract/reqshim_uapi       # ioctl round-trip
ninja -C build test                       # 全绿
```

**依赖**：仅 OS + 内核头，无任何 SSU/UBS 依赖。✅ 可独立验证。

---

### MVP-1 发现纳管（用 mock SSU 池验证视图构建）

**目标**：Plugin `discover`/`connect` + Controller 纳管，Manager 全局视图能纳管 mock SSU 并标记 ONLINE。

**桩化策略**
- mock SSU = 开发机上的 `null_blk` 设备（或 `loop`），代表"物理块设备 `/dev/nvmeXnY`"。
- mock plugin 的 `discover` 扫描 `null_blk` 生成的 `/dev/nullb*` 当作 SSU；`connect` 直接 `open()` 该设备（无 fabric）；LCNE/CPLD/EID/CNA 用 mock 元数据（配置/文件）填充。
- 真实 LBC INI 的 NVMe Discover/Connect（§6.2.1）在 MVP 不接——mock plugin 走"设备已就绪"捷径。

**交付物**
- `libssu_plugin_mock.so` 的 `discover`/`connect`/`health_check` 真实实现（基于 `null_blk`）。
- Controller 发现纳管逻辑（§4.2.2）：udev 监听（§6.1.1 `udev_monitor_*`）+ 周期扫描任务（timerfd）+ 内存视图 `ClusterView`（§7.1）构建。
- `runtime/` 角色装配（§4.5.5）：`ssu-mgr --role=manager`、`ssu-agent --role=agent` 启动入口。
- `query(SSU_QUERY_POOL)` 实现（§3.3）：返回纳管的 SSU 列表。

**任务清单**
1. 实现 mock plugin `discover`：扫 `null_blk` → 生成 `ssu_resource_info_t[]`（含 mock ssu_id/host/容量）。
2. 实现 Controller `ClusterView`（RCU）+ 纳管流程（写锁更新、标记 ONLINE）。
3. 实现用户态主循环（epoll + udev + timerfd，§6.1.1）。
4. 实现 `ssu_resource_query(POOL)` + `health_check`（mock：读 `/sys/block/nullb*/size`）。
5. `runtime/` 解析 `ssu.conf` 的 `role`，装配子系统。

**独立验收（`tests/mvp1/check.sh`）**
```bash
sudo modprobe null_blk nr_devices=2
sudo ./build/src/user/runtime/ssu-mgr --role=manager --config tests/mvp1/ssu.conf &
sleep 2
ubsectl query --type pool          # 应见 2 个 ONLINE mock SSU
# 拔掉一个 null_blk，health_check 后视图标 DEGRADED/OFFLINE
```

**依赖**：`null_blk`（内核自带）。✅ 可独立验证。

---

### MVP-2 控制面（alloc/mount/unmount/release 全生命周期）

**目标**：5 操作端到端，namespace 与逻辑设备生命周期正确；能力门控生效。

**桩化策略**
- `NVMeCreateNS`/`DeleteNS`（§6.2.2）在 mock 后端等价于：在 mock SSU（`null_blk`）上用一个元数据表登记"ns_id↔区间"，不真的建 NVMe namespace（`null_blk` 无 ns 概念）。**真实 CreateNS 逻辑放到真实厂商 plugin**，接口不变。
- 挂载 = 经 `ioctl(SSU_IOC_LOGDEV_CREATE)` 建 ReqShim 逻辑设备 + `ioctl(SSU_IOC_MAP_ADD)` 把逻辑区间映射到 mock `/dev/nullb*`（ReqShim 不关心底层是真 NVMe 还是 null_blk，它只收映射）。

**交付物**
- ReqShim：`reqshim_blk.c`（逻辑 gendisk 生命周期：`blk_mq_alloc_tag_set`/`add_disk`/`del_gendisk`，§6.1.2a）+ `reqshim_map.c`（映射表 RCU 增删查）+ `reqshim_ioctl.c`（`LOGDEV_CREATE/MAP_ADD/MAP_DEL/MAP_QUERY/LOGDEV_DESTROY`，§6.1.2e）。
- mock plugin：`create_ns`/`delete_ns`/`mount`/`unmount`（基于 mock ns 元数据表 + ReqShim ioctl）。
- Controller：`alloc`（Saga，逐组件 SSU 建 ns，§4.2.3）、`mount`/`unmount`（§4.2.4）、`release`（先擦后删，§4.2.5）、`query(ALLOCATION/LOGDEV)`。
- API 层：`alloc`/`mount`/`unmount`/`release` 全部接通，RPC 路由链（§3.2）MVP 可先用本地直连（Agent+Manager 同进程）跑通，RPC 框架后续接。

**任务清单**
1. ReqShim 映射表（区间树 + RCU）+ ioctl 五命令 + 逻辑 gendisk 生命周期。
2. mock plugin `create_ns/delete_ns`（mock 元数据表，记录 ns_id↔设备↔区间）。
3. Controller `alloc` Saga + 回滚；`mount`（调 plugin.mount → ReqShim ioctl）；`unmount`/`release`。
4. `query(ALLOCATION)`（extent 明细）、`query(LOGDEV)`（ReqShim 映射明细）。
5. 两步释放语义测试（unmount 后 ns 仍在可再 mount；release 擦删）。

**独立验收（`tests/mvp2/check.sh`）**
```bash
# alloc（mock STRIPE 直通，建 ns）→ mount（建 /dev/ssuN + 映射）→ 写数据 → unmount → release
ubsectl alloc --size 1G --stripe --share exclusive --out aid
sudo ubsectl mount --aid $aid --host local --dev /dev/ssu0
test -b /dev/ssu0
ubsectl query --type logdev | grep ssu0          # 见映射
# 能力门控
ubsectl alloc --size 1G --replica 3   # → SSU_ERR_UNSUPPORTED
```

**依赖**：MVP-1 的 mock 池 + ReqShim `.ko`。✅ 可独立验证（写数据在 MVP-3 验证正确性，此处只验生命周期）。

---

### MVP-3 数据面（普通读写：命令转换 + SGL + 下发 + 回写）

**目标**：UBS IO 替身对 `/dev/ssuN` 读写，数据经 ReqShim 命令转换、下发到 mock 物理设备后读回一致。

**桩化策略**
- 数据面 mock 物理设备用 `null_blk`（真块设备，能存数据）或 `loop` 挂文件。ReqShim 把 `/dev/ssuN` 的 I/O 按映射转换后，经 mock 物理设备的请求队列下发——因为是真块设备，数据真落盘，可读回校验。
- **无需真实 URMA/LBC TGT**：mock 场景 ReqShim 直接经标准块 I/O（bio 重映射）下发到 `null_blk`，等价于"LBC TGT 处理 + 回写"由 `null_blk` 完成。真实 URMA 路径（§4.6.5）在接真实 LBC INI 时再启用，**ReqShim 数据面用一层抽象（phys I/O 后端）隔开 mock 与真实**。

**交付物**
- ReqShim `reqshim_cmd.c`（命令转换核心：查映射→拆分/重映射，§4.6.5 步骤①②⑤）+ `reqshim_phys.c`（数据面后端抽象：mock 走 `submit_bio` 重映射到 `null_blk`，真实走 NVMe 命令队列，§6.1.2b）。
- `queue_rq` 实现：取请求 → SGL（`sg_init_table`/`dma_map_sg`，§6.1.2c）→ 转换 → 后端下发 → 完成回调。
- Scheduler 最简策略（§4.3）：MVP 单 SSU/首个可用，不做评分。
- `ssu_smoke`（UBS IO 替身，`tests/stubs/ubs_io/`）：`open("/dev/ssuN")` → 写已知 pattern → 读回比对。

**任务清单**
1. ReqShim 数据面后端抽象层（`reqshim_phys`：mock 路径 = bio 重映射到目标 bdev；留真实 NVMe 命令路径接口）。
2. `queue_rq` + SGL/DMA + 完成路径（§6.1.2b/c）。
3. Scheduler `select_plan` MVP 版（过滤：在线；选片：首个可用，无评分）。
4. `ssu_smoke` 写读比对程序 + `fio`/`dd` 兼容性验证。

**独立验收（`tests/mvp3/check.sh`）**
```bash
# 沿用 MVP-2 建 /dev/ssu0 映射到 null_blk
sudo dd if=/dev/urandom of=tmp bs=1M count=64
sudo dd if=tmp of=/dev/ssu0 bs=1M count=64 oflag=direct
sudo dd if=/dev/ssu0 of=out bs=1M count=64 iflag=direct
cmp tmp out            # 数据一致
sudo ./build/tests/stubs/ubs_io/ssu_smoke /dev/ssu0   # pattern 读写校验
```

**依赖**：MVP-2 控制面 + `null_blk`。✅ 可独立验证（这是"脱离 UBSE"的关键证明：标准块 I/O 即可验数据面）。

---

### MVP-4 端到端（程序化 libubse + UBS IO 替身全链路）

**目标**：UBS IO 替身程序化调 `libubse` SDK 跑通 alloc→mount→读写→unmount/release 全链路，证明 SDK 可用且全链路闭环。

**交付物**
- `libubse` SDK 5 操作稳定 ABI（与 `libssu_api.so` 同库或封装层），extent 缓冲区协议（§3.3 `ssu_resource_alloc` 的 `out_extents`/`inout_extent_count` + `SSU_ERR_BUFFER_TOO_SMALL` 重试）。
- `ssu_smoke` 升级：完全程序化——调 `ssu_resource_alloc`/`mount` → 对返回的 `/dev/ssuN` 做块 I/O → `unmount`/`release`，全程不手动 `ubsectl`。
- RPC 路由链（§3.2）：Agent→Agent Controller→Master Controller。MVP 单节点可同进程，但 RPC 抽象层先就位（Unix socket + protobuf）便于多节点扩展。

**任务清单**
1. `libubse` 封装 + extent 缓冲区协议测试（小 buf → `BUFFER_TOO_SMALL` → 重试）。
2. `ssu_smoke` 全程序化（SDK 驱动，非 CLI）。
3. RPC 抽象层（`proto/` 定义 + 单节点 loopback 实现）。
4. 端到端用例 1（验收清单，§12.1）。

**独立验收（`tests/mvp4/check.sh`）**
```bash
sudo ./build/tests/stubs/ubs_io/ssu_smoke \
    --alloc --size 1G --stripe \
    --mount --dev /dev/ssu0 \
    --io --pattern verify \
    --unmount --release
echo $?   # 0 = 全链路通过
```

**依赖**：MVP-3 数据面 + SDK。✅ 可独立验证。

---

### MVP-5 扩缩容（独立验收：共享 namespace 的节点路径增删）

**目标**：在业务系统保证无并发写冲突的前提下，新节点 mount 可读共享设备，缩节点 unmount 不影响其他节点、数据保留。不承担多主写一致性/fencing。

**桩化策略**
- 多节点用**两台开发机**（或两 QEMU）跑两个 `ssu-agent`，共享同一 mock SSU（同一 `null_blk`/文件后端，经 NFS/共享盘模拟"集群共享 namespace"）。
- 扩缩容本质是 mount/unmount 复用（§4.2.7）：alloc 一次（共享 namespace）→ 节点 A mount 写 → 节点 B mount 读一致 → B unmount → A 不受影响。

**交付物**
- Controller 扩缩容逻辑（§4.2.7）：`mount` 复用已存在的 allocate_id 的 namespace（共享读）；`unmount` 只拆本节点路径。
- 多节点 RPC（Agent↔Manager）真实跨机版（非 loopback）。

**任务清单**
1. Controller 共享 mount 路径（校验 namespace 已存在则复用）。
2. 跨机 RPC（两个 agent + 一个 manager）。
3. 扩缩容独立验收脚本（§12.1 MVP-5 用例）。

**独立验收（`tests/mvp5/check.sh`，需两节点）**
```bash
# 节点 A: alloc + mount + 写
ssh nodeA 'ubsectl alloc --size 1G --share shared --out aid; ubsectl mount --aid $aid --dev /dev/ssu0; dd ...'
# 节点 B: mount 读一致
ssh nodeB 'ubsectl mount --aid $aid --dev /dev/ssu0; cmp ...'   # 一致
ssh nodeB 'ubsectl unmount /dev/ssu0'                          # 缩容
ssh nodeA 'dd /dev/ssu0 ...'                                    # A 不受影响
```

**依赖**：MVP-4 + 两节点 + 共享后端。✅ 可独立验证（声明"业务系统保证无并发写"为前提）。

---

## 2. 测试基础设施（支撑"独立验证"）

| 设施 | 作用 | 位置 |
| ---- | ---- | ---- |
| `tests/stubs/ubs_io/ssu_smoke` | UBS IO 替身：SDK 驱动 + 块 I/O pattern 校验 | `tests/stubs/ubs_io/` |
| `tests/stubs/lbc_ini/` | mock LBC INI adapter（null_blk 后端）+ mock ns 元数据 | `tests/stubs/lbc_ini/` |
| `null_blk`/`loop` | mock SSU 物理设备（内核自带） | 内核 |
| `tests/mvpN/check.sh` | 每阶段独立验收脚本 | `tests/mvpN/` |
| QEMU 镜像 | 跑 `.ko` + 多节点（CI 无内核调试权限时） | `tests/vm/` |

**ReqShim 数据面后端抽象（关键去耦点）**：`reqshim_phys` 层定义"下发到物理设备"接口，mock 实现走标准 `submit_bio` 重映射到 `null_blk`，真实实现走 NVMe 命令队列→LBC INI→LBC TGT→URMA。这样 MVP-3 用 mock 跑通数据面，联调时只换该层实现，上层命令转换逻辑零改动。

---

## 3. 依赖与风险

| 项 | 说明 |
| ---- | ---- |
| **接口冻结风险** | 三套契约（API/plugin ops/UAPI）MVP-0 必须定稿。对策：MVP-0 先出契约文档 + 测试，评审通过再进 MVP-1。 |
| **null_blk 数据面差异** | null_blk 是标准块设备，无 NVMe 命令语义；ReqShim 在 mock 下走 bio 重映射，与真实 NVMe 命令路径不同。对策：`reqshim_phys` 抽象层隔离，命令转换逻辑（§4.6.5 ①②⑤）在两层共用。 |
| **共享后端模拟** | MVP-5 多节点共享 namespace 用共享盘/NFS 模拟，与真实集群共享语义可能有差。对策：MVP-5 定位为"访问路径增删"验证，不验分布式一致性（设计已声明由业务系统保证）。 |
| **无上游 daemon** | MVP 自跑 `ssu-mgr/agent`，未接 ubse daemon。对策：`runtime/` 角色装配独立，M10 接 daemon 时只改装载方式。 |

---

## 4. 里程碑时间线（建议）

| 阶段 | 周期 | 出口标准 |
| ---- | ---- | ---- |
| MVP-0 | 1.5 周 | 三套契约冻结 + .ko 加载 + 契约测试绿 |
| MVP-1 | 1 周 | mock 池纳管 + query(POOL) |
| MVP-2 | 2 周 | 控制面 5 操作 + 能力门控 + 两步释放 |
| MVP-3 | 2 周 | 数据面 mock 跑通 + ssu_smoke 读写一致 |
| MVP-4 | 1.5 周 | SDK 程序化全链路 |
| MVP-5 | 1 周 | 两节点扩缩容 |
| **MVP 联调** | 待硬件 | 换真实 LBC INI + UBS IO 跑同一套验收脚本 |

> MVP-0~4 是核心闭环（约 8 周），完成后即可用 mock 全套验收脚本证明设计与接口成立；真实联调时把 mock 桩替换为真实依赖，跑同一脚本。MVP-5 独立验收，不阻塞核心闭环。
