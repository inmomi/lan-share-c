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

/* 访问日志要毫秒级计时，而 -std=c99 会把 POSIX 的 clock_gettime 藏起来。
 * 显式声明 POSIX.1-2008，Windows 分支不需要。 */
#ifndef _WIN32
#  ifndef _POSIX_C_SOURCE
#    define _POSIX_C_SOURCE 200809L
#  endif
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdint.h>
#include <limits.h>
#include <errno.h>
#include <stdarg.h>
#include <time.h>
#include <fcntl.h>
#include <sys/stat.h>

/* 内嵌的前端资源（index.html / app.js / style.css 的字节数组）。
 * 由 tools/embed-web.js 生成，make 会自动重新生成。
 * 没有它，只把二进制拷到路由器上打开页面会得到 404 Not Found。 */
#include "web_assets.h"

/* Windows 的 SOCKET 句柄值远大于 Linux 的 fd，默认 FD_SETSIZE=64 会让
 * FD_SET() 写越界。这里在任何 winsock 头文件之前放大它。
 * Linux 同样放大：ulimit -n 调高的服务器上客户端 fd 可能 >= 1024，
 * FD_SET 会写越 fd_set 位图数组导致栈破坏。 */
#ifndef FD_SETSIZE
#  define FD_SETSIZE 4096
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

/* 单位跟随数量级：最小单位是 KB，往上是 MB / GB / TB（1024 进制）。
 * 个位数保留两位小数，否则 84 字节会显示成 "0.0 KB"，看不出量级。 */
