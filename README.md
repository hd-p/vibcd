# Baby Monitor — RV1106

婴儿监控设备，基于 Luckfox RV1106 SDK。视频采集、RTSP 推流、硬件移动检测、
音频采集、婴儿哭声检测、软硬件看门狗。

## 架构

```
┌─ 进程 baby_monitor（supervisor）──────────────────────────┐
│  创建共享内存 · fork/回收子进程 · 喂 /dev/watchdog          │
└───┬──────────────────────────────┬────────────────────────┘
    │                              │
┌───▼── baby_media ──────────┐  ┌──▼── baby_audio ─────────┐
│                            │  │                          │
│  VI 1080p30                │  │  AI 16kHz                │
│   └─bind─> VPSS            │  │   └─bind─> AENC (G.711A) │
│        ├─ch0─> VENC ─> RTSP│  │   └─────-> BCD 哭声检测   │
│        └─ch1─> IVS  ─> 运动 │  │                          │
│                            │  │                          │
│  像素全程不进 CPU            │  │  20ms/帧 = 640 字节       │
└────────────────────────────┘  └──────────────────────────┘
             │                              │
             └──── 共享内存（432 字节）───────┘
                   运动矩形 · 哭声结论 · 心跳
```

核心思路：**视频链在一个进程内用 `RK_MPI_SYS_Bind` 全硬件直连，
跨进程只传结论，不传像素。**

## 为什么视频链不拆进程

绑定后的 channel handle 属于创建它的进程。把 VENC 或 IVS 拆出去，
像素就必须回到共享内存，也就回到每帧多次 memcpy 的老路。

单核上拆进程也换不到并行收益——进程本来就是时间片轮转，
拆开只增加上下文切换和拷贝。所以隔离只用在真正容易崩的地方：
哭声检测的 NN 推理放在独立进程，它挂掉不影响视频推流。

| | 旧设计 | 现在 |
|---|---|---|
| 每帧 memcpy | 6 次 × 2.97 MB | 0 |
| memcpy 带宽 @30fps | ~535 MB/s | 0 |
| 共享内存 | 11.87 MB | 432 字节 |
| 移动检测 | CPU 帧差循环 | IVS 硬件 |
| 进程栈上帧缓冲 | 11.9 MB | 0 |

移动检测的 CPU 循环换成了 IVS 硬件模块，直接输出带坐标的运动矩形。

## 硬件前提

板子实测（`free -m` / `cat /proc/cmdline`）：

```
总内存   170 MB
CMA 池    66 MB   (rk_dma_heap_cma=66M，DMA buffer 从这里出)
栈上限     8 MB   (ulimit -s 8192)
```

DMA buffer 走 CMA，不占那 170 MB。这是 1080p30 能跑的前提。

## 编译

```bash
make                                   # 默认 SDK 路径
make SDK_PATH=/path/to/rv1106_SDK_luckfox
make clean
```

工具链和头文件都从 SDK 里取，包含路径是 `$(SDK_PATH)/media/out`。

## 部署运行

```bash
make
adb push bin/baby_monitor /root/
adb shell chmod +x /root/baby_monitor
adb shell /root/baby_monitor
```

播放：

```bash
ffplay rtsp://<板子IP>:554/live/0
```

## 参数

```
视频
  -w, --width <px>       sensor 宽（默认 1920）
  -h, --height <px>      sensor 高（默认 1080）
  -b, --bitrate <kbps>   码率（默认 2048）
  -e, --encoder <codec>  h264 | h265（默认 h264）
  -p, --port <n>         RTSP 端口（默认 554）
      --stream <WxH>     编码分辨率（默认等于 sensor）

移动检测
  -s, --sensitivity <n>  1 低 / 2 中 / 3 高（默认 2）
      --detect <WxH>     IVS 分析分辨率（默认 640x360）
      --motion-area <n>  运动面积阈值，千分比（默认 20）

音频
  -r, --rate <hz>        采样率（默认 16000）
      --cry-model <path> BCD 模型路径

看门狗
      --watchdog <path>  设备节点，传空字符串禁用
      --max-restarts <n> 每个 worker 的重启上限（默认 5）
```

