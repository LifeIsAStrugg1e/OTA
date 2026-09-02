# 项目进度文档 —— 嵌入式网络监测终端

> 用途：记录项目从开始到现在的完整进度、关键决策、坑点和待办，供后续对话快速恢复上下文。
> 每次完成一个里程碑就更新本文件。

## 一、项目目标与背景

- 求职方向：嵌入式软件工程师（应届本科，2026 届，电子与计算机工程）。
- 起因：面试官说简历里项目偏硬件，所以要做一个明显偏**嵌入式软件**、能体现 RTOS、多任务、网络通信、驱动、调试能力的项目。
- 项目方向：**基于 STM32F429 + FreeRTOS + Ethernet + LwIP 的嵌入式网络监测与设备管理系统**。
- 最终用途：简历、面试讲解、面试官追问、Git 代码展示、软件架构展示。

## 二、硬件与开发环境

- 主控：正点原子**阿波罗 V2 STM32F429IGT6** 开发板（核心板 + 底板，LQFP176）。
- 网口 PHY：**YT8512C**（不是 LAN8742！），RMII，PHY 地址 0。
  - RMII 引脚：PA1(REF_CLK，50M 时钟由 PHY 的 TXC 给出)、PA2(MDIO)、PA7(CRS_DV)、PC1(MDC)、PC4(RXD0)、PC5(RXD1)、PB11(TX_EN)、PG13(TXD0)、PG14(TXD1)。
- PHY 复位：`ETH_RESET` 接在 **PCF8574（I2C IO 扩展）的 P7**；PCF8574 地址 0x20，SCL=PH4、SDA=PH5、INT=PB12。
- LED：PB0=绿灯(DS1)、PB1=红灯(DS0)，**低电平点亮**。
- 调试/下载：DAPLink（CMSIS-DAP Link）。
- 串口：USART1，115200 8N1；TX=PA9。
- 开发环境：STM32CubeMX 6.18.1 + STM32Cube FW_F4 V1.28.3 + Keil MDK V5.32（ARMCC V5.06）。
- Git 仓库：https://github.com/LifeIsAStrugg1e/OTA.git（分支 main）。

## 三、当前网络拓扑（测试用）

- 小米路由器 LAN 网段：192.168.31.x，网关 192.168.31.1。
- 电脑（WiFi）：192.168.31.121。
- 板子静态 IP：**192.168.31.20 / 255.255.255.0 / 网关 192.168.31.1**。
- 板子网线接小米路由器 LAN 口。
- TCP 端口：**5000**（命令通道）。
- UDP 端口：**6000**（数据上报），目标 `192.168.31.121`（电脑）。

## 四、已完成（里程碑）

### 配置层（CubeMX / .ioc）

- 时钟：HSE 25MHz → PLL(M25/N336/P2/Q7) → **168MHz**；APB1=/4=42M、APB2=/2=84M；Flash 5 等待周期，电压 Scale1。
- FreeRTOS：CMSIS-V2、heap_4、TOTAL_HEAP_SIZE=65536、抢占式、Tick 1000Hz、MAX_PRIORITIES 56。
- 任务：defaultTask(Normal,128字)、SensorTask(Normal,512字)、NetworkTask(AboveNormal,1024字)。
- 队列：`SensorDataQueue`，8 个 × 16 字节。
- LwIP 2.1.2：WITH_RTOS=1、DHCP 关、静态 IP。
- HAL 时基 = **TIM6**（SysTick 归 FreeRTOS）。
- SDIO 已关闭（避免没插卡开机卡死）。
- 工具链 MDK-ARM；Keil 勾选 **MicroLIB**。

### 功能里程碑（全部完成 ✅）

1. ✅ 168MHz 启动 + 串口日志（printf 重定向 `fputc`）+ FreeRTOS 调度正常。
2. ✅ 以太网 Link Up + 静态 IP + **ping 通**。
3. ✅ TCP 服务器（5000 端口）+ `LED_ON`/`LED_OFF` 命令点灯。
4. ✅ **数据流打通**：SensorTask（模拟数据 + 滑动平均滤波 + 阈值报警）→ SensorDataQueue → NetworkTask → TCP 上报 `T=xx.x H=xx.x A=xx.x TS=xx`（当前 5 秒一条）。
5. ✅ **命令系统**：函数指针命令表 + mutex 远程配置。命令：`VERSION`/`UPTIME`/`SENSOR`/`STATUS`/`SET_ALARM`/`LED_ON`/`LED_OFF`。
6. ✅ **UDP 数据上报**：TCP(5000) 管命令 + UDP(6000) 管数据，双通道架构。

## 五、关键代码位置

- `Core/Src/main.c`
  - `SensorData_t`（float temperature/humidity/air_quality + uint32_t timestamp，16 字节）。
  - `ftoa_1()`（MicroLIB 不支持 %f，自己转 "xx.x" 文本）。
  - `fputc()`（printf 串口重定向）。
  - `StartDefaultTask()`：`MX_LWIP_Init()` 后置 `lwip_ready=1`。
  - `StartSensorTask()`：模拟采集 + 滑动平均滤波(窗口5) + 阈值(>26.5℃) + `osMessageQueuePut`。
  - `StartNetworkTask()`：TCP 服务器 + 非阻塞事件循环（accept/recv/队列上报）。