static void human_size(unsigned long long bytes, char *out, size_t outsz) {
    double v;
    const char *unit;
    int digits;

    if (bytes == 0) {
        snprintf(out, outsz, "0 KB");
        return;
    }
    if (bytes >= (1ULL << 40)) {
        v = (double)bytes / (double)(1ULL << 40);
        unit = "TB";
    } else if (bytes >= (1ULL << 30)) {
        v = (double)bytes / (double)(1ULL << 30);
        unit = "GB";
    } else if (bytes >= (1ULL << 20)) {
        v = (double)bytes / (double)(1ULL << 20);
        unit = "MB";
    } else {
        v = (double)bytes / 1024.0;
        unit = "KB";
    }
    digits = (v < 1.0) ? 2 : (v < 100.0 ? 1 : 0);
    snprintf(out, outsz, "%.*f %s", digits, v, unit);
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
/* 本文件用的是"文件名最大长度"，和 POSIX 的 NAME_MAX（255）不是一个东西。
 * 开了 _POSIX_C_SOURCE 之后 limits.h 会带进来一个，先清掉避免重定义告警。 */
#undef  NAME_MAX
#define NAME_MAX        64
#define ID_MAX          40
#define PATH_MAX_LOCAL  1024

enum { MODE_HTTP = 0, MODE_WS = 1, MODE_UPLOAD = 2, MODE_CLIPBOARD = 3 };

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
    /* 访问日志用：本条连接最近一个请求的方法、目标（已抹掉口令）和起始时刻。
     * keep-alive 上每个新请求覆盖一次，响应入队时打一行。 */
    char req_method[16];  /* 与 process_request 的 method 缓冲同宽，避免超长方法名被截断 */
    char req_target[PATH_MAX_LOCAL];   /* 与 target 同宽，避免长 URL 日志截断 */
    uint64_t req_t0;
    time_t last_active;   /* 最近一次收/发成功的时间，用于超时回收 */
    time_t ws_ping_at;    /* 上次给 WS 客户端发 ping 的时间 */

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
    int trace_cand;       /* 还能打印多少条 ICE 候选（-1 = 已打满） */
    int trace_lost;       /* 已打印多少条"信令丢失"，只打前几条 */

    int upfd;
    uint64_t upleft;
    char upname[PATH_MAX_LOCAL];
    int pending_hello;   /* 握手响应发完后还需补发一次 peers 广播 */

    /* 分块上传（断点续传）状态。整包上传时这几个字段保持 0。 */
    int up_chunked;              /* 1 = 本次是分块上传 */
    uint64_t up_size;            /* 该文件应有的完整大小 */
    uint64_t up_base;            /* 本次请求之前的已收字节（offset） */
    uint64_t up_reqlen;          /* 本次请求体的字节数 */
    /* 上一个请求被提前拒绝时，它的请求体还剩多少字节没读。必须排水，
     * 否则这些字节会被当成下一个请求的行，整条连接错位。 */
    uint64_t discard;
    int close_after;             /* 残体太多不值得排水：响应发完就关连接 */
    char up_final[NAME_MAX * 4]; /* 收满后要落成的文件名 */
    char clip_kind[8];
    char clip_mime[80];
    char clip_name[NAME_MAX * 4];

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
/* 可选访问口令。空字符串 = 不启用（默认），启用后 /api 下的接口与 /ws 都必须带。
 * 只从命令行 --token 读，不写日志，避免口令跟着终端记录/进程列表泄露。 */
static char g_token[128] = "";
/* 文件库总配额，默认 4 GiB；--quota 0 表示不限制。剪贴板有独立配额，不计入。 */
static uint64_t g_quota_bytes = 4096ULL * 1024ULL * 1024ULL;

/* 可选 TURN 中继配置：一段合法 JSON 数组文本（如
 * [{"urls":"turn:1.2.3.4:3478","username":"u","credential":"p"}]），由
 * LAN_SHARE_TURN 环境变量提供。为空表示纯内网离线直连（默认）。serve_static
 * 在返回首页时把占位符替换成该 JSON，让前端把 TURN 加进 iceServers。
 * AP / 客户端隔离下设备间直连被丢弃时即可走中继。 */
static char *g_turn_config = NULL;

/* 设备名记忆表：按 IP 记住上次用过的名字，供"重连后名字不变"兜底。
 * 只在客户端没带 /ws?name=（换浏览器、清缓存）时才用；同一个 IP 后面对应
 * 多台设备（端口转发/隧道）时，记忆名如果已被在线设备占用就不再复用。 */
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

/* 这个设备名当前是否已被在线连接占用。 */
static int name_in_use(const char *name) {
    int i;
    for (i = 0; i < MAX_CLIENTS; i++) {
        if (g_clients[i].used && g_clients[i].name[0]
            && strcmp(g_clients[i].name, name) == 0) {
            return 1;
        }
    }
    return 0;
}

/* 设备名：主机1、主机2……最多到 100，跳过在线设备已占用的号码。
 *
 * 原来按 IP 记名字，出发点是不让刷新换名。但两台设备经同一个端口转发/隧道
 * 过来时源 IP 完全相同，于是共用同一个名字——界面上就成了两台一模一样的
 * "安静鲸鱼"，谁也分不清谁。编号与 IP 无关；客户端拿到后存 localStorage，
 * 刷新时通过 /ws?name= 带回来，所以只有第一次连接会消耗一个号码。 */
static unsigned long g_host_no = 0;

static void assign_host_name(char *out, size_t cap) {
    int i, n;
    for (i = 0; i < 100; i++) {
        char want[NAME_MAX];
        int taken = 0;
        g_host_no = g_host_no % 100 + 1;
        snprintf(want, sizeof want, "主机%lu", g_host_no);
        for (n = 0; n < MAX_CLIENTS; n++) {
            if (g_clients[n].used && strcmp(g_clients[n].name, want) == 0) {
                taken = 1;
                break;
            }
        }
        if (!taken) {
            snprintf(out, cap, "%s", want);
            return;
        }
    }
    snprintf(out, cap, "主机%lu", g_host_no);
}

/* 设备名来自浏览器，会原样进 JSON 和日志：控制字符、引号、反斜杠必须去掉，
 * 否则一条改名消息就能把 peers 的 JSON 打断。 */
static void clean_display_name(const char *in, char *out, size_t cap) {
    size_t o = 0;
    for (; *in && o + 1 < cap; in++) {
        unsigned char ch = (unsigned char)*in;
        if (ch < 0x20 || ch == '"' || ch == '\\') continue;
        out[o++] = (char)ch;
    }
    out[o] = '\0';
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

static void make_id(char *out, size_t outsz) {
    snprintf(out, outsz, "dev-%lu-%04x", g_seq, (unsigned)(rand() & 0xFFFF));
}

/* ==========================================================================
 * 输出缓冲
 * ========================================================================== */

/* 日志分级：默认只打错误。具体含义见下方"日志"小节的 log_emit。
 * 这里提前声明是因为 queue_mem 等函数比日志实现更早。 */
enum { LOG_ERROR = 0, LOG_INFO = 1, LOG_DEBUG = 2 };
static int g_log_level = LOG_ERROR;
static void log_err(const char *fmt, ...);
static void log_info(const char *fmt, ...);
static void log_dbg(const char *fmt, ...);

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
                if (!nb) { /* 内存不足时保留原有内容，本次丢弃，但留日志便于定位 */
                    log_err("queue_mem OOM：丢弃 %zu 字节（连接 %s）",
                            len, c->name[0] ? c->name : c->id);
                    return;
                }
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

/* ------------------------------------------------------------------ 日志 */

/* 日志时间戳。没有时间戳的日志没法跟浏览器上的操作对齐——原来的"关闭连接"
 * 只有先后顺序没有时刻，看不出哪一次点击对应哪一行。 */
static void log_stamp(char *out, size_t cap) {
    time_t now = time(NULL);
    struct tm tmv;
#ifdef _WIN32
    localtime_s(&tmv, &now);
#else
    localtime_r(&now, &tmv);
#endif
    strftime(out, cap, "%H:%M:%S", &tmv);
}

/* 单调毫秒，只用来算耗时（不受系统时间调整影响）。 */
static uint64_t log_now_ms(void) {
#ifdef _WIN32
    return (uint64_t)GetTickCount64();
#else
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
#endif
}

/* 日志分级：默认只打错误，避免正常使用时刷屏。
 *   （默认）  只输出失败/异常：内存不足、落盘失败、断流、select 错误……
 *   -v        再加常规操作：连接建立/关闭、上传下载、访问日志、信令
 *   -vv       再加逐帧/逐块/ICE 候选这类调试细节
 * 启动横幅（监听地址、访问口令提示等）不经过这里，永远显示。 */
static void log_emit(int level, const char *fmt, va_list ap) {
    char stamp[16];
    if (level > g_log_level) return;
    log_stamp(stamp, sizeof stamp);
    fprintf(stderr, "[%s] ", stamp);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    fflush(stderr);
}

static void log_err(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    log_emit(LOG_ERROR, fmt, ap);
    va_end(ap);
}

static void log_info(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    log_emit(LOG_INFO, fmt, ap);
    va_end(ap);
}

static void log_dbg(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    log_emit(LOG_DEBUG, fmt, ap);
    va_end(ap);
}

/* 口令可能出现在查询串里（<img>/<a download>/WS 只能这么传），日志必须抹掉。 */
static void redact_token(char *s) {
    char *p = s;
    while ((p = strstr(p, "token=")) != NULL) {
        char *v = p + 6;
        char *e = v;
        while (*e && *e != '&') e++;
        if (v == e) { p = e; continue; }
        if (e - v >= 3) {
            v[0] = '*'; v[1] = '*'; v[2] = '*';
            memmove(v + 3, e, strlen(e) + 1);
            p = v + 3;
        } else {
            size_t tlen = (size_t)(e - v), i;
            for (i = 0; i < tlen; i++) v[i] = '*';
            p = e;
        }
    }
}

/* 一行一个请求：方法、目标、状态码、字节数、耗时、来源。
 * 304 是剪贴板轮询的"没变化"，每 2 秒一条会把日志刷满，直接跳过。
 * 访问日志只在 -v 及以上才输出：正常使用时它是最吵的一类。 */
static void log_access(ls_client *c, int code, size_t bytes) {
    uint64_t now, ms = 0;
    char szbuf[24];
    if (g_log_level < LOG_INFO) return;
    if (!c->req_method[0] || code == 304) return;
    now = log_now_ms();
    if (c->req_t0 && now >= c->req_t0) ms = now - c->req_t0;
    human_size((unsigned long long)bytes, szbuf, sizeof szbuf);
    log_info("%s %s -> %d %s %llums ip=%s", c->req_method, c->req_target,
             code, szbuf, (unsigned long long)ms, c->ip[0] ? c->ip : "-");
}

static void queue_response_extra(ls_client *c, int code, const char *ctype,
                                 const char *body, size_t blen, const char *extra) {
    const char *reason;
    char head[384];
    char *buf;
    int hl;
    size_t total;

    log_access(c, code, blen);

    if (code == 200) reason = "OK";
    else if (code == 302) reason = "Found";
    else if (code == 304) reason = "Not Modified";
    else if (code == 400) reason = "Bad Request";
    else if (code == 401) reason = "Unauthorized";
    else if (code == 403) reason = "Forbidden";
    else if (code == 409) reason = "Conflict";
    else if (code == 404) reason = "Not Found";
    else if (code == 405) reason = "Method Not Allowed";
    else if (code == 411) reason = "Length Required";
    else if (code == 413) reason = "Payload Too Large";
    else if (code == 507) reason = "Insufficient Storage";
    else reason = "Error";

    hl = snprintf(head, sizeof head,
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %llu\r\n"
        "%s"
        "Cache-Control: no-store\r\n"
        "Connection: %s\r\n\r\n",
        code, reason, ctype, (unsigned long long)blen, extra ? extra : "",
        c->close_after ? "close" : "keep-alive");
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

static void queue_response(ls_client *c, int code, const char *ctype,
                           const char *body, size_t blen) {
    queue_response_extra(c, code, ctype, body, blen, NULL);
}

/* 返回 1 完成，0 未完成，-1 出错。 */
static int flush_out(ls_client *c) {
    if (c->pendlen > 0) {
        int n = (int)send(c->fd, c->pend, (int)c->pendlen, 0);
        if (n > 0) {
            c->last_active = time(NULL);
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
                c->last_active = time(NULL);
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
            c->last_active = time(NULL);
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
    if (c->relay_in == 0) {
        /* 二进制开始走服务器 = 直连没建起来。这条必须在日志里明说，
         * 否则用户只会看到"测速很慢"，不知道慢在绕路。 */
        log_info("中转 开始 设备=%s→%s（直连未建立，字节走服务器）",
                c->name[0] ? c->name : c->id, to);
    }
    c->relay_in += len;
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
 * 字符串，加上收发位置和中转计数，一次就能定位。 */
static const char *mode_name(int m) {
    switch (m) {
    case MODE_HTTP: return "HTTP";
    case MODE_WS: return "WS";
    case MODE_UPLOAD: return "上传中";
    case MODE_CLIPBOARD: return "剪贴板";
    default: return "?";
    }
}

static void client_close_why(ls_client *c, const char *why) {
    int was_ws = (c->mode == MODE_WS);
    int err = (int)ls_last_err();
    char errbuf[24];

    /* 上传途中被对端关掉时，最要紧的是"哪个文件、收到多少"。原来这条汇总行
     * 既没有文件名也没有进度，等于什么都没说。 */
    if (c->mode == MODE_UPLOAD) {
        const char *slash = strrchr(c->upname, '/');
        const char *bs = strrchr(c->upname, '\\');
        const char *base = (slash && (!bs || slash > bs)) ? slash + 1
                         : (bs ? bs + 1 : c->upname);
        uint64_t total = c->up_chunked ? c->up_size : c->up_reqlen;
        uint64_t got = c->up_base
                     + (c->up_reqlen > c->upleft ? c->up_reqlen - c->upleft : 0);
        char gotbuf[24], totbuf[24];
        human_size(got, gotbuf, sizeof gotbuf);
        human_size(total, totbuf, sizeof totbuf);
        log_err("上传 中断 设备=%s name=%s 已收=%s/%s 原因=%s",
                c->name[0] ? c->name : c->ip, base[0] ? base : "-",
                gotbuf, totbuf, why);
    }

    /* ENOENT/EAGAIN 是正常收尾后读到的残留值，打出来只会误导 */
    if (err == 0 || err == 2 || err == 11) errbuf[0] = '\0';
    else snprintf(errbuf, sizeof errbuf, " errno=%d", err);

    /* 普通 HTTP 连接每个请求一条，正常 FIN 时打出来只是噪声——请求本身
     * 已经有一行访问日志了。只有"非正常关闭"或还有数据没发完才值得记。 */
    if (!(c->mode == MODE_HTTP && c->wlen == 0 && c->out.sent == 0
          && c->relay_in == 0 && c->relay_out == 0
          && strstr(why, "对端 FIN") != NULL)) {
        log_info("连接关闭 设备=%s mode=%s ip=%s 原因=%s%s 未发=%lu 待发=%lu/%lu "
                "中转 收=%llu 发=%llu",
                c->name[0] ? c->name : "-", mode_name(c->mode),
                c->ip[0] ? c->ip : "-", why, errbuf,
                (unsigned long)c->wlen,
                (unsigned long)c->out.sent, (unsigned long)c->out.len,
                (unsigned long long)c->relay_in, (unsigned long long)c->relay_out);
    }
    if (c->upfd >= 0) {
        ls_closefile(c->upfd);
        c->upfd = -1;
    }
    if (c->mode == MODE_CLIPBOARD && c->upname[0]) ls_remove_file(c->upname);
    out_reset(c);
    if (c->fd >= 0) ls_closesock(c->fd);
    memset(c, 0, sizeof *c);
    c->fd = -1;
    c->upfd = -1;
    if (was_ws) broadcast_peers();
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
        char candidate[NAME_MAX * 4 + 64];   /* stem(255)+" (999)"(6)+ext(31)+NUL 最坏 294 */
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
static int parts_dir_path(char *out, size_t outsz) {
    int n = snprintf(out, outsz, "%s/.lanshare-parts", g_upload_dir);
    /* -d 太长时 half 截断出来的路径会指到别处，必须让调用方知道 */
    return (n > 0 && (size_t)n < outsz) ? 0 : -1;
}

#define CLIPBOARD_MAX (16u * 1024u * 1024u)
#define CLIPBOARD_HISTORY_ITEMS 32
#define CLIPBOARD_HISTORY_BYTES (128ULL * 1024ULL * 1024ULL)

typedef struct {
    int id;
    char kind[8];
    char mime[80];
    char name[NAME_MAX * 4];
    uint64_t size;
    unsigned long long updated;
} ls_clipboard_meta;

static unsigned long long g_clipboard_updated;

static int clipboard_dir_path(char *out, size_t outsz) {
    int n = snprintf(out, outsz, "%s/.lanshare-clipboard", g_upload_dir);
    return n > 0 && (size_t)n < outsz ? 0 : -1;
}

static int clipboard_file_path(const char *leaf, char *out, size_t outsz) {
    char dir[PATH_MAX_LOCAL];
    if (clipboard_dir_path(dir, sizeof dir) != 0) return -1;
    return path_join(dir, leaf, out, outsz);
}

static unsigned long long clipboard_next_updated(void) {
    unsigned long long now = (unsigned long long)time(NULL) * 1000ULL;
    if (now <= g_clipboard_updated) now = g_clipboard_updated + 1;
    g_clipboard_updated = now;
    return now;
}

static void clipboard_slot_leaf(int id, const char *suffix, char *out, size_t outsz) {
    snprintf(out, outsz, "item-%02d.%s", id, suffix);
}

static int clipboard_slot_path(int id, const char *suffix, char *out, size_t outsz) {
    char leaf[32];
    if (id < 0 || id >= CLIPBOARD_HISTORY_ITEMS) return -1;
    clipboard_slot_leaf(id, suffix, leaf, sizeof leaf);
    return clipboard_file_path(leaf, out, outsz);
}

static int clipboard_mime_allowed(const char *kind, const char *mime) {
    if (strcmp(kind, "text") == 0) return strcmp(mime, "text/plain") == 0;
    if (strcmp(kind, "image") != 0) return 0;
    return strcmp(mime, "image/png") == 0 || strcmp(mime, "image/jpeg") == 0 ||
           strcmp(mime, "image/gif") == 0 || strcmp(mime, "image/webp") == 0;
}

static void trim_line(char *s) {
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '\r' || s[n - 1] == '\n')) s[--n] = '\0';
}

static int read_clipboard_meta_path(const char *path, ls_clipboard_meta *meta) {
    char line[32];
    FILE *f;
    f = ls_fopen_rb(path);
    if (!f) return 0;
    memset(meta, 0, sizeof *meta);
    if (!fgets(meta->kind, sizeof meta->kind, f) ||
        !fgets(meta->mime, sizeof meta->mime, f) ||
        !fgets(meta->name, sizeof meta->name, f) ||
        !fgets(line, sizeof line, f)) {
        fclose(f);
        return 0;
    }
    trim_line(meta->kind);
    trim_line(meta->mime);
    trim_line(meta->name);
    meta->size = (uint64_t)strtoull(line, NULL, 10);
    if (!fgets(line, sizeof line, f)) {
        fclose(f);
        return 0;
    }
    meta->updated = strtoull(line, NULL, 10);
    fclose(f);
    return clipboard_mime_allowed(meta->kind, meta->mime) && meta->name[0] != '\0';
}

static int read_clipboard_slot(int id, ls_clipboard_meta *meta) {
    char leaf[32], path[PATH_MAX_LOCAL];
    if (id < 0 || id >= CLIPBOARD_HISTORY_ITEMS) return 0;
    clipboard_slot_leaf(id, "meta", leaf, sizeof leaf);
    if (clipboard_file_path(leaf, path, sizeof path) != 0 ||
        !read_clipboard_meta_path(path, meta)) return 0;
    meta->id = id;
    return 1;
}

static int read_clipboard_meta(ls_clipboard_meta *meta) {
    ls_clipboard_meta candidate;
    int i, found = 0;
    memset(meta, 0, sizeof *meta);
    for (i = 0; i < CLIPBOARD_HISTORY_ITEMS; i++) {
        if (read_clipboard_slot(i, &candidate) &&
            (!found || candidate.updated > meta->updated)) {
            *meta = candidate;
            found = 1;
        }
    }
    return found;
}

static int clipboard_evict_oldest(void) {
    ls_clipboard_meta candidate;
    int i, oldest = -1;
    unsigned long long oldest_updated = ULLONG_MAX;
    char path[PATH_MAX_LOCAL];

    for (i = 0; i < CLIPBOARD_HISTORY_ITEMS; i++) {
        if (read_clipboard_slot(i, &candidate) && candidate.updated < oldest_updated) {
            oldest = i;
            oldest_updated = candidate.updated;
        }
    }
    if (oldest < 0) return 0;
    if (clipboard_slot_path(oldest, "data", path, sizeof path) == 0) ls_remove_file(path);
    if (clipboard_slot_path(oldest, "meta", path, sizeof path) == 0) ls_remove_file(path);
    return 1;
}

static int clipboard_make_room(uint64_t incoming, int *slot) {
    ls_clipboard_meta meta;
    unsigned count;
    uint64_t total;
    int i;
    for (;;) {
        count = 0;
        total = 0;
        for (i = 0; i < CLIPBOARD_HISTORY_ITEMS; i++) {
            if (read_clipboard_slot(i, &meta)) {
                count++;
                total += meta.size;
            }
        }
        if (count < CLIPBOARD_HISTORY_ITEMS && total + incoming <= CLIPBOARD_HISTORY_BYTES) {
            for (i = 0; i < CLIPBOARD_HISTORY_ITEMS; i++) {
                if (!read_clipboard_slot(i, &meta)) {
                    *slot = i;
                    return 1;
                }
            }
        }
        if (!clipboard_evict_oldest()) return 0;
    }
}

static long clipboard_write_retry(int fd, const void *data, size_t len) {
    const char *p = (const char *)data;
    size_t off = 0;
    while (off < len) {
        long n = ls_write(fd, p + off, len - off);
        if (n > 0) {
            off += (size_t)n;
        } else if (errno == ENOSPC && clipboard_evict_oldest()) {
            continue;
        } else {
            return -1;
        }
    }
    return (long)off;
}

static int write_clipboard_meta(const ls_clipboard_meta *meta, const char *path) {
    char body[768];
    int len = snprintf(body, sizeof body, "%s\n%s\n%s\n%llu\n%llu\n",
        meta->kind, meta->mime, meta->name,
        (unsigned long long)meta->size, meta->updated);
    int fd;
    if (len <= 0 || (size_t)len >= sizeof body) return -1;
    fd = ls_openwrite_at(path, 0, 0);
    if (fd < 0) return -1;
    if (clipboard_write_retry(fd, body, (size_t)len) != len) {
        ls_closefile(fd);
        return -1;
    }
    ls_closefile(fd);
    return 0;
}

static void clipboard_migrate_legacy(void) {
    ls_clipboard_meta meta;
    char legacy_data[PATH_MAX_LOCAL], legacy_meta[PATH_MAX_LOCAL];
    char new_data[PATH_MAX_LOCAL], new_meta[PATH_MAX_LOCAL], temp_meta[PATH_MAX_LOCAL];
    int slot;

    if (clipboard_file_path("meta", legacy_meta, sizeof legacy_meta) != 0 ||
        clipboard_file_path("data", legacy_data, sizeof legacy_data) != 0 ||
        !read_clipboard_meta_path(legacy_meta, &meta) || !file_exists(legacy_data)) return;
    if (!clipboard_make_room(meta.size, &slot)) return;
    if (clipboard_slot_path(slot, "data", new_data, sizeof new_data) != 0 ||
        clipboard_slot_path(slot, "meta", new_meta, sizeof new_meta) != 0 ||
        clipboard_slot_path(slot, "meta.tmp", temp_meta, sizeof temp_meta) != 0) return;

    meta.id = slot;
    if (write_clipboard_meta(&meta, temp_meta) != 0) return;
    if (ls_rename_file(legacy_data, new_data) != 0 ||
        ls_rename_file(temp_meta, new_meta) != 0) {
        ls_remove_file(temp_meta);
        return;
    }
    ls_remove_file(legacy_meta);
    if (meta.updated > g_clipboard_updated) g_clipboard_updated = meta.updated;
}

static void ensure_clipboard_dir(void) {
    char dir[PATH_MAX_LOCAL];
    if (clipboard_dir_path(dir, sizeof dir) != 0) return;
#ifdef _WIN32
    _mkdir(dir);
#else
    mkdir(dir, 0755);
#endif
    clipboard_migrate_legacy();
}

/* 半成品文件名取 name|size 的 SHA-1 十六进制。
 * 中文或带特殊字符的文件名在文件系统上容易出岔子，哈希后只剩 [0-9a-f]，
 * 同时保证"同名不同大小"互不干扰。 */
static int part_path_for(const char *name, uint64_t size, char *out, size_t outsz) {
    char key[NAME_MAX * 4 + 32];
    unsigned char dig[20];
    char hex[41];
    char dir[PATH_MAX_LOCAL];
    ls_sha1 ctx;
    int i, n;

    snprintf(key, sizeof key, "%s|%llu", name, (unsigned long long)size);
    sha1_init(&ctx);
    sha1_update(&ctx, key, strlen(key));
    sha1_final(&ctx, dig);
    for (i = 0; i < 20; i++) snprintf(hex + (size_t)i * 2, 3, "%02x", dig[i]);
    hex[40] = '\0';

    if (parts_dir_path(dir, sizeof dir) != 0) return -1;
    n = snprintf(out, outsz, "%s/%s.part", dir, hex);
    return (n > 0 && (size_t)n < outsz) ? 0 : -1;
}

/* 某个文件目前收了多少字节。没开始过就是 0。 */
static uint64_t part_received(const char *name, uint64_t size) {
    char p[PATH_MAX_LOCAL];
    uint64_t sz;
    if (part_path_for(name, size, p, sizeof p) != 0) return 0;
    if (!file_exists(p)) return 0;
    sz = file_size(p);
    if (size > 0 && sz > size) sz = size;
    return sz;
}

/* 文件库已用空间。只统计上传目录下的普通文件；剪贴板与 .part 在子目录里，
 * 有各自的配额/清理策略，不重复计算。 */
static uint64_t upload_dir_used(void) {
    uint64_t total = 0;
#ifdef _WIN32
    char pat[PATH_MAX_LOCAL];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    snprintf(pat, sizeof pat, "%s\\*", g_upload_dir);
    h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            total += ((uint64_t)fd.nFileSizeHigh << 32) | (uint64_t)fd.nFileSizeLow;
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR *d = opendir(g_upload_dir);
    struct dirent *e;
    if (!d) return 0;
    while ((e = readdir(d)) != NULL) {
        char p[PATH_MAX_LOCAL];
        struct stat st;
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        if (path_join(g_upload_dir, e->d_name, p, sizeof p) != 0) continue;
        if (stat(p, &st) == 0 && S_ISREG(st.st_mode)) total += (uint64_t)st.st_size;
    }
    closedir(d);
#endif
    return total;
}

static void api_list_files(ls_client *c) {
    char *body;
    size_t cap = 8192, len = 0;
    uint64_t total = 0;
    int count = 0;

    body = (char *)malloc(cap);
    if (!body) return;
    len = body_append(body, len, cap, "{\"files\":[");

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
                len = body_append(body, len, cap,
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
                len = body_append(body, len, cap,
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
    len = body_append(body, len, cap, "],\"total\":%llu}", (unsigned long long)total);
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

static void serve_static(ls_client *c, const char *rel, int authed) {
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
    /* 开了口令但还没认证：首页给登录页，其它静态资源一律重定向回首页。
     * 这样未认证的人连 app.js 都拿不到，页面结构也看不到。 */
    if (g_token[0] && !authed && strcmp(rel, "/login.html") != 0) {
        if (strcmp(rel, "/index.html") == 0) {
            rel = "/login.html";
        } else {
            queue_response_extra(c, 302, "text/plain; charset=utf-8", "", 0,
                                 "Location: /\r\n");
            return;
        }
    }
    {
        /* -w 目录加上请求路径可能超出缓冲：截断出来的路径会指到别处，
         * 与其静默 404 或读到意外文件，不如明确判定"路径过长"。 */
        int pn = snprintf(path, sizeof path, "%s%s", g_web_dir, rel);
        if (pn < 0 || (size_t)pn >= sizeof path) {
            queue_response(c, 404, "text/plain; charset=utf-8", "Not Found", 9);
            return;
        }
    }
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
        else if (strstr(rel, ".png")) ctype = "image/png";
        else if (strstr(rel, ".svg")) ctype = "image/svg+xml";
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
    else if (strstr(rel, ".png")) ctype = "image/png";
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
    /* 流式下载不走 queue_response，单独记一条，否则日志里看不到任何下载 */
    {
        char szbuf[24];
        human_size((unsigned long long)length, szbuf, sizeof szbuf);
        log_info("下载 name=%s %s%s", safe, szbuf,
                has_range ? "（Range 分段）" : "");
    }
}

static void api_delete(ls_client *c, const char *name) {
    char safe[NAME_MAX * 4];
    char esc[NAME_MAX * 8];
    char path[PATH_MAX_LOCAL];
    char body[NAME_MAX * 8 + 64];
    sanitize_name(name, safe, sizeof safe);
    if (path_join(g_upload_dir, safe, path, sizeof path) != 0) return;
    if (!file_exists(path)) {
        queue_response(c, 404, "application/json; charset=utf-8", "{\"error\":\"not found\"}", sizeof("{\"error\":\"not found\"}") - 1);
        return;
    }
    if (ls_remove_file(path) != 0) {
        queue_response(c, 500, "application/json; charset=utf-8", "{\"error\":\"remove failed\"}", sizeof("{\"error\":\"remove failed\"}") - 1);
        return;
    }
    json_escape(safe, esc, sizeof esc);
    snprintf(body, sizeof body, "{\"ok\":true,\"name\":\"%s\"}", esc);
    queue_response(c, 200, "application/json; charset=utf-8", body, strlen(body));
}

/* 定义在 HTTP 解析段，这里先用（剪贴板 ETag 要读 If-None-Match）。 */
static const char *find_header(const char *req, size_t len, const char *key);
static void header_value(const char *p, const char *end, char *out, size_t outsz);

static int cmp_clip_meta_desc(const void *pa, const void *pb) {
    const ls_clipboard_meta *a = (const ls_clipboard_meta *)pa;
    const ls_clipboard_meta *b = (const ls_clipboard_meta *)pb;
    if (a->updated != b->updated) return a->updated < b->updated ? 1 : -1;
    if (a->id != b->id) return a->id < b->id ? 1 : -1;
    return 0;
}

static void api_clipboard_meta(ls_client *c, const char *req, size_t len) {
    ls_clipboard_meta items[CLIPBOARD_HISTORY_ITEMS];
    ls_clipboard_meta latest;
    char *body;
    char esc[NAME_MAX * 8];
    char etag[64], inm[64], extra[96];
    size_t cap = 32768, len2 = 0;
    int count = 0, i;

    for (i = 0; i < CLIPBOARD_HISTORY_ITEMS; i++) {
        if (read_clipboard_slot(i, &items[count])) count++;
    }
    if (count == 0) {
        queue_response(c, 404, "application/json; charset=utf-8",
                       "{\"error\":\"empty\"}", sizeof("{\"error\":\"empty\"}") - 1);
        return;
    }
    qsort(items, (size_t)count, sizeof items[0], cmp_clip_meta_desc);
    latest = items[0];

    /* ETag 用最新一条的 updated：没有新内容时前端 2 秒轮询只拿一个 304，
     * 不再反复传输整份历史和图片元数据。 */
    snprintf(etag, sizeof etag, "\"%llu\"", (unsigned long long)latest.updated);
    inm[0] = '\0';
    header_value(find_header(req, len, "If-None-Match"), req + len, inm, sizeof inm);
    if (strcmp(inm, etag) == 0) {
        snprintf(extra, sizeof extra, "ETag: %s\r\n", etag);
        queue_response_extra(c, 304, "text/plain; charset=utf-8", "", 0, extra);
        return;
    }
    body = (char *)malloc(cap);
    if (!body) {
        queue_response(c, 500, "application/json; charset=utf-8", "{\"error\":\"out of memory\"}",
                       sizeof("{\"error\":\"out of memory\"}") - 1);
        return;
    }
    json_escape(latest.name, esc, sizeof esc);
    len2 = body_append(body, len2, cap,
        "{\"kind\":\"%s\",\"mime\":\"%s\",\"name\":\"%s\",\"size\":%llu,\"updated\":%llu,\"id\":%d,\"items\":[",
        latest.kind, latest.mime, esc, (unsigned long long)latest.size,
        latest.updated, latest.id);
    for (i = 0; i < count; i++) {
        json_escape(items[i].name, esc, sizeof esc);
        len2 = body_append(body, len2, cap,
            "%s{\"id\":%d,\"kind\":\"%s\",\"mime\":\"%s\",\"name\":\"%s\",\"size\":%llu,\"updated\":%llu}",
            i ? "," : "", items[i].id, items[i].kind, items[i].mime, esc,
            (unsigned long long)items[i].size, items[i].updated);
    }
    len2 = body_append(body, len2, cap, "]}");
    snprintf(extra, sizeof extra, "ETag: %s\r\n", etag);
    queue_response_extra(c, 200, "application/json; charset=utf-8", body, len2, extra);
    free(body);
}

static void api_clipboard_data(ls_client *c, int id) {
    ls_clipboard_meta meta;
    char path[PATH_MAX_LOCAL];
    char leaf[32];
    char head[512];
    FILE *f;
    int n;
    if (id < 0) {
        if (!read_clipboard_meta(&meta)) id = -1;
        else id = meta.id;
    }
    if (id < 0 || !read_clipboard_slot(id, &meta)) {
        queue_response(c, 404, "text/plain; charset=utf-8", "Not Found", 9);
        return;
    }
    clipboard_slot_leaf(id, "data", leaf, sizeof leaf);
    if (clipboard_file_path(leaf, path, sizeof path) != 0) {
        queue_response(c, 500, "text/plain; charset=utf-8", "Path Too Long", sizeof("Path Too Long") - 1);
        return;
    }
    f = ls_fopen_rb(path);
    if (!f) {
        queue_response(c, 404, "text/plain; charset=utf-8", "Not Found", 9);
        return;
    }
    n = snprintf(head, sizeof head,
        "HTTP/1.1 200 OK\r\nContent-Type: %s%s\r\nContent-Length: %llu\r\n"
        "Cache-Control: no-store\r\nConnection: keep-alive\r\n\r\n",
        meta.mime, strcmp(meta.kind, "text") == 0 ? "; charset=utf-8" : "",
        (unsigned long long)meta.size);
    if (n <= 0 || (size_t)n > sizeof c->pend) {
        fclose(f);
        return;
    }
    out_reset(c);
    memcpy(c->pend, head, (size_t)n);
    c->pendlen = (size_t)n;
    c->out.file = f;
    c->out.is_mem = 0;
    c->out.total = meta.size;
    c->out.sent = 0;
    c->out.has = 1;
}

static void api_clipboard_delete(ls_client *c, int id) {
    ls_clipboard_meta current;
    char data[PATH_MAX_LOCAL], meta[PATH_MAX_LOCAL];
    char body[96];
    int i, removed = 0;
    if (id >= 0) {
        if (!read_clipboard_slot(id, &current)) {
            queue_response(c, 404, "application/json; charset=utf-8",
                           "{\"error\":\"not found\"}", sizeof("{\"error\":\"not found\"}") - 1);
            return;
        }
        if (clipboard_slot_path(id, "data", data, sizeof data) != 0 ||
            clipboard_slot_path(id, "meta", meta, sizeof meta) != 0) {
            queue_response(c, 500, "application/json; charset=utf-8",
                           "{\"error\":\"path too long\"}", sizeof("{\"error\":\"path too long\"}") - 1);
            return;
        }
        if (ls_remove_file(data) != 0 || ls_remove_file(meta) != 0) {
            queue_response(c, 500, "application/json; charset=utf-8",
                           "{\"error\":\"delete failed\"}", sizeof("{\"error\":\"delete failed\"}") - 1);
            return;
        }
        removed = 1;
    } else {
        for (i = 0; i < CLIPBOARD_HISTORY_ITEMS; i++) {
            if (!read_clipboard_slot(i, &current)) continue;
            if (clipboard_slot_path(i, "data", data, sizeof data) == 0) ls_remove_file(data);
            if (clipboard_slot_path(i, "meta", meta, sizeof meta) == 0) ls_remove_file(meta);
            removed++;
        }
        clipboard_file_path("data", data, sizeof data);
        clipboard_file_path("meta", meta, sizeof meta);
        ls_remove_file(data);
        ls_remove_file(meta);
    }
    snprintf(body, sizeof body, "{\"ok\":true,\"removed\":%d}", removed);
    queue_response(c, 200, "application/json; charset=utf-8", body, strlen(body));
}

static void finish_clipboard(ls_client *c) {
    ls_clipboard_meta meta;
    char data[PATH_MAX_LOCAL];
    char meta_path[PATH_MAX_LOCAL];
    char meta_tmp[PATH_MAX_LOCAL];
    char response[256];
    int slot;
    int n;

    if (c->upfd >= 0) {
        ls_closefile(c->upfd);
        c->upfd = -1;
    }
    c->mode = MODE_HTTP;
    if (file_size(c->upname) != c->up_reqlen) {
        ls_remove_file(c->upname);
        c->upname[0] = '\0';
        queue_response(c, 500, "application/json; charset=utf-8",
                   "{\"error\":\"write failed\"}", sizeof("{\"error\":\"write failed\"}") - 1);
        return;
    }

    if (!clipboard_make_room(c->up_reqlen, &slot)) {
        ls_remove_file(c->upname);
        c->upname[0] = '\0';
        queue_response(c, 507, "application/json; charset=utf-8",
                       "{\"error\":\"clipboard history full\"}",
                       sizeof("{\"error\":\"clipboard history full\"}") - 1);
        return;
    }
    if (clipboard_slot_path(slot, "data", data, sizeof data) != 0 ||
        clipboard_slot_path(slot, "meta", meta_path, sizeof meta_path) != 0 ||
        clipboard_slot_path(slot, "meta.tmp", meta_tmp, sizeof meta_tmp) != 0) {
        ls_remove_file(c->upname);
        c->upname[0] = '\0';
        queue_response(c, 500, "application/json; charset=utf-8",
                   "{\"error\":\"path too long\"}", sizeof("{\"error\":\"path too long\"}") - 1);
        return;
    }
    /* 槽位是按"没有有效 meta"挑出来的，但可能残留上次中途失败（进程被杀 /
     * 掉电）留下的数据文件——那种文件没有 meta 指向它，谁也读不到。
     * POSIX 的 rename 会直接覆盖，Windows 的 _wrename 不会：不先清掉，
     * 这个槽位以后每次上传都报 save failed。 */
    ls_remove_file(data);
    ls_remove_file(meta_path);
    memset(&meta, 0, sizeof meta);
    meta.id = slot;
    snprintf(meta.kind, sizeof meta.kind, "%s", c->clip_kind);
    snprintf(meta.mime, sizeof meta.mime, "%s", c->clip_mime);
    snprintf(meta.name, sizeof meta.name, "%s", c->clip_name);
    meta.size = c->up_reqlen;
    meta.updated = clipboard_next_updated();

    if (write_clipboard_meta(&meta, meta_tmp) != 0) {
        ls_remove_file(c->upname);
        c->upname[0] = '\0';
        queue_response(c, 500, "application/json; charset=utf-8",
                   "{\"error\":\"metadata write failed\"}", sizeof("{\"error\":\"metadata write failed\"}") - 1);
        return;
    }
    if (ls_rename_file(c->upname, data) != 0 || ls_rename_file(meta_tmp, meta_path) != 0) {
        ls_remove_file(data);
        ls_remove_file(c->upname);
        ls_remove_file(meta_tmp);
        c->upname[0] = '\0';
        queue_response(c, 500, "application/json; charset=utf-8",
                   "{\"error\":\"save failed\"}", sizeof("{\"error\":\"save failed\"}") - 1);
        return;
    }
    c->upname[0] = '\0';
    n = snprintf(response, sizeof response,
        "{\"ok\":true,\"id\":%d,\"kind\":\"%s\",\"size\":%llu}",
        meta.id, meta.kind, (unsigned long long)meta.size);
    if (n > 0) queue_response(c, 200, "application/json; charset=utf-8", response, strlen(response));
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
    /* 名字经 json_escape 后最长可达 NAME_MAX*8-1，500 多字节的 body 会被截断；
     * 而下面用 strlen 取长度，缓冲必须真的装得下。 */
    char body[NAME_MAX * 8 + 128];
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
                queue_response(c, 200, "application/json; charset=utf-8", body, strlen(body));
            }
            {
                char cur[24], acc[24], all[24];
                human_size((unsigned long long)c->up_reqlen, cur, sizeof cur);
                human_size((unsigned long long)total, acc, sizeof acc);
                human_size((unsigned long long)c->up_size, all, sizeof all);
                log_dbg("上传 分块 name=%s 本次=%s 累计=%s/%s",
                        c->up_final, cur, acc, all);
            }
            return;
        }
        /* 收满了：把半成品改名成正式文件（重名会自动加 (1)） */
        {
            char finalpath[PATH_MAX_LOCAL];
            unique_path(c->up_final, finalpath, sizeof finalpath);
            if (ls_rename_file(c->upname, finalpath) != 0) {
                queue_response(c, 500, "application/json; charset=utf-8",
                               "{\"error\":\"落盘失败\"}", sizeof("{\"error\":\"落盘失败\"}") - 1);
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
    {
        char szbuf[24];
        human_size(sz, szbuf, sizeof szbuf);
        log_info("上传 完成 name=%s %s", base, szbuf);
    }
    if (n > 0) queue_response(c, 200, "application/json; charset=utf-8", body, strlen(body));
}

/* 返回已消耗的字节数；0 表示请求还不完整；-1 表示应关闭连接。 */
/* 大小写不敏感比较。只用于 Host/Origin 这类 ASCII 头，不做 locale 处理。 */
static int ci_equal(const char *a, const char *b) {
    while (*a && *b) {
        unsigned char ca = (unsigned char)*a++;
        unsigned char cb = (unsigned char)*b++;
        if (ca >= 'A' && ca <= 'Z') ca = (unsigned char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (unsigned char)(cb - 'A' + 'a');
        if (ca != cb) return 0;
    }
    return *a == '\0' && *b == '\0';
}

/* 定长比较，避免按第一个不同字节提前返回泄露口令长度/前缀。 */
static int token_matches(const char *a, const char *b) {
    size_t la = strlen(a), lb = strlen(b);
    size_t n = la > lb ? la : lb;
    unsigned char diff = (unsigned char)(la != lb);
    size_t i;
    for (i = 0; i < n; i++) {
        unsigned char ca = i < la ? (unsigned char)a[i] : 0;
        unsigned char cb = i < lb ? (unsigned char)b[i] : 0;
        diff |= (unsigned char)(ca ^ cb);
    }
    return diff == 0;
}

/* 登录成功后写进 Cookie 的值是 sha1(口令) 的十六进制，而不是口令原文：
 * 一是不让口令里的 ';'、空格、非 ASCII 破坏 Cookie 语法；二是即使有人看到
 * Cookie 也拿不到可用口令（服务端只比摘要）。 */
static void token_digest(char *out, size_t outsz) {
    unsigned char dig[20];
    char hex[41];
    ls_sha1 ctx;
    int i;

    sha1_init(&ctx);
    sha1_update(&ctx, g_token, strlen(g_token));
    sha1_final(&ctx, dig);
    for (i = 0; i < 20; i++) snprintf(hex + (size_t)i * 2, 3, "%02x", dig[i]);
    hex[40] = '\0';
    snprintf(out, outsz, "%s", hex);
}

/* 从 Cookie 头里取 ls_auth 的值（不存在时 out 为空串）。 */
static void cookie_token(const char *req, size_t header_end, char *out, size_t outsz) {
    char cookies[512];
    const char *p;
    size_t i = 0;

    out[0] = '\0';
    cookies[0] = '\0';
    header_value(find_header(req, header_end, "Cookie"),
                 req + header_end, cookies, sizeof cookies);
    p = strstr(cookies, "ls_auth=");
    if (!p) return;
    p += 8;
    while (*p && *p != ';' && *p != ' ' && i + 1 < outsz) out[i++] = *p++;
    out[i] = '\0';
}

/* WebSocket 的跨站防护：浏览器一定带 Origin，必须和 Host 完全一致。
 * 非浏览器客户端（curl / 测试脚本）没有 Origin，放行，靠可选口令保护。 */
static int origin_allowed(const char *origin, const char *host) {
    const char *p;
    if (!origin[0]) return 1;
    if (strcmp(origin, "null") == 0) return 0;
    p = strstr(origin, "://");
    if (!p) return 0;
    return ci_equal(p + 3, host);
}

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
    int authed = 1;   /* 没开口令 / 凭据有效 / 走登录接口时为 1 */

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

    /* 访问日志的请求上下文。口令会出现在查询串里（<img>/<a download>/WS
     * 带不了自定义头，只能这么传），落盘前必须抹掉。 */
    snprintf(c->req_method, sizeof c->req_method, "%s", method);
    snprintf(c->req_target, sizeof c->req_target, "%s", target);
    redact_token(c->req_target);
    c->req_t0 = log_now_ms();

    /* 只扫请求头，不能扫到 body：否则客户端可以在正文里伪造
     * Content-Length / X-Lan-Share-Request 骗过校验。 */
    header_value(find_header(req, header_end, "Content-Length"), req + header_end, clen_str, sizeof clen_str);
    header_value(find_header(req, header_end, "Sec-WebSocket-Key"), req + header_end, wskey, sizeof wskey);
    if (clen_str[0] != '\0') {
        char *endp = NULL;
        errno = 0;
        content_len = strtoull(clen_str, &endp, 10);
        if (clen_str[0] == '-' || endp == clen_str || *endp != '\0'
            || errno == ERANGE || content_len > (1ULL << 40)) {
            static const char body[] = "{\"error\":\"invalid content-length\"}";
            queue_response(c, 400, "application/json; charset=utf-8", body, sizeof body - 1);
            return (long)len;
        }
    }

    /* 本服务端不解析 chunked 请求体。以前对这类请求按 content_len=0 处理：
     * 客户端发 2 MB 分块正文，服务端建出 0 字节文件还回 200——数据全丢却报成功，
     * 排查时只能看到"上传完成但文件是空的"。明确回 411 让调用方改用
     * Content-Length（浏览器的 XHR/fetch + Blob 本来就会带）。 */
    if (clen_str[0] == '\0') {
        char te[64];
        te[0] = '\0';
        header_value(find_header(req, header_end, "Transfer-Encoding"),
                     req + header_end, te, sizeof te);
        if (te[0] != '\0') {
            static const char body[] =
                "{\"error\":\"chunked request body not supported, send Content-Length\"}";
            queue_response(c, 411, "application/json; charset=utf-8", body, sizeof body - 1);
            return (long)len;
        }
    }

    /* 提前拒绝（401 / 411 / 400 / 409 / 507）时请求体往往还没到齐，剩下的
     * 字节不会有人消费。默认把它们排水掉；体量太大就直接关连接，不为一个
     * 已经被拒的请求白读几 MB。真正要消费 body 的分支（流式上传、剪贴板）
     * 会把这个计数清掉。 */
    {
        uint64_t in_buf = (uint64_t)(len - header_end);
        uint64_t left = (content_len > in_buf) ? (content_len - in_buf) : 0;
        if (left > (8ULL << 20)) { c->discard = 0; c->close_after = 1; }
        else { c->discard = left; c->close_after = 0; }
    }

    /* 访问口令 + CSRF + WS 跨站校验。全部在分发之前。
     * 口令开着时静态资源也要过这一关（只有登录页本身放行），否则未认证
     * 的人会直接看到主界面；认证结果 authed 后面传给 serve_static 用。 */
    {
        int is_ws = (wskey[0] != '\0');
        int is_api = (strncmp(path, "/api/", 5) == 0);
        /* 登录接口本身不能要求口令，否则永远进不去 */
        int is_login = (strcmp(path, "/api/login") == 0);
        if (g_token[0]) {
            char th[192], tq[192], ck[128], want[64];
            th[0] = '\0'; tq[0] = '\0'; ck[0] = '\0';
            header_value(find_header(req, header_end, "X-Lan-Share-Token"),
                         req + header_end, th, sizeof th);
            extract_query_value(query, "token", tq, sizeof tq);
            cookie_token(req, header_end, ck, sizeof ck);
            authed = 0;
            if (th[0] && token_matches(th, g_token)) authed = 1;
            if (!authed && tq[0] && token_matches(tq, g_token)) authed = 1;
            if (!authed && ck[0]) {
                token_digest(want, sizeof want);
                if (token_matches(ck, want)) authed = 1;
            }
        }
        if (is_ws || is_api) {
            if (g_token[0] && !authed && !is_login) {
                static const char body[] = "{\"error\":\"unauthorized\"}";
                queue_response(c, 401, "application/json; charset=utf-8", body, sizeof body - 1);
                return (long)len;
            }
            if (is_api && (strcmp(method, "POST") == 0 || strcmp(method, "PUT") == 0
                           || strcmp(method, "PATCH") == 0 || strcmp(method, "DELETE") == 0)) {
                char csrf[16];
                csrf[0] = '\0';
                header_value(find_header(req, header_end, "X-Lan-Share-Request"),
                             req + header_end, csrf, sizeof csrf);
                if (strcmp(csrf, "1") != 0) {
                    static const char body[] = "{\"error\":\"missing csrf header\"}";
                    queue_response(c, 403, "application/json; charset=utf-8", body, sizeof body - 1);
                    return (long)len;
                }
            }
            if (is_ws) {
                char origin[256], host[256];
                origin[0] = '\0';
                host[0] = '\0';
                header_value(find_header(req, header_end, "Origin"), req + header_end,
                             origin, sizeof origin);
                header_value(find_header(req, header_end, "Host"), req + header_end,
                             host, sizeof host);
                if (!origin_allowed(origin, host)) {
                    static const char body[] = "{\"error\":\"bad origin\"}";
                    queue_response(c, 403, "application/json; charset=utf-8", body, sizeof body - 1);
                    return (long)len;
                }
            }
        }
    }

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

        /* 名字：优先用 /ws?name=（客户端 localStorage 里存的上次名字），
         * 没有就分配一个"主机N"。必须在拼 self 消息前定下来。 */
        if (c->name[0] == '\0') {
            char given[NAME_MAX * 4];
            given[0] = '\0';
            if (extract_query_value(query, "name", given, sizeof given) && given[0]) {
                clean_display_name(given, c->name, sizeof c->name);
            }
            if (c->name[0] == '\0') {
                const char *remembered = name_mem_get(c->ip);
                /* 记忆名还挂在别的在线设备上 = 这个 IP 后面不止一台机器，
                 * 复用就会两台同名，宁可重新编号。 */
                if (remembered && !name_in_use(remembered)) {
                    clean_display_name(remembered, c->name, sizeof c->name);
                }
            }
            if (c->name[0] == '\0') assign_host_name(c->name, sizeof c->name);
        }

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
        c->trace_cand = 40;
        /* 注意：这里绝不能清 c->hlen。调用方靠 `hlen -= used` 推进读指针，
         * 清零会导致下溢成巨大 size_t，随后越界写直接段错误。
         * 101 响应占用了输出缓冲，peers 广播必须等它发完再补发，
         * 否则会把握手响应覆盖掉，浏览器会认为握手失败。 */
        c->pending_hello = 1;
        log_info("WS 就绪 设备=%s ip=%s（当前在线 %d 台）",
                c->name, c->ip, count_ws_clients());
        /* 只吃掉请求头。原来返回 len（"整段都算我吃了"），如果客户端把第一帧
         * 和握手塞在同一个 TCP 段里带过来，那部分字节会被调用方的 memmove
         * 直接丢掉——第一帧没了，此后整个帧流错位，后面全是莫名其妙的报错。
         * 返回 header_end 让调用方把剩余字节挪到缓冲区开头，再由 WS 通路接手
         * （见 client_readable 里 MODE_WS 的分支）。 */
        return (long)header_end;
    }

    if (strcmp(method, "GET") == 0 && strcmp(path, "/api/clipboard") == 0) {
        api_clipboard_meta(c, req, header_end);
        return (long)len;
    }
    if (strcmp(method, "GET") == 0 && strcmp(path, "/api/clipboard/data") == 0) {
        char idstr[16];
        idstr[0] = '\0';
        extract_query_value(query, "id", idstr, sizeof idstr);
        api_clipboard_data(c, idstr[0] ? atoi(idstr) : -1);
        return (long)len;
    }
    if (strcmp(method, "DELETE") == 0 && strcmp(path, "/api/clipboard") == 0) {
        char idstr[16];
        idstr[0] = '\0';
        extract_query_value(query, "id", idstr, sizeof idstr);
        api_clipboard_delete(c, idstr[0] ? atoi(idstr) : -1);
        return (long)len;
    }
    if (strcmp(method, "POST") == 0 && strcmp(path, "/api/clipboard") == 0) {
        char kind[8], mime[80], rawname[NAME_MAX * 4], safe[NAME_MAX * 4];
        char leaf[ID_MAX + 16], temp[PATH_MAX_LOCAL];
        size_t bodylen = (len > header_end) ? (len - header_end) : 0;
        int fd;

        kind[0] = mime[0] = rawname[0] = '\0';
        extract_query_value(query, "kind", kind, sizeof kind);
        extract_query_value(query, "mime", mime, sizeof mime);
        extract_query_value(query, "name", rawname, sizeof rawname);
        if (strcmp(kind, "text") == 0) {
            snprintf(mime, sizeof mime, "text/plain");
            if (!rawname[0]) snprintf(rawname, sizeof rawname, "共享文字.txt");
        } else if (!rawname[0]) {
            snprintf(rawname, sizeof rawname, "shared-image");
        }
        /* 空内容一律拒：以前只拦了空图片，空文字会被当成一条正常记录存下来，
         * 页面上就是一条点不开也删不掉的空白条目。 */
        if (content_len == 0) {
            queue_response(c, 400, "application/json; charset=utf-8",
                           "{\"error\":\"empty clipboard content\"}",
                           sizeof("{\"error\":\"empty clipboard content\"}") - 1);
            return (long)len;
        }
        if (!clipboard_mime_allowed(kind, mime)) {
            queue_response(c, 400, "application/json; charset=utf-8",
                           "{\"error\":\"invalid clipboard content\"}",
                           sizeof("{\"error\":\"invalid clipboard content\"}") - 1);
            return (long)len;
        }
        if (content_len > CLIPBOARD_MAX) {
            queue_response(c, 413, "application/json; charset=utf-8",
                           "{\"error\":\"clipboard item too large\"}",
                           sizeof("{\"error\":\"clipboard item too large\"}") - 1);
            return (long)len;
        }
        sanitize_name(rawname, safe, sizeof safe);
        snprintf(leaf, sizeof leaf, "data-%s.tmp", c->id);
        if (clipboard_file_path(leaf, temp, sizeof temp) != 0) {
            queue_response(c, 500, "application/json; charset=utf-8",
                           "{\"error\":\"path too long\"}", sizeof("{\"error\":\"path too long\"}") - 1);
            return (long)len;
        }
        fd = ls_openwrite_at(temp, 0, 0);
        if (fd < 0) {
            queue_response(c, 500, "application/json; charset=utf-8",
                           "{\"error\":\"create failed\"}", sizeof("{\"error\":\"create failed\"}") - 1);
            return (long)len;
        }
        if ((uint64_t)bodylen > content_len) bodylen = (size_t)content_len;
        if (bodylen && clipboard_write_retry(fd, req + header_end, bodylen) != (long)bodylen) {
            ls_closefile(fd);
            ls_remove_file(temp);
            queue_response(c, 500, "application/json; charset=utf-8",
                           "{\"error\":\"write failed\"}", sizeof("{\"error\":\"write failed\"}") - 1);
            return (long)len;
        }
        c->discard = 0;              /* 剪贴板本体同样由本函数接管 */
        c->close_after = 0;
        c->upfd = fd;
        c->upleft = content_len - (uint64_t)bodylen;
        c->up_reqlen = content_len;
        snprintf(c->upname, sizeof c->upname, "%s", temp);
        snprintf(c->clip_kind, sizeof c->clip_kind, "%s", kind);
        snprintf(c->clip_mime, sizeof c->clip_mime, "%s", mime);
        snprintf(c->clip_name, sizeof c->clip_name, "%s", safe);
        if (c->upleft == 0) finish_clipboard(c);
        else c->mode = MODE_CLIPBOARD;
        return (long)len;
    }

    /* 登录：口令放在查询串里（POST + 空 body），校验通过后下发 HttpOnly Cookie。
     * 口令不放 body 是为了不依赖请求体是否已经完整收到——几 KB 的正文在
     * 慢链路/分片时可能还没到，而查询串和请求头一定在第一批数据里。 */
    if (strcmp(path, "/api/login") == 0) {
        char given[192];
        char dig[64];
        char cookie_hdr[192];
        static const char ok_body[] = "{\"ok\":true}";
        static const char bad_body[] = "{\"error\":\"unauthorized\"}";

        if (strcmp(method, "POST") != 0) {
            queue_response(c, 405, "application/json; charset=utf-8",
                           "{\"error\":\"method not allowed\"}",
                           sizeof("{\"error\":\"method not allowed\"}") - 1);
            return (long)len;
        }
        if (!g_token[0]) {           /* 没开口令：直接放行，前端跳转即可 */
            queue_response(c, 200, "application/json; charset=utf-8", ok_body, sizeof ok_body - 1);
            return (long)len;
        }
        given[0] = '\0';
        extract_query_value(query, "token", given, sizeof given);
        if (!given[0] || !token_matches(given, g_token)) {
            queue_response(c, 401, "application/json; charset=utf-8", bad_body, sizeof bad_body - 1);
            return (long)len;
        }
        token_digest(dig, sizeof dig);
        snprintf(cookie_hdr, sizeof cookie_hdr,
                 "Set-Cookie: ls_auth=%s; Path=/; HttpOnly; SameSite=Strict; Max-Age=2592000\r\n",
                 dig);
        queue_response_extra(c, 200, "application/json; charset=utf-8",
                             ok_body, sizeof ok_body - 1, cookie_hdr);
        return (long)len;
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
        /* uploadDir 须 json_escape 且 body 动态分配：原实现 body[512] 装不下长目录
         * 会截断出残缺 JSON，前端 JSON.parse 失败。 */
        char escdir[PATH_MAX_LOCAL * 2 + 8];
        size_t cap;
        char *body;
        int n;
        json_escape(g_upload_dir, escdir, sizeof escdir);
        cap = strlen(escdir) + 64;
        body = (char *)malloc(cap);
        if (!body) {
            queue_response(c, 503, "application/json; charset=utf-8", "{\"error\":\"oom\"}", sizeof("{\"error\":\"oom\"}") - 1);
            return (long)len;
        }
        n = snprintf(body, cap,
            "{\"peers\":%d,\"uploadDir\":\"%s\"}",
            count_ws_clients(), escdir);
        if (n > 0) queue_response(c, 200, "application/json; charset=utf-8", body, strlen(body));
        free(body);
        return (long)len;
    }
    /* 续传前先问：这个文件已经收了多少字节 */
    if (strcmp(method, "GET") == 0 && strcmp(path, "/api/upload-status") == 0) {
        char rawname[NAME_MAX * 4];
        char safe[NAME_MAX * 4];
        char sizestr[32];
        char esc[NAME_MAX * 8];
        char body[NAME_MAX * 8 + 128];
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
        if (n > 0) queue_response(c, 200, "application/json; charset=utf-8", body, strlen(body));
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

        if (g_quota_bytes) {
            uint64_t need = chunked ? fsize : content_len;
            if (need < content_len) need = content_len;
            if (upload_dir_used() + need > g_quota_bytes) {
                static const char body[] = "{\"error\":\"quota exceeded\"}";
                queue_response(c, 507, "application/json; charset=utf-8", body, sizeof body - 1);
                return (long)len;
            }
        }

        if (chunked) {
            /* 校验本次写入不超出声明大小：否则客户端可声明 size=100 却发 200 字节，
             * 写出越界数据后 finish_upload 直接 rename，最终文件大于声明 size。 */
            if (fsize == 0 || fsize > (1ULL << 40) || foffset > fsize || foffset + content_len > fsize) {
                queue_response(c, 400, "application/json; charset=utf-8",
                               "{\"error\":\"bad range\"}", sizeof("{\"error\":\"bad range\"}") - 1);
                return (long)len;
            }
            uint64_t existing = part_received(safe, fsize);
            if (foffset > existing) {
                /* 客户端要跳到还没收到的位置。顺序写做不到，
                 * 放过去会产出一个中间空洞的文件，直接拒绝让它按实际进度重来。 */
                char body[128];
                int n = snprintf(body, sizeof body,
                    "{\"error\":\"offset 超出已接收范围\",\"received\":%llu}",
                    (unsigned long long)existing);
                if (n > 0) {
                    queue_response(c, 409, "application/json; charset=utf-8", body, strlen(body));
                }
                return (long)len;
            }
            if (part_path_for(safe, fsize, fullpath, sizeof fullpath) != 0) {
                queue_response(c, 400, "application/json; charset=utf-8",
                               "{\"error\":\"path too long\"}", sizeof("{\"error\":\"path too long\"}") - 1);
                return (long)len;
            }
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
            queue_response(c, 500, "application/json; charset=utf-8", "{\"error\":\"create failed\"}", sizeof("{\"error\":\"create failed\"}") - 1);
            return (long)len;
        }
        if (bodylen > 0) {
            if (ls_write(fd, req + header_end, bodylen) != (long)bodylen) {
                ls_closefile(fd);
                queue_response(c, 500, "application/json; charset=utf-8", "{\"error\":\"write failed\"}", sizeof("{\"error\":\"write failed\"}") - 1);
                return (long)len;
            }
        }
        snprintf(c->upname, sizeof c->upname, "%s", fullpath);
        c->discard = 0;              /* 本体由本函数接管，别再当残体排水 */
        c->close_after = 0;
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
        serve_static(c, path, authed);
        return (long)len;
    }

    queue_response(c, 405, "application/json; charset=utf-8", "{\"error\":\"method not allowed\"}", sizeof("{\"error\":\"method not allowed\"}") - 1);
    return (long)len;
}

/* ==========================================================================
 * WebSocket 消息处理
 * ========================================================================== */

/* ICE 候选是"走不走内网"的唯一答案：
 *   host  = 局域网地址直连（这才算走内网）
 *   srflx = NAT 映射后的地址（同一局域网也要绕到路由器外再回来）
 *   relay = TURN 中转（完全没直连）
 * 只打前若干条：32 条 lane 的 trickle 会把日志刷满。 */
/* id → 设备名，只给日志用。找不到就原样返回 id，不返回 NULL。 */
static const char *name_of_id(const char *id) {
    int i;
    for (i = 0; i < MAX_CLIENTS; i++) {
        if (g_clients[i].used && g_clients[i].id[0]
            && strcmp(g_clients[i].id, id) == 0 && g_clients[i].name[0]) {
            return g_clients[i].name;
        }
    }
    return id;
}

static void log_ice_candidate(ls_client *c, const char *to, const char *cand) {
    char buf[256];
    char *tok;
    char transport[8] = "-", addr[64] = "-", port[8] = "-", typ[12] = "?";
    int idx = 0;

    if (c->trace_cand < 0) return;
    if (c->trace_cand == 0) {
        log_info("候选日志已达上限，后续不再打印");
        c->trace_cand = -1;
        return;
    }
    c->trace_cand--;

    snprintf(buf, sizeof buf, "%s", cand);
    if (strncmp(buf, "candidate:", 10) == 0) memmove(buf, buf + 10, strlen(buf + 10) + 1);
    for (tok = strtok(buf, " "); tok; tok = strtok(NULL, " ")) {
        if (idx == 2) snprintf(transport, sizeof transport, "%s", tok);
        else if (idx == 4) snprintf(addr, sizeof addr, "%s", tok);
        else if (idx == 5) snprintf(port, sizeof port, "%s", tok);
        else if (idx == 7) snprintf(typ, sizeof typ, "%s", tok);
        idx++;
    }
    log_dbg("候选 %s→%s %s %s:%s 类型=%s",
            c->name[0] ? c->name : c->id, name_of_id(to),
            transport, addr, port, typ);
}

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
            clean_display_name(nm, c->name, sizeof c->name);
            if (c->name[0] == '\0') snprintf(c->name, sizeof c->name, "%s", nm);
            /* 记住这个名字，换浏览器/清缓存重连时还能认出来 */
            name_mem_set(c->ip, c->name);
            /* 日志里认设备靠名字，id 是随机的没人看得懂 */
            log_info("设备重命名 设备=%s ip=%s", c->name, c->ip);
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
        /* 信令里的 ICE 候选直接决定走内网还是绕公网，值得单独一行 */
        {
            const char *cand = strstr(msg, "\"candidate\":\"");
            if (cand) {
                char cbuf[256];
                size_t k = 0;
                cand += 13;
                while (cand[k] && cand[k] != '"' && k + 1 < sizeof cbuf) {
                    cbuf[k] = cand[k];
                    k++;
                }
                cbuf[k] = '\0';
                log_ice_candidate(c, to, cbuf);
            }
        }
        for (i = 0; i < MAX_CLIENTS; i++) {
            if (!g_clients[i].used || g_clients[i].mode != MODE_WS) continue;
            if (strcmp(g_clients[i].id, to) == 0) {
                forward_signal(&g_clients[i], c->id, msg);
                return;
            }
        }
        {
            /* 必须把目标 id 回给客户端，它才知道该丢掉哪条连接。不回的话
             * 客户端只能干弹提示，旧连接上的 lane 还会继续重发。 */
            char safe_to[ID_MAX * 2];
            char lost[160];
            size_t o = 0;
            const char *p;
            for (p = to; *p && o + 1 < sizeof safe_to; p++) {
                unsigned char ch = (unsigned char)*p;
                if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z')
                    || (ch >= '0' && ch <= '9') || ch == '-' || ch == '_') {
                    safe_to[o++] = (char)ch;
                }
            }
            safe_to[o] = '\0';
            if (c->trace_lost < 3) {
                c->trace_lost++;
                log_info("信令丢失 %s→%s（对方不在线）",
                        c->name[0] ? c->name : c->id, safe_to[0] ? safe_to : "?");
            }
            snprintf(lost, sizeof lost, "{\"type\":\"signal-lost\",\"to\":\"%s\"}", safe_to);
            ws_send_text(c, lost);
        }
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
        /* RFC 6455 §5.5：控制帧不得分片且载荷≤125 */
        if ((opcode == 0x8 || opcode == 0x9 || opcode == 0xA) && (fin == 0 || plen > 125)) {
            client_close_why(c, "控制帧违规");
            return;
        }
        mask[0] = mask[1] = mask[2] = mask[3] = 0;
        if (masked) {
            if (avail < off + 4) return;
            memcpy(mask, b + off, 4);
            off += 4;
        } else {
            /* RFC 6455 §5.1：服务端必须关闭未掩码的客户端帧。
             * 浏览器始终掩码；仅恶意/违规客户端发未掩码帧。 */
            client_close_why(c, "未掩码帧");
            return;
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
                log_dbg("帧 %s op=0x%x fin=%d len=%llu%s",
                        c->name[0] ? c->name : c->id, opcode, fin,
                        (unsigned long long)plen,
                        *budget == 0 ? "（该类帧后续不再逐帧打印）" : "");
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
        c->last_active = time(NULL);

        if (c->mode == MODE_UPLOAD || c->mode == MODE_CLIPBOARD) {
            int clipboard = (c->mode == MODE_CLIPBOARD);
            size_t w = (size_t)n;
            if ((uint64_t)w > c->upleft) w = (size_t)c->upleft;
            if ((clipboard ? clipboard_write_retry(c->upfd, buf, w) : ls_write(c->upfd, buf, w)) != (long)w) {
                client_close_why(c, clipboard ? "剪贴板写盘失败" : "上传写盘失败");
                return;
            }
            c->upleft -= (uint64_t)w;
            if (c->upleft == 0) {
                if (clipboard) finish_clipboard(c);
                else finish_upload(c);
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
            size_t skip = 0;
            if (c->discard > 0) {
                /* 上一条被拒请求的残余 body：丢掉，不进解析缓冲 */
                skip = ((uint64_t)n < c->discard) ? (size_t)n : (size_t)c->discard;
                c->discard -= skip;
                if (skip == (size_t)n) continue;
            }
            if (c->hlen + ((size_t)n - skip) > HEAD_BUF) {
                client_close_why(c, "HTTP 头缓冲超限");
                return;
            }
            memcpy(c->hbuf + c->hlen, buf + skip, (size_t)n - skip);
            c->hlen += (size_t)n - skip;
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
        if (fd >= FD_SETSIZE) {   /* fd 超限则 FD_SET 越界，直接拒收 */
            ls_closesock(fd);
            continue;
        }
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
        g_clients[i].last_active = time(NULL);
        g_clients[i].ws_ping_at = g_clients[i].last_active;
        g_seq++;
        make_id(g_clients[i].id, sizeof g_clients[i].id);
        {
            const char *r = inet_ntop(AF_INET, &addr.sin_addr, g_clients[i].ip, sizeof g_clients[i].ip);
            if (!r) g_clients[i].ip[0] = '\0';
        }
        /* 名字要等到 WS 握手时才定：HTTP 连接是每个请求一条，不能占用编号，
         * 而且 /ws?name= 里的名字只有握手阶段才拿得到。 */
    }
}

/* 回收卡住的连接。没有这一步，半个请求头就能长期占住 24 个槽位之一，
 * 24 个这样的连接就能把服务打死（Slowloris）。WS 用 ping 保活，收不到
 * pong 也没有数据就认为对端已经失联。 */
static void enforce_timeouts(time_t now) {
    int i;
    for (i = 0; i < MAX_CLIENTS; i++) {
        ls_client *c = &g_clients[i];
        int idle;
        if (!c->used) continue;
        idle = (int)(now - c->last_active);
        if (c->mode == MODE_WS) {
            if (idle > 120) {
                client_close_why(c, "WS 空闲超时");
                continue;
            }
            if (now - c->ws_ping_at >= 30) {
                unsigned char ping[16];
                size_t pl = ws_frame_build(0x9, NULL, 0, ping, sizeof ping);
                if (pl) queue_mem(c, (const char *)ping, pl);
                c->ws_ping_at = now;
            }
        } else if (c->mode == MODE_UPLOAD || c->mode == MODE_CLIPBOARD) {
            if (idle > 60) client_close_why(c, "传输超时");
        } else if (c->hlen > 0) {
            if (idle > 20) client_close_why(c, "请求头超时");
        } else if (idle > 120) {
            client_close_why(c, "空闲连接超时");
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
    if (parts_dir_path(parts, sizeof parts) == 0) {
#ifdef _WIN32
        _mkdir(parts);
#else
        mkdir(parts, 0755);
#endif
    } else {
        log_err("半成品目录路径过长（-d 目录 %zu 字节），断点续传将不可用",
                strlen(g_upload_dir));
    }
    ensure_clipboard_dir();
}

/* 超过 24 小时没动过的断点续传半成品，启动时和运行中定期清掉，避免小闪存
 * 被永远传不完的 .part 占满。只动 .lanshare-parts 目录里的 *.part。 */
static void cleanup_stale_parts(void) {
    time_t now = time(NULL);
    char parts[PATH_MAX_LOCAL];
    if (parts_dir_path(parts, sizeof parts) != 0) return;
#ifdef _WIN32
    char pat[PATH_MAX_LOCAL];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    snprintf(pat, sizeof pat, "%s\\*.part", parts);
    h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        ULARGE_INTEGER t;
        uint64_t secs;
        t.LowPart = fd.ftLastWriteTime.dwLowDateTime;
        t.HighPart = fd.ftLastWriteTime.dwHighDateTime;
        secs = t.QuadPart / 10000000ULL;
        if (secs > 11644473600ULL && (uint64_t)now > secs - 11644473600ULL
            && (uint64_t)now - (secs - 11644473600ULL) > 86400ULL) {
            char p[PATH_MAX_LOCAL];
            snprintf(p, sizeof p, "%s\\%s", parts, fd.cFileName);
            DeleteFileA(p);
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR *d = opendir(parts);
    struct dirent *e;
    if (!d) return;
    while ((e = readdir(d)) != NULL) {
        char p[PATH_MAX_LOCAL];
        struct stat st;
        size_t n = strlen(e->d_name);
        if (n < 5 || strcmp(e->d_name + n - 5, ".part") != 0) continue;
        if (path_join(parts, e->d_name, p, sizeof p) != 0) continue;
        if (stat(p, &st) == 0 && S_ISREG(st.st_mode)
            && now > st.st_mtime && now - st.st_mtime > 86400) {
            unlink(p);
        }
    }
    closedir(d);
#endif
}

static void usage(const char *prog) {
    fprintf(stderr,
        "用法: %s [-p 端口] [-d 文件库目录] [-w 前端目录] [--token 口令] [--quota MiB] [-v|-vv]\n"
        "\n"
        "  -p  监听端口，默认 8080\n"
        "  -d  文件库落盘目录，默认 ./uploads（路由器上建议指到外接存储）\n"
        "  -w  前端静态文件目录，默认 ./public\n"
        "  --token  可选访问口令；开启后先显示登录页，认证通过才能进入\n"
        "  --quota  文件库总配额，单位 MiB，默认 4096，0 表示不限制\n"
        "  -v   输出常规日志（连接/上传/下载/访问记录）；默认只输出错误\n"
        "  -vv  额外输出逐帧、逐块等调试日志\n"
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

/* 探测本机在局域网里的 IPv4 地址，让启动横幅直接给出能点开的地址。
 * 做法：建一个 UDP socket 并 connect 到一个外部地址——UDP 的 connect 只设置
 * 对端、不会真的发包，随后 getsockname 拿到内核"选出口"时挑中的那张网卡 IP。
 * 没有任何默认路由（纯内网/无网关，路由器上很常见）时会失败，于是退回
 * 127.0.0.1，横幅里同时把两个地址都列出来。 */
static void detect_local_ip(char *out, size_t cap) {
    struct sockaddr_in probe, me;
    socklen_t mlen = sizeof me;
    int fd;

    snprintf(out, cap, "127.0.0.1");
    fd = (int)socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return;
    memset(&probe, 0, sizeof probe);
    probe.sin_family = AF_INET;
    probe.sin_port = htons(53);
    probe.sin_addr.s_addr = htonl(0x08080808u);      /* 8.8.8.8，仅用于选路 */
    if (connect(fd, (struct sockaddr *)&probe, sizeof probe) == 0
        && getsockname(fd, (struct sockaddr *)&me, &mlen) == 0
        && me.sin_family == AF_INET) {
        unsigned long ip = ntohl(me.sin_addr.s_addr);
        /* 0.0.0.0 / 127.x 说明没选到真实网卡，保持回退值 */
        if (ip != 0 && ((ip >> 24) & 0xff) != 127) {
            snprintf(out, cap, "%lu.%lu.%lu.%lu",
                     (ip >> 24) & 0xff, (ip >> 16) & 0xff,
                     (ip >> 8) & 0xff, ip & 0xff);
        }
    }
    ls_closesock(fd);
}

int main(int argc, char **argv) {
    int listen_fd;
    int stun_fd;
    struct sockaddr_in addr;
    int i;
    int one = 1;
    time_t last_cleanup = 0;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            g_port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
            /* 超长路径不能靠 snprintf 静默截断：截断到别的目录比启动失败更难查 */
            if (strlen(argv[i + 1]) >= sizeof g_upload_dir) {
                fprintf(stderr, "-d 路径过长（上限 %zu 字节）\n", sizeof g_upload_dir - 1);
                return 1;
            }
            snprintf(g_upload_dir, sizeof g_upload_dir, "%s", argv[++i]);
        } else if (strcmp(argv[i], "-w") == 0 && i + 1 < argc) {
            if (strlen(argv[i + 1]) >= sizeof g_web_dir) {
                fprintf(stderr, "-w 路径过长（上限 %zu 字节）\n", sizeof g_web_dir - 1);
                return 1;
            }
            snprintf(g_web_dir, sizeof g_web_dir, "%s", argv[++i]);
        } else if (strcmp(argv[i], "--token") == 0 && i + 1 < argc) {
            const char *t = argv[++i];
            size_t tl = strlen(t);
            if (tl == 0 || tl >= sizeof g_token) {
                fprintf(stderr, "口令长度必须在 1..%lu 之间\n",
                        (unsigned long)(sizeof g_token - 1));
                return 1;
            }
            snprintf(g_token, sizeof g_token, "%s", t);
        } else if (strcmp(argv[i], "--quota") == 0 && i + 1 < argc) {
            char *endp = NULL;
            unsigned long long mb;
            errno = 0;
            mb = strtoull(argv[++i], &endp, 10);
            if (endp == argv[i] || *endp != '\0' || errno == ERANGE || mb > 1024ULL * 1024ULL) {
                fprintf(stderr, "配额必须是 MiB 整数（0 表示不限制）: %s\n", argv[i]);
                return 1;
            }
            g_quota_bytes = mb * 1024ULL * 1024ULL;
        } else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0) {
            /* -v 常规日志（连接/上传/下载/访问），-vv 再叠加逐帧逐块调试细节 */
            if (g_log_level < LOG_INFO) g_log_level = LOG_INFO;
        } else if (strcmp(argv[i], "-vv") == 0) {
            g_log_level = LOG_DEBUG;
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
    cleanup_stale_parts();

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
    {
        char lanip[64];
        detect_local_ip(lanip, sizeof lanip);
        printf("  浏览器打开:  http://%s:%d\n", lanip, g_port);
        if (strcmp(lanip, "127.0.0.1") != 0) {
            printf("                http://127.0.0.1:%d（本机）\n", g_port);
        }
    }
    printf("  访问口令:    %s\n", g_token[0] ? "已启用" : "未启用");
    printf("  文件库配额:  %s\n",
           g_quota_bytes ? "见下方 MiB" : "不限制");
    if (g_quota_bytes) {
        printf("               %llu MiB\n", (unsigned long long)(g_quota_bytes / (1024ULL * 1024ULL)));
    }
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
            /* fd 超出 FD_SETSIZE 时 FD_SET 会越界写 fd_set 位图，直接关掉该连接。 */
            if (g_clients[i].fd < 0 || g_clients[i].fd >= FD_SETSIZE) {
                client_close_why(&g_clients[i], "fd 超出 FD_SETSIZE");
                continue;
            }
            FD_SET(g_clients[i].fd, &rfds);
            if (g_clients[i].out.has || g_clients[i].pendlen > 0) FD_SET(g_clients[i].fd, &wfds);
            if (g_clients[i].fd > maxfd) maxfd = g_clients[i].fd;
        }

        tv.tv_sec = 5;
        tv.tv_usec = 0;
        r = select(maxfd + 1, &rfds, &wfds, NULL, &tv);
        if (r < 0) {
            /* EINTR 被信号打断正常重试；其它真错误记日志，避免静默吞掉导致 busy-loop。 */
            int e = errno;
            if (would_block()) continue;
            log_err("select 错误 errno=%d", e);
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
                if (fr == 1 && g_clients[i].close_after) {
                    client_close_why(&g_clients[i], "响应后关闭（请求体过大，不排水）");
                    continue;
                }
            }
            if (!g_clients[i].used) continue;
            if (FD_ISSET(g_clients[i].fd, &rfds)) client_readable(&g_clients[i]);
        }

        {
            time_t now = time(NULL);
            enforce_timeouts(now);
            if (now - last_cleanup >= 600) {
                cleanup_stale_parts();
                last_cleanup = now;
            }
        }
    }

    return 0;
}
