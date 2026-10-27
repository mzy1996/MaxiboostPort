# GUNDAM EXVS MBON —— Windows 移植工程交接说明

> 本文档写给接手的 AI / 工程师。读完这一份 + `../.workbuddy/memory/MEMORY.md`，
> 就能接着干，不需要重读全部历史会话。
>
> 最后更新：2026-10-27

---

## 1. 这是什么

把 PS4 独占游戏《GUNDAM EXVS MBON》（CUSA15006）移植到 Windows x64。
做法是**在宿主上直接跑游戏的原生 x86-64 代码**：把 `eboot.bin` 和它依赖的系统模块
（`libc.prx` 等）链接成一个可执行映像，再由自研运行时 `bb-probe.exe` 提供 PS4 的
内核/系统 API，用 `bbgpu.dll` 提供图形后端。

- 代码仓库：`repo/`
- 游戏本体：`Game/App/CUSA15006`（不入库，见 `.gitignore`）
- 交付包：`repo/out/mbport-windows-*`，根目录还有 `mbport-windows-final.zip`

## 2. 当前状态：能跑，但还有一个崩溃没修完

**已达成（可交付的状态）**

- 游戏能稳定运行在 **57.8 FPS**，主流程（进入关卡、渲染、战斗）正常。
- 此前修掉的三个崩溃：镜像陈旧、Windows 故障处理器顺序错误、`0x1001` 之谜。

**未修完的崩溃（本轮主线）**

约 2 分钟后，线程 `CApplicationBase` 跳到 `rip=0`（NULL 函数指针调用），guest 栈几乎全零。

根因链已经查清到最后一环（见第 4 节），但**最后一步"谁把池清零了"还没定位完**。

## 3. 怎么构建、怎么跑

```bash
# 构建运行时（Windows，MinGW-w64）
BUILD_WINDOWS=1 bash tools/build_windows_all.sh
#   产物：repo/out/win/bb-probe.exe

# 重新链接镜像（改了 link_modules.py 的补丁后必须重跑！）
python3 scripts/link_modules.py <game目录> --out out/relink
#   产物：repo/out/relink/boot-linked.bin

# 打包交付
bash tools/package_windows.sh
```

跑测（在 `repo/out/runNNdir/` 这种准备好的运行目录里，目录里必须有 `bbgpu.dll` 等 DLL）：

```bash
cd out/run44dir
cp ../win/bb-probe.exe .
BB_R1_WATCH=1 timeout 150 ./bb-probe.exe ../boot-linked.bin \
  --content-profile ../content.bin --patches ../patches.bin \
  --app0 "J:/GUNDAM_EXVS_MBON_PC_PORT/Game/App/CUSA15006" --user ../user_fresh \
  > ../diag26.log 2>&1
```

**调试环境变量**（都在 `src/probe.c` / `src/runtime_*.c` 里读）

| 变量 | 作用 |
|---|---|
| `BB_R1_WATCH=1` | 每 20 ms 采样 R1 注册表状态（free_head / head.next / used / next_id），链断时打印节点内容。**本轮主线用它。** |
| `BB_MEMORY_TRACE=1` | 直接内存的分配/释放写进 `memory.txt`（`A <phys> <size>` / `R <phys> <size>`） |
| `BB_CPU_WATCH=<镜像偏移>[:<长度>]` | 把镜像某页设为只读，捕捉写它的那条指令（POSIX 路径） |
| `BB_FILE_TRACE=1` | 文件读写打印 guest 调用点 |
| `BB_FAULT_RECOVER=0` | 关掉访问违例自动恢复，让崩溃直接结束（便于取现场） |
| `BB_WAIT_TRACE=1` | 线程等待点 |

## 4. 最后一个崩溃：已经查清的部分

### 4.1 症状

```
Fault: code 0xc0000005 rip 0x0000000000000000 at 0x0000000000000000
  thread CApplicationBase
  [R1] free_head=0x800410040 next_id=0x105f used=0 limit=0x19000
  [R1] key 0: ABSENT (bucket 0)
```

`used=0`：注册表里**一个条目都没有**，但 `next_id` 已经发到 `0x105f`（发了 95 个 id）。
即"只发号不建节点"——每次查找都失败，链接器把符号解析成 NULL，游戏调用 NULL → `rip=0`。

### 4.2 R1 注册表的结构（**已从反汇编确认**，可直接引用）

注册表本体在**镜像里**（eboot.bin 的 .bss）：

| 地址 | 含义 |
|---|---|
| `0x160b600` | 控制块 / next_id 计数器（初值 `0x1000`） |
| `0x160b680` | pool 副本 |
| `0x160b688` | free-list head |
| `0x160b690` | limit（`0x19000`） |
| `0x160b694` | used 计数 |
| `0x160b700` | 桶数组（`0x19000` 项） |
| `0x160b698` | 哨兵节点 |
| `0x161d080` | pool 基址指针 |

