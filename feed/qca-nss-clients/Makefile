include $(TOPDIR)/rules.mk

PKG_NAME:=qca-nss-clients
PKG_RELEASE:=8

PKG_SOURCE_DATE:=2024-09-11
PKG_SOURCE_VERSION:=51be82d43ef85079f78aa163d014223b05baa6a2
QSDK_VERSION:=12.5.5
# Upstream: QSDK branch NHSS.QSDK.12.5.5 -- the newest branch that still ships the
# NSS-core clients (bridge/vlan/pppoe/nss_qdisc). QSDK 14 dropped them: its client
# feed is PPE-only, and IPQ5018 has no PPE block.
PKG_SOURCE_PROTO:=git
PKG_SOURCE_URL:=https://git.codelinaro.org/clo/qsdk/oss/lklm/nss-clients.git
PKG_MIRROR_HASH:=skip
PKG_VERSION:=$(QSDK_VERSION).$(subst -,.,$(PKG_SOURCE_DATE))~$(PKG_SOURCE_VERSION)

PKG_BUILD_PARALLEL:=1

PKG_CONFIG_DEPENDS:=\
	CONFIG_NSS_DRV_IGS_ENABLE \
	CONFIG_NSS_DRV_PPPOE_ENABLE \
	CONFIG_NSS_DRV_SHAPER_ENABLE \
	CONFIG_NSS_FIRMWARE_VERSION_11_4 \
	CONFIG_NSS_FIRMWARE_VERSION_12_5

include $(INCLUDE_DIR)/kernel.mk
include $(INCLUDE_DIR)/package.mk

ifeq ($(CONFIG_TARGET_SUBTARGET), "ipq807x")
  SOC="ipq807x_64"
  subtarget:=$(CONFIG_TARGET_SUBTARGET)
else ifeq ($(CONFIG_TARGET_SUBTARGET), "ipq60xx")
  SOC="ipq60xx_64"
  subtarget:=$(CONFIG_TARGET_SUBTARGET)
else ifeq ($(CONFIG_TARGET_SUBTARGET), "ipq50xx")
  SOC="ipq50xx_64"
  subtarget:=$(CONFIG_TARGET_SUBTARGET)
else
  SOC=$(CONFIG_TARGET_BOARD)
endif

define KernelPackage/qca-nss-drv-pppoe
  SECTION:=kernel
  CATEGORY:=Kernel modules
  SUBMENU:=Network Devices
  TITLE:=NSS connection manager for PPPoE protocol
  DEPENDS:=@(TARGET_qualcommax||TARGET_ipq806x) \
	   +kmod-qca-nss-drv \
	   +@NSS_DRV_PPPOE_ENABLE \
	   +kmod-ppp \
	   +kmod-pppoe
  FILES:=$(PKG_BUILD_DIR)/pppoe/qca-nss-pppoe.ko
endef

define KernelPackage/qca-nss-drv-pppoe/Description
Kernel modules for NSS connection manager - Support for PPPoE
endef

define KernelPackage/qca-nss-drv-qdisc
  SECTION:=kernel
  CATEGORY:=Kernel modules
  SUBMENU:=Network Devices
  TITLE:=Qdisc for configuring shapers in NSS
  DEPENDS:=@(TARGET_qualcommax||TARGET_ipq806x) \
	   +kmod-qca-nss-drv \
	   +@NSS_DRV_SHAPER_ENABLE \
	   +@NSS_DRV_IGS_ENABLE
  FILES:=$(PKG_BUILD_DIR)/nss_qdisc/qca-nss-qdisc.ko
  KCONFIG:=CONFIG_NET_CLS_ACT=y
endef

define KernelPackage/qca-nss-drv-qdisc/Description
Linux qdisc that aids in configuring shapers in the NSS
endef

define KernelPackage/qca-nss-drv-igs
  SECTION:=kernel
  CATEGORY:=Kernel modules
  SUBMENU:=Network Devices
  TITLE:=NSS connection manager to perform ingress shaping
  DEPENDS:=@(TARGET_qualcommax||TARGET_ipq806x) \
	   +kmod-qca-nss-drv \
	   +@NSS_DRV_IGS_ENABLE \
	   +kmod-qca-nss-drv-qdisc \
	   +kmod-ifb \
	   +kmod-nf-conntrack \
	   +kmod-sched-core
  FILES:=$(PKG_BUILD_DIR)/nss_qdisc/igs/act_nssmirred.ko
endef

define KernelPackage/qca-nss-drv-igs/description
Linux action that helps in offloading traffic to an IFB interface to perform ingress shaping.
endef

