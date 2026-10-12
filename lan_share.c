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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdint.h>
#include <stdarg.h>
#include <time.h>
#include <fcntl.h>
#include <sys/stat.h>

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
#  include <netinet/in.h>
#  include <netinet/tcp.h>
#  include <arpa/inet.h>
#  define ls_strncasecmp    strncasecmp
#  define ls_write(fd,b,n)  write((fd),(b),(n))
#  define ls_closefile(fd)  close(fd)
/* POSIX 下路径就是原始字节，内核不解释编码，直接用窄字符版本即可 */
static int ls_openwrite(const char *p) {
    return open(p, O_WRONLY | O_CREAT | O_TRUNC, 0644);
}
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
#define IO_BUF          8192
#define HEAD_BUF        8192
#define WS_BUF          16384
#define NAME_MAX        64
#define ID_MAX          40
#define PATH_MAX_LOCAL  512

enum { MODE_HTTP = 0, MODE_WS = 1, MODE_UPLOAD = 2 };

typedef struct {
    char *buf;
    size_t len;
    size_t sent;
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
    int frag_op;
    unsigned char frag[WS_BUF];
    size_t fraglen;

    int upfd;
    uint64_t upleft;
    char upname[PATH_MAX_LOCAL];
    int pending_hello;   /* 握手响应发完后还需补发一次 peers 广播 */

    char pend[IO_BUF];
    size_t pendlen;

    ls_out out;
} ls_client;

static ls_client g_clients[MAX_CLIENTS];
static unsigned long g_seq = 0;
static char g_upload_dir[PATH_MAX_LOCAL] = "./uploads";
static char g_web_dir[PATH_MAX_LOCAL] = "./public";
static int g_port = 8080;

static const char *ADJ[] = { "蓝色", "银色", "安静", "敏捷", "温暖", "明亮", "沉稳", "轻盈", "清澈", "柔和" };
static const char *ANI[] = { "海豚", "猎鹰", "水獭", "狐狸", "鲸鱼", "山猫", "燕子", "小鹿", "海豹", "苍鹭" };

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

