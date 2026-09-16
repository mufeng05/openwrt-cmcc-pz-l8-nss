include $(TOPDIR)/rules.mk

PKG_NAME:=qca-nss-ecm
PKG_RELEASE:=9

PKG_SOURCE_DATE:=2026-08-06
PKG_SOURCE_VERSION:=5997ffd40deef3ee6788a411bd3a06dbb5beb784
QSDK_VERSION:=14.0
# Upstream: Qualcomm QSDK 14.0.r9 (NHSS.QSDK.14.0.r9-00040-O), pinned by manifest
# (branch win_qsdk.common.1.0.r42).
PKG_SOURCE_PROTO:=git
PKG_SOURCE_URL:=https://git.codelinaro.org/clo/qsdk/oss/lklm/qca-nss-ecm.git
PKG_MIRROR_HASH:=skip
PKG_VERSION:=$(QSDK_VERSION).$(subst -,.,$(PKG_SOURCE_DATE))~$(PKG_SOURCE_VERSION)

PKG_BUILD_PARALLEL:=1
PKG_FLAGS:=nonshared
PKG_BUILD_FLAGS:=gc-sections lto

PKG_CONFIG_DEPENDS:= \
	CONFIG_NSS_DRV_PPPOE_ENABLE \
	CONFIG_NSS_FIRMWARE_VERSION_11_4 \
	CONFIG_NSS_FIRMWARE_VERSION_12_5 \
	CONFIG_PACKAGE_kmod-gre \
	CONFIG_PACKAGE_kmod-gre6 \
	CONFIG_PACKAGE_kmod-ip6-tunnel \
	CONFIG_PACKAGE_kmod-ipsec \
	CONFIG_PACKAGE_kmod-macvlan \
	CONFIG_PACKAGE_kmod-nat46 \
	CONFIG_PACKAGE_kmod-pppoe \
	CONFIG_PACKAGE_kmod-qca-mcs \
	CONFIG_PACKAGE_kmod-qca-nss-drv \
	CONFIG_PACKAGE_kmod-qca-nss-drv-igs \
	CONFIG_PACKAGE_kmod-sit \
	CONFIG_PACKAGE_kmod-vxlan

include $(INCLUDE_DIR)/kernel.mk
include $(INCLUDE_DIR)/package.mk

define KernelPackage/qca-nss-ecm
  SECTION:=kernel
  CATEGORY:=Kernel modules
  SUBMENU:=Network Support
  DEPENDS:=@(TARGET_qualcommbe||TARGET_qualcommax||TARGET_ipq806x) \
		   +ethtool \
		   +kmod-nf-conntrack \
		   +@(TARGET_qualcommax||TARGET_ipq806x):NSS_DRV_IPV6_ENABLE \
		   +@(TARGET_qualcommax||TARGET_ipq806x):NSS_DRV_VIRT_IF_ENABLE \
		   +@((TARGET_qualcommax||TARGET_ipq806x)&&PACKAGE_kmod-pppoe):NSS_DRV_PPPOE_ENABLE \
		   +PACKAGE_kmod-qca-nss-drv:kmod-qca-nss-drv \
		   +PACKAGE_kmod-qca-mcs:kmod-qca-mcs \
		   +PACKAGE_kmod-bonding:kmod-bonding \
		   +PACKAGE_kmod-vxlan:kmod-vxlan \
		   +PACKAGE_kmod-nat46:kmod-nat46 \
		   +PACKAGE_kmod-ppp:kmod-ppp \
		   +PACKAGE_kmod-pppoe:kmod-pppoe
  TITLE:=QCA NSS Enhanced Connection Manager (ECM)
  FILES:=$(PKG_BUILD_DIR)/ecm.ko
  KCONFIG:= \
	CONFIG_NF_CONNTRACK_DSCPREMARK_EXT=y \
	CONFIG_NF_CONNTRACK_EVENTS=y
endef

define KernelPackage/qca-nss-ecm/Description
This package contains the QCA NSS Enhanced Connection Manager
endef

define KernelPackage/qca-nss-ecm/conffiles
/etc/config/ecm
endef

