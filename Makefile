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
TARGET   = lan-share
SRC      = lan_share.c
WEBHDR   = web_assets.h
WEBGEN   = tools/embed-web.js
NODE    ?= node
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

.PHONY: all test clean strip webgen

all: $(TARGET)

# 前端被编进二进制，所以只把可执行文件拷到路由器上也能打开页面。
# 前端改了要重新生成；web_assets.h 依赖 public/ 里全部文件的修改时间。
# 注意 p2p-pump.js 必须在 app.js 之前加载（app.js 顶层就解构 LSPump）。
WEBFILES = $(WEBDIR)/index.html $(WEBDIR)/login.html $(WEBDIR)/favicon.png \
           $(WEBDIR)/p2p-pump.js $(WEBDIR)/app.js $(WEBDIR)/style.css $(WEBDIR)/relay-channel.js

$(WEBHDR): $(WEBGEN) $(WEBFILES)
	$(NODE) $(WEBGEN)

webgen:
	$(NODE) $(WEBGEN)

$(TARGET): $(SRC) $(WEBHDR)
	$(CC) $(CFLAGS) -o $@ $(SRC) $(STATIC) $(LDFLAGS)

test: $(TARGET)
	@mkdir -p $(UPLOADDIR)
	$(TEST_CMD)

strip: $(TARGET)
	strip $(TARGET) 2>/dev/null || true

clean:
	rm -f $(TARGET)
