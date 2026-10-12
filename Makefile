# LAN Share (C) — 零依赖局域网文件分享
#
#   make                 本机编译
#   make CC=...          指定交叉编译器（路由器请用交叉工具链）
#   make test            编译并跑端到端测试
#   make clean
#
# 交叉编译示例（OpenWrt / 路由器）：
#   make CC=mipsel-openwrt-linux-gcc STATIC=-static
#   make CC=mipsel-linux-muslsf-gcc  STATIC=-static
#
# Windows (MinGW):
#   make LDFLAGS="-static -lws2_32"

CC      ?= cc
CFLAGS  ?= -Os -std=c99 -Wall
STATIC  ?=
TARGET   = lan-share
SRC      = lan_share.c
WEBDIR   = public
UPLOADDIR= uploads

# Linux 下默认静态链接，产出任何发行版都能直接跑的二进制。
# macOS 无法静态链接系统 libc，检测到就自动跳过。
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Linux)
  STATIC ?= -static
endif
ifeq ($(findstring MINGW,$(UNAME_S)),MINGW)
  LDFLAGS += -static -lws2_32
endif
ifeq ($(findstring MSYS,$(UNAME_S)),MSYS)
  LDFLAGS += -static -lws2_32
endif

TEST_PORT ?= 8099
TEST_CMD   = SPAWN="./$(TARGET) -p $(TEST_PORT) -d $(UPLOADDIR) -w $(WEBDIR)" \
             BASE=http://127.0.0.1:$(TEST_PORT) \
             TRAVERSAL_PROBE=$(SRC) \
             node test.js

.PHONY: all test clean strip

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $(SRC) $(STATIC) $(LDFLAGS)

test: $(TARGET)
	@mkdir -p $(UPLOADDIR)
	$(TEST_CMD)

strip: $(TARGET)
	strip $(TARGET) 2>/dev/null || true

clean:
	rm -f $(TARGET)
