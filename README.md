# CMCC PZ-L8 — 带 QSDK NSS 硬件卸载的 OpenWrt

**中文** | [English](README.en.md)

在**未修改的 OpenWrt 25.12.5 基线**（内核 6.12.94，aarch64）上为 CMCC PZ-L8
（IPQ5018 + QCA8337 + QCN6122）启用高通 NSS 硬件卸载，所有驱动直接取自高通
官方的 QSDK 14.0 源码，而不是某个厂商 SDK 分支。

`main` 上每个提交的构建产物都在
[Releases](../../releases) 页面——`sysupgrade.bin` 用于升级已经在跑
OpenWrt 的路由器，`factory.ubi` 用于从 U-Boot 首次刷入。

## 实机实测

转发路径为 LAN/WiFi → 路由器 → WAN，iperf3 `-P 4`，每方向 15 秒。
CPU 指两个 Cortex-A53 主机核；NSS 的 UBI32 核是独立的，不计在内。

| 路径 | 本构建 | nwrt（原厂栈） | 原生 OpenWrt |
|---|---|---|---|
| **有线 ↔ WAN** | **949 上 / 949 下 Mbps，+0 % CPU** | 924 / 926 @ 4 % | 502 @ 100 %（DSA）|
| WiFi 链路本身（HE80 2×2）| 641 / 470 Mbps | — | — |
| **WiFi 5 GHz ↔ WAN** | **510 上 / 438 下 Mbps，+0.3 % CPU** | ~624 Mbps @ 25–35 % | 308 Mbps |
| **WiFi 2.4 GHz ↔ WAN** | **58 上 / 61 下 Mbps，+0.3 % CPU** | – | – |
| MemAvailable（全部服务起来后）| **40 MB** | 36 MB | — |

CPU 一列是增量值：测试全程每秒采样一次 `/proc/stat`，再减去同一次采集的
空载基线——因为采样器本身在这两个核上就要吃 2–5 %。有线那一行落在自己的基线
上或以下——949 Mbps 的开销已经小到这个测量方法分辨不出来。nwrt 的 WiFi
成绩比本构建快约 20 %，代价是 25–35 % 的主机 CPU。

5 GHz 那一行是在 NSS 固件从 12.2 换到 12.5 **之前**测的。

那一列有空格，是因为当时没有逐项实测参照镜像。后来 v1.6 和 nwrt 都刷回这块
板子测过一遍——同一个客户端、同一个服务端、同一个下午——但用的是四路并发
HTTP 而不是 iperf3，所以那些数字单独列表，不填进上面的空格：

| 四路并发 HTTP，20 秒 | v1.6 | nwrt | 本构建 |
|---|---|---|---|
| 有线 LAN → WAN | **912.1** | 901.8 | 900.9 |
| **5 GHz LAN → WAN** | **730** | **722** | 425 |
| 2.4 GHz LAN → WAN | 98.8 | **119.4** | 尚未用此法测 |
| 5 GHz 时主机 CPU | 测不出，见下 | 16.1 % | 尚未用此法测 |

两个参照镜像都不是未改动的 OEM 固件：v1.6 是厂商 4.4 SDK 上的社区构建，
nwrt 是 5.4 上的社区构建，两者都用闭源 qca-wifi。

5 GHz 的差距主要是信道宽度：两个参照都跑 **160 MHz**，客户端协商到 1922–2162
Mbit/s；ath11k 跑 80 MHz，同一个客户端协商到 1201。而 v1.6 比 nwrt 多协商 12%
却只多跑 1%，说明两者已经撞到同一个上限。

v1.6 那一格填不出数字，是因为它空闲时 CPU 就已经被四个卡死的 `hostapd_cli`
占满（load average 7.5，0% idle）——反过来这恰好证明了转发没走主机 CPU：
两个核被占满的情况下它仍然跑出 730 Mbit/s。

其余在 [docs/WIFILI.md](docs/WIFILI.md)，包括配置差异，以及本构建有意偏离
两个参照的地方。

有线转发跑在线速，主机 CPU **完全空闲**——包根本没进 Linux。**现在 WiFi
转发也是如此**，两个射频、两个方向都是：每 GB 流量主机大约只看到一百个帧，
其余全在 NSS 核内转发。

本文以前的版本曾说 WiFi 卸载做不到是结构性的——因为 ath11k 不是 NSS 托管的
接口，调参改不了。**那是错的。** 它已经做成了；过程和还残留的问题都在
[docs/WIFILI.md](docs/WIFILI.md)。上表的数字只统计**转发路径**——客户端在 WiFi，
经路由器 NAT 到一台有线主机——因为由路由器自身发起的流量测的是它自己的用户态，
说明不了卸载的效果。