define KernelPackage/qca-nss-drv-bridge-mgr
  SECTION:=kernel
  CATEGORY:=Kernel modules
  SUBMENU:=Network Devices
  TITLE:=NSS bridge manager (hardware bridge offload)
  DEPENDS:=@(TARGET_qualcommax_ipq807x||TARGET_qualcommax_ipq60xx) \
	   +kmod-qca-nss-drv \
	   +@NSS_DRV_BRIDGE_ENABLE \
	   +TARGET_qualcommax_ipq807x:kmod-qca-ppe-nss \
	   +TARGET_qualcommax_ipq60xx:kmod-qca-ppe-nss
  FILES:=$(PKG_BUILD_DIR)/bridge/qca-nss-bridge-mgr.ko
endef

define KernelPackage/qca-nss-drv-bridge-mgr/Description
Offloads Linux-bridge L2 forwarding into the NSS firmware by binding the
bridge's qca_ppe member ports to a firmware bridge interface. No AUTOLOAD:
loaded at runtime after the NSS data plane is up (see nss-up).
endef

define KernelPackage/qca-nss-drv-vlan-mgr
  SECTION:=kernel
  CATEGORY:=Kernel modules
  SUBMENU:=Network Devices
  TITLE:=NSS VLAN manager (802.1q offload interfaces)
  DEPENDS:=@TARGET_qualcommax_ipq50xx \
	   +kmod-qca-nss-drv \
	   +@NSS_DRV_VLAN_ENABLE
  FILES:=$(PKG_BUILD_DIR)/vlan/qca-nss-vlan.ko
endef

define KernelPackage/qca-nss-drv-vlan-mgr/Description
Creates NSS VLAN interfaces (dynamic type 17) for 8021q netdevs over an
NSS-known parent, so ECM can resolve VLAN hierarchies to the acceleration
engine. ipq50xx only in this tree: on PPE SoCs the manager compiles
against the fal/ssdk headers this stack does not carry. No AUTOLOAD.
endef

define KernelPackage/qca-nss-drv-netlink
  SECTION:=kernel
  CATEGORY:=Kernel modules
  SUBMENU:=Network Devices
  TITLE:=NSS netlink manager (userspace control and stats interface)
  DEPENDS:=@TARGET_qualcommax \
	   +kmod-qca-nss-drv \
	   +@NSS_DRV_IPV6_ENABLE \
	   +@NSS_DRV_WIFIOFFLOAD_ENABLE
  FILES:=$(PKG_BUILD_DIR)/netlink/qca-nss-netlink.ko
endef

define KernelPackage/qca-nss-drv-netlink/Description
Generic-netlink families exposing NSS firmware state (per-interface and
per-subsystem statistics, dynamic interfaces, rule injection) to
userspace tools such as nssinfo.
endef

define KernelPackage/qca-nss-drv-netlink/config
	source "$(SOURCE)/Config.in"
endef

define KernelPackage/qca-nss-drv-mirror
  SECTION:=kernel
  CATEGORY:=Kernel modules
  SUBMENU:=Network Devices
  TITLE:=NSS interface mirroring (copies firmware-path packets to a host netdev for capture)
  DEPENDS:=@TARGET_qualcommax \
	   +kmod-qca-nss-drv \
	   +@NSS_DRV_MIRROR_ENABLE
  FILES:=$(PKG_BUILD_DIR)/mirror/qca-nss-mirror.ko
endef

define KernelPackage/qca-nss-drv-mirror/Description
Creates mirror netdevs that receive copies of packets flowing through
the NSS firmware, making offloaded traffic visible to host capture
tools (tcpdump). Configured via /proc/sys/dev/nss/mirror/config.
endef

define KernelPackage/qca-nss-drv-wifi-meshmgr
  SECTION:=kernel
  CATEGORY:=Kernel modules
  SUBMENU:=Network Devices
  TITLE:=NSS Wi-Fi mesh manager (802.11s mesh offload)
  DEPENDS:=@TARGET_qualcommax \
	   +kmod-qca-nss-drv \
	   +@NSS_DRV_WIFI_MESH_ENABLE
  FILES:=$(PKG_BUILD_DIR)/wifi_meshmgr/qca-nss-wifi-meshmgr.ko
endef

define KernelPackage/qca-nss-drv-wifi-meshmgr/Description
Manages NSS firmware Wi-Fi mesh interfaces (mesh path and proxy path
tables) for 802.11s mesh offload. No AUTOLOAD of its own: ath11k links
against it when built with ATH11K_NSS_MESH_SUPPORT, so it loads as a
module dependency; its init is inert until a mesh device is created.
endef

