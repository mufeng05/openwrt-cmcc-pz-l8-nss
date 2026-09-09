include $(TOPDIR)/rules.mk

PKG_NAME:=qca-ssdk-shell
PKG_RELEASE:=1

PKG_SOURCE_DATE:=2026-05-07
PKG_SOURCE_VERSION:=338d08ce6ad9100c9e69a7eea8d0471676b56888
QSDK_VERSION:=14.0
# Upstream: Qualcomm QSDK 14.0 (win_qsdk.common.1.0.r42), pinned by
# manifest/nss-pinned.tsv as oss/ssdk-shell.
PKG_SOURCE_PROTO:=git
PKG_SOURCE_URL:=https://git.codelinaro.org/clo/qsdk/oss/ssdk-shell.git
PKG_MIRROR_HASH:=skip
PKG_VERSION:=$(QSDK_VERSION).$(subst -,.,$(PKG_SOURCE_DATE))~$(PKG_SOURCE_VERSION)

PKG_FLAGS:=nonshared
PKG_BUILD_FLAGS:=no-lto

include $(INCLUDE_DIR)/kernel.mk
include $(INCLUDE_DIR)/package.mk

define Package/qca-ssdk-shell
  SECTION:=utils
  CATEGORY:=Utilities
  SUBMENU:=Network
  URL:=http://www.qca.qualcomm.com
  MAINTAINER:=Qualcomm Atheros, Inc.
  TITLE:=Qualcomm SSDK user-space shell (ssdk_sh)
  DEPENDS:=@TARGET_qualcommax +kmod-qca-ssdk
endef

define Package/qca-ssdk-shell/description
 ssdk_sh reaches qca-ssdk through its UK_IF character device (minor 254) and
 exposes the full FAL API from the shell: port state and counters, VLAN and
 FDB tables, PHY registers, per-device configuration.

 It is how the vendor firmware inspects and configures the switch, and it is
 the supported route to ssdk's "switch_ext" functionality - OpenWrt's swconfig
 has no way to carry an extended option list over netlink, so that attribute
 is registered but not settable from `swconfig`.

 On cmcc,pz-l8 device 0 is the IPQ5018 internal ESS and device 1 the external
 QCA8337, e.g.:

     ssdk_sh dev 1 port ctrl status get 5     # the MDI link to GMAC0/eth0
     ssdk_sh dev 1 vlan entry show
     ssdk_sh dev 1 fdb entry show
endef

# ssdk-shell's own ./config already defaults every IN_* feature to TRUE and
# sets UK_IF=TRUE / UK_MINOR_DEV=254, matching how we build qca-ssdk.  Its
# make/config.mk only accepts CHIP_TYPE ISIS, ISISC or ALL_CHIP; ISISC is the
# AR8327/AR8337 family, i.e. our QCA8337.
# ssdk-shell links its user-space objects into a shared libssdk_us.so but only
# passes -fPIC at the link step, so the objects have to be built PIC as well or
# aarch64 ld rejects R_AARCH64_ADR_PREL_PG_HI21 against ssdk_mii_reg_get.
# -fPIC has to ride on CC rather than EXTRA_CFLAGS: make/linux_opt.mk fills
# EXTRA_CFLAGS with the -I paths, and a command-line assignment would replace
# them (sw.h: No such file or directory).
MAKE_FLAGS+= \
	CC="$(firstword $(TOOLCHAIN_BIN_DIRS))/$(TARGET_CROSS)gcc -fPIC" \
	TOOL_PATH=$(firstword $(TOOLCHAIN_BIN_DIRS)) \
	SYS_PATH=$(LINUX_DIR) \
	TOOLPREFIX=$(TARGET_CROSS) \
	KVER=$(LINUX_VERSION) \
	ARCH=$(LINUX_KARCH) \
	CHIP_TYPE=ISISC \
	SHELL="$(BASH)"

define Build/Compile
	$(MAKE) $(MAKE_FLAGS) -C $(PKG_BUILD_DIR) PRJ_PATH=$(PKG_BUILD_DIR) all
endef

define Package/qca-ssdk-shell/install
	$(INSTALL_DIR) $(1)/usr/sbin $(1)/usr/lib
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/build/bin/ssdk_sh $(1)/usr/sbin/ssdk_sh
	$(CP) $(PKG_BUILD_DIR)/build/bin/libssdk_us.so $(1)/usr/lib/
endef

$(eval $(call BuildPackage,qca-ssdk-shell))
