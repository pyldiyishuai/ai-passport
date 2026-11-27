# AI Passport 固件（个人开发仓）

FoloToy AI Passport 的可穿戴设备固件，基于官方公开代码（MIT）继续开发，**提交历史已重写为单一初始提交**，本仓只做自己的玩法。

当前内容：**Focus Timer**（三键专注计时器）——待机切档位、OK 开始/暂停/继续、长按重置、NVS 记住档位与完成轮数、完成提示音。

## 硬件要点

| 项 | 值 |
|---|---|
| SoC | ESP32-C3，8 MB Flash，**无 PSRAM** |
| 屏幕 | ST7789P3 240×320 竖屏 RGB565，SPI，无 MISO / 无 TE / 无触摸 |
| 按键 | UP / DOWN / OK，ADC 电阻梯挂在 GPIO0，**不是**三个独立 GPIO |
| 音频 | ES8311（I2S0 全双工）+ 喇叭 |
| 电量 | CW2017 电量计，与 ES8311 **共用 I2C0** |
| 控制台 | 原生 USB Serial/JTAG（GPIO18/19），不是 UART0 |
| BLE | 仅 NimBLE 广播，C3 无 Bluetooth Classic |
| 外壳宣称 NFC | BSP 未暴露任何 NFC API，别指望 |

## 环境

- ESP-IDF **v5.5.3**（官方基线要求的版本），本机在 `D:\aipassport\esp\esp-idf`
- Node.js ≥ 20（模拟器用）

## 一条命令构建

```powershell
.\tools\dev.ps1 -Task all                                    # 编译 + 合并镜像
.\tools\dev.ps1 -Task shot                                   # 编译 + 合并 + 模拟器无头截图
.\tools\dev.ps1 -Task flash -Port COM5                        # 编译 + 合并 + 烧录
.\tools\dev.ps1 -Task monitor                                 # 串口看日志（Ctrl+] 退出）
.\tools\dev.ps1 -Task all -Clean                              # 全量重编
```

产物：

```
build\FoloToy-AI-Passport.bin          app 镜像（0x10000）
build\FoloToy-AI-Passport-full.bin     合并镜像，从 0x0 刷（851,872 B，首字节 0xE9）
```

> `idf.py merge-bin` 是在 `build\` 目录里执行的，`-o` **不要**带 `build\` 前缀，否则会去找 `build\build\...` 直接失败。
> 用分段 `idf.py flash` 会保留 NVS（档位、轮数），整片刷合并镜像会重置 NVS。

## 仓库自带校验

```powershell
# 需要先激活 ESP-IDF 环境（export.ps1），并用 IDF 自带 python
$py = "$env:USERPROFILE\.espressif\python_env\idf5.5_py3.9_env\Scripts\python.exe"
& $py tools\check_repo.py                  # 静态检查 → PASS
& $py tools\check_glyphs.py                # 字形覆盖 → 没字体时 SKIPPED
& $py tools\verify_firmware.py build       # 镜像布局 + 合并镜像 → PASS
& $py tools\archive_firmware.py create build --archive-root build\firmware
```

`tools/validate.sh` 需要 bash，但 ESP-IDF 5.5 会拒绝 MSYS/Mingw 环境（`idf.py` 直接退出 49），所以 Windows 上直接调上面几个 python 脚本，效果等价。

## 模拟器（改 UI 不用烧板子）

```powershell
cd D:\aipassport\simulator
npm start                 # http://127.0.0.1:4190
```

- 键盘：`↑` `↓` 切档，`Enter` = OK，`P` 电源
- 把 `build\FoloToy-AI-Passport-full.bin` 拖进页面即可运行自己的固件
- 不模拟：CW2017 电量（UI 显示 `--`）、BLE 控制器、真实时序与功耗
- `tools\emulator-shot.mjs` 用 Chrome CDP 自动加载固件、按指定键、截图：

```powershell
node tools\emulator-shot.mjs build\FoloToy-AI-Passport-full.bin shot.png "ok,up" 9000
```

## 目录

```
main/            app_main.c（UI/按键/节拍/NVS/提示音）、focus_logic.{h,c}（纯逻辑状态机）
components/bsp/  官方板级支持包，未改动
tests/           test_focus_logic.c 主机测试（需主机 C 编译器）
tools/           dev.ps1、emulator-shot.mjs、verify_firmware.py 等
assets/ docs/    官方资源与文档
```

## 上报 bug 前请自查

1. `main/app_main.c` 里所有 LVGL 调用是否都在 `bsp_lvgl_lock()` 保护下（LVGL 非线程安全）
2. 按键回调是否只入队、不阻塞（回调跑在共享 esp_timer 任务上）
3. UI 文本是否全 ASCII（默认 Montserrat 无中文字形）
4. 电量读取失败是否优雅降级为 `--`

## 许可证与署名

`LICENSE` 为 MIT，来自上游 FoloToy 公开代码。本仓保留该许可证；上游原始项目：<https://github.com/FoloToy/ai-passport>。

上游更新可用 `git fetch upstream` 后 `git diff upstream/main` 手工挑补丁（历史已重写，无法直接 merge）。
