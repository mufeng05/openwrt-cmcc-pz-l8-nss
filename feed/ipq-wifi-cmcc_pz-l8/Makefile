# Board data (BDF) for CMCC PZ-L8 (IPQ5018 + QCN6122).
#
# OpenWrt 25.12.x defines Device/cmcc_pz-l8 without DEVICE_PACKAGES, so neither
# the ath11k firmware nor this board data ships in the stock image and WiFi
# never probes. The two board-2.bin blobs here were taken from the official
# qualcommax snapshot image (r31246), which does carry ipq-wifi-cmcc_pz-l8;
# both contain PZ-L8 specific calibration (entry
# bus=ahb,qmi-chip-id=0,qmi-board-id=255).
#
# They intentionally overwrite the generic board-2.bin installed by
# ath11k-firmware-ipq5018-qcn6122, which is how OpenWrt's own ipq-wifi-*
# packages work.
include $(TOPDIR)/rules.mk

PKG_NAME:=ipq-wifi-cmcc_pz-l8
PKG_RELEASE:=1
PKG_FLAGS:=nonshared

include $(INCLUDE_DIR)/package.mk

define Package/ipq-wifi-cmcc_pz-l8
  SECTION:=firmware
  CATEGORY:=Firmware
  TITLE:=Board data for CMCC PZ-L8 (IPQ5018 + QCN6122)
  DEPENDS:=@TARGET_qualcommax_ipq50xx
endef

define Package/ipq-wifi-cmcc_pz-l8/description
  Device specific ath11k board data for the CMCC PZ-L8.
endef

define Build/Compile
endef

define Package/ipq-wifi-cmcc_pz-l8/install
	$(INSTALL_DIR) $(1)/lib/firmware/ath11k/IPQ5018/hw1.0
	$(INSTALL_DATA) ./files/board-2.bin.IPQ5018 $(1)/lib/firmware/ath11k/IPQ5018/hw1.0/board-2.bin
	$(INSTALL_DIR) $(1)/lib/firmware/ath11k/QCN6122/hw1.0
	$(INSTALL_DATA) ./files/board-2.bin.QCN6122 $(1)/lib/firmware/ath11k/QCN6122/hw1.0/board-2.bin
endef

$(eval $(call BuildPackage,ipq-wifi-cmcc_pz-l8))
