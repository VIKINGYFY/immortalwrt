DTS_DIR := $(DTS_DIR)/qcom

define Device/8devices_kiwi-dvk
	$(call Device/FitImage)
	$(call Device/EmmcImage)
	DEVICE_VENDOR := 8devices
	DEVICE_MODEL := Kiwi-DVK
	DEVICE_DTS_CONFIG := config@8dev-kiwi
	SOC := ipq9570
	DEVICE_PACKAGES := kmod-ath12k ath12k-firmware-qcn9274 \
		ipq-wifi-8devices_kiwi f2fsck mkf2fs kmod-sfp \
		kmod-phy-maxlinear kmod-phy-realtek rtl826x-firmware
	IMAGE/factory.bin := qsdk-ipq-factory-nor
endef
TARGET_DEVICES += 8devices_kiwi-dvk

define Device/askey_sbe1v1k
	$(call Device/FitImage)
	$(call Device/EmmcImage)
	DEVICE_VENDOR := Askey
	DEVICE_MODEL := SBE1V1K
	DEVICE_ALT0_VENDOR := Askey
	DEVICE_ALT0_MODEL := RTQ7300T
	DEVICE_ALT1_VENDOR := Spectrum
	DEVICE_ALT1_MODEL := SBE1V1K
	DEVICE_DTS_CONFIG := config@rtq7300t-rev0
	KERNEL_LOADADDR := 0x42200000
	SOC := ipq9570
	DEVICE_PACKAGES := ipq-wifi-askey_sbe1v1k ath12k-firmware-qcn9274-ddwrt \
		kmod-hwmon-pwmfan kmod-phy-realtek rtl826x-firmware
endef
TARGET_DEVICES += askey_sbe1v1k

define Device/linksys_ln6001
	$(call Device/FitImage)
	DEVICE_VENDOR := Linksys
	DEVICE_MODEL := LN6001
	DEVICE_DTS := ipq9554-linksys-ln6001
	SOC := ipq9554
	DEVICE_PACKAGES += bluetooth-uart kmod-leds-pwm kmod-fs-f2fs mkf2fs f2fsck
	IMAGE/sysupgrade.bin/squashfs := append-rootfs | pad-to 64k | \
		check-size 128m | sysupgrade-tar rootfs=$$$$@ | append-metadata
endef
TARGET_DEVICES += linksys_ln6001

define Device/qcom_rdp433
	$(call Device/FitImageLzma)
	DEVICE_VENDOR := Qualcomm Technologies, Inc.
	DEVICE_MODEL := RDP433
	DEVICE_VARIANT := AP-AL02-C4
	BOARD_NAME := ap-al02.1-c4
	DEVICE_DTS_CONFIG := config@rdp433
	DEVICE_DTS_DIR := $(DTS_DIR)
	SOC := ipq9574
	KERNEL_INSTALL := 1
	KERNEL_SIZE := 6096k
	IMAGE_SIZE := 25344k
	IMAGE/sysupgrade.bin := append-kernel | pad-to 64k | append-rootfs | pad-rootfs | check-size | append-metadata
endef
TARGET_DEVICES += qcom_rdp433

define Device/nokia_beacon10
	$(call Device/FitImage)
	$(call Device/EmmcImage)
	DEVICE_VENDOR := Nokia
	DEVICE_MODEL := Beacon 10
	DEVICE_DTS_CONFIG := config@al02-c2
	KERNEL_SIZE := 10240k
	SOC := ipq9574
	SUPPORTED_DEVICES += nokia,beacon10
	DEVICE_PACKAGES := -kmod-ath12k kmod-ath11k-ahb kmod-ath11k-pci \
		ath11k-firmware-ipq9574-ddwrt ath11k-firmware-qcn9074-ddwrt \
		ipq-wifi-nokia_beacon10 kmod-phy-realtek rtl826x-firmware
	IMAGE/factory.bin := append-kernel | pad-to $$(KERNEL_SIZE) | append-rootfs | append-metadata
endef
TARGET_DEVICES += nokia_beacon10

define Device/xiaomi_be7000-common
	$(call Device/FitImage)
	$(call Device/UbiFit)
	DEVICE_VENDOR := Xiaomi
	DEVICE_DTS_CONFIG := config@al02-c6
	BLOCKSIZE := 128k
	PAGESIZE := 2048
	DEVICE_PACKAGES := kmod-ath11k-ahb ath11k-firmware-ipq9574-ddwrt \
		ath12k-firmware-qcn9274-ddwrt kmod-usb-storage uboot-envtools
endef

define Device/xiaomi_be7000
	$(call Device/xiaomi_be7000-common)
	DEVICE_MODEL := BE7000
	SOC := ipq9554
	DEVICE_PACKAGES += ipq-wifi-xiaomi_be7000
	SUPPORTED_DEVICES += xiaomi,be7000
endef
TARGET_DEVICES += xiaomi_be7000

define Device/xiaomi_be7000-pro
	$(call Device/xiaomi_be7000-common)
	DEVICE_MODEL := BE7000 Pro
	SOC := ipq9574
	DEVICE_PACKAGES += ipq-wifi-xiaomi_be7000-pro
	SUPPORTED_DEVICES += xiaomi,be7000-pro
endef
TARGET_DEVICES += xiaomi_be7000-pro