define KernelPackage/qca-nss-ecm/install
	$(INSTALL_DIR) $(1)/etc/firewall.d $(1)/etc/init.d $(1)/usr/bin $(1)/lib/netifd/offload $(1)/etc/config $(1)/etc/uci-defaults $(1)/etc/sysctl.d $(1)/etc/hotplug.d/net
	$(INSTALL_DATA) ./files/qca-nss-ecm.firewall $(1)/etc/firewall.d/qca-nss-ecm
	$(INSTALL_BIN) ./files/qca-nss-ecm.init $(1)/etc/init.d/qca-nss-ecm
	$(INSTALL_BIN) ./files/ecm_dump.sh $(1)/usr/bin/
	$(INSTALL_DATA) ./files/disable_offloads.sh $(1)/lib/netifd/offload
	$(INSTALL_DATA) ./files/on-demand-down $(1)/lib/netifd/offload/on-demand-down
	$(INSTALL_DATA) ./files/qca-nss-ecm.uci $(1)/etc/config/ecm
	$(INSTALL_DATA) ./files/qca-nss-ecm.defaults $(1)/etc/uci-defaults/99-qca-nss-ecm
	$(INSTALL_DATA) ./files/qca-nss-ecm.sysctl $(1)/etc/sysctl.d/qca-nss-ecm.conf
	$(INSTALL_DATA) ./files/disable_offloads.hotplug $(1)/etc/hotplug.d/net/99-disable_offloads
endef

# Vendor code is not warning-clean; keep warnings non-fatal. The accumulated
# EXTRA_CFLAGS are handed to kbuild via NOSTDINC_FLAGS: kbuild dropped
# EXTRA_CFLAGS support in kernel 6.15.
EXTRA_CFLAGS+= -Wno-error

EXTRA_CFLAGS+= \
		 -I$(STAGING_DIR)/usr/include/qca-nss-drv \
		 -I$(STAGING_DIR)/usr/include/qca-mcs \
		 -I$(STAGING_DIR)/usr/include/nat46

ifneq (, $(findstring $(CONFIG_TARGET_BOARD), "qualcommax" "ipq60xx"))
ECM_MAKE_OPTS+= ECM_NON_PORTED_SUPPORT_ENABLE=y \
			   ECM_INTERFACE_VLAN_ENABLE=y \
			   ECM_CLASSIFIER_MARK_ENABLE=y \
			   ECM_CLASSIFIER_DSCP_ENABLE=y \
			   ECM_CLASSIFIER_PCC_ENABLE=n
endif

ifneq ($(CONFIG_PACKAGE_kmod-qca-nss-drv),)
ECM_MAKE_OPTS+=ECM_FRONT_END_NSS_ENABLE=y
endif

# Disable ECM IPv6 support when global IPv6 support is disabled.
ifneq ($(CONFIG_IPV6),)
ECM_MAKE_OPTS+=ECM_IPV6_ENABLE=y
endif

# Disable ECM Bridge VLAN Filtering support when global kernel config is disabled
ifneq ($(CONFIG_BRIDGE_VLAN_FILTERING),)
ECM_MAKE_OPTS+=ECM_BRIDGE_VLAN_FILTERING_ENABLE=y
endif

ifneq ($(CONFIG_PACKAGE_kmod-qca-nss-drv-igs),)
ECM_MAKE_OPTS+=ECM_CLASSIFIER_DSCP_IGS=y
endif

ifneq ($(CONFIG_PACKAGE_kmod-qca-nss-drv-ovpn-link),)
ECM_MAKE_OPTS+=ECM_INTERFACE_OVPN_ENABLE=y
endif

ifeq ($(CONFIG_PACKAGE_kmod-vxlan),y)
ECM_MAKE_OPTS+=ECM_INTERFACE_VXLAN_ENABLE=y
endif

ifneq ($(CONFIG_PACKAGE_kmod-qca-ovsmgr),)
ECM_MAKE_OPTS+=ECM_INTERFACE_OVS_BRIDGE_ENABLE=y \
			   ECM_CLASSIFIER_OVS_ENABLE=y
EXTRA_CFLAGS+= -I$(STAGING_DIR)/usr/include/qca-ovsmgr
endif

# Deliberately not enabled with kmod-macvlan.  ECM's macvlan support calls
# macvlan_get_mode() and macvlan_offload_stats_update(), which QSDK adds to the
# kernel and OpenWrt does not have anywhere in 6.12 - ecm_interface.c then fails
# to compile with two implicit-declaration errors and takes the build with it.
#
# kmod-macvlan is an ordinary module someone may select for its own sake, so the
# coupling goes rather than the package: macvlan works, ECM does not accelerate
# it. Restoring this needs drivers/net/macvlan.c patched with the two helpers.
#
# ifneq ($(CONFIG_PACKAGE_kmod-macvlan),)
# ECM_MAKE_OPTS+=ECM_INTERFACE_MACVLAN_ENABLE=y
# endif

