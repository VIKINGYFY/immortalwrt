# Airoha xPON package

This package combines the Linux 6.18 compatibility work from the LEDE
snapshot with the EN757x reference material from Sirherobrine23.

The AN7581/AN7583 build uses the LEDE `bsp`, `xpon_phy_10g` and `xpon_10g`
sources as the functional baseline. The EN7572AN-specific `lddla` module and
its PM/DM firmware are taken from Sirherobrine23. `econet_bob` remains enabled
to extract the board calibration table into `/etc/lddla/en7572_bob.conf`.
The other legacy helpers still use vendor-only `struct sk_buff` fields. They
must be ported to the shared `XPON_SKB_CB()` metadata before being enabled in a
production image.

The Nokia XG-040G-MD image selects `kmod-airoha-xpon-en757x`,
`kmod-airoha-pon-plugins`, and `airoha-pon-firmware`.