调试时建议禁用硬件看门狗，否则断点会让板子重启：

```bash
/root/baby_monitor --watchdog ''
```

## 哭声检测用的是 SDK 自带能力

不需要自己训模型。SDK 提供了 BCD（Baby Cry Detection）：

```
RK_MPI_AI_SetBcdAttr / RK_MPI_AI_EnableBcd / RK_MPI_AI_GetBcdResult
结果字段：AI_BCD_RESULT_S::bBabyCry
模型文件：/oem/usr/share/vqefiles/rkaudio_model_sed_bcd.rknn
```

模型缺失时哭声检测会关闭，但音频采集和视频链照常运行。

## 看门狗分两层

**软件层**：supervisor 用 `waitpid` 回收子进程，并检查心跳计数器。
这能同时覆盖两种故障——进程崩了（进程不存在），以及进程活着但主循环卡死
（心跳不再增长）。

注意不能用 `kill(pid, 0)` 判断存活：子进程退出但未回收时是僵尸，
给僵尸发信号是成功的，于是死进程会被判定为健康。

**硬件层**：`/dev/watchdog` 负责 supervisor 自己死掉或卡死的情况。

关键顺序：**先做完软件检查，再喂狗**。如果 supervisor 主循环卡住，
喂狗随之停止，板子重启。无条件喂狗会让一个卡死的系统一直"活着"。

关闭时先写magic字符 `'V'` 再 close，否则正常退出几秒后板子会重启。

## 跨进程锁用 robust mutex，不用 seqlock

单核上无锁自旋是负担而非优势：任意时刻只有一个进程在跑，
reader 自旋时 writer 根本没在执行，纯粹烧自己的时间片。
阻塞会把核让给 writer，让它更早写完——**单核上阻塞严格优于自旋**。

`PTHREAD_MUTEX_ROBUST` 顺带解决持有者崩溃：
下一个加锁者拿到 `EOWNERDEAD`，调 `pthread_mutex_consistent()` 接管，
不会留下永久死锁。

## 目录结构

```
src/
├── base/
│   ├── rk_platform.h      所有 RK 头文件的统一入口
│   ├── shared_memory.h    POSIX 共享内存 RAII（Create/Attach 分离）
│   ├── shared_records.h   跨进程记录：运动矩形、哭声、心跳
│   ├── robust_mutex.*     进程间 robust mutex
│   └── child_process.*    子进程生命周期（只有父进程能回收）
├── media/
│   └── media_pipeline.*   VI → VPSS → {VENC→RTSP, IVS→运动}
├── audio/
│   └── audio_pipeline.*   AI → AENC，加 BCD 哭声检测
├── app/
│   ├── media_service.*    媒体进程入口
│   ├── audio_service.*    音频进程入口
│   └── supervisor.*       fork/回收/心跳/看门狗
└── main.cpp               参数解析
```

## 安全提醒

RTSP 服务没有任何认证和加密，任何能访问该端口的人都能看到画面和声音。
婴儿房的视频音频属于高度敏感数据，**只应放在可信内网，不要做端口转发**。
启动时日志里会有相应警告。

## 尚未验证的部分

以下都编译通过，但**没有在板子上实机跑过**（开发环境的容器拿不到 USB 设备节点，
`adb devices` 是空的）：

- `RK_MPI_SYS_Bind` 三条链路的实际数据流
- IVS 输出的运动矩形坐标和面积是否符合预期
- BCD 模型在目标 rootfs 里是否真的存在于该路径
- 硬件看门狗超时值被驱动 clamp 成多少
- 1080p30 下的实际 CPU 占用

上板第一步建议：

```bash
/root/baby_monitor --watchdog '' 2>&1 | tee /tmp/first-run.log
# 另一个终端
top -d 1
```