ifneq ($(CONFIG_PACKAGE_kmod-qca-mcs),)
ECM_MAKE_OPTS+=ECM_MULTICAST_ENABLE=y
endif

ifneq ($(CONFIG_PACKAGE_kmod-nat46),)
ECM_MAKE_OPTS+=ECM_INTERFACE_MAP_T_ENABLE=y
endif

ifneq ($(CONFIG_PACKAGE_kmod-ipsec),)
ECM_MAKE_OPTS+=ECM_INTERFACE_IPSEC_ENABLE=y
endif

ifneq ($(CONFIG_PACKAGE_kmod-pppoe),)
ECM_MAKE_OPTS+=ECM_INTERFACE_PPPOE_ENABLE=y \
			   ECM_INTERFACE_PPP_ENABLE=y
else
ECM_MAKE_OPTS+=ECM_INTERFACE_PPPOE_ENABLE=n \
			   ECM_INTERFACE_PPP_ENABLE=n
endif

# The EDMA-rework kernel carries only the PPPoE ppp hooks (0961); the
# PPTP/L2TP kernel hooks (qosmio 0603-2/0603-3) are not ported, so
# these interface types must stay off regardless of selected kmods.
ECM_MAKE_OPTS+=ECM_INTERFACE_PPTP_ENABLE=n \
			   ECM_INTERFACE_L2TPV2_ENABLE=n

ifneq ($(CONFIG_PACKAGE_kmod-gre)$(CONFIG_PACKAGE_kmod-gre6),)
ECM_MAKE_OPTS+=ECM_INTERFACE_GRE_TAP_ENABLE=y \
			   ECM_INTERFACE_GRE_TUN_ENABLE=y
endif

ifneq ($(CONFIG_PACKAGE_kmod-sit),)
ECM_MAKE_OPTS+=ECM_INTERFACE_SIT_ENABLE=y
endif

ifneq ($(CONFIG_PACKAGE_kmod-ip6-tunnel),)
ECM_MAKE_OPTS+=ECM_INTERFACE_TUNIPIP6_ENABLE=y
endif

ifneq ($(CONFIG_PACKAGE_kmod-qca-nss-drv-mscs),)
ECM_MAKE_OPTS+=ECM_CLASSIFIER_MSCS_ENABLE=y
endif

# The EDMA-rework kernel does not carry the QCA bonding hooks
# (bond_get_tx_dev), so LAG offload must stay off regardless of
# kmod-bonding.
ECM_MAKE_OPTS+=ECM_INTERFACE_BOND_ENABLE=n

ifneq ($(CONFIG_PACKAGE_kmod-qmi_wwan_q)$(CONFIG_PACKAGE_kmod-usb-net-qmi-wwan-quectel),)
ECM_MAKE_OPTS+=ECM_INTERFACE_RAWIP_ENABLE=y
endif

ifneq ($(CONFIG_NSS_FIRMWARE_VERSION_12_5),)
EXTRA_CFLAGS+=-DNSS_FIRMWARE_VERSION_12_5
endif

define Build/InstallDev
	mkdir -p $(1)/usr/include/qca-nss-ecm
	$(CP) $(PKG_BUILD_DIR)/exports/* $(1)/usr/include/qca-nss-ecm
endef

ifeq ($(CONFIG_TARGET_BOARD), "qualcommax")
    SOC:=$(CONFIG_TARGET_SUBTARGET)
endif

define Build/Compile
	+$(MAKE) -C "$(LINUX_DIR)" $(strip $(ECM_MAKE_OPTS)) \
		CROSS_COMPILE="$(TARGET_CROSS)" \
		ARCH="$(LINUX_KARCH)" \
		M="$(PKG_BUILD_DIR)" \
		CONFIG_WERROR=n \
		NOSTDINC_FLAGS="$(KERNEL_NOSTDINC_FLAGS) $(EXTRA_CFLAGS)" \
		SoC=$(SOC) \
		$(KERNEL_MAKE_FLAGS) \
		$(PKG_JOBS) \
		modules
endef

$(eval $(call KernelPackage,qca-nss-ecm))