static int ls_openwrite(const char *p) {
    wchar_t *w = ls_utf8_to_wide(p);
    int fd;
    if (!w) return -1;
    fd = _wopen(w, _O_WRONLY | _O_CREAT | _O_TRUNC | _O_BINARY, 0644);
    free(w);
    return fd;
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
    out_reset(c);
    c->out.buf = (char *)malloc(len + 1);
    if (!c->out.buf) return;
    memcpy(c->out.buf, data, len);
    c->out.buf[len] = '\0';
    c->out.len = len;
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
        char buf[IO_BUF];
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

static void client_close(ls_client *c) {
    int was_ws = (c->mode == MODE_WS);
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
        queue_response(c, 404, "text/plain; charset=utf-8", "Not Found", 9);
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
    queue_response(c, 200, ctype, body, got);
    free(body);
}

static void api_download(ls_client *c, const char *name) {
    char safe[NAME_MAX * 4];
    char path[PATH_MAX_LOCAL];
    char head[512];
    char encoded[NAME_MAX * 12];
    uint64_t sz;
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

    hl = snprintf(head, sizeof head,
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/octet-stream\r\n"
        "Content-Length: %llu\r\n"
        "Content-Disposition: attachment; filename*=UTF-8''%s\r\n"
        "Cache-Control: no-store\r\n"
        "Connection: keep-alive\r\n\r\n",
        (unsigned long long)sz, encoded);
    if (hl <= 0) {
        fclose(f);
        return;
    }

    /* 响应头必须走 c->pend（flush_out 里 pend 优先级最高，先于 out 发出）。
     * 不能图省事用 queue_mem 再改 is_mem：那样 flush_out 会直接进文件分支，
     * 响应头一个字节都不会发出去，客户端收到的是没有状态行的裸数据。 */
    out_reset(c);
    if ((size_t)hl <= sizeof c->pend) {
        memcpy(c->pend, head, (size_t)hl);
        c->pendlen = (size_t)hl;
    }
    c->out.file = f;
    c->out.is_mem = 0;
    c->out.total = sz;
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

    if (c->upfd >= 0) {
        ls_closefile(c->upfd);
        c->upfd = -1;
    }
    c->mode = MODE_HTTP;
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
        "{\"ok\":true,\"name\":\"%s\",\"size\":%llu,\"sizeText\":\"%s\"}",
        esc, (unsigned long long)sz, szt);
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
        c->fraglen = 0;
        /* 注意：这里绝不能清 c->hlen。调用方靠 `hlen -= used` 推进读指针，
         * 清零会导致下溢成巨大 size_t，随后越界写直接段错误。
         * 101 响应占用了输出缓冲，peers 广播必须等它发完再补发，
         * 否则会把握手响应覆盖掉，浏览器会认为握手失败。 */
        c->pending_hello = 1;
        return (long)len;
    }

    if (strcmp(method, "GET") == 0 && strcmp(path, "/api/files") == 0) {
        api_list_files(c);
        return (long)len;
    }
    if (strcmp(method, "GET") == 0 && strcmp(path, "/api/download") == 0) {
        char name[NAME_MAX * 4];
        name[0] = '\0';
        extract_query_value(query, "name", name, sizeof name);
        api_download(c, name);
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
    if (strcmp(method, "POST") == 0 && strcmp(path, "/api/upload") == 0) {
        char rawname[NAME_MAX * 4];
        char safe[NAME_MAX * 4];
        char fullpath[PATH_MAX_LOCAL];
        size_t bodylen = (len > header_end) ? (len - header_end) : 0;
        int fd;

        rawname[0] = '\0';
        extract_query_value(query, "name", rawname, sizeof rawname);
        sanitize_name(rawname, safe, sizeof safe);
        unique_path(safe, fullpath, sizeof fullpath);

        fd = ls_openwrite(fullpath);
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
            client_close(c);
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

        if (opcode == 0x8) {
            client_close(c);
            return;
        }

        if (opcode == 0x0) {
            if (c->fraglen + (size_t)plen > WS_BUF) {
                client_close(c);
                return;
            }
            memcpy(c->frag + c->fraglen, payload, (size_t)plen);
            c->fraglen += (size_t)plen;
            if (fin && c->frag_op == 0x1) {
                c->frag[c->fraglen] = '\0';
                handle_ws_message(c, (const char *)c->frag, c->fraglen);
                c->fraglen = 0;
            }
        } else if (opcode == 0x1) {
            if (!fin) {
                c->frag_op = 0x1;
                c->fraglen = 0;
                memcpy(c->frag, payload, (size_t)plen);
                c->fraglen = (size_t)plen;
            } else {
                char tmp[WS_BUF + 1];
                memcpy(tmp, payload, (size_t)plen);
                tmp[plen] = '\0';
                handle_ws_message(c, tmp, (size_t)plen);
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
    int n = (int)recv(c->fd, buf, sizeof buf, 0);

    if (n == 0) {
        client_close(c);
        return;
    }
    if (n < 0) {
        if (would_block()) return;
        client_close(c);
        return;
    }

    if (c->mode == MODE_UPLOAD) {
        size_t w = (size_t)n;
        if ((uint64_t)w > c->upleft) w = (size_t)c->upleft;
        if (ls_write(c->upfd, buf, w) != (long)w) {
            client_close(c);
            return;
        }
        c->upleft -= (uint64_t)w;
        if (c->upleft == 0) finish_upload(c);
        return;
    }

    if (c->mode == MODE_WS) {
        if (c->wlen + (size_t)n > WS_BUF) {
            client_close(c);
            return;
        }
        memcpy(c->wbuf + c->wlen, buf, (size_t)n);
        c->wlen += (size_t)n;
        ws_consume(c);
        return;
    }

    if (c->hlen + (size_t)n > HEAD_BUF) {
        client_close(c);
        return;
    }
    memcpy(c->hbuf + c->hlen, buf, (size_t)n);
    c->hlen += (size_t)n;
    c->hbuf[c->hlen] = '\0';
    for (;;) {
        long used = process_request(c, c->hbuf, c->hlen);
        if (used < 0) {
            client_close(c);
            return;
        }
        if (used == 0) return;
        memmove(c->hbuf, c->hbuf + used, c->hlen - (size_t)used);
        c->hlen -= (size_t)used;
        c->hbuf[c->hlen] = '\0';
        if (c->hlen == 0) return;
        if (c->mode != MODE_HTTP) return;
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
        random_name(g_clients[i].name, sizeof g_clients[i].name);
        {
            const char *r = inet_ntop(AF_INET, &addr.sin_addr, g_clients[i].ip, sizeof g_clients[i].ip);
            if (!r) g_clients[i].ip[0] = '\0';
        }
    }
}

/* 创建上传目录。已存在时各平台都会返回失败，这里不关心返回值。 */
static void ensure_upload_dir(void) {
#ifdef _WIN32
    _mkdir(g_upload_dir);
#else
    mkdir(g_upload_dir, 0755);
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

int main(int argc, char **argv) {
    int listen_fd;
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
    srand((unsigned)time(NULL) ^ (unsigned)((uintptr_t)listen_fd * 2654435761u));

    printf("LAN Share (C) 已启动\n");
    printf("  端口:        %d\n", g_port);
    printf("  文件库目录:  %s\n", g_upload_dir);
    printf("  前端目录:    %s\n", g_web_dir);
    printf("  浏览器打开:  http://<路由器IP>:%d\n", g_port);
    printf("  设备间传文件走 WebRTC 直连，本程序只转发信令。\n\n");
    fflush(stdout);

    for (;;) {
        fd_set rfds, wfds;
        struct timeval tv;
        int maxfd = listen_fd;
        int r;

        FD_ZERO(&rfds);
        FD_ZERO(&wfds);
        FD_SET(listen_fd, &rfds);
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

        for (i = 0; i < MAX_CLIENTS; i++) {
            if (!g_clients[i].used) continue;
            if (FD_ISSET(g_clients[i].fd, &wfds)) {
                int fr = flush_out(&g_clients[i]);
                if (fr < 0) {
                    client_close(&g_clients[i]);
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
