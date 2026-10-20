/*
 * lan_share.c - 零依赖局域网文件分享服务，为小内存嵌入式设备（路由器）设计。
 *
 * 两条路径：
 *   1. 设备直传：浏览器之间 WebRTC 点对点，本程序只转发信令，不接触文件字节。
 *   2. 服务端文件库：HTTP 上传落盘、列表、下载、删除。
 *
 * 编译：
 *   gcc -Os -std=c99 -o lan_share lan_share.c                    （本机 Linux）
 *   aarch64-openwrt-linux-gcc -Os -std=c99 -static -o lan_share lan_share.c
 *   gcc -O2 -std=c99 -o lan_share.exe lan_share.c -lws2_32       （Windows 验证用）
 *
 * 运行：
 *   ./lan_share -p 8080 -d /mnt/sda1/share -w ./public
 *
 * 只依赖 libc。WebSocket 需要的 SHA-1 与 Base64 均在本文件内实现，不链接 OpenSSL。
 */

/* 必须在所有头文件之前：32 位路由器（MIPS/ARM）上让 off_t 与 fseek/stat
 * 走 64 位版本，否则下载超过 2GB 的文件时 Range 偏移会算错。
 * 对本程序唯一的代价是结构体略大，可以忽略。 */
#ifndef _FILE_OFFSET_BITS
#  define _FILE_OFFSET_BITS 64
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdint.h>
#include <stdarg.h>
#include <time.h>
#include <fcntl.h>
#include <sys/stat.h>

/* 内嵌的前端资源（index.html / app.js / style.css 的字节数组）。
 * 由 tools/embed-web.js 生成，make 会自动重新生成。
 * 没有它，只把二进制拷到路由器上打开页面会得到 404 Not Found。 */
#include "web_assets.h"

/* Windows 的 SOCKET 句柄值远大于 Linux 的 fd，默认 FD_SETSIZE=64 会让
 * FD_SET() 写越界。这里在任何 winsock 头文件之前放大它。Linux 下无害。 */
#ifdef _WIN32
#  ifndef FD_SETSIZE
#    define FD_SETSIZE 1024
#  endif
#endif

/* ==========================================================================
 * 第一部分：纯算法工具（不依赖任何平台头文件）
 * ========================================================================== */

typedef struct {
    uint32_t h[5];
    unsigned char block[64];
    size_t blocklen;
    uint64_t total;
} ls_sha1;

static uint32_t sha1_rol(uint32_t v, unsigned n) {
    return (v << n) | (v >> (32 - n));
}

static void sha1_compress(uint32_t h[5], const unsigned char *p) {
    uint32_t w[80];
    uint32_t a, b, c, d, e;
    unsigned i;
    for (i = 0; i < 16; i++) {
        w[i] = ((uint32_t)p[i * 4] << 24) | ((uint32_t)p[i * 4 + 1] << 16) |
               ((uint32_t)p[i * 4 + 2] << 8) | (uint32_t)p[i * 4 + 3];
    }
    for (i = 16; i < 80; i++) {
        w[i] = sha1_rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }
    a = h[0]; b = h[1]; c = h[2]; d = h[3]; e = h[4];
    for (i = 0; i < 80; i++) {
        uint32_t f, k, t;
        if (i < 20) {
            f = (b & c) | (~b & d);
            k = 0x5A827999u;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1u;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDCu;
        } else {
            f = b ^ c ^ d;
            k = 0xCA62C1D6u;
        }
        t = sha1_rol(a, 5) + f + e + k + w[i];
        e = d; d = c; c = sha1_rol(b, 30); b = a; a = t;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
}

static void sha1_init(ls_sha1 *ctx) {
    ctx->h[0] = 0x67452301u;
    ctx->h[1] = 0xEFCDAB89u;
    ctx->h[2] = 0x98BADCFEu;
    ctx->h[3] = 0x10325476u;
    ctx->h[4] = 0xC3D2E1F0u;
    ctx->blocklen = 0;
    ctx->total = 0;
}

static void sha1_update(ls_sha1 *ctx, const void *data, size_t len) {
    const unsigned char *p = (const unsigned char *)data;
    ctx->total += (uint64_t)len;
    while (len > 0) {
        size_t need = 64 - ctx->blocklen;
        size_t take = (len < need) ? len : need;
        memcpy(ctx->block + ctx->blocklen, p, take);
        ctx->blocklen += take;
        p += take;
        len -= take;
        if (ctx->blocklen == 64) {
            sha1_compress(ctx->h, ctx->block);
            ctx->blocklen = 0;
        }
    }
}

static void sha1_final(ls_sha1 *ctx, unsigned char out[20]) {
    uint64_t bits = ctx->total * 8u;
    unsigned char one = 0x80, zero = 0x00;
    unsigned char tail[8];
    int i;
    sha1_update(ctx, &one, 1);
    while (ctx->blocklen != 56) sha1_update(ctx, &zero, 1);
    for (i = 0; i < 8; i++) tail[i] = (unsigned char)(bits >> (56 - 8 * i));
    sha1_update(ctx, tail, 8);
    for (i = 0; i < 5; i++) {
        out[i * 4 + 0] = (unsigned char)(ctx->h[i] >> 24);
        out[i * 4 + 1] = (unsigned char)(ctx->h[i] >> 16);
        out[i * 4 + 2] = (unsigned char)(ctx->h[i] >> 8);
        out[i * 4 + 3] = (unsigned char)(ctx->h[i]);
    }
}

static const char B64TAB[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static size_t b64_encode(const unsigned char *in, size_t len, char *out) {
    size_t i, o = 0;
    for (i = 0; i < len; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16;
        if (i + 1 < len) v |= (uint32_t)in[i + 1] << 8;
        if (i + 2 < len) v |= (uint32_t)in[i + 2];
        out[o++] = B64TAB[(v >> 18) & 63];
        out[o++] = B64TAB[(v >> 12) & 63];
        out[o++] = (i + 1 < len) ? B64TAB[(v >> 6) & 63] : '=';
        out[o++] = (i + 2 < len) ? B64TAB[v & 63] : '=';
    }
    out[o] = '\0';
    return o;
}

static int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static size_t url_decode(const char *in, char *out, size_t outsz) {
    size_t i, o = 0;
    if (outsz == 0) return 0;
    for (i = 0; in[i] != '\0' && o + 1 < outsz; i++) {
        if (in[i] == '%') {
            int hi = hex_val(in[i + 1]);
            int lo = hex_val(in[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out[o++] = (char)((hi << 4) | lo);
                i += 2;
                continue;
            }
        }
        out[o++] = (in[i] == '+') ? ' ' : in[i];
    }
    out[o] = '\0';
    return o;
}

/* 取 basename、剔除控制字符与路径分隔符、去掉尾部空格和点。 */
static void sanitize_name(const char *in, char *out, size_t outsz) {
    const char *base = in;
    const char *p;
    size_t o = 0;
    if (outsz == 0) return;
    for (p = in; *p != '\0'; p++) {
        if (*p == '/' || *p == '\\') base = p + 1;
    }
    for (p = base; *p != '\0' && o + 1 < outsz; p++) {
        unsigned char c = (unsigned char)*p;
        if (c < 32 || c == 127) continue;
        if (c == '/' || c == '\\') continue;
        out[o++] = (char)c;
    }
    while (o > 0 && (out[o - 1] == ' ' || out[o - 1] == '.')) o--;
    out[o] = '\0';
    if (o == 0) memcpy(out, "unnamed", 8);
}

static void human_size(unsigned long long bytes, char *out, size_t outsz) {
    static const char *units[] = { "B", "KB", "MB", "GB", "TB" };
    double v = (double)bytes;
    int idx = 0;
    while (v >= 1024.0 && idx < 4) {
        v /= 1024.0;
        idx++;
    }
    if (idx == 0) snprintf(out, outsz, "%llu B", bytes);
    else snprintf(out, outsz, "%.1f %s", v, units[idx]);
}

static size_t json_escape(const char *in, char *out, size_t outsz) {
    size_t i, o = 0;
    if (outsz == 0) return 0;
    for (i = 0; in[i] != '\0' && o + 7 < outsz; i++) {
        unsigned char c = (unsigned char)in[i];
        if (c == '"' || c == '\\') {
            out[o++] = '\\'; out[o++] = (char)c;
        } else if (c == '\n') {
            out[o++] = '\\'; out[o++] = 'n';
        } else if (c == '\r') {
            out[o++] = '\\'; out[o++] = 'r';
        } else if (c == '\t') {
            out[o++] = '\\'; out[o++] = 't';
        } else if (c < 32) {
            unsigned hi = (unsigned)(c >> 4), lo = (unsigned)(c & 15);
            out[o++] = '\\'; out[o++] = 'u'; out[o++] = '0'; out[o++] = '0';
            out[o++] = (char)((hi < 10) ? ('0' + hi) : ('a' + hi - 10));
            out[o++] = (char)((lo < 10) ? ('0' + lo) : ('a' + lo - 10));
        } else {
            out[o++] = (char)c;
        }
    }
    out[o] = '\0';
    return o;
}

/* 从极简 JSON 中取出某个字符串字段（处理常见转义与 \uXXXX）。 */
static int json_get_str(const char *json, const char *key, char *out, size_t outsz) {
    char needle[64];
    const char *p;
    size_t o = 0;
    snprintf(needle, sizeof needle, "\"%s\"", key);
    p = strstr(json, needle);
    if (!p) return 0;
    p += strlen(needle);
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (*p != ':') return 0;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (*p != '"') return 0;
    p++;
    while (*p != '\0' && o + 1 < outsz) {
        if (*p == '\\') {
            p++;
            if (*p == '\0') break;
            if (*p == 'n') { out[o++] = '\n'; p++; }
            else if (*p == 'r') { out[o++] = '\r'; p++; }
            else if (*p == 't') { out[o++] = '\t'; p++; }
            else if (*p == 'b') { out[o++] = '\b'; p++; }
            else if (*p == 'f') { out[o++] = '\f'; p++; }
            else if (*p == 'u') {
                unsigned cp = 0;
                int k;
                p++;
                for (k = 0; k < 4 && p[k] != '\0'; k++) {
                    int hv = hex_val(p[k]);
                    if (hv < 0) break;
                    cp = (cp << 4) | (unsigned)hv;
                }
                p += 3;
                if (cp < 0x80) {
                    out[o++] = (char)cp;
                } else if (cp < 0x800) {
                    if (o + 2 < outsz) {
                        out[o++] = (char)(0xC0 | (cp >> 6));
                        out[o++] = (char)(0x80 | (cp & 0x3F));
                    }
                } else if (o + 3 < outsz) {
                    out[o++] = (char)(0xE0 | (cp >> 12));
                    out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                    out[o++] = (char)(0x80 | (cp & 0x3F));
                }
                p++;
            } else {
                out[o++] = *p++;
            }
            continue;
        }
        if (*p == '"') break;
        out[o++] = *p++;
    }
    out[o] = '\0';
    return 1;
}

/* ==========================================================================
 * 第二部分：平台相关头文件与类型
 * ========================================================================== */

#ifdef _WIN32
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <windows.h>
#  include <io.h>
#  include <direct.h>
#  define ls_strncasecmp    _strnicmp
#  define ls_write(fd,b,n)  _write((fd),(b),(unsigned)(n))
#  define ls_closefile(fd)  _close(fd)
/* ls_openwrite / ls_fopen_rb / ls_remove_file 在下方以函数实现：
 * Windows 上必须走宽字符 API，见那里的说明。 */
#else
#  include <unistd.h>
#  include <fcntl.h>
#  include <errno.h>
#  include <signal.h>
#  include <strings.h>
#  include <dirent.h>
#  include <sys/types.h>
#  include <sys/stat.h>
#  include <sys/socket.h>
/* fd_set / FD_ZERO / FD_SET / struct timeval 来自 sys/select.h。
 * 必须显式包含：winsock2.h 会顺带提供它们（Windows 分支因此不受影响），
 * 某些 libc 的 sys/socket.h 也会间接引入，但新版 glibc 已经不再间接包含，
 * 漏掉就会在 Linux 上报 "unknown type name 'fd_set'"。 */
#  include <sys/select.h>
#  include <sys/time.h>
#  include <netinet/in.h>
#  include <netinet/tcp.h>
#  include <arpa/inet.h>
#  define ls_strncasecmp    strncasecmp
#  define ls_write(fd,b,n)  write((fd),(b),(n))
#  define ls_closefile(fd)  close(fd)
/* POSIX 下路径就是原始字节，内核不解释编码，直接用窄字符版本即可 */
static FILE *ls_fopen_rb(const char *p) {
    return fopen(p, "rb");
}
static int ls_remove_file(const char *p) {
    return remove(p);
}
#endif

#ifdef _WIN32
#  define LS_EAGAIN       WSAEWOULDBLOCK
#  define LS_EWOULDBLOCK  WSAEWOULDBLOCK
#  define ls_last_err()   WSAGetLastError()
#  define ls_closesock(f) closesocket(f)
#else
#  define LS_EAGAIN       EAGAIN
#  define LS_EWOULDBLOCK  EWOULDBLOCK
#  define ls_last_err()   errno
#  define ls_closesock(f) close(f)
#endif

#define MAX_CLIENTS     24
/* IO 缓冲。原来只有 8KB：每次可读事件最多 recv 8KB，千兆网下要十多万次
 * 系统调用/秒才喂得满，路由器那种单核 MIPS/ARM CPU 直接被打满，
 * 实测吞吐就卡在几 MB/s。改成 64KB 并配合下面"一次事件读完"的循环。 */
#define IO_BUF          65536
#define HEAD_BUF        8192
/* WS 缓冲同时也是单帧上限。兜底数据通道要发 64KB 的分块（1 字节 id 长度 +
 * id + 载荷），16KB 装不下，放宽到 128KB。24 个客户端各 256KB（wbuf + frag），
 * 静态区里按需占用，路由器上也吃得下。 */
#define WS_BUF          131072
#define NAME_MAX        64
#define ID_MAX          40
#define PATH_MAX_LOCAL  512

enum { MODE_HTTP = 0, MODE_WS = 1, MODE_UPLOAD = 2 };

typedef struct {
    char *buf;
    size_t len;
    size_t sent;
    /* 已分配的字节数。有了它才能在"还有空间"时直接追加，不必每次都把未发送
     * 的部分挪到开头——兜底通道每秒要转发上百个 32KB 帧，每次都 memmove 一份
     * 可能几 MB 的残留，实测把端到端吞吐压到 3MB/s。 */
    size_t cap;
    int is_mem;
    FILE *file;
    uint64_t total;
    int has;
} ls_out;

typedef struct {
    int fd;
    int used;
    int mode;
    char id[ID_MAX];
    char name[NAME_MAX];
    char ip[48];

    char hbuf[HEAD_BUF + 1];
    size_t hlen;

    unsigned char wbuf[WS_BUF];
    size_t wlen;
    int frag_op;          /* 正在拼接的分片消息首帧 opcode（0 = 没有） */
    unsigned char frag[WS_BUF];
    size_t fraglen;
    /* 帧级跟踪：每条连接只打前若干帧。帧流一旦错位，光看"分片缓冲超限"
     * 这种结果级报错是查不出来的——必须看到 op/fin/len 才知道到底收到了什么。
     * 文本和二进制分开计数：信令（ICE 候选）会刷掉几十条小文本帧，
     * 合用一个预算的话，兜底通道的大二进制帧一条都轮不到打印。 */
    int trace_text;       /* 还能打印多少条文本帧 */
    int trace_bin;        /* 还能打印多少条二进制/续帧 */

    int upfd;
    uint64_t upleft;
    char upname[PATH_MAX_LOCAL];
    int pending_hello;   /* 握手响应发完后还需补发一次 peers 广播 */

    /* 分块上传（断点续传）状态。整包上传时这几个字段保持 0。 */
    int up_chunked;              /* 1 = 本次是分块上传 */
    uint64_t up_size;            /* 该文件应有的完整大小 */
    uint64_t up_base;            /* 本次请求之前的已收字节（offset） */
    uint64_t up_reqlen;          /* 本次请求体的字节数 */
    char up_final[NAME_MAX * 4]; /* 收满后要落成的文件名 */

    char pend[IO_BUF];
    size_t pendlen;

    /* 兜底通道（二进制中转）统计。关闭时打出来，就能看出"是谁先断的、
     * 断的时候两边各走到哪一步"——只报一句"连接中断"是查不出东西的。 */
    uint64_t relay_in;    /* 从本连接收到的二进制帧字节（含帧首 id 前缀） */
    uint64_t relay_out;   /* 转发给本连接的二进制帧字节（纯载荷） */

    ls_out out;
} ls_client;

static ls_client g_clients[MAX_CLIENTS];
static unsigned long g_seq = 0;
static char g_upload_dir[PATH_MAX_LOCAL] = "./uploads";
static char g_web_dir[PATH_MAX_LOCAL] = "./public";
static int g_port = 8080;

/* 可选 TURN 中继配置：一段合法 JSON 数组文本（如
 * [{"urls":"turn:1.2.3.4:3478","username":"u","credential":"p"}]），由
 * LAN_SHARE_TURN 环境变量提供。为空表示纯内网离线直连（默认）。serve_static
 * 在返回首页时把占位符替换成该 JSON，让前端把 TURN 加进 iceServers。
 * AP / 客户端隔离下设备间直连被丢弃时即可走中继。 */
static char *g_turn_config = NULL;

static const char *ADJ[] = { "蓝色", "银色", "安静", "敏捷", "温暖", "明亮", "沉稳", "轻盈", "清澈", "柔和" };
static const char *ANI[] = { "海豚", "猎鹰", "水獭", "狐狸", "鲸鱼", "山猫", "燕子", "小鹿", "海豹", "苍鹭" };

/* 设备名记忆表：按 IP 记住上次用过的名字。
 *
 * 每次新建连接都会随机取名，没有这层记忆的话浏览器刷新一次就换一个名字，
 * 对端根本认不出这台机器。客户端另有 localStorage 记一层，这里是服务端兜底
 * （换浏览器、清缓存、隐私模式下仍然有效）。
 *
 * 只存 32 条，满了覆盖第一条——设备名不是关键数据，丢了重新随机即可。 */
#define NAME_MEM_MAX 32
typedef struct {
    char ip[48];
    char name[NAME_MAX];
} ls_name_mem;
static ls_name_mem g_name_mem[NAME_MEM_MAX];

static const char *name_mem_get(const char *ip) {
    int i;
    for (i = 0; i < NAME_MEM_MAX; i++) {
        if (g_name_mem[i].ip[0] != '\0' && strcmp(g_name_mem[i].ip, ip) == 0) {
            return g_name_mem[i].name;
        }
    }
    return NULL;
}

static void name_mem_set(const char *ip, const char *name) {
    int i;
    int free_slot = -1;
    for (i = 0; i < NAME_MEM_MAX; i++) {
        if (g_name_mem[i].ip[0] == '\0') {
            if (free_slot < 0) free_slot = i;
            continue;
        }
        if (strcmp(g_name_mem[i].ip, ip) == 0) {
            snprintf(g_name_mem[i].name, sizeof g_name_mem[i].name, "%s", name);
            return;
        }
    }
    if (free_slot < 0) free_slot = 0;
    snprintf(g_name_mem[free_slot].ip, sizeof g_name_mem[free_slot].ip, "%s", ip);
    snprintf(g_name_mem[free_slot].name, sizeof g_name_mem[free_slot].name, "%s", name);
}

/* ==========================================================================
 * 基础 IO 帮助
 * ========================================================================== */

#ifdef _WIN32
/* Windows 的文件系统调用必须走宽字符版本。
 *
 * 文件名在这里是 UTF-8 字节序列，若直接用 ANSI 版本（_open / fopen / _stat），
 * 系统会拿当前代码页（简体中文环境是 GBK 936）去解释这些字节，只要 UTF-8
 * 字节恰好拼出非法的 GBK 双字节序列，转换就失败、调用返回错误。
 *
 * 症状看起来毫无规律：例如 "最.txt"(e6 9c 80) 能创建而 "中.txt"(e4 b8 ad) 不能，
 * 因为 0x80 在 GBK 里是单字节的欧元符号，而 ad 2e 不是合法的双字节组合。
 *
 * 正确做法：显式把 UTF-8 转成 UTF-16 再调 _w* 系列。
 * Linux 下路径是原始字节、内核不解释编码，不存在这个问题。 */
static wchar_t *ls_utf8_to_wide(const char *s) {
    int n;
    wchar_t *w;
    if (!s) return NULL;
    n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    if (n <= 0) return NULL;
    w = (wchar_t *)malloc((size_t)n * sizeof(wchar_t));
    if (!w) return NULL;
    if (MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n) <= 0) {
        free(w);
        return NULL;
    }
    return w;
}

static void ls_wide_to_utf8(const wchar_t *w, char *out, size_t outsz) {
    int n;
    if (outsz == 0) return;
    n = WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)outsz, NULL, NULL);
    if (n <= 0) out[0] = '\0';
}