## 已经可用的部分

- NSS 核正常启动，ECM 卸载栈每次开机自动加载，带一个会自行解除的安全网
- 外置 QCA8337 由 `qca-ssdk`/swconfig 驱动而非 DSA——两个独立 GMAC，MTU 1500，
  无 DSA tag
- ath11k 双频 WiFi（2.4 GHz + 5 GHz HE80），两个射频都已卸载
- 每个射频多个 SSID、`option isolate`、以及 `iw station dump` 里的每站点接收速率——
  这几项都需要改驱动，见 [docs/WIFILI.md](docs/WIFILI.md)
- 真正能整形的 SQM，通过 `sqm-scripts-nss` 和 NSS qdisc 实现
- 状态 LED、WAN DHCP、LuCI、sysupgrade

## 已知限制

- QoS 必须用 `nss-edma` 这个 SQM 脚本：数据路径在 NSS 核里，Linux 的 qdisc
  根本看不到流量，cake 或 fq_codel **会静静地什么都不做**。见
  [docs/WIFILI.md](docs/WIFILI.md)。
- **内存紧张。** 256 MB 的板子，MemTotal 只有 173 MB——预留里有 48 MB 是 Q6
  无线固件，无法缩减——剩下的里 ath11k 又占 46 MB。ath11k 的数据路径环已经
  从上游尺寸调小，否则和 NSS 放不下。MemAvailable 落在 40 MB；剩下的去哪了看
  `docs/WIFILI.md`。
- 射频的 monitor 模式抓包实际上被那些环尺寸禁掉了。
- **防火墙里的流量分载开关在这里什么都不做。** 硬件分载是 `[fixed]` 关闭——
  `qca-nss-dp` 没有 flowtable 支持；软件分载则是空转，因为 ECM 在 netfilter
  转发路径看到连接之前就把它交给了 NSS。实测：开启后 flowtable 接到零条连接，
  而 NSS 照常加速。两个都保持关闭。
- `qca-ssdk-shell`（`ssdk_sh`）编译不过——`-fPIC` 穿不过它的递归 make 传不到
  `src/sal/sd`。

## 构建

### GitHub Actions

`main` 上每次构建成功都会发布一个 [release](../../releases)，包含 sysupgrade、
factory、initramfs 三个镜像、它们的 sha256sums 和 manifest。镜像同时也作为 run
artifact 附在构建上，但 artifact 会过期，release 不会。Pull request 会构建但不
发布；手动运行 **Build CMCC PZ-L8 (NSS)** 时可以取消勾选 `make_release` 来不发布。

### 本地

```sh
git clone https://github.com/openwrt/openwrt -b v25.12.5
git clone https://github.com/mufeng05/openwrt-cmcc-pz-l8-nss pzl8-nss
pzl8-nss/scripts/setup.sh ./openwrt
cd openwrt && make -j$(nproc)
```

`setup.sh` 是幂等的，并且会把自己动过的东西都打印出来。

### 自己选包

`setup.sh` 跑完之后，这就是一棵普通的 OpenWrt 源码树：

```sh
cd openwrt
make menuconfig        # 加 LuCI 应用、工具，想加什么加什么
make -j$(nproc)
```

`setup.sh` 只在**没有** `.config` 时才播种配置，所以为了拉新提交而重跑它不会
丢掉你的选择。`RESEED=1 pzl8-nss/scripts/setup.sh ./openwrt` 则是明确要求回到
仓库自带的配置。两种情况它都会以 `make defconfig` 结尾，所以上游新增的包会被
补上默认值。

有四个符号必须保持开启，而 menuconfig 不会拦着你关掉它们——其中两个根本不是包。
`setup.sh` 每次运行都会检查并警告：

| | |
|---|---|
| `CONFIG_NSS_DRV_WIFIOFFLOAD_ENABLE` | 编译 qca-nss-drv 的 wifili 和 wifi_vdev 那一半。没它 `ath11k.ko` **链接不了**——modpost 报十个未定义的 `nss_wifili_*` 符号 |
| `CONFIG_NSS_FIRMWARE_VERSION_12_5` | 选定固件 blob，**并且**通过 qca-nss-drv 的 0022 补丁选定 wifili 消息 ABI。关掉它**编译照样过**，但 peer 统计数组步长会比固件发的短 16 字节 |
| `CONFIG_PACKAGE_kmod-qca-nss-drv` | NSS 驱动本体 |
| `CONFIG_PACKAGE_nss-firmware-ipq50xx` | 固件 blob |