- `LWIP/Target/ethernetif.c`
  - 软件 I2C + `PCF8574_Write()` + `ETH_PHY_Reset()`（释放 PHY 复位）。
  - 通用 BMSR 链路检测（**替换了 CubeMX 生成的 LAN8742 代码**）。
  - `low_level_init()` 里 MAC 直接配 100M 全双工。
- `LWIP/App/lwip.c`：静态 IP（192.168.31.20）。
- `Core/Inc/FreeRTOSConfig.h`：`configENABLE_FPU=1`。

## 六、重要坑与决策（务必记住）

1. **MicroLIB**：printf/sprintf 不支持 `%f`；用标准库会 semihosting 卡死，所以用 MicroLIB，浮点打印用 `ftoa_1` 或整数表示。
2. **FPU**：Keil 是硬件浮点(FPU2)，FreeRTOS 必须 `configENABLE_FPU=1`，否则多任务浮点会出错。
3. **PHY 是 YT8512C 不是 LAN8742**：CubeMX 里选 LAN8742 会生成错误 PHY 驱动（读 LAN8742 特有寄存器），已改成通用 BMSR 读寄存器。
4. **PHY 复位**：必须 I2C 写 PCF8574 的 P7（先 0x7F 再 0xFF）释放，否则网口无链路。
5. **USART1 RX 引脚错误（待修）**：当前 RX 配成 PB7，但板子串口 RX 在 PA10；现在只发不收不影响，做命令接收/CLI 时要改回 PA10。
6. **任务初始化顺序**：NetworkTask 优先级高于 defaultTask，靠 `lwip_ready` 标志等 LwIP 初始化完成后再创建 socket。
7. **软件 I2C 用 HAL_Delay(1)** 做位延时，只在启动时跑一次，慢一点没关系。

## 七、待完成（路线图）

1. 【下一步】网络增强：心跳、断线重连、网络状态机。
2. 日志系统：环形缓冲区 + LogTask + 分级 + 时间戳。
3. 看门狗 + 故障恢复。
4. SD 卡日志存储（FATFS）。
5. OTA 远程升级（Bootloader + 双区 + VTOR）。
6. 性能/内存分析：任务栈高水位、FreeRTOS 运行统计、内存泄漏排查。
7. 面试准备（原始需求里有 33 道题，见下文）。

## 八、测试方式速查

- 串口：USB-TTL 模块 RX 接 PA9、共地，115200 8N1。
- 网络：板子接路由器 LAN 口，电脑 `ping 192.168.31.20`；NetAssist 选 TCP Client 连 `192.168.31.20:5000`（命令）；UDP 本地端口 `6000` 收数据。
- 命令：`LED_ON` / `LED_OFF` 点灯；连上后每 5 秒自动收到一条传感器数据。

## 九、之后要细学的清单（项目完成后逐个攻）

用户要求：先完成项目，再回头系统理解。用“我讲 → 用户复述 → 提问检验”方式逐个消化：

1. 任务与调度：任务/优先级/抢占/阻塞（osDelay 为什么不卡别人）。
2. 生产者-消费者：SensorTask → Queue → NetworkTask；队列 vs 全局变量。
3. 互斥锁：为什么 SET_ALARM 要加锁、数据竞争是什么。
4. TCP 服务器：socket/bind/listen/accept/recv/send 每一步在干什么。
5. 非阻塞事件循环：O_NONBLOCK；单任务怎么同时收命令和发数据。
6. UDP：sendto；为什么数据用 UDP；TCP vs UDP。
7. 函数指针命令表：为什么比 if/else 好。
8. FPU/configENABLE_FPU、MicroLIB/%f、软件 I2C、PHY 复位（PCF8574）。

## 十、面试准备要点（核心）

- 为什么用 FreeRTOS / 为什么分任务 / 优先级怎么定 / 为什么 NetworkTask 更高。
- 队列和全局变量的区别；队列传结构体的注意事项（按值拷贝、不能有指针、大小一致）。
- 信号量 vs 互斥锁；Task Notification 使用场景；heap_4 是什么。
- 栈大小怎么定、栈溢出怎么查；configMAX_SYSCALL_INTERRUPT_PRIORITY 是什么。
- MAC/PHY/RMII 是什么；LwIP 是什么；tcpip_thread 和应用任务的关系。
- TCP vs UDP；断线怎么处理；DHCP vs 静态 IP；网络状态机设计。
- 为什么用软件 I2C；为什么 PHY 复位要 PCF8574。
- 为什么 MicroLIB、为什么开 FPU、为什么 HAL 时基和 FreeRTOS tick 分开。
- 嵌入式软件如何模块化、如何保证可维护性、如何定位 HardFault。
