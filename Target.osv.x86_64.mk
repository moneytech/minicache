TARGET=linux
CONFIG_OSVAPP=y
CONFIG_SHELL=n

CONFIG_SHFS_CACHE_READAHEAD		?= 8
CONFIG_SHFS_CACHE_POOL_NB_BUFFERS	?= 512
CONFIG_SHFS_CACHE_GROW			?= n
CONFIG_BUILD_OPTIMISATION		?= -O3

CFLAGS+=-DCONFIG_LWIP_CHECKSUM_NOCHECK

include Target.$(TARGET).$(ARCH).mk