static FILE *ls_fopen_rb(const char *p) {
    wchar_t *w = ls_utf8_to_wide(p);
    FILE *f;
    if (!w) return NULL;
    f = _wfopen(w, L"rb");
    free(w);
    return f;
}

static int ls_remove_file(const char *p) {
    wchar_t *w = ls_utf8_to_wide(p);
    int r;
    if (!w) return -1;
    r = _wremove(w);
    free(w);
    return r;
}
#endif

/* 打开文件用于写入并定位到 offset。
 * keep=0：从零重写（整包上传），keep=1：保留已落盘内容（分块续传）。
 * 续传的关键就在这里——不能在续写时截断，否则已收的部分全没了。 */
static int ls_openwrite_at(const char *p, uint64_t offset, int keep) {
    int fd;
#ifdef _WIN32
    wchar_t *w = ls_utf8_to_wide(p);
    if (!w) return -1;
    fd = _wopen(w, _O_WRONLY | _O_CREAT | (keep ? 0 : _O_TRUNC) | _O_BINARY, 0644);
    free(w);
#else
    fd = open(p, O_WRONLY | O_CREAT | (keep ? 0 : O_TRUNC), 0644);
#endif
    if (fd < 0) return -1;
    if (offset > 0) {
#ifdef _WIN32
        if (_lseeki64(fd, (__int64)offset, SEEK_SET) < 0) {
            ls_closefile(fd);
            return -1;
        }
#else
        if (lseek(fd, (off_t)offset, SEEK_SET) < 0) {
            ls_closefile(fd);
            return -1;
        }
#endif
    }
    return fd;
}

/* 收满之后把 .part 半成品改名成正式文件。用 rename 而不是复制，
 * 路由器上 flash 慢，能省一次整文件读写。 */
static int ls_rename_file(const char *from, const char *to) {
#ifdef _WIN32
    wchar_t *wf = ls_utf8_to_wide(from);
    wchar_t *wt = ls_utf8_to_wide(to);
    int r;
    if (!wf || !wt) {
        free(wf);
        free(wt);
        return -1;
    }
    r = _wrename(wf, wt);
    free(wf);
    free(wt);
    return r;
#else
    return rename(from, to);
#endif
}

static void set_nonblock(int fd) {
#ifdef _WIN32
    u_long mode = 1;
    ioctlsocket((SOCKET)fd, FIONBIO, &mode);
#else
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl >= 0) (void)fcntl(fd, F_SETFL, fl | O_NONBLOCK);
#endif
}

