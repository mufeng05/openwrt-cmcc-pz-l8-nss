# Patches not used by this project

Carried over from the upstream feed but not applicable to our architecture.

## 0102-nss_init-defer-probe-until-data-plane-armed.patch
Adds a `nss_dp_probe_gate()` call so nss-drv returns -EPROBE_DEFER until a
data-plane glue module arms a port. It exists for trees where ipq50xx was
converted to the upstream stmmac driver (`dwmac-ipq5018`) and NSS is bolted on
via `qca-dwmac-nss` / `qca-ppe-nss`: there, booting NSS firmware before the
glue is armed hijacks CPU-port delivery and kills host ethernet RX.

This project keeps the QSDK-native data plane (`qca-nss-dp`, `syn_gmac_dp`)
that OpenWrt 25.12.x still ships for qualcommax/ipq50xx. nss-dp and nss-drv are
the matched vendor pair (as in the vendor/nwrt firmware), so there is no
"unarmed" window to gate against, and `nss_dp_probe_gate()` does not exist in
the native nss-dp API.
