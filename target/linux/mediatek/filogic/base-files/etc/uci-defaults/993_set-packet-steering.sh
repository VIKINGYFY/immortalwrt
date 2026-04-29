#!/bin/sh

#关闭 数据包引导 功能
uci set network.globals.packet_steering='0'
uci commit network

exit 0