define Build/InstallDev
	$(INSTALL_DIR) $(1)/usr/include/qca-nss-clients
	$(CP) $(PKG_BUILD_DIR)/netlink/include/* $(1)/usr/include/qca-nss-clients/
	$(CP) $(PKG_BUILD_DIR)/exports/* $(1)/usr/include/qca-nss-clients/
endef

# Vendor code is not warning-clean; keep warnings non-fatal. The accumulated
# EXTRA_CFLAGS are handed to kbuild via NOSTDINC_FLAGS: kbuild dropped
# EXTRA_CFLAGS support in kernel 6.15.
EXTRA_CFLAGS+= -Wno-error

EXTRA_CFLAGS+= \
	-I$(STAGING_DIR)/usr/include/qca-nss-drv

# Build individual packages if selected
define KernelPackage/qca-nss-drv-map-t
  SECTION:=kernel
  CATEGORY:=Kernel modules
  SUBMENU:=Network Devices
  TITLE:=NSS connection manager for MAP-T
  DEPENDS:=@(TARGET_qualcommax||TARGET_ipq806x) \
	   +kmod-qca-nss-drv \
	   +kmod-nat46
  FILES:=$(PKG_BUILD_DIR)/map/map-t/qca-nss-map-t.ko
  AUTOLOAD:=$(call AutoLoad,51,qca-nss-map-t)
endef

define KernelPackage/qca-nss-drv-map-t/description
NSS connection manager for MAP-T - hardware offload of 464xlat/MAP-T.
endef

ifneq ($(CONFIG_PACKAGE_kmod-qca-nss-drv-qdisc),)
NSS_CLIENTS_MAKE_OPTS+=qdisc=y
# PPE shaper mode and NSS-bridge qdisc attach need qca-ssdk and
# bridge-mgr, neither of which exists on the EDMA stack. The NSS-fw
# accel mode (accel_mode 0) is unaffected.
NSS_CLIENTS_MAKE_OPTS+=NSS_QDISC_PPE_ENABLE=n
endif

ifneq ($(CONFIG_PACKAGE_kmod-qca-nss-drv-igs),)
NSS_CLIENTS_MAKE_OPTS+=igs=y
endif

ifneq ($(CONFIG_PACKAGE_kmod-qca-nss-drv-pppoe),)
NSS_CLIENTS_MAKE_OPTS+=pppoe=y
endif

ifneq ($(CONFIG_PACKAGE_kmod-qca-nss-drv-map-t),)
NSS_CLIENTS_MAKE_OPTS+=map-t=y
EXTRA_CFLAGS+= -I$(STAGING_DIR)/usr/include/nat46
endif

ifneq ($(CONFIG_PACKAGE_kmod-qca-nss-drv-bridge-mgr),)
NSS_CLIENTS_MAKE_OPTS+=bridge-mgr=y
endif

ifneq ($(CONFIG_PACKAGE_kmod-qca-nss-drv-vlan-mgr),)
NSS_CLIENTS_MAKE_OPTS+=vlan-mgr=y
endif

ifneq ($(CONFIG_PACKAGE_kmod-qca-nss-drv-netlink),)
NSS_CLIENTS_MAKE_OPTS+=netlink=y
endif

ifneq ($(CONFIG_PACKAGE_kmod-qca-nss-drv-mirror),)
NSS_CLIENTS_MAKE_OPTS+=mirror=y
endif

ifneq ($(CONFIG_PACKAGE_kmod-qca-nss-drv-wifi-meshmgr),)
NSS_CLIENTS_MAKE_OPTS+=wifi-meshmgr=y
endif

# The 11.4 firmware line uses the per-node shaper statistics wire format
# (see the nss_qdisc 11.4 stats patch); select it to match the firmware.
ifdef CONFIG_NSS_FIRMWARE_VERSION_11_4
EXTRA_CFLAGS+=-DNSS_FIRMWARE_VERSION_11_4
endif

EXTRA_CFLAGS+= \
	-Wno-missing-prototypes \
	-Wno-missing-declarations \
	-Wno-empty-body

define Build/Compile
	+$(MAKE) -C "$(LINUX_DIR)" $(strip $(NSS_CLIENTS_MAKE_OPTS)) \
	  CROSS_COMPILE="$(TARGET_CROSS)" \
	  ARCH="$(LINUX_KARCH)" \
	  M="$(PKG_BUILD_DIR)" \
		CONFIG_WERROR=n \
	  NOSTDINC_FLAGS="$(KERNEL_NOSTDINC_FLAGS) $(EXTRA_CFLAGS)" \
	  CFLAGS_MODULE="-include $(PKG_BUILD_DIR)/compat.h -Wno-error" \
	  SoC=$(SOC) \
	  $(KERNEL_MAKE_FLAGS) \
	  $(PKG_JOBS) \
	  modules
endef

$(eval $(call KernelPackage,qca-nss-drv-pppoe))
$(eval $(call KernelPackage,qca-nss-drv-qdisc))
$(eval $(call KernelPackage,qca-nss-drv-igs))
$(eval $(call KernelPackage,qca-nss-drv-map-t))
$(eval $(call KernelPackage,qca-nss-drv-bridge-mgr))
$(eval $(call KernelPackage,qca-nss-drv-vlan-mgr))
$(eval $(call KernelPackage,qca-nss-drv-netlink))
$(eval $(call KernelPackage,qca-nss-drv-mirror))
$(eval $(call KernelPackage,qca-nss-drv-wifi-meshmgr))