想把你的选择固化进仓库，写回种子配置即可：

```sh
cd openwrt
./scripts/diffconfig.sh > ../pzl8-nss/config/cmcc_pz-l8.config
```

workflow 在 `defconfig` 之后会重新校验同样这四个符号，所以种子配置要是丢了哪个，
会在几秒内报 `MISS <symbol>`，而不是四十分钟后死在 modpost。

## 刷机

从正在运行的 OpenWrt 跑 `sysupgrade -n`，或者用 `192.168.10.10` 的 U-Boot
网页恢复。默认 LAN 地址是 **192.168.10.1**。

两者要的镜像格式不同，而且都不直观——网页恢复执行的是
`source $imgaddr:script`，因此它要的是一个**内含刷写脚本的 FIT**，而不是 `.ubi`。
[docs/RECOVERY.md](docs/RECOVERY.md) 里有两条路径、flash 布局，以及 bootloader 到底
检查了什么。

原版 `platform.sh` 在这块板子上会中止 sysupgrade——它调了
`elecom_upgrade_prepare()`，而那个函数期待一对 A/B rootfs，PZ-L8 没有，于是刷写
静静地什么都没做。这里已打补丁。

## 目录结构

```
config/          .config 种子（diffconfig 输出）
docs/            工程笔记——先看 FINDINGS.md
feed/            本项目的包：qca-nss-drv、-ecm、-clients、qca-mcs、
                 nss-firmware、ipq-wifi、qca-ssdk-shell
files/           rootfs 覆盖层：nss-offload 启动脚本等
manifest/        这些源码锁定到的 QSDK 14.0 revision
openwrt/
  0001-*.patch   对 OpenWrt 已有文件的修改
  tree/          OpenWrt 没有的文件，原样拷贝
scripts/setup.sh 把以上全部应用到一棵干净的源码树
```

## 源码为什么锁在这些版本

- **NSS 驱动 / ECM / mcs**：QSDK 14.0（`NHSS.QSDK.14.0.r9-00040-O`），来自
  git.codelinaro.org。
- **NSS clients**（pppoe、qdisc、vlan-mgr）：QSDK **12.5.5**。QSDK 14 的 client
  feed 是纯 PPE 的，而 IPQ5018 没有 PPE 模块——高通自己的 `nss-ppe/Makefile`
  里只列了 ipq52xx/53xx/54xx/95xx/96xx。
- **NSS 固件**：`NSS.FW.12.5-210-MP.R`，以及与之匹配的 wifili ABI。本文以前的版本
  写的是 12.2-156，因为当时以为 12.5 在这个 SoC 上不能用；它能用，而且经两轮
  A/B 对比，WiFi 快约 65 %。坑在于 **blob 不能单独换**——qca-nss-drv 的 0022
  补丁把四个 `uint32_t` 藏在 `NSS_FIRMWARE_VERSION_12_5` 后面，而且它们全在
  peer 统计消息的**每-peer 数组内部**，所以选固件必须连 ABI 一起选，否则第一个
  peer 之后的每一个都会读错偏移。兼容 mesh 的仍然只有 11.4 这条线。
- **数据平面**：OpenWrt 自带的 `qca-nss-dp`（`syn_gmac_dp`），和原厂固件用的是同
  一个驱动。没用 `qca-dwmac-nss` 这层 shim：那是给把 ipq50xx 改成上游 stmmac 的
  树用的，本项目不是那种情况。

## 致谢

QSDK 源码属于高通，来自
<https://git.codelinaro.org/clo/qsdk>。

打包骨架和大量 kernel 6.x 修复来自社区 NSS feed，均为 GPL：

- Julius Bairaktaris —— <https://github.com/JuliusBairaktaris>。`nss-packages`
  源头在他这里；而 `openwrt-nss-edma`——本项目在 `docs/WIFILI.md` 全程对照的
  那棵树——也是他的。`openwrt/tree/` 里的 iproute2 NSS qdisc、nssmirred
  补丁以及多个 qualcommax 内核补丁同样带着他的 Signed-off-by。
- Stanislaw Pal（kuncy7）—— <https://github.com/kuncy7>。本项目对这两棵树的
  checkout 用的是他的 fork。
- Sean K（qosmio），NSS 固件 blob 的重新打包者 ——
  <https://github.com/qosmio/qca-sdk-nss-fw>

ath11k 缩环的做法参考了
<https://github.com/openwrt/openwrt/pull/21495>，该 PR 未被合并。
