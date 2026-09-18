# Baby Monitor — RV1106

婴儿监控，跑在 Luckfox RV1106 上。

## 功能

- 1080p30 H.264/H.265 视频，RTSP 推流
- 硬件移动检测，运动区域在画面上用青色框标出
- 婴儿哭声检测，检测到时画面左上角出现红色方块
- G.711A 音频采集
- 软硬件双层看门狗，子进程崩溃自动重启

## 编译

```bash
make                                   # 默认 SDK 路径
make SDK_PATH=/path/to/rv1106_SDK_luckfox
make clean
```

## 部署

```bash
make && ./deploy.sh        # 上传到 root@192.168.31.195:/userdata/baby_monitor
```

## 运行

板上：

```bash
killall rkipc                                  # 出厂 rkipc 占着 ISP 和 554 端口
export LD_LIBRARY_PATH=/oem/usr/lib:/oem/lib
/userdata/baby_monitor > /userdata/run.log 2>&1 &
```

播放：

```bash
ffplay rtsp://192.168.31.195:554/live/0
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
      --iqfiles <dir>    ISP 调优文件目录（默认 /etc/iqfiles）

移动检测
  -s, --sensitivity <n>  1 低 / 2 中 / 3 高（默认 2）
      --detect <WxH>     分析分辨率（默认 640x360）
      --motion-area <n>  运动面积阈值，千分比（默认 20）
      --no-motion        关闭移动检测
      --no-overlay       不在画面上画框和哭声提示

音频
  -r, --rate <hz>        采样率（默认 16000）
      --audio-card <hw>  采集声卡 hw:<card>,<device>（默认 hw:0,0）
      --cry-model <path> 哭声模型路径

看门狗
      --watchdog <path>  设备节点，传空字符串禁用
      --max-restarts <n> 每个子进程的重启上限（默认 5）

调试
      --no-fork <svc>    media 或 audio，单进程运行，不 fork、不重启、不喂狗
```

## 安全提醒

RTSP 没有认证和加密，任何能访问该端口的人都能看到画面和声音。
**只应放在可信内网，不要做端口转发。**