static void set_nodelay(int fd) {
    int one = 1;
    (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
}

static int would_block(void) {
    int e = (int)ls_last_err();
    return (e == (int)LS_EAGAIN || e == (int)LS_EWOULDBLOCK);
}

static void random_name(char *out, size_t outsz) {
    snprintf(out, outsz, "%s%s", ADJ[rand() % 10], ANI[rand() % 10]);
}

static void make_id(char *out, size_t outsz) {
    snprintf(out, outsz, "dev-%lu-%04x", g_seq, (unsigned)(rand() & 0xFFFF));
}

/* ==========================================================================
 * 输出缓冲
 * ========================================================================== */

static void out_reset(ls_client *c) {
    if (c->out.is_mem && c->out.buf) free(c->out.buf);
    if (!c->out.is_mem && c->out.file) fclose(c->out.file);
    memset(&c->out, 0, sizeof c->out);
}

static void queue_mem(ls_client *c, const char *data, size_t len) {
    /* 上一段还没发完就直接再来一段？必须追加，不能覆盖。
     * 单线程 select 循环里 send 只会写进内核缓冲区，事件循环往往要等下一轮
     * 才会真正 flush，所以"改名回执 self + peers 广播"这种连发两条的场景
     * 会撞在一起——旧实现开头的 out_reset() 会把第一条整帧丢掉，
     * 表现就是前端改名后收不到 self 回执。这里改成把未发送的部分挪到开头再追加。 */
    if (c->out.has && c->out.is_mem && c->out.buf) {
        /* 快路径：尾部还放得下就直接追加，一次 memmove 都不做。
         * 这是兜底通道的正常路径（帧小、发送快、队列留不住）——原来每追加一帧
         * 都要把"未发送部分"挪到开头，几百帧规模下就是几十 GB 的内存搬移，
         * 实测把中转吞吐压到 3MB/s。 */
        size_t need = c->out.len + len + 1;
        if (need <= c->out.cap) {
            memcpy(c->out.buf + c->out.len, data, len);
            c->out.len += len;
            c->out.buf[c->out.len] = '\0';
            return;
        }
        /* 尾部放不下：先回收已发送的空间再试一次。
         * 这一步不能省——下面按 c->out.len 追加，而 len 是含已发送前缀的总长，
         * 不先压缩的话，扩容算出来的容量只够"未发送部分 + 本次"，实际写入却从
         * len 开始，sent > 0 时会写到缓冲区外面去（堆溢出）。 */
        {
            size_t rest = c->out.len - c->out.sent;
            if (c->out.sent > 0) {
                if (rest) memmove(c->out.buf, c->out.buf + c->out.sent, rest);
                c->out.sent = 0;
                c->out.len = rest;
            }
            need = c->out.len + len + 1;
            if (need > c->out.cap) {
                size_t ncap = c->out.cap ? c->out.cap * 2 : need;
                char *nb;
                if (ncap < need) ncap = need;
                nb = (char *)realloc(c->out.buf, ncap);
                if (!nb) return; /* 内存不足时保留原有内容，本次丢弃 */
                c->out.buf = nb;
                c->out.cap = ncap;
            }
        }
        memcpy(c->out.buf + c->out.len, data, len);
        c->out.len += len;
        c->out.buf[c->out.len] = '\0';
        return;
    }
    out_reset(c);
    c->out.buf = (char *)malloc(len + 1);
    if (!c->out.buf) return;
    memcpy(c->out.buf, data, len);
    c->out.buf[len] = '\0';
    c->out.len = len;
    c->out.cap = len + 1;
    c->out.sent = 0;
    c->out.is_mem = 1;
    c->out.has = 1;
}

static void queue_response(ls_client *c, int code, const char *ctype,
                           const char *body, size_t blen) {
    const char *reason;
    char head[320];
    char *buf;
    int hl;
    size_t total;

    if (code == 200) reason = "OK";
    else if (code == 404) reason = "Not Found";
    else if (code == 405) reason = "Method Not Allowed";
    else reason = "Error";

    hl = snprintf(head, sizeof head,
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %llu\r\n"
        "Cache-Control: no-store\r\n"
        "Connection: keep-alive\r\n\r\n",
        code, reason, ctype, (unsigned long long)blen);
    if (hl <= 0) return;

    total = (size_t)hl + blen;
    buf = (char *)malloc(total + 1);
    if (!buf) return;
    memcpy(buf, head, (size_t)hl);
    if (blen) memcpy(buf + hl, body, blen);
    buf[total] = '\0';

    out_reset(c);
    c->out.buf = buf;
    c->out.len = total;
    c->out.cap = total + 1;
    c->out.sent = 0;
    c->out.is_mem = 1;
    c->out.has = 1;
}

/* 返回 1 完成，0 未完成，-1 出错。 */
static int flush_out(ls_client *c) {
    if (c->pendlen > 0) {
        int n = (int)send(c->fd, c->pend, (int)c->pendlen, 0);
        if (n > 0) {
            memmove(c->pend, c->pend + n, c->pendlen - (size_t)n);
            c->pendlen -= (size_t)n;
            if (c->pendlen > 0) return 0;
        } else if (n < 0 && would_block()) {
            return 0;
        } else {
            return -1;
        }
    }

    if (c->out.is_mem) {
        while (c->out.sent < c->out.len) {
            size_t chunk = c->out.len - c->out.sent;
            int n;
            if (chunk > IO_BUF) chunk = IO_BUF;
            n = (int)send(c->fd, c->out.buf + c->out.sent, (int)chunk, 0);
            if (n > 0) {
                c->out.sent += (size_t)n;
                continue;
            }
            if (n < 0 && would_block()) return 0;
            return -1;
        }
        out_reset(c);
        return 1;
    }

    while (c->out.sent < c->out.total) {
        /* 单线程服务，flush_out 不会重入，用静态缓冲省掉 64KB 栈空间
         * （client_readable 里也有一份同样大的）。 */
        static char buf[IO_BUF];
        size_t want = (size_t)(c->out.total - c->out.sent);
        size_t got, off = 0;
        if (want > sizeof buf) want = sizeof buf;
        got = fread(buf, 1, want, c->out.file);
        if (got == 0) {
            out_reset(c);
            return 1;
        }
        while (off < got) {
            int n = (int)send(c->fd, buf + off, (int)(got - off), 0);
            if (n > 0) {
                off += (size_t)n;
                continue;
            }
            if (n < 0 && would_block()) {
                if (off < got) {
                    memcpy(c->pend, buf + off, got - off);
                    c->pendlen = got - off;
                }
                c->out.sent += off;
                return 0;
            }
            return -1;
        }
        c->out.sent += got;
    }
    out_reset(c);
    return 1;
}

/* ==========================================================================
 * WebSocket 帧构造
 * ========================================================================== */

static size_t ws_frame_build(int opcode, const char *data, size_t len,
                             unsigned char *out, size_t outsz) {
    size_t hlen, i;
    if (outsz < len + 10) return 0;
    out[0] = (unsigned char)(0x80 | opcode);
    if (len < 126) {
        out[1] = (unsigned char)len;
        hlen = 2;
    } else if (len < 65536) {
        out[1] = 126;
        out[2] = (unsigned char)((len >> 8) & 0xFF);
        out[3] = (unsigned char)(len & 0xFF);
        hlen = 4;
    } else {
        out[1] = 127;
        for (i = 0; i < 8; i++) out[2 + i] = (unsigned char)(((uint64_t)len >> (56 - 8 * i)) & 0xFF);
        hlen = 10;
    }
    if (len) memcpy(out + hlen, data, len);
    return hlen + len;
}

static void ws_send_text(ls_client *c, const char *text) {
    size_t tlen = strlen(text);
    unsigned char *frame = (unsigned char *)malloc(tlen + 16);
    size_t flen;
    if (!frame) return;
    flen = ws_frame_build(0x1, text, tlen, frame, tlen + 16);
    if (flen) queue_mem(c, (const char *)frame, flen);
    free(frame);
}

/* 二进制帧下行（opcode 0x2）。这是"兜底数据通道"的载体：WebRTC 直连建不起来
 * 时（典型是 Chrome 把 host 候选伪装成 mDNS .local 名字、而 Wi-Fi AP 或
 * 有线/无线桥接把组播拦了，候选永远解析不出来），文件字节就走这里中转。
 * 帧格式统一为 [1 字节: 对端 id 长度][id][payload]，收发双方用同一套解析。 */
static void ws_send_binary(ls_client *c, const unsigned char *payload, size_t len,
                           const char *peer_id) {
    size_t idlen = peer_id ? strlen(peer_id) : 0;
    unsigned char *frame;
    size_t body, hlen, i;
    if (idlen > 255) return;
    body = 1 + idlen + len;
    if (body > WS_BUF) {
        /* 超限只能丢（一帧塞不下）。静默丢会让发送端永远等不到确认——
         * 表现是进度卡死在某个百分比，而服务端日志一片空白。 */
        static unsigned long toobig = 0;
        toobig++;
        if (toobig <= 5 || toobig % 500 == 0) {
            fprintf(stderr, "[lan-share] 中转丢弃：单帧 %lu > WS_BUF=%d（累计 %lu 帧）\n",
                    (unsigned long)body, WS_BUF, toobig);
            fflush(stderr);
        }
        return;
    }
    frame = (unsigned char *)malloc(body + 16);
    if (!frame) return;
    /* 先按 body 长度算出帧头占几个字节，再把 body 写到正确偏移上。
     * 不能直接调 ws_frame_build(out == data)：它先写帧头再 memcpy 载荷，
     * 同一个缓冲区上两步会互相覆盖，payload 的前几字节会被帧头吃掉。 */
    hlen = (body < 126) ? 2 : (body < 65536 ? 4 : 10);
    frame[0] = (unsigned char)(0x80 | 0x2);
    if (hlen == 2) {
        frame[1] = (unsigned char)body;
    } else if (hlen == 4) {
        frame[1] = 126;
        frame[2] = (unsigned char)((body >> 8) & 0xFF);
        frame[3] = (unsigned char)(body & 0xFF);
    } else {
        frame[1] = 127;
        for (i = 0; i < 8; i++) frame[2 + i] = (unsigned char)(((uint64_t)body >> (56 - 8 * i)) & 0xFF);
    }
    frame[hlen] = (unsigned char)idlen;
    if (idlen) memcpy(frame + hlen + 1, peer_id, idlen);
    if (len) memcpy(frame + hlen + 1 + idlen, payload, len);
    queue_mem(c, (const char *)frame, hlen + body);
    free(frame);
    c->relay_out += len;
}

/* 把一条二进制帧转给目标客户端。找不到就静默丢弃——对端可能刚断线，
 * 上层有超时和重传兜着，没必要为了一个找不到的 id 关掉转发方的连接。
 * 但丢了多少必须留下痕迹：否则表现就是"进度条突然不动了"，而日志全空白。 */
static void forward_binary(ls_client *c, const unsigned char *payload, size_t len) {
    char to[ID_MAX];
    size_t idlen;
    int i;
    if (len < 1) return;
    idlen = payload[0];
    if (idlen == 0 || idlen > len - 1) return;
    if (idlen >= sizeof to) return;
    memcpy(to, payload + 1, idlen);
    to[idlen] = '\0';
    c->relay_in += len;
    /* 每 8MB 打一行进度。只报"连接中断"是没用的，得知道断之前双方各走了多远。 */
    if (c->relay_in / (8u << 20) != (c->relay_in - len) / (8u << 20)) {
        fprintf(stderr, "[lan-share] 中转进度 %s→%s：已转发 %llu MB\n",
                c->id, to, (unsigned long long)(c->relay_in / (1u << 20)));
        fflush(stderr);
    }
    for (i = 0; i < MAX_CLIENTS; i++) {
        if (!g_clients[i].used || g_clients[i].mode != MODE_WS) continue;
        if (strcmp(g_clients[i].id, to) == 0) {
            ws_send_binary(&g_clients[i], payload + 1 + idlen, len - 1 - idlen, c->id);
            return;
        }
    }
    static unsigned long dropped = 0;
    dropped++;
    if (dropped <= 5 || dropped % 500 == 0) {
        fprintf(stderr, "[lan-share] 中转丢弃：找不到目标 %s（累计 %lu 帧，来自 %s）\n",
                to, dropped, c->id);
        fflush(stderr);
    }
}

/* ==========================================================================
 * 客户端管理
 * ========================================================================== */

static int count_ws_clients(void) {
    int i, n = 0;
    for (i = 0; i < MAX_CLIENTS; i++) {
        if (g_clients[i].used && g_clients[i].mode == MODE_WS) n++;
    }
    return n;
}

/* 安全追加格式化：snprintf 返回的是"若不截断会写入的长度"，直接累加会让
 * len 越过缓冲区容量，下一次 `cap - len` 下溢成巨大 size_t 而越界写。
 * 这里统一夹在边界内，设备多时宁可截断也不要崩。 */
static size_t body_append(char *body, size_t len, size_t cap, const char *fmt, ...) {
    va_list ap;
    int n;
    if (len >= cap) return len;
    va_start(ap, fmt);
    n = vsnprintf(body + len, cap - len, fmt, ap);
    va_end(ap);
    if (n < 0) return len;
    if (len + (size_t)n >= cap) return cap - 1;
    return len + (size_t)n;
}

/* 构造 peers 列表并广播给所有已握手的客户端。 */
static void broadcast_peers(void) {
    char body[4096];
    char esc[NAME_MAX * 4];
    size_t len = 0;
    size_t cap = sizeof body;
    int first = 1;
    int i;

    len = body_append(body, len, cap, "{\"type\":\"peers\",\"peers\":[");
    for (i = 0; i < MAX_CLIENTS; i++) {
        if (!g_clients[i].used || g_clients[i].mode != MODE_WS) continue;
        if (!first) len = body_append(body, len, cap, ",");
        first = 0;
        json_escape(g_clients[i].name, esc, sizeof esc);
        len = body_append(body, len, cap,
            "{\"id\":\"%s\",\"name\":\"%s\",\"ip\":\"%s\"}",
            g_clients[i].id, esc, g_clients[i].ip);
    }
    len = body_append(body, len, cap, "]}");

    for (i = 0; i < MAX_CLIENTS; i++) {
        if (g_clients[i].used && g_clients[i].mode == MODE_WS) ws_send_text(&g_clients[i], body);
    }
}

/* 关连接一律走这里，并且必须说清"为什么关"。
 *
 * 原来有十来个调用点直接 client_close(c)，什么也不打。于是出现这种局面：
 * 客户端报 1006（对端没说再见就消失），服务端进程还在、日志一片空白，
 * 谁关的、在哪一步关的完全无从判断——只能靠猜。现在每条关闭都带一个原因
 * 字符串，加上 errno、收发位置和中转计数，一次就能定位。 */
static void client_close_why(ls_client *c, const char *why) {
    int was_ws = (c->mode == MODE_WS);
    int err = (int)ls_last_err();
    fprintf(stderr,
            "[lan-share] 关闭连接 id=%s ip=%s mode=%d 原因=%s errno=%d "
            "wlen=%lu 待发=%lu/%lu 中转 收=%llu 发=%llu\n",
            c->id[0] ? c->id : "-", c->ip[0] ? c->ip : "-", c->mode, why, err,
            (unsigned long)c->wlen,
            (unsigned long)c->out.sent, (unsigned long)c->out.len,
            (unsigned long long)c->relay_in, (unsigned long long)c->relay_out);
    fflush(stderr);
    if (c->upfd >= 0) {
        ls_closefile(c->upfd);
        c->upfd = -1;
    }
    out_reset(c);
    if (c->fd >= 0) ls_closesock(c->fd);
    memset(c, 0, sizeof *c);
    c->fd = -1;
    c->upfd = -1;
    if (was_ws) broadcast_peers();
}

static void client_close(ls_client *c) {
    client_close_why(c, "未标注");
}

static void forward_signal(ls_client *target, const char *from_id, const char *msg) {
    size_t mlen = strlen(msg);
    char *out = (char *)malloc(mlen + ID_MAX + 32);
    if (!out) return;
    if (msg[0] == '{') {
        snprintf(out, mlen + ID_MAX + 32, "{\"from\":\"%s\",%s", from_id, msg + 1);
    } else {
        snprintf(out, mlen + ID_MAX + 32, "{\"from\":\"%s\"}", from_id);
    }
    ws_send_text(target, out);
    free(out);
}

/* ==========================================================================
 * 文件库
 * ========================================================================== */

static int path_join(const char *dir, const char *name, char *out, size_t outsz) {
    int n = snprintf(out, outsz, "%s/%s", dir, name);
    return (n > 0 && (size_t)n < outsz) ? 0 : -1;
}

static int file_exists(const char *path) {
    FILE *f = ls_fopen_rb(path);
    if (f) {
        fclose(f);
        return 1;
    }
    return 0;
}

static void unique_path(const char *name, char *out, size_t outsz) {
    char stem[NAME_MAX * 4];
    char ext[32];
    char *dot;
    int i;

    if (path_join(g_upload_dir, name, out, outsz) != 0) return;
    if (!file_exists(out)) return;

    snprintf(stem, sizeof stem, "%s", name);
    dot = strrchr(stem, '.');
    if (dot && dot != stem) {
        snprintf(ext, sizeof ext, "%s", dot);
        *dot = '\0';
    } else {
        ext[0] = '\0';
    }
    for (i = 1; i < 999; i++) {
        char candidate[NAME_MAX * 4];
        snprintf(candidate, sizeof candidate, "%s (%d)%s", stem, i, ext);
        if (path_join(g_upload_dir, candidate, out, outsz) != 0) return;
        if (!file_exists(out)) return;
    }
}

static uint64_t file_size(const char *path) {
#ifdef _WIN32
    struct _stat64 st;
    wchar_t *w = ls_utf8_to_wide(path);
    int r;
    if (!w) return 0;
    r = _wstat64(w, &st);
    free(w);
    if (r != 0) return 0;
#else
    struct stat st;
    if (stat(path, &st) != 0) return 0;
#endif
    return (uint64_t)st.st_size;
}

/* 分块上传的半成品目录。用子目录而不是和正式文件混在一起，
 * 这样文件列表里不会冒出没传完的东西。 */
static void parts_dir_path(char *out, size_t outsz) {
    snprintf(out, outsz, "%s/.lanshare-parts", g_upload_dir);
}

/* 半成品文件名取 name|size 的 SHA-1 十六进制。
 * 中文或带特殊字符的文件名在文件系统上容易出岔子，哈希后只剩 [0-9a-f]，
 * 同时保证"同名不同大小"互不干扰。 */
static void part_path_for(const char *name, uint64_t size, char *out, size_t outsz) {
    char key[NAME_MAX * 4 + 32];
    unsigned char dig[20];
    char hex[41];
    ls_sha1 ctx;
    int i;

    snprintf(key, sizeof key, "%s|%llu", name, (unsigned long long)size);
    sha1_init(&ctx);
    sha1_update(&ctx, key, strlen(key));
    sha1_final(&ctx, dig);
    for (i = 0; i < 20; i++) snprintf(hex + (size_t)i * 2, 3, "%02x", dig[i]);
    hex[40] = '\0';

    {
        char dir[PATH_MAX_LOCAL];
        parts_dir_path(dir, sizeof dir);
        snprintf(out, outsz, "%s/%s.part", dir, hex);
    }
}

/* 某个文件目前收了多少字节。没开始过就是 0。 */
static uint64_t part_received(const char *name, uint64_t size) {
    char p[PATH_MAX_LOCAL];
    uint64_t sz;
    part_path_for(name, size, p, sizeof p);
    if (!file_exists(p)) return 0;
    sz = file_size(p);
    if (size > 0 && sz > size) sz = size;
    return sz;
}

static void api_list_files(ls_client *c) {
    char *body;
    size_t cap = 8192, len = 0;
    uint64_t total = 0;
    int count = 0;

    body = (char *)malloc(cap);
    if (!body) return;
    len += (size_t)snprintf(body + len, cap - len, "{\"files\":[");

#ifdef _WIN32
    {
        WIN32_FIND_DATAW fd;
        HANDLE h;
        char pattern[PATH_MAX_LOCAL];
        wchar_t *wpattern;
        snprintf(pattern, sizeof pattern, "%s\\*", g_upload_dir);
        wpattern = ls_utf8_to_wide(pattern);
        if (wpattern) {
        h = FindFirstFileW(wpattern, &fd);
        free(wpattern);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                char full[PATH_MAX_LOCAL];
                char esc[NAME_MAX * 8];
                char szt[32];
                char cname[NAME_MAX * 4];
                uint64_t sz;
                unsigned long long mtime;
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                ls_wide_to_utf8(fd.cFileName, cname, sizeof cname);
                snprintf(full, sizeof full, "%s\\%s", g_upload_dir, cname);
                sz = file_size(full);
                if (len + 600 > cap) {
                    char *nb = (char *)realloc(body, cap * 2);
                    if (!nb) break;
                    cap *= 2;
                    body = nb;
                }
                json_escape(cname, esc, sizeof esc);
                human_size(sz, szt, sizeof szt);
                /* FILETIME 是 1601-01-01 起的 100ns 计数，减去到 Unix 纪元的偏移换秒 */
                mtime = 0;
                {
                    uint64_t ft = ((uint64_t)fd.ftLastWriteTime.dwHighDateTime << 32) |
                                  (uint64_t)fd.ftLastWriteTime.dwLowDateTime;
                    uint64_t sec = ft / 10000000ULL;
                    if (sec > 11644473600ULL) mtime = (unsigned long long)(sec - 11644473600ULL);
                }
                len += (size_t)snprintf(body + len, cap - len,
                    "%s{\"name\":\"%s\",\"size\":%llu,\"sizeText\":\"%s\",\"mtime\":%llu}",
                    count ? "," : "", esc, (unsigned long long)sz, szt, mtime);
                total += sz;
                count++;
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        }
    }
#else
    {
        DIR *d = opendir(g_upload_dir);
        if (d) {
            struct dirent *ent;
            while ((ent = readdir(d)) != NULL) {
                char full[PATH_MAX_LOCAL];
                struct stat st;
                char esc[NAME_MAX * 8];
                char szt[32];
                if (path_join(g_upload_dir, ent->d_name, full, sizeof full) != 0) continue;
                if (stat(full, &st) != 0) continue;
                if (!S_ISREG(st.st_mode)) continue;
                if (len + 600 > cap) {
                    char *nb = (char *)realloc(body, cap * 2);
                    if (!nb) break;
                    cap *= 2;
                    body = nb;
                }
                json_escape(ent->d_name, esc, sizeof esc);
                human_size((unsigned long long)st.st_size, szt, sizeof szt);
                len += (size_t)snprintf(body + len, cap - len,
                    "%s{\"name\":\"%s\",\"size\":%llu,\"sizeText\":\"%s\",\"mtime\":%llu}",
                    count ? "," : "", esc, (unsigned long long)st.st_size, szt,
                    (unsigned long long)((uint64_t)st.st_mtime * 1000ULL));
                total += (uint64_t)st.st_size;
                count++;
            }
            closedir(d);
        }
    }
#endif

    if (len + 64 > cap) {
        char *nb = (char *)realloc(body, cap + 128);
        if (nb) {
            body = nb;
            cap += 128;
        }
    }
    len += (size_t)snprintf(body + len, cap - len, "],\"total\":%llu}", (unsigned long long)total);
    queue_response(c, 200, "application/json; charset=utf-8", body, len);
    free(body);
}

/* 首页里有一个占位符（默认就是 null，纯内网直连）。若启动时通过
 * LAN_SHARE_TURN 提供了 TURN 配置（一段合法 JSON 数组文本），则把占位符
 * 整段替换成该 JSON，让前端（app.js 的 makeLane）把 TURN 加进 iceServers。
 * 这样 AP / 客户端隔离下设备间直连被丢弃时，仍可经自建 TURN 中继打通。 */

/* 在长度为 hlen 的字节串中查找子串 needle，返回首次出现的位置，找不到返回 NULL。 */
static const char *memstr(const char *hay, size_t hlen, const char *needle) {
    size_t nlen = strlen(needle);
    if (nlen == 0 || nlen > hlen) return NULL;
    for (size_t i = 0; i + nlen <= hlen; i++) {
        if (memcmp(hay + i, needle, nlen) == 0) return hay + i;
    }
    return NULL;
}

/* 返回新分配的缓冲（以 '\0' 结尾），*out_len 为内容长度（不含结尾 '\0'）；
 * 无占位符或未配置 TURN 时返回 NULL（调用方直接用原缓冲即可）。 */
static char *apply_turn_injection(const char *in, size_t in_len, size_t *out_len) {
    static const char TOKEN[] = "/*__TURN_CONFIG__*/ null";
    if (!g_turn_config || g_turn_config[0] == '\0') return NULL;
    const char *p = memstr(in, in_len, TOKEN);
    /* 排查用：曾出现"服务端说已启用、但页面里 window.__LAN_SHARE_TURN__
     * 仍是 null"，只能靠这行日志区分是"根本没调用"还是"调用了但没找到占位符"。
     * 输出到 stderr，CI 里随 /tmp/lanshare.log 一起捞出来。 */
    fprintf(stderr, "[turn-inject] cfg=%s body_len=%zu found=%d\n",
            g_turn_config ? g_turn_config : "(null)", in_len, p ? 1 : 0);
    if (!p) return NULL;
    size_t tok_len = sizeof(TOKEN) - 1;
    size_t cfg_len = strlen(g_turn_config);
    size_t new_len = in_len - tok_len + cfg_len;
    char *out = (char *)malloc(new_len + 1);
    if (!out) return NULL;
    size_t head = (size_t)(p - in);
    memcpy(out, in, head);
    memcpy(out + head, g_turn_config, cfg_len);
    memcpy(out + head + cfg_len, in + head + tok_len, in_len - head - tok_len);
    out[new_len] = '\0';
    *out_len = new_len;
    return out;
}

static void serve_static(ls_client *c, const char *rel) {
    char path[PATH_MAX_LOCAL];
    char *body;
    uint64_t sz;
    FILE *f;
    const char *ctype;
    size_t got;

    if (rel[0] == '\0' || strcmp(rel, "/") == 0) rel = "/index.html";
    if (strstr(rel, "..") != NULL) {
        queue_response(c, 404, "text/plain; charset=utf-8", "Not Found", 9);
        return;
    }
    if (strncmp(rel, "/api/", 5) == 0 || strcmp(rel, "/ws") == 0) {
        queue_response(c, 404, "text/plain; charset=utf-8", "Not Found", 9);
        return;
    }
    snprintf(path, sizeof path, "%s%s", g_web_dir, rel);
    if (!file_exists(path)) {
        /* 磁盘上没有就回退到编进二进制的前端。
         * 没有这层回退的话，只把可执行文件拷到路由器上（最常见的用法）
         * 打开页面就是 404 Not Found，而 /api/info 却是好的，非常迷惑。 */
        const ls_web_asset *hit = NULL;
        size_t k;
        for (k = 0; k < LS_WEB_ASSET_COUNT; k++) {
            if (strcmp(ls_web_assets[k].route, rel) == 0) {
                hit = &ls_web_assets[k];
                break;
            }
        }
        if (!hit) {
            queue_response(c, 404, "text/plain; charset=utf-8", "Not Found", 9);
            return;
        }
        if (strstr(rel, ".css")) ctype = "text/css; charset=utf-8";
        else if (strstr(rel, ".js")) ctype = "text/javascript; charset=utf-8";
        else ctype = "text/html; charset=utf-8";
        int is_html = (strncmp(ctype, "text/html", 9) == 0);
        if (is_html) {
            size_t inj_len = 0;
            char *inj = apply_turn_injection((const char *)hit->data, hit->len, &inj_len);
            if (inj) {
                queue_response(c, 200, ctype, inj, inj_len);
                free(inj);
                return;
            }
        }
        queue_response(c, 200, ctype, (const char *)hit->data, hit->len);
        return;
    }
    sz = file_size(path);
    if (sz > (uint64_t)8 * 1024 * 1024) {
        queue_response(c, 404, "text/plain; charset=utf-8", "Too Large", 9);
        return;
    }
    f = ls_fopen_rb(path);
    if (!f) {
        queue_response(c, 404, "text/plain; charset=utf-8", "Not Found", 9);
        return;
    }
    body = (char *)malloc((size_t)sz + 1);
    if (!body) {
        fclose(f);
        return;
    }
    got = fread(body, 1, (size_t)sz, f);
    fclose(f);
    body[got] = '\0';
    if (strstr(rel, ".css")) ctype = "text/css; charset=utf-8";
    else if (strstr(rel, ".js")) ctype = "text/javascript; charset=utf-8";
    else if (strstr(rel, ".svg")) ctype = "image/svg+xml";
    else ctype = "text/html; charset=utf-8";
    int is_html = (strncmp(ctype, "text/html", 9) == 0);
    if (is_html) {
        size_t inj_len = 0;
        char *inj = apply_turn_injection(body, got, &inj_len);
        if (inj) {
            queue_response(c, 200, ctype, inj, inj_len);
            free(inj);
            free(body);
            return;
        }
    }
    queue_response(c, 200, ctype, body, got);
    free(body);
}

/* range 是 Range 请求头的值（可能为 NULL 或空串）。
 * 支持单段 bytes=start[-end] 与 bytes=-suffix；多段区间忽略并走全量。 */
static int parse_byte_range(const char *range, uint64_t size,
                            uint64_t *start, uint64_t *end);

static void api_download(ls_client *c, const char *name, const char *range) {
    char safe[NAME_MAX * 4];
    char path[PATH_MAX_LOCAL];
    char head[640];
    char encoded[NAME_MAX * 12];
    uint64_t sz;
    uint64_t start = 0;
    uint64_t end = 0;
    uint64_t length;
    int has_range = 0;
    FILE *f;
    size_t i, o = 0;
    int hl;

    sanitize_name(name, safe, sizeof safe);
    if (path_join(g_upload_dir, safe, path, sizeof path) != 0) return;
    if (!file_exists(path)) {
        queue_response(c, 404, "text/plain; charset=utf-8", "Not Found", 9);
        return;
    }
    f = ls_fopen_rb(path);
    if (!f) {
        queue_response(c, 404, "text/plain; charset=utf-8", "Not Found", 9);
        return;
    }
    sz = file_size(path);

    if (range && range[0] != '\0') {
        has_range = parse_byte_range(range, sz, &start, &end);
    }
    if (has_range) {
        if (end > sz - 1) end = sz - 1;
        /* 起点越界，或者区间反了：按 RFC 7233 回 416 并带上真实长度 */
        if (start >= sz || start > end) {
            fclose(f);
            hl = snprintf(head, sizeof head,
                "HTTP/1.1 416 Range Not Satisfiable\r\n"
                "Content-Range: bytes */%llu\r\n"
                "Content-Length: 0\r\n"
                "Accept-Ranges: bytes\r\n"
                "Connection: keep-alive\r\n\r\n",
                (unsigned long long)sz);
            if (hl > 0 && (size_t)hl <= sizeof c->pend) {
                memcpy(c->pend, head, (size_t)hl);
                c->pendlen = (size_t)hl;
            }
            return;
        }
    }

    for (i = 0; safe[i] != '\0' && o + 4 < sizeof encoded; i++) {
        unsigned char ch = (unsigned char)safe[i];
        if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
            (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '.' || ch == '~') {
            encoded[o++] = (char)ch;
        } else {
            o += (size_t)snprintf(encoded + o, sizeof encoded - o, "%%%02X", ch);
        }
    }
    encoded[o] = '\0';

    /* 响应头必须走 c->pend（flush_out 里 pend 优先级最高，先于 out 发出）。
     * 不能图省事用 queue_mem 再改 is_mem：那样 flush_out 会直接进文件分支，
     * 响应头一个字节都不会发出去，客户端收到的是没有状态行的裸数据。 */
    out_reset(c);

    if (has_range) {
        length = end - start + 1;
#ifdef _WIN32
        _fseeki64(f, (__int64)start, SEEK_SET);
#else
        fseek(f, (long)start, SEEK_SET);
#endif
        hl = snprintf(head, sizeof head,
            "HTTP/1.1 206 Partial Content\r\n"
            "Content-Type: application/octet-stream\r\n"
            "Content-Length: %llu\r\n"
            "Content-Range: bytes %llu-%llu/%llu\r\n"
            "Content-Disposition: attachment; filename*=UTF-8''%s\r\n"
            "Accept-Ranges: bytes\r\n"
            "Cache-Control: no-store\r\n"
            "Connection: keep-alive\r\n\r\n",
            (unsigned long long)length,
            (unsigned long long)start, (unsigned long long)end, (unsigned long long)sz,
            encoded);
    } else {
        length = sz;
        hl = snprintf(head, sizeof head,
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: application/octet-stream\r\n"
            "Content-Length: %llu\r\n"
            "Content-Disposition: attachment; filename*=UTF-8''%s\r\n"
            "Accept-Ranges: bytes\r\n"
            "Cache-Control: no-store\r\n"
            "Connection: keep-alive\r\n\r\n",
            (unsigned long long)sz, encoded);
    }
    if (hl <= 0) {
        fclose(f);
        return;
    }

    if ((size_t)hl <= sizeof c->pend) {
        memcpy(c->pend, head, (size_t)hl);
        c->pendlen = (size_t)hl;
    }
    c->out.file = f;
    c->out.is_mem = 0;
    c->out.total = length;
    c->out.sent = 0;
    c->out.has = 1;
}

static void api_delete(ls_client *c, const char *name) {
    char safe[NAME_MAX * 4];
    char path[PATH_MAX_LOCAL];
    char body[256];
    sanitize_name(name, safe, sizeof safe);
    if (path_join(g_upload_dir, safe, path, sizeof path) != 0) return;
    if (!file_exists(path)) {
        queue_response(c, 404, "application/json; charset=utf-8", "{\"error\":\"not found\"}", 20);
        return;
    }
    if (ls_remove_file(path) != 0) {
        queue_response(c, 500, "application/json; charset=utf-8", "{\"error\":\"remove failed\"}", 24);
        return;
    }
    snprintf(body, sizeof body, "{\"ok\":true,\"name\":\"%s\"}", safe);
    queue_response(c, 200, "application/json; charset=utf-8", body, strlen(body));
}

/* ==========================================================================
 * HTTP 解析
 * ========================================================================== */

static int extract_query_value(const char *query, const char *key, char *out, size_t outsz) {
    const char *p = query;
    size_t klen = strlen(key);
    if (!query) return 0;
    while (*p != '\0') {
        if ((p == query || p[-1] == '&') && strncmp(p, key, klen) == 0 && p[klen] == '=') {
            const char *v = p + klen + 1;
            const char *e = strchr(v, '&');
            size_t vlen = e ? (size_t)(e - v) : strlen(v);
            char *raw = (char *)malloc(vlen + 1);
            if (!raw) return 0;
            memcpy(raw, v, vlen);
            raw[vlen] = '\0';
            url_decode(raw, out, outsz);
            free(raw);
            return 1;
        }
        p++;
    }
    return 0;
}

/* 解析单段 bytes Range。返回 1 表示语法有效，0 表示忽略不支持/畸形值。 */
static int parse_byte_range(const char *range, uint64_t size,
                            uint64_t *start, uint64_t *end) {
    const char *p;
    uint64_t first = 0, second = 0;
    int have_first = 0, have_second = 0;

    if (strncmp(range, "bytes=", 6) != 0) return 0;
    p = range + 6;
    if (*p == '-') {
        p++;
        if (*p < '0' || *p > '9') return 0;
        while (*p >= '0' && *p <= '9') {
            unsigned digit = (unsigned)(*p++ - '0');
            if (second > (UINT64_MAX - digit) / 10) return 0;
            second = second * 10 + digit;
        }
        if (*p != '\0') return 0;
        if (size == 0 || second == 0) {
            *start = size;
            *end = size ? size - 1 : 0;
        } else {
            *start = second >= size ? 0 : size - second;
            *end = size - 1;
        }
        return 1;
    }

    while (*p >= '0' && *p <= '9') {
        unsigned digit = (unsigned)(*p++ - '0');
        if (first > (UINT64_MAX - digit) / 10) return 0;
        first = first * 10 + digit;
        have_first = 1;
    }
    if (!have_first || *p++ != '-') return 0;
    if (*p != '\0') {
        while (*p >= '0' && *p <= '9') {
            unsigned digit = (unsigned)(*p++ - '0');
            if (second > (UINT64_MAX - digit) / 10) return 0;
            second = second * 10 + digit;
            have_second = 1;
        }
        if (!have_second || *p != '\0') return 0;
    }

    *start = first;
    *end = have_second ? second : (size ? size - 1 : 0);
    return 1;
}

static const char *find_header(const char *req, size_t len, const char *key) {
    size_t klen = strlen(key);
    size_t i = 0;
    while (i + klen + 1 < len) {
        if ((i == 0 || req[i - 1] == '\n') && req[i + klen] == ':' &&
            ls_strncasecmp(req + i, key, klen) == 0) {
            return req + i + klen + 1;
        }
        i++;
    }
    return NULL;
}

static void header_value(const char *p, const char *end, char *out, size_t outsz) {
    size_t o = 0;
    if (!p) {
        if (outsz) out[0] = '\0';
        return;
    }
    while (p < end && (*p == ' ' || *p == '\t')) p++;
    while (p < end && *p != '\r' && *p != '\n' && o + 1 < outsz) out[o++] = *p++;
    while (o > 0 && (out[o - 1] == ' ' || out[o - 1] == '\t')) o--;
    out[o] = '\0';
}

static void finish_upload(ls_client *c) {
    char body[512];
    char szt[32];
    char base[PATH_MAX_LOCAL];
    char esc[NAME_MAX * 8];
    const char *slash, *bs, *b;
    uint64_t sz;
    int n;
    int done = 1;

    if (c->upfd >= 0) {
        ls_closefile(c->upfd);
        c->upfd = -1;
    }
    c->mode = MODE_HTTP;

    /* 分块上传：本次收完可能还没到完整大小。
     * 没收满就回进度、把 .part 留着等下一次续传，绝不落进正式文件列表。 */
    if (c->up_chunked) {
        uint64_t total = c->up_base + c->up_reqlen;
        if (total < c->up_size) {
            char escn[NAME_MAX * 8];
            json_escape(c->up_final, escn, sizeof escn);
            human_size(total, szt, sizeof szt);
            n = snprintf(body, sizeof body,
                "{\"ok\":true,\"name\":\"%s\",\"size\":%llu,\"sizeText\":\"%s\","
                "\"done\":false,\"received\":%llu}",
                escn, (unsigned long long)total, szt, (unsigned long long)total);
            if (n > 0) {
                queue_response(c, 200, "application/json; charset=utf-8", body, (size_t)n);
            }
            return;
        }
        /* 收满了：把半成品改名成正式文件（重名会自动加 (1)） */
        {
            char finalpath[PATH_MAX_LOCAL];
            unique_path(c->up_final, finalpath, sizeof finalpath);
            if (ls_rename_file(c->upname, finalpath) != 0) {
                queue_response(c, 500, "application/json; charset=utf-8",
                               "{\"error\":\"落盘失败\"}", 21);
                return;
            }
            snprintf(c->upname, sizeof c->upname, "%s", finalpath);
        }
    }

    sz = file_size(c->upname);

    slash = strrchr(c->upname, '/');
    bs = strrchr(c->upname, '\\');
    b = c->upname;
    if (slash && (!bs || slash > bs)) b = slash + 1;
    else if (bs) b = bs + 1;
    snprintf(base, sizeof base, "%s", b);
    json_escape(base, esc, sizeof esc);
    human_size(sz, szt, sizeof szt);

    n = snprintf(body, sizeof body,
        "{\"ok\":true,\"name\":\"%s\",\"size\":%llu,\"sizeText\":\"%s\",\"done\":%s}",
        esc, (unsigned long long)sz, szt, done ? "true" : "false");
    if (n > 0) queue_response(c, 200, "application/json; charset=utf-8", body, (size_t)n);
}

/* 返回已消耗的字节数；0 表示请求还不完整；-1 表示应关闭连接。 */
static long process_request(ls_client *c, const char *req, size_t len) {
    char method[16];
    char target[PATH_MAX_LOCAL];
    char query[PATH_MAX_LOCAL];
    char path[PATH_MAX_LOCAL];
    char clen_str[32];
    char wskey[128];
    const char *sp1, *sp2, *q;
    uint64_t content_len = 0;
    size_t header_end = 0;
    size_t i, mlen, tlen;

    method[0] = '\0'; target[0] = '\0'; query[0] = '\0'; path[0] = '\0';
    clen_str[0] = '\0'; wskey[0] = '\0';

    for (i = 0; i + 3 < len; i++) {
        if (req[i] == '\r' && req[i + 1] == '\n' && req[i + 2] == '\r' && req[i + 3] == '\n') {
            header_end = i + 4;
            break;
        }
    }
    if (header_end == 0) {
        if (len >= HEAD_BUF) return -1;
        return 0;
    }

    sp1 = (const char *)memchr(req, ' ', len);
    if (!sp1) return -1;
    mlen = (size_t)(sp1 - req);
    if (mlen >= sizeof method) mlen = sizeof method - 1;
    memcpy(method, req, mlen);
    method[mlen] = '\0';

    sp2 = (const char *)memchr(sp1 + 1, ' ', len - (size_t)(sp1 + 1 - req));
    if (!sp2) return -1;
    tlen = (size_t)(sp2 - sp1 - 1);
    if (tlen >= sizeof target) tlen = sizeof target - 1;
    memcpy(target, sp1 + 1, tlen);
    target[tlen] = '\0';

    q = strchr(target, '?');
    if (q) {
        size_t plen = (size_t)(q - target);
        if (plen >= sizeof path) plen = sizeof path - 1;
        memcpy(path, target, plen);
        path[plen] = '\0';
        url_decode(q + 1, query, sizeof query);
    } else {
        url_decode(target, path, sizeof path);
        query[0] = '\0';
    }

    header_value(find_header(req, len, "Content-Length"), req + len, clen_str, sizeof clen_str);
    header_value(find_header(req, len, "Sec-WebSocket-Key"), req + len, wskey, sizeof wskey);
    if (clen_str[0] != '\0') content_len = strtoull(clen_str, NULL, 10);

    if (wskey[0] != '\0') {
        static const char GUID[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
        char combined[256];
        unsigned char digest[20];
        char b64[64];
        char resp[512];
        unsigned char frame[512];
        char selfmsg[384];
        char escn[NAME_MAX * 4];
        char *combined_out;
        ls_sha1 ctx;
        int n;
        size_t flen;

        snprintf(combined, sizeof combined, "%s%s", wskey, GUID);
        sha1_init(&ctx);
        sha1_update(&ctx, combined, strlen(combined));
        sha1_final(&ctx, digest);
        b64_encode(digest, 20, b64);

        n = snprintf(resp, sizeof resp,
            "HTTP/1.1 101 Switching Protocols\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Accept: %s\r\n\r\n", b64);
        if (n <= 0) return -1;

        json_escape(c->name, escn, sizeof escn);
        snprintf(selfmsg, sizeof selfmsg, "{\"type\":\"self\",\"id\":\"%s\",\"name\":\"%s\"}",
                 c->id, escn);
        flen = ws_frame_build(0x1, selfmsg, strlen(selfmsg), frame, sizeof frame);

        combined_out = (char *)malloc((size_t)n + flen + 1);
        if (!combined_out) return -1;
        memcpy(combined_out, resp, (size_t)n);
        if (flen) memcpy(combined_out + n, frame, flen);

        queue_mem(c, combined_out, (size_t)n + flen);
        free(combined_out);

        c->mode = MODE_WS;
        c->wlen = 0;
        c->frag_op = 0;
        c->fraglen = 0;
        c->trace_text = 6;
        c->trace_bin = 16;
        /* 注意：这里绝不能清 c->hlen。调用方靠 `hlen -= used` 推进读指针，
         * 清零会导致下溢成巨大 size_t，随后越界写直接段错误。
         * 101 响应占用了输出缓冲，peers 广播必须等它发完再补发，
         * 否则会把握手响应覆盖掉，浏览器会认为握手失败。 */
        c->pending_hello = 1;
        fprintf(stderr, "[lan-share] WS 就绪 id=%s ip=%s（当前 WS 连接 %d 个）\n",
                c->id, c->ip, count_ws_clients());
        fflush(stderr);
        /* 只吃掉请求头。原来返回 len（"整段都算我吃了"），如果客户端把第一帧
         * 和握手塞在同一个 TCP 段里带过来，那部分字节会被调用方的 memmove
         * 直接丢掉——第一帧没了，此后整个帧流错位，后面全是莫名其妙的报错。
         * 返回 header_end 让调用方把剩余字节挪到缓冲区开头，再由 WS 通路接手
         * （见 client_readable 里 MODE_WS 的分支）。 */
        return (long)header_end;
    }

    if (strcmp(method, "GET") == 0 && strcmp(path, "/api/files") == 0) {
        api_list_files(c);
        return (long)len;
    }
    if (strcmp(method, "GET") == 0 && strcmp(path, "/api/download") == 0) {
        char name[NAME_MAX * 4];
        char rangev[64];
        name[0] = '\0';
        rangev[0] = '\0';
        extract_query_value(query, "name", name, sizeof name);
        header_value(find_header(req, len, "Range"), req + len, rangev, sizeof rangev);
        api_download(c, name, rangev);
        return (long)len;
    }
    if (strcmp(method, "DELETE") == 0 && strcmp(path, "/api/files") == 0) {
        char name[NAME_MAX * 4];
        name[0] = '\0';
        extract_query_value(query, "name", name, sizeof name);
        api_delete(c, name);
        return (long)len;
    }
    if (strcmp(method, "GET") == 0 && strcmp(path, "/api/info") == 0) {
        char body[512];
        int n = snprintf(body, sizeof body,
            "{\"peers\":%d,\"uploadDir\":\"%s\",\"engine\":\"c\"}",
            count_ws_clients(), g_upload_dir);
        if (n > 0) queue_response(c, 200, "application/json; charset=utf-8", body, (size_t)n);
        return (long)len;
    }
    /* 续传前先问：这个文件已经收了多少字节 */
    if (strcmp(method, "GET") == 0 && strcmp(path, "/api/upload-status") == 0) {
        char rawname[NAME_MAX * 4];
        char safe[NAME_MAX * 4];
        char sizestr[32];
        char esc[NAME_MAX * 8];
        char body[512];
        uint64_t fsize;
        int n;

        rawname[0] = '\0';
        sizestr[0] = '\0';
        extract_query_value(query, "name", rawname, sizeof rawname);
        extract_query_value(query, "size", sizestr, sizeof sizestr);
        sanitize_name(rawname, safe, sizeof safe);
        fsize = strtoull(sizestr, NULL, 10);
        json_escape(safe, esc, sizeof esc);

        n = snprintf(body, sizeof body,
            "{\"ok\":true,\"name\":\"%s\",\"size\":%llu,\"received\":%llu}",
            esc, (unsigned long long)fsize,
            (unsigned long long)part_received(safe, fsize));
        if (n > 0) queue_response(c, 200, "application/json; charset=utf-8", body, (size_t)n);
        return (long)len;
    }

    if (strcmp(method, "POST") == 0 && strcmp(path, "/api/upload") == 0) {
        char rawname[NAME_MAX * 4];
        char safe[NAME_MAX * 4];
        char fullpath[PATH_MAX_LOCAL];
        char sizestr[32];
        char offstr[32];
        size_t bodylen = (len > header_end) ? (len - header_end) : 0;
        uint64_t fsize = 0;
        uint64_t foffset = 0;
        int chunked = 0;
        int fd;

        rawname[0] = '\0';
        sizestr[0] = '\0';
        offstr[0] = '\0';
        extract_query_value(query, "name", rawname, sizeof rawname);
        sanitize_name(rawname, safe, sizeof safe);

        /* 带 size 或 offset 任意一个就走分块续传；都不带是老的整包上传，行为不变。 */
        if (extract_query_value(query, "size", sizestr, sizeof sizestr)) {
            chunked = 1;
            fsize = strtoull(sizestr, NULL, 10);
        }
        if (extract_query_value(query, "offset", offstr, sizeof offstr)) {
            chunked = 1;
            foffset = strtoull(offstr, NULL, 10);
        }

        if (chunked) {
            uint64_t existing = part_received(safe, fsize);
            if (foffset > existing) {
                /* 客户端要跳到还没收到的位置。顺序写做不到，
                 * 放过去会产出一个中间空洞的文件，直接拒绝让它按实际进度重来。 */
                char body[128];
                int n = snprintf(body, sizeof body,
                    "{\"error\":\"offset 超出已接收范围\",\"received\":%llu}",
                    (unsigned long long)existing);
                if (n > 0) {
                    queue_response(c, 409, "application/json; charset=utf-8", body, (size_t)n);
                }
                return (long)len;
            }
            part_path_for(safe, fsize, fullpath, sizeof fullpath);
            c->up_chunked = 1;
            c->up_size = fsize;
            c->up_base = foffset;
            snprintf(c->up_final, sizeof c->up_final, "%s", safe);
        } else {
            unique_path(safe, fullpath, sizeof fullpath);
            c->up_chunked = 0;
            c->up_size = 0;
            c->up_base = 0;
            c->up_final[0] = '\0';
        }
        c->up_reqlen = content_len;

        fd = ls_openwrite_at(fullpath, foffset, chunked);
        if (fd < 0) {
            queue_response(c, 500, "application/json; charset=utf-8", "{\"error\":\"create failed\"}", 24);
            return (long)len;
        }
        if (bodylen > 0) {
            if (ls_write(fd, req + header_end, bodylen) != (long)bodylen) {
                ls_closefile(fd);
                queue_response(c, 500, "application/json; charset=utf-8", "{\"error\":\"write failed\"}", 23);
                return (long)len;
            }
        }
        snprintf(c->upname, sizeof c->upname, "%s", fullpath);
        c->upfd = fd;
        c->upleft = (content_len > (uint64_t)bodylen) ? (content_len - (uint64_t)bodylen) : 0;
        if (c->upleft == 0) {
            finish_upload(c);
        } else {
            c->mode = MODE_UPLOAD;
        }
        return (long)len;
    }
    if (strcmp(method, "GET") == 0) {
        serve_static(c, path);
        return (long)len;
    }

    queue_response(c, 405, "application/json; charset=utf-8", "{\"error\":\"method not allowed\"}", 31);
    return (long)len;
}

/* ==========================================================================
 * WebSocket 消息处理
 * ========================================================================== */

static void handle_ws_message(ls_client *c, const char *msg, size_t len) {
    char type[32];
    char to[ID_MAX];
    char nm[NAME_MAX];
    int i;

    (void)len;
    type[0] = '\0';
    if (!json_get_str(msg, "type", type, sizeof type)) return;

    if (strcmp(type, "rename") == 0) {
        nm[0] = '\0';
        if (json_get_str(msg, "name", nm, sizeof nm) && nm[0] != '\0') {
            char selfmsg[384];
            char escn[NAME_MAX * 4];
            snprintf(c->name, sizeof c->name, "%s", nm);
            /* 记住这个名字，让对端下次连上来还是它 */
            name_mem_set(c->ip, nm);
            json_escape(c->name, escn, sizeof escn);
            snprintf(selfmsg, sizeof selfmsg, "{\"type\":\"self\",\"id\":\"%s\",\"name\":\"%s\"}",
                     c->id, escn);
            ws_send_text(c, selfmsg);
            broadcast_peers();
        }
        return;
    }

    if (strcmp(type, "signal") == 0) {
        to[0] = '\0';
        if (!json_get_str(msg, "to", to, sizeof to)) return;
        for (i = 0; i < MAX_CLIENTS; i++) {
            if (!g_clients[i].used || g_clients[i].mode != MODE_WS) continue;
            if (strcmp(g_clients[i].id, to) == 0) {
                forward_signal(&g_clients[i], c->id, msg);
                return;
            }
        }
        ws_send_text(c, "{\"type\":\"signal-lost\"}");
    }
}

/* 一条完整的 WS 消息该去哪：文本 = 信令 JSON，二进制 = 兜底通道的字节。
 * opcode 是这条消息的**首帧** opcode（分片消息也一样），分片拼接由调用方负责，
 * 所以到这里必须已经是一整条消息——二进制尤其重要，因为下游要靠载荷开头的
 * [idlen][id] 前缀决定转发给谁，半条消息的载荷前缀是坏的。 */
static void handle_ws_data_frame(ls_client *c, int opcode,
                                 const unsigned char *payload, size_t plen) {
    if (opcode == 0x1) {
        /* 单线程服务，handle_ws_message 不会绕回 ws_consume，静态缓冲安全；
         * 用它省掉 128KB 的栈占用（原来每收一条文本帧都要在栈上开一份）。 */
        static char tmp[WS_BUF + 1];
        memcpy(tmp, payload, plen);
        tmp[plen] = '\0';
        handle_ws_message(c, tmp, plen);
    } else {
        forward_binary(c, payload, plen);
    }
}

static void ws_consume(ls_client *c) {
    for (;;) {
        unsigned char *b = c->wbuf;
        size_t avail = c->wlen;
        unsigned char b0, b1;
        int fin, opcode, masked;
        uint64_t plen;
        size_t off;
        unsigned char mask[4];
        unsigned char *payload;
        size_t i;

        if (avail < 2) return;
        b0 = b[0];
        b1 = b[1];
        fin = (b0 & 0x80) != 0;
        opcode = (int)(b0 & 0x0f);
        masked = (b1 & 0x80) != 0;
        plen = (uint64_t)(b1 & 0x7f);
        off = 2;

        if (plen == 126) {
            if (avail < 4) return;
            plen = ((uint64_t)b[2] << 8) | (uint64_t)b[3];
            off = 4;
        } else if (plen == 127) {
            if (avail < 10) return;
            plen = 0;
            for (i = 0; i < 8; i++) plen = (plen << 8) | (uint64_t)b[2 + i];
            off = 10;
        }
        if (plen > WS_BUF) {
            client_close_why(c, "单帧超限");
            return;
        }
        mask[0] = mask[1] = mask[2] = mask[3] = 0;
        if (masked) {
            if (avail < off + 4) return;
            memcpy(mask, b + off, 4);
            off += 4;
        }
        if (avail < off + (size_t)plen) return;

        payload = b + off;
        if (masked) {
            for (i = 0; i < (size_t)plen; i++) payload[i] ^= mask[i % 4];
        }

        /* 前若干帧逐帧打出来。目的很具体：确认"收到的帧流到底是什么形状"——
         * 是不是分片的、首片和续帧各多大、有没有因为错位读出莫名其妙的 op。
         * 只看到"分片缓冲超限"这种结果级报错，是没法反推上面这些的。 */
        {
            int *budget = (opcode == 0x2 || opcode == 0x0) ? &c->trace_bin : &c->trace_text;
            if (*budget > 0) {
                (*budget)--;
                fprintf(stderr, "[lan-share] 帧 %s op=0x%x fin=%d len=%llu%s\n",
                        c->id, opcode, fin, (unsigned long long)plen,
                        *budget == 0 ? "（该类帧后续不再逐帧打印）" : "");
                fflush(stderr);
            }
        }

        if (opcode == 0x8) {
            /* 对端说了再见：按规范回一个关闭帧再关，否则它看到的是 1006
             * （异常断开），和网络掉线长得一模一样，查起来白费功夫。 */
            unsigned char bye[16];
            size_t bl = ws_frame_build(0x8, NULL, 0, bye, sizeof bye);
            if (bl) {
                (void)send(c->fd, (const char *)bye, (int)bl, 0);
            }
            client_close_why(c, "收到对端关闭帧");
            return;
        }

        if (opcode == 0x0) {
            /* 续帧。只有前面确实开了一段分片消息才有意义；
             * 没有起始帧的续帧是协议错误，丢掉就好，别为一条坏帧断链。 */
            if (c->frag_op != 0) {
                if ((size_t)plen >= WS_BUF - c->fraglen) {
                    client_close_why(c, "分片缓冲超限");
                    return;
                }
                memcpy(c->frag + c->fraglen, payload, (size_t)plen);
                c->fraglen += (size_t)plen;
                if (fin) {
                    int op = c->frag_op;
                    c->frag_op = 0;
                    handle_ws_data_frame(c, op, c->frag, c->fraglen);
                    c->fraglen = 0;
                }
            }
        } else if (opcode == 0x1 || opcode == 0x2) {
            if (fin) {
                /* 完整消息。上一段落了一半的分片消息按协议不该出现，
                 * 丢掉半截继续——不能因为对端发错一帧就断掉整条连接。 */
                c->frag_op = 0;
                c->fraglen = 0;
                handle_ws_data_frame(c, opcode, payload, (size_t)plen);
            } else {
                /* 分片消息的第一片。
                 *
                 * 关键：二进制（0x2）也会分片，必须和文本一样攒起来。
                 * 原来只认文本首帧，二进制的续帧全落进 frag 又永远等不到
                 * frag_op==0x1 的清零条件，于是 fraglen 一路涨到 WS_BUF、
                 * 把整条连接关掉——T5 的"分片缓冲超限"就是这么来的。
                 * 网上那句"浏览器一次 send 就是一帧"是错的：Chrome 发大消息
                 * 时会自己切成多片（首片带原 opcode，其余是续帧）。 */
                if ((size_t)plen >= WS_BUF) {
                    client_close_why(c, "分片首片就超限");
                    return;
                }
                c->frag_op = opcode;
                memcpy(c->frag, payload, (size_t)plen);
                c->fraglen = (size_t)plen;
            }
        } else if (opcode == 0x9) {
            unsigned char pong[16];
            size_t pl = ws_frame_build(0xA, NULL, 0, pong, sizeof pong);
            if (pl) queue_mem(c, (const char *)pong, pl);
        }

        memmove(c->wbuf, c->wbuf + off + (size_t)plen, avail - (off + (size_t)plen));
        c->wlen = avail - (off + (size_t)plen);
    }
}

/* ==========================================================================
 * 读事件
 * ========================================================================== */

static void client_readable(ls_client *c) {
    char buf[IO_BUF];
    int rounds;

    /* 一次可读事件就把 socket 里已有的数据尽量读完。
     * 原来只 recv 一次、而且一次最多 8KB：客户端一个 4MB 的分块发过来，
     * 要几十万次 select 唤醒 + recv 才吃得完，路由器那种单核 MIPS/ARM CPU
     * 直接被打满，吞吐就卡在几 MB/s。现在循环到 EWOULDBLOCK。
     *
     * 但每次收多少要看模式，不能一律 IO_BUF：
     *   HTTP 态——hbuf 只有 HEAD_BUF(8KB)，头还没解析完时多收的字节全是 body，
     *     塞进 hbuf 会撑爆头缓冲、被当成畸形请求直接关连接（上传第一个分块时
     *     必现，症状是客户端 "socket hang up"）。所以一次只收"还放得下"的量，
     *     等 process_request 把模式切成 MODE_UPLOAD 之后再按 IO_BUF 尽量多收。
     *   WS 态——wbuf 只有 WS_BUF 那么大（同时也是单帧上限），同理按剩余空间收。
     *   UPLOAD 态——数据直接写盘，没有中间缓冲，放心按 IO_BUF 收。 */
    for (rounds = 0; rounds < 64; rounds++) {
        size_t want;
        int n;

        if (c->mode == MODE_HTTP) want = HEAD_BUF - c->hlen;
        else if (c->mode == MODE_WS) want = WS_BUF - c->wlen;
        else want = IO_BUF;
        if (want == 0 || want > IO_BUF) want = IO_BUF;

        n = (int)recv(c->fd, buf, want, 0);

        if (n == 0) {
            client_close_why(c, "对端 FIN（recv 返回 0）");
            return;
        }
        if (n < 0) {
            if (would_block()) return;
            client_close_why(c, "recv 出错");
            return;
        }

        if (c->mode == MODE_UPLOAD) {
            size_t w = (size_t)n;
            if ((uint64_t)w > c->upleft) w = (size_t)c->upleft;
            if (ls_write(c->upfd, buf, w) != (long)w) {
                client_close_why(c, "上传写盘失败");
                return;
            }
            c->upleft -= (uint64_t)w;
            if (c->upleft == 0) {
                finish_upload(c);
                return;
            }
        } else if (c->mode == MODE_WS) {
            if (c->wlen + (size_t)n > WS_BUF) {
                client_close_why(c, "WS 接收缓冲放不下");
                return;
            }
            memcpy(c->wbuf + c->wlen, buf, (size_t)n);
            c->wlen += (size_t)n;
            ws_consume(c);
        } else {
            /* HTTP：先攒进头缓冲再解析。process_request 会一次性吃掉整个缓冲区
             * （连同跟着头一起来的 body），所以正常情况下 hbuf 会被清空。 */
            if (c->hlen + (size_t)n > HEAD_BUF) {
                client_close_why(c, "HTTP 头缓冲超限");
                return;
            }
            memcpy(c->hbuf + c->hlen, buf, (size_t)n);
            c->hlen += (size_t)n;
            c->hbuf[c->hlen] = '\0';
            for (;;) {
                long used = process_request(c, c->hbuf, c->hlen);
                if (used < 0) {
                    client_close_why(c, "请求解析/响应失败");
                    return;
                }
                if (used == 0) return;   /* 头还没收全，等下一次可读 */
                memmove(c->hbuf, c->hbuf + used, c->hlen - (size_t)used);
                c->hlen -= (size_t)used;
                c->hbuf[c->hlen] = '\0';
                if (c->mode == MODE_WS) {
                    /* 升级成功。请求头后面可能紧跟着帧字节（客户端把握手和
                     * 第一帧塞进同一个 TCP 段），它们现在躺在 hbuf 里，必须
                     * 交给 wbuf——否则第一帧被静默吞掉，之后整个帧流错位。 */
                    if (c->hlen) {
                        if (c->wlen + c->hlen > WS_BUF) {
                            client_close_why(c, "WS 接收缓冲放不下");
                            return;
                        }
                        memcpy(c->wbuf + c->wlen, c->hbuf, c->hlen);
                        c->wlen += c->hlen;
                        c->hlen = 0;
                    }
                    ws_consume(c);
                    return;
                }
                if (c->hlen == 0) return;
                if (c->mode != MODE_HTTP) return;   /* MODE_UPLOAD：本体已进写盘通路 */
            }
        }

        /* 没读满说明 socket 基本空了，不必再试 */
        if ((size_t)n < want) return;
    }
}

/* ==========================================================================
 * 主程序
 * ========================================================================== */

static void accept_all(int listen_fd) {
    for (;;) {
        struct sockaddr_in addr;
        socklen_t alen = sizeof addr;
        int fd = (int)accept(listen_fd, (struct sockaddr *)&addr, &alen);
        int i;
        if (fd < 0) return;
        for (i = 0; i < MAX_CLIENTS; i++) {
            if (!g_clients[i].used) break;
        }
        if (i >= MAX_CLIENTS) {
            ls_closesock(fd);
            return;
        }
        set_nonblock(fd);
        set_nodelay(fd);
        memset(&g_clients[i], 0, sizeof g_clients[i]);
        g_clients[i].used = 1;
        g_clients[i].fd = fd;
        g_clients[i].mode = MODE_HTTP;
        g_clients[i].upfd = -1;
        g_seq++;
        make_id(g_clients[i].id, sizeof g_clients[i].id);
        {
            const char *r = inet_ntop(AF_INET, &addr.sin_addr, g_clients[i].ip, sizeof g_clients[i].ip);
            if (!r) g_clients[i].ip[0] = '\0';
        }
        /* 同一个 IP 之前用过名字就复用，别每次连接都换一个。
         * IP 必须先填好才能查记忆表，所以这段在 inet_ntop 之后。 */
        {
            const char *remembered = name_mem_get(g_clients[i].ip);
            if (remembered) {
                snprintf(g_clients[i].name, sizeof g_clients[i].name, "%s", remembered);
            } else {
                random_name(g_clients[i].name, sizeof g_clients[i].name);
            }
        }
    }
}

/* 创建上传目录。已存在时各平台都会返回失败，这里不关心返回值。 */
static void ensure_upload_dir(void) {
    char parts[PATH_MAX_LOCAL];
#ifdef _WIN32
    _mkdir(g_upload_dir);
#else
    mkdir(g_upload_dir, 0755);
#endif
    /* 断点续传的半成品目录。分块上传第一次用到它，
     * 不建的话续传会卡在"打不开文件"上。 */
    parts_dir_path(parts, sizeof parts);
#ifdef _WIN32
    _mkdir(parts);
#else
    mkdir(parts, 0755);
#endif
}

static void usage(const char *prog) {
    fprintf(stderr,
        "用法: %s [-p 端口] [-d 文件库目录] [-w 前端目录]\n"
        "\n"
        "  -p  监听端口，默认 8080\n"
        "  -d  文件库落盘目录，默认 ./uploads（路由器上建议指到外接存储）\n"
        "  -w  前端静态文件目录，默认 ./public\n"
        "  -h  显示本帮助\n"
        "\n"
        "示例:\n"
        "  %s -p 8080 -d /mnt/sda1/share -w /usr/share/lan-share\n",
        prog, prog);
}

/* ==========================================================================
 * 局域网 STUN（自反射候选）——服务端兼任，零配置
 *
 * 为什么需要它：Chrome 默认把 host 候选伪装成 mDNS .local 名字，对端必须靠
 * 组播（224.0.0.251:5353）解析出真实 IP。普通家用路由器默认开 IGMP Snooping /
 * 组播增强，无线侧经常收不到组播，候选就永远解析不出来，ICE 一条都建不起来——
 * 表现就是"有线↔有线通，无线↔无线、有线↔无线都不通"，而且和应用层没关系。
 *
 * 关键点是 Chrome **不对 srflx 候选做 mDNS 伪装**：只要有一个可达的 STUN，
 * 客户端就能拿到"自己真实内网 IP:端口"的候选，直连不依赖组播。
 * 把 STUN 做进服务端（UDP 与 TCP 同端口号互不冲突），局域网里就永远有一个
 * 可达的 STUN：不依赖外网（stun.l.google.com 在国内本来就不可达）、不需要
 * 部署 coturn、纯离线也能用。
 * ========================================================================== */

#define STUN_MAGIC      0x2112A442u
#define STUN_FP_XOR     0x5354554Eu

/* STUN 指纹用的 CRC32（IEEE 802.3，反射多项式 0xEDB88320） */
static unsigned long stun_crc32(const unsigned char *p, size_t n) {
    unsigned long crc = 0xFFFFFFFFu;
    size_t i;
    int b;
    for (i = 0; i < n; i++) {
        crc ^= p[i];
        for (b = 0; b < 8; b++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (unsigned long)(-(long)(crc & 1u)));
        }
    }
    return crc ^ 0xFFFFFFFFu;
}

static void put16(unsigned char *p, unsigned short v) {
    p[0] = (unsigned char)((v >> 8) & 0xFF);
    p[1] = (unsigned char)(v & 0xFF);
}

static void put32(unsigned char *p, unsigned long v) {
    p[0] = (unsigned char)((v >> 24) & 0xFF);
    p[1] = (unsigned char)((v >> 16) & 0xFF);
    p[2] = (unsigned char)((v >> 8) & 0xFF);
    p[3] = (unsigned char)(v & 0xFF);
}

/* 只解析并应答 Binding Request（class=00, method=0x001）。
 * 其余一律忽略——这是个单文件服务，不是通用 STUN 服务。 */
static void stun_reply(int fd, const unsigned char *req, size_t rlen,
                       const struct sockaddr_in *from, socklen_t alen) {
    unsigned char out[128];
    unsigned long cookie, type, crc;
    unsigned long mapped_addr;
    unsigned short mapped_port;
    size_t pos;

    if (rlen < 20) return;
    if ((req[0] & 0xC0) != 0x00) return;         /* 前两位必须是 0 */
    cookie = ((unsigned long)req[4] << 24) | ((unsigned long)req[5] << 16)
           | ((unsigned long)req[6] << 8) | (unsigned long)req[7];
    if (cookie != STUN_MAGIC) return;            /* RFC 3489 老格式，不处理 */
    type = ((unsigned long)req[0] << 8) | (unsigned long)req[1];
    if ((type & 0x0110u) != 0x0000u) return;     /* 只要请求 */
    if ((type & 0x3EEFu) != 0x0001u) return;     /* 只要 Binding */

    mapped_addr = (unsigned long)ntohl(from->sin_addr.s_addr);
    mapped_port = (unsigned short)ntohs(from->sin_port);

    /* 头：类型 Binding Response(0x0101) + 长度 + magic + 事务 ID 原样带回 */
    put16(out, 0x0101);
    put16(out + 2, 0);                            /* 长度稍后填 */
    put32(out + 4, STUN_MAGIC);
    memcpy(out + 8, req + 8, 12);
    pos = 20;

    /* MAPPED-ADDRESS（RFC 3489 客户端用） */
    put16(out + pos, 0x0001);
    put16(out + pos + 2, 8);
    out[pos + 4] = 0;
    out[pos + 5] = 0x01;                          /* IPv4 */
    put16(out + pos + 6, mapped_port);
    put32(out + pos + 8, mapped_addr);
    pos += 12;

    /* XOR-MAPPED-ADDRESS（RFC 5389，Chrome 认这个） */
    put16(out + pos, 0x0020);
    put16(out + pos + 2, 8);
    out[pos + 4] = 0;
    out[pos + 5] = 0x01;
    put16(out + pos + 6, (unsigned short)(mapped_port ^ (unsigned short)((STUN_MAGIC >> 16) & 0xFFFF)));
    put32(out + pos + 8, mapped_addr ^ STUN_MAGIC);
    pos += 12;

    /* FINGERPRINT：让应答完全符合 RFC 5389，省掉对端严格实现的兼容问题 */
    put16(out + 2, (unsigned short)(pos - 20 + 8));
    crc = stun_crc32(out, pos) ^ STUN_FP_XOR;
    put16(out + pos, 0x8028);
    put16(out + pos + 2, 4);
    put32(out + pos + 4, crc);
    pos += 8;
    put16(out + 2, (unsigned short)(pos - 20));

    sendto(fd, (const char *)out, (int)pos, 0,
           (const struct sockaddr *)from, alen);
}

/* 一次可读事件里把队列里的 STUN 请求都答完（非阻塞 UDP） */
static void handle_stun(int fd) {
    unsigned char buf[2048];
    struct sockaddr_in from;
    socklen_t alen;
    int n, round;

    for (round = 0; round < 64; round++) {
        alen = sizeof from;
        memset(&from, 0, sizeof from);
        n = (int)recvfrom(fd, (char *)buf, sizeof buf, 0, (struct sockaddr *)&from, &alen);
        if (n <= 0) return;
        stun_reply(fd, buf, (size_t)n, &from, alen);
    }
}

int main(int argc, char **argv) {
    int listen_fd;
    int stun_fd;
    struct sockaddr_in addr;
    int i;
    int one = 1;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            g_port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
            snprintf(g_upload_dir, sizeof g_upload_dir, "%s", argv[++i]);
        } else if (strcmp(argv[i], "-w") == 0 && i + 1 < argc) {
            snprintf(g_web_dir, sizeof g_web_dir, "%s", argv[++i]);
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "未知参数: %s\n\n", argv[i]);
            usage(argv[0]);
            return 1;
        }
    }
    if (g_port <= 0 || g_port > 65535) {
        fprintf(stderr, "端口无效: %d\n\n", g_port);
        usage(argv[0]);
        return 1;
    }

    /* 可选 TURN 中继配置（见 g_turn_config 注释与 serve_static 的占位符替换）。
     * 格式必须是以 '[' 开头、']' 结尾的 JSON 数组文本，且不含 '<' '>'，避免
     * 注入破坏页面。留空则纯内网直连。 */
    {
        const char *env_turn = getenv("LAN_SHARE_TURN");
        if (env_turn && *env_turn) {
            size_t L = strlen(env_turn);
            int ok = (L > 1 && env_turn[0] == '[' && env_turn[L - 1] == ']'
                      && strpbrk(env_turn, "<>") == NULL);
            if (ok) {
                g_turn_config = (char *)malloc(L + 1);
                if (g_turn_config) memcpy(g_turn_config, env_turn, L + 1);
                printf("  TURN 中继:    已启用（来自 LAN_SHARE_TURN）\n");
            } else {
                fprintf(stderr, "警告: LAN_SHARE_TURN 不是合法 JSON 数组，已忽略（仍走纯直连）\n");
            }
        }
    }

