# Router Role（路由器角色）

独立包 `luci-app-router-role` 在 **网络 → 接口 → 全局网络选项** 只增加“中继 AP 模式”开关，默认关闭。构建时复制当前 feed 的接口页面并应用本包补丁，通过在原菜单之后加载的菜单定义覆盖视图，不修改 feed 工作树。

| 模式 | 选择方式 | IPv4 | IPv6 |
| --- | --- | --- | --- |
| 主路由 | AP 关闭、WAN 协议为 PPPoE 时自动采用 | 保留拨号账号、密码和 LAN DHCPv4 设置 | WAN 启用 IPv6 协商，WAN6 跟随 `@wan` 请求地址/PD，LAN 提供 RA/SLAAC、DHCPv6/PD |
| 子路由 | AP 关闭、WAN 为其他协议时自动采用；默认 DHCP 上联 | 保留 DHCP/静态等 WAN 协议和原 LAN DHCPv4 设置，检测实际上联与 LAN 网段重叠时更换 LAN 网段 | WAN6 请求 PD，odhcpd hybrid：有 PD 时服务 LAN，没有 PD 时中继 RA/DHCPv6/NDP |
| 中继 AP | 手动勾选后“保存并应用” | LAN 桥为 DHCPv4 客户端，本机 DHCPv4 服务关闭，终端直接使用上级 DHCP | WAN6 在 LAN 桥上获取本机 IPv6、不请求 PD；本机 RA/DHCPv6 服务关闭，终端直接接收上级 IPv6 |

不再提供手动主路由、子路由和自定义选项。可以开机后直接配置 PPPoE；无需先选择角色。LuCI 接口表单（含协议切换弹窗）全部解析后，同一次保存暂存相应主/子模式设置，随后按标准“保存并应用”生效。

包内 `router-role` 服务在开机 netifd 启动前初始化自动模式，并利用 procd 的网络配置和接口事件覆盖其他页面/CLI 写入后生效的配置。没有轮询或独立常驻进程。同一个主/子模式下不反复覆盖详细 IPv6、防火墙设置；WAN 协议从 PPPoE 切换至其他协议或相反时才调整模式。真实 WAN 地址变化时仍检查 IPv4 网段重叠。后台串行处理，跳过未提交的 CLI UCI 更改，不读取 LuCI 会话内尚未应用的配置。

中继 AP 只由开关启用。开关改变不会立即写配置，“保存”仅暂存，“保存并应用”才激活；后台自动逻辑在 AP 开启时完全跳过。板级 LAN/WAN 端口与现有 LAN 桥端口全部并入一个桥，任意口都可上联；附加物理上联接口停用；无线 AP 接入 LAN，不改 SSID、密钥或无线禁用状态。关闭 AP 开关时，逐项还原启用 AP 前被改动的网络、DHCP、防火墙、无线选项，并删除 AP 创建的配置节；PPP 凭据不另存。恢复数据随 UCI 保存，标准 LuCI 应用回滚也覆盖开关和恢复记录。不要手工删除恢复记录；缺失时拒绝猜测恢复。

AP 转换由 LuCI 保存回调生成暂存配置，点击“保存并应用”后才激活。直接通过 UCI、配置导入或其他管理界面设置 `router_role_ap=1` 不会自动转换或记录原配置；如果没有 `router_role_ap_state` 快照，后台每次开机最多记录一次 `router-role` 警告，继续保持配置不变。

AP 自动转换支持普通独立网口/DSA 拓扑（如 BE6500）；通过 `/etc/board.json` 获取端口。已有桥 VLAN/swconfig 或无法确定全部网口时不自动转换，需要手工配置这些拓扑。

主/子自动配置使用同一份 `auto.json`，前端和后端都据此处理 IPv6、防火墙。防火墙随模式调整 LAN/WAN 网络归属、IPv4 NAT、LAN→WAN 转发、DHCP 和必要 ICMPv6 放行；子路由增加 UDP 547 中继回复放行，AP 将 LAN 和 WAN6 均归入 LAN 区域。AP 取消时恢复原设置，不停止防火墙，不启用 IPv6 NAT。

IPv6 公网连接取决于上级是否提供 IPv6。LAN 的 `ip6assign=60` 为下级路由器预留 PD 空间，netifd 在上级前缀不足时缩小到可用长度；只有 `/64` 时不能再分出独立的下级 PD，后续子路由可走 hybrid 中继路径。PPPoE 使用 `ipv6=1`，由独立 WAN6 获取 IPv6，避免另外自动创建 `wan_6`。

语言模板和翻译通过 LuCI 官方脚本扫描、合并；在项目根目录执行：

```sh
feeds/luci/build/i18n-scan.pl package/emortal/luci-app-router-role > package/emortal/luci-app-router-role/po/templates/router-role.pot
feeds/luci/build/i18n-update.pl package/emortal/luci-app-router-role/po
```

扫描后补全新增词条的翻译，保留脚本生成的来源位置。