节点布局：`{+0x00}=free-list next（空闲时）/+0x08=BST 链接, +0x10=key(dword), +0x18=value(qword)`
哈希：`imul rax,0x747ed91f; shr rax,0x2c; imul eax,0x2329; sub ecx,eax`
注册函数 `0xb69be0`（address→id）、查找 `0xb68550`（id→address）

### 4.3 初始化代码（镜像偏移 `0x685a80` 附近）

```
0x685ad5: mov dword [rip+…], 0x1000      ; next_id = 0x1000
0x685adf: mov edi, 0x320000               ; 3 MiB
0x685ae4: mov esi, 8                      ; 对齐 8
0x685ae9: call 0x661f50                   ; eboot 自己的 operator new
0x685af1: mov qword [rip+…], rax          ; pool = 结果
0x685b0f: mov qword [rip+…], rax          ; free_head = pool
0x685b20: mov qword [rax-0x20], rax       ; 串 free list（0x18fff 次）
```

**关键结论：pool 是 eboot 自己 `operator new` 出来的 3 MiB 堆块，落在 guest
`0x800410040`——也就是游戏自己那 1 GiB direct memory 区
（`0x800410000-0x840410000`，phys `0x4900000`）里面的头 64 字节之后。**

它**不是**镜像里的静态数组，所以运行时无法从映像侧直接看出它是谁的。

### 4.4 观测到的关键时间线（`out/diag26.log`，`BB_R1_WATCH=1`）

```
227: R1 free_head=0x800410040 head.next=0x800410060 used=0 next_id=0x1000
228:     head node 0x800410040: 0x800410060 0xa44b435544 0x20c32594c50 0x3c48545753
229: R1 chain head word = 0x800410060 (was 0)
     ... 30 次 File trace: pread -> 131072 bytes（loader 线程在读文件）
256: R1 free_head=0x800410040 head.next=0 used=0 next_id=0x1000
257:     head node 0x800410040: 0 0 0 0
259: R1 chain head word = 0 (was 0x800410060)
```

链断的**确切时刻**：free-list 的 head 节点第一个字从 `0x800410060` 变成 `0`，
节点 4 个字全变 0（即整段被清零，不是单字被改）。
时间点落在 `loader` 线程创建之后、一串 `pread`（读模块文件）期间。

### 4.5 剩下的最后一步

需要区分两种可能：

- **(A) 运行时回收了它** —— `runtime_memory.c` 的 `direct_release` / `flex_free`
  会 `platform_section_punch_hole`（MEM_RESET），读回来就是 0。但 `zero_phys()`
  当前**没有调用点**（`grep zero_phys` 只有定义），而 `direct_release` 走的是
  `unmap_locked` → `platform_unmap`，也不是清零。所以运行时这条路嫌疑较小。
- **(B) 游戏自己的堆管理器回收了它** —— pool 在游戏那 1 GiB direct 区里，
  游戏自己的分配器把它当普通块 free 掉了（很多引擎 free 时会 memset 归零）。

**下一步建议的具体做法**（按性价比排序）：

1. **在运行时加 VMA 事件环形缓冲**：每次 `direct_release` / `place` / `drop_range`
   改动到覆盖 pool 地址（guest `0x800410040`，phys `0x4900040`）的区域时，
   记一条（操作类型 + 物理区间 + `runtime_guest_call_sites()` 拿到的 guest 调用点）。
   跑一轮 `BB_R1_WATCH=1`，看链断的那一刻有没有对应事件。
   - 有事件 → 路径 (A)，是运行时的问题，改 `runtime_memory.c`。
   - 无事件 → 路径 (B)，是游戏堆管理器回收的，要在链接补丁层面解决
     （例如让 eboot 的 `operator new` 给这个池一块不会回收的内存）。
2. **备选：给 pool 首字设硬件断点**（`cpu_watch` 机制现成，但目前只支持镜像内偏移，
   pool 在 direct memory 里，需要扩展成支持任意宿主地址）。
3. **备选：直接打补丁绕开** —— 在链接时让 R1 的初始化改用一块独立的、
   永不被游戏回收的内存（例如改 `operator new` 的调用点，或让 pool 从
   runtime 的 direct 池单独分配）。这是"治标但能出货"的路线。

## 5. 本轮新增的诊断代码（都在未提交的改动里）

