SUBTARGET:=ipq53xx
BOARDNAME:=Qualcomm Atheros IPQ53xx
DEFAULT_PACKAGES += ath12k-firmware-ipq5332-ddwrt ath12k-firmware-qcn9274-ddwrt

define Target/Description
	Build firmware images for Qualcomm Atheros IPQ53XX based boards.
endef
