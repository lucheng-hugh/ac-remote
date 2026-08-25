# ESP32-S3 格力空调遥控器

For English, see [README-EN.md](README-EN.md).

ESP32-S3 格力空调智能遥控器，支持：

- 手机本地网页遥控
- JSON API 遥控
- 每周重复定时任务（保存到 NVS）
- 接收 YAP0F21 原装遥控器后同步网页状态
- 睡眠模式关闭/1/2/3/4（网页、API、定时任务及遥控同步）
- 原生 HomeKit / Siri 控制与双向状态同步
- ESP32 主动探测 iPhone，在离家后自动关闭、回家后自动开启
- 所有控制入口的设定温度范围统一为 16–28℃
- 首次启动通过设备热点录入 Wi-Fi，不把密码写入源码

## 接线

- 接收器：OUT -> GPIO4，VCC -> 3V3，GND -> GND
- 发射器：DAT/OUT -> GPIO5，VCC -> 5Vin，GND -> GND

## 本地命令

```sh
.venv/bin/pio run
.venv/bin/pio run --target upload
.venv/bin/pio device monitor
```

## 首次配网

1. 连接 Wi-Fi `AC-Remote-Setup`，密码 `acremote123`。
2. 浏览器打开 `http://192.168.4.1`，填写家里的 2.4GHz Wi-Fi。
3. 设备重启后访问 `http://ac-remote.local`，也可使用串口打印的局域网 IP。

## API

- `GET /api/state`：读取空调状态
- `POST /api/control`：发送空调状态
- `GET /api/schedules`：读取定时任务
- `POST /api/schedules`：新增或更新任务
- `DELETE /api/schedules?id=1`：删除任务
- `GET|POST /api/time`：读取或校准设备时间
- `GET|POST /api/homekit`：读取 HomeKit 状态或在设备网页中设置配对码
- `GET|POST /api/presence`：读取或设置 iPhone 在家检测

`POST /api/control` 使用 `"sleepMode": 0..4` 设置睡眠模式；旧的
`"sleep": true/false` 字段仍兼容，并对应睡眠 1/关闭。

## 自动离家关闭 / 回家开启

不需要 Apple TV、HomePod 或路由器脚本。最多可以配置 4 部手机，ESP32
每 15 秒依次主动探测它们的固定局域网 IP。任意一部手机连续两次探测成功
即判定有人回家；只有全部手机持续探测不到达到设定时间后才判定离家。默认
离家延迟为 10 分钟，用于避免手机锁屏或短暂掉线造成误关。

1. 在路由器的 DHCP/静态租约设置中，给 iPhone 绑定固定局域网 IP。
2. 分别用每部手机打开 `http://ac-remote.local`。
3. 在“自动离家 / 回家”中点击“添加当前手机”并保存，再换下一部手机操作。
4. 页面会实时显示预计关闭时间和剩余倒计时。第一次启用或 ESP32 重启后，
   检测到在家不会自动开机；如果全部手机离线，倒计时结束后仍会自动关机。

这种方式依赖 iPhone 响应局域网 ICMP 探测。不同路由器和 iOS 版本的省电
行为可能不同，建议先使用 10–15 分钟离家延迟观察。如仍有误判，读取路由器
的 Wi-Fi 客户端关联表会比 ESP32 主动探测更可靠。

## HomeKit / Siri

设备网页底部可自行设置 8 位 HomeKit 配对码。然后在 iPhone“家庭”中选择
“添加配件 → 更多选项 → 格力空调”。制冷/制热/自动、目标温度、风速、扫风、
除湿、送风和睡眠 1–4 均作为同一空调配件的服务，可控制且共享同一房间；
原装遥控器的状态会同步回家庭 App。

当前硬件没有室温传感器，因此 HomeKit 的“当前温度”暂时显示设定温度。