| 位置 | 内容 |
|---|---|
| `src/probe.c` | `dump_registry_r1()` 打印注册表全貌；`dump_guest_frames()` 沿 rbp 链回溯 guest 调用者；导入槽诊断（`import_slots`，崩溃时报告哪个符号的槽是空的）；`BB_R1_WATCH` 监视线程（在 `watchdog_thread` 里，**不是**主线程） |
| `src/runtime.h` / `src/runtime_memory.c` | `runtime_memory_describe()` / `runtime_memory_restore_access()` |
| `tools/disasm.py` | 反汇编镜像某段（新） |
| `tools/findref.py` | 扫描镜像里对某偏移的 RIP-relative 引用（新） |
| `tools/make_selfheal.py` `make_selfheal2.py` | 生成自愈查找补丁的机器码（新） |

**⚠️ 交接注意**：`src/probe.c` 里的诊断代码是**临时的**，定位完根因后应当清理或
收敛成稳定开关。`BB_R1_WATCH` 的采样循环**必须留在 watchdog 线程里**——
早前误放进主线程会卡死游戏（进程活着、CPU 不涨、日志停住）。

## 6. 仓库状态

```
 M scripts/link_modules.py     # 新增 _PATCH_SELFHEAL / _PATCH_SELFHEAL2 等补丁组
 M src/probe.c                 # 本轮诊断代码（见上）
 M src/runtime.h               # 两个新接口声明
 M src/runtime_memory.c        # describe / restore_access 实现
 M tools/build_windows_runtime.sh
 M tools/package_windows.sh
?? tools/disasm.py             # 新工具
?? tools/findref.py            # 新工具
?? tools/make_selfheal.py      # 新工具
?? tools/make_selfheal2.py     # 新工具
```

改动**尚未提交**。`out/` 整个被 `.gitignore` 忽略（日志、镜像、构建产物都不入库）。
接手后建议先 `git add -A && git commit`，把这一轮的诊断代码固化下来。

## 7. 必须知道的坑（都在 `MEMORY.md` 里，这里再强调最要命的几条）

1. **运行目录的 exe 必须叫 `bb-probe.exe`**：`bbgpu.dll` 的 import 表按模块名解析符号，
   改名 → 加载器解析不到 → 调用时执行 import name 的 RVA → 立刻崩（`rip=0x346ee6`）。
   这条曾把排查带偏很久。
2. **每跑一轮新测试前，先结束上一轮**：`TaskStop` 掉后台任务 + `Stop-Process` 清掉
   残留 `bb-probe`（卡在 GPU 驱动的用 `taskkill /F /PID`）。残留进程占着 exe 文件，
   `cp` 会报 "Device or resource busy"，还会抢 GPU，让新一轮看起来像"改坏了"。
   （已退出的僵尸进程不占文件，`Get-Process` 仍会列出，`HasExited=True`。）
3. **改完 `link_modules.py` 的补丁必须重跑链接**，否则镜像里还是旧补丁。
   （曾因此白查一轮：`_PATCH_WALKER` 从没被打进去，只有 `_PATCH_DRET` 在。）
4. **guest 地址要翻译**：客人的地址空间是 identity 映射（guest VA == host VA），
   但**镜像**被放在宿主随机地址上。读镜像里的东西（如 `0x160b600` 一带）必须用
   `image + va`，直接传 guest VA 会读到没映射的地方（表现为 UNREADABLE）。
5. **Windows 的 VEH 与 POSIX signal 顺序不同**：Windows 要先恢复可恢复故障再打印报告；
   这条修过一次。

## 8. 记忆与日志

- `../.workbuddy/memory/MEMORY.md` —— 长期笔记（结构、坑、已知遗留）。**接手先读这个。**
- `../.workbuddy/memory/2026-10-*.md` —— 逐日工作日志，按时间线记了每一次排查。
- 本轮（10-27）的日志末尾有 R1 注册表的完整反汇编结论。

## 9. 尚未解决的其他遗留

1. **启动竞态**：libc 的 4 MiB 堆与游戏的 1 MiB 池块抢同一个 6 GiB direct memory 池，
   libc 堆抢输 → `direct 4 MiB blocks: asked=1, failed=1` → libc 初始化直接终局（偶发）。
   候选修法：预留 libc 堆、扩池、或宿主兜底分配。
2. **import 解析**：游戏自己的链接器把模块条目里的符号解析成地址；解析不到的会变成
   NULL 函数指针（`rip=0`）。已用"自愈查找"补丁（`_PATCH_SELFHEAL` /
   `_PATCH_SELFHEAL2`）把两处查找改成"查不到就现注册再查"。
   **注意**：自愈依赖 `0xb69be0` / `0xb68550`，而这两个依赖 R1 注册表可用——
   在当前 pool 被清零的状态下，自愈**实际不生效**。修好 pool 之后自愈才会起作用，
   需要重新验证。