#ifdef _WIN32
    {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
            fprintf(stderr, "WSAStartup failed\n");
            return 1;
        }
    }
#else
    signal(SIGPIPE, SIG_IGN);
#endif

    memset(g_clients, 0, sizeof g_clients);
    for (i = 0; i < MAX_CLIENTS; i++) {
        g_clients[i].fd = -1;
        g_clients[i].upfd = -1;
    }

    /* 确保上传目录存在。把二进制单独拷到路由器上时，这个目录通常还不存在，
     * 不建的话第一次上传会直接失败。已存在时调用会失败，忽略即可。 */
    ensure_upload_dir();

    listen_fd = (int)socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        fprintf(stderr, "socket failed\n");
        return 1;
    }
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof one);
    set_nonblock(listen_fd);

    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((unsigned short)g_port);
    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
        fprintf(stderr, "bind failed on port %d\n", g_port);
        return 1;
    }
    if (listen(listen_fd, 16) < 0) {
        fprintf(stderr, "listen failed\n");
        return 1;
    }
    /* 同一个端口号上再开一个 UDP socket 做 STUN。TCP 的 8080 和 UDP 的 8080
     * 是两个独立的 socket，互不冲突，所以前端写 stun:<host>:<port> 就能命中，
     * 用户不需要额外记一个端口、路由器上也不用多开一个洞。
     * 起不来不致命——退化成"没有 STUN"，行为和以前一样，只是拿不到自反射候选。 */
    stun_fd = (int)socket(AF_INET, SOCK_DGRAM, 0);
    if (stun_fd >= 0) {
        struct sockaddr_in saddr;
        memset(&saddr, 0, sizeof saddr);
        saddr.sin_family = AF_INET;
        saddr.sin_addr.s_addr = htonl(INADDR_ANY);
        saddr.sin_port = htons((unsigned short)g_port);
        if (bind(stun_fd, (struct sockaddr *)&saddr, sizeof saddr) < 0) {
            ls_closesock(stun_fd);
            stun_fd = -1;
        } else {
            set_nonblock(stun_fd);
        }
    }

    srand((unsigned)time(NULL) ^ (unsigned)((uintptr_t)listen_fd * 2654435761u));

    printf("LAN Share (C) 已启动\n");
    printf("  端口:        %d\n", g_port);
    printf("  文件库目录:  %s\n", g_upload_dir);
    printf("  前端目录:    %s\n", g_web_dir);
    printf("  浏览器打开:  http://<路由器IP>:%d\n", g_port);
    printf("  设备间传文件走 WebRTC 直连，本程序只转发信令。\n");
    printf("  局域网 STUN: %s（UDP %d，给客户端发真实内网 IP 的自反射候选，\n"
           "                 绕开 Chrome 的 mDNS 伪装对组播的依赖）\n\n",
           stun_fd >= 0 ? "已启用" : "未启用", g_port);
    fflush(stdout);

    for (;;) {
        fd_set rfds, wfds;
        struct timeval tv;
        int maxfd = listen_fd;
        int r;

        FD_ZERO(&rfds);
        FD_ZERO(&wfds);
        FD_SET(listen_fd, &rfds);
        if (stun_fd >= 0) {
            FD_SET(stun_fd, &rfds);
            if (stun_fd > maxfd) maxfd = stun_fd;
        }
        for (i = 0; i < MAX_CLIENTS; i++) {
            if (!g_clients[i].used) continue;
            FD_SET(g_clients[i].fd, &rfds);
            if (g_clients[i].out.has || g_clients[i].pendlen > 0) FD_SET(g_clients[i].fd, &wfds);
            if (g_clients[i].fd > maxfd) maxfd = g_clients[i].fd;
        }

        tv.tv_sec = 5;
        tv.tv_usec = 0;
        r = select(maxfd + 1, &rfds, &wfds, NULL, &tv);
        if (r < 0) {
            if (would_block()) continue;
            continue;
        }

        if (FD_ISSET(listen_fd, &rfds)) accept_all(listen_fd);
        if (stun_fd >= 0 && FD_ISSET(stun_fd, &rfds)) handle_stun(stun_fd);

        for (i = 0; i < MAX_CLIENTS; i++) {
            if (!g_clients[i].used) continue;
            if (FD_ISSET(g_clients[i].fd, &wfds)) {
                int fr = flush_out(&g_clients[i]);
                if (fr < 0) {
                    client_close_why(&g_clients[i], "发送失败（flush_out<0）");
                    continue;
                }
                if (fr == 1 && g_clients[i].pending_hello) {
                    g_clients[i].pending_hello = 0;
                    broadcast_peers();
                }
            }
            if (!g_clients[i].used) continue;
            if (FD_ISSET(g_clients[i].fd, &rfds)) client_readable(&g_clients[i]);
        }
    }

    return 0;
}
