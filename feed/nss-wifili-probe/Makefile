include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/kernel.mk

PKG_NAME:=nss-wifili-probe
PKG_RELEASE:=1

PKG_FLAGS:=nonshared
PKG_BUILD_FLAGS:=no-lto

include $(INCLUDE_DIR)/package.mk

define KernelPackage/nss-wifili-probe
  SECTION:=kernel
  CATEGORY:=Kernel modules
  SUBMENU:=Network Devices
  TITLE:=NSS wifili feasibility probe (bring-up only)
  DEPENDS:=@TARGET_qualcommax_ipq50xx +kmod-qca-nss-drv +kmod-ath11k
  FILES:=$(PKG_BUILD_DIR)/nss_wifili_probe.ko
  AUTOLOAD:=
endef

define KernelPackage/nss-wifili-probe/description
 Sends a single NSS_WIFILI_INIT_MSG built from ath11k's real data-path rings
 and reports whether the NSS firmware accepts it.

 This decides whether an ath11k NSS wifi offload is possible at all.  The ring
 descriptions map field-for-field between struct hal_srng and struct
 nss_wifili_hal_srng_info, but the firmware was written against Qualcomm's own
 driver and may still reject ath11k's ring ids or geometry.  Finding that out
 costs ~300 lines here instead of several thousand in a full glue layer.

 Takes no ring over and changes no behaviour.  Trigger by hand:

     echo 0 > /sys/kernel/debug/nss_wifili_probe/run
     dmesg | grep WIFILIPROBE

 Bring-up aid.  Not for a shipping image.
endef

NSS_DRV_DIR:=$(STAGING_DIR)/usr/include/qca-nss-drv
ATH11K_DIR:=$(LINUX_DIR)/../mac80211-regular/backports-$(strip $(shell \
	ls -d $(LINUX_DIR)/../mac80211-regular/backports-* 2>/dev/null | \
	head -1 | sed 's/.*backports-//'))/drivers/net/wireless/ath/ath11k

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) ./src/* $(PKG_BUILD_DIR)/
	# the flat ring description is owned by the ath11k patch; take it from
	# there rather than keeping a second copy that can drift
	$(CP) $(ATH11K_DIR)/ath11k_nss_export.h $(PKG_BUILD_DIR)/
endef

define Build/Compile
	+$(KERNEL_MAKE) M="$(PKG_BUILD_DIR)" \
		EXTRA_CFLAGS="-I$(NSS_DRV_DIR) -I$(PKG_BUILD_DIR)" \
		CONFIG_NSS_WIFILI_PROBE=m \
		modules
endef

$(eval $(call KernelPackage,nss-wifili-probe))
