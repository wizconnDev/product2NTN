
#if 0
/*********************** toNTNSatellite.c (stack-safe / stream JSON) ************************/

#include "toNTN.h"
#include "usart.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "debug.h"

/* ========= 配置区 ========= */
#define NTN_UART (&huart1)

/* ========= 全局状态 ========= */
NetConfig g_ntn_cfg;
volatile int g_ntn_cfg_ready = 0;

uint8_t ntn_rx_buf[NTN_RX_BUF_SZ];
volatile int ntn_rx_len = 0;

volatile int g_ntn_hello_done = 0;

int g_user_sock = -1;               // 长连接：用户 server socket
volatile int g_user_sock_ready = 0; // 1=socket 已经连上并可复用

/* ✅ UDP 复用 socket（必须是文件全局变量） */
static int g_udp_sock = -1;
static volatile int g_udp_sock_ready = 0;

static void NTN_DropUdpSock(const char* reason);
static int  NTN_EnsureUdpSockReady(void);
static int g_use_udp_first_for_cfg = 0;   // 1=NTN卡先UDP，0=普通卡先TCP
static char g_device_imei[24] = {0};      // 15位IMEI足够，这里留大一点

/* ========== LoRa Queue ========== */
#define LORA_Q_MAX          7
#define LORA_Q_PAYLOAD_MAX  64

typedef struct {
    uint8_t  src;
    uint8_t   len;
    uint8_t  data[LORA_Q_PAYLOAD_MAX];
} TxMsg;

static TxMsg  g_lora_q[LORA_Q_MAX];
static volatile uint8_t g_lora_q_w = 0;
static volatile uint8_t g_lora_q_r = 0;

/* 额外：flush 用的临时缓存（避免在栈上放 TxMsg） */
static TxMsg g_flush_msg;

/* ========== 不上栈的大缓冲（全部静态） ========== */
#define NTN_MAX_SEND_PAYLOAD   128   /* 你 WithSrc 里用到 300，保持一致 */
static uint8_t g_withsrc_buf[NTN_MAX_SEND_PAYLOAD];     /* 用于 NTN_Send_Payload_WithSrc */
static char    g_hex_buf[NTN_MAX_SEND_PAYLOAD * 2 + 4]; /* 600+ */
static char    g_cmd_buf[720];                          /* NSOSD/NSOST 拼 AT */

static char    g_http_get[256];

//失效流程
void NTN_Invalidate_UserSock_AndCfg(const char* reason)
{
    if (reason)
    {
        HAL_UART_Transmit(&hlpuart1, (uint8_t*)reason, (uint16_t)strlen(reason), 200);
        HAL_UART_Transmit(&hlpuart1, (uint8_t*)"\r\n", 2, 200);
    }

    // 1) 复用 socket 失效
    g_user_sock_ready = 0;
    g_user_sock = -1;

    // 2) 强制重新拉配置 + 重新 hello
    g_ntn_cfg_ready = 0;
    g_ntn_hello_done = 0;

    /* ✅ 同时丢弃 UDP 复用 socket */
       NTN_DropUdpSock("[NTN] invalidate -> drop udp sock");
}


/* ========== JSON 流式提取（避免 http_hex_total[1500]） ========== */
typedef struct {
    uint8_t started;
    int     depth;
    size_t  j;
} JsonStreamState;


static int hex_val(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

typedef struct {
    size_t j;
} LineStreamState;

static LineStreamState g_ls;
static char g_line_buf[96];

static void LineStream_Reset(void)
{
    g_ls.j = 0;
    g_line_buf[0] = '\0';
}

/* 从 HEX 里提取一行 ASCII，遇到 '\n' 返回 1 */
static int LineStream_FeedHex(const char *hex, int hex_len)
{
    for (int i = 0; i + 1 < hex_len; i += 2)
    {
        int vh = hex_val(hex[i]);
        int vl = hex_val(hex[i+1]);
        if (vh < 0 || vl < 0) continue;

        char c = (char)((vh << 4) | vl);

        if (c == '\n') return 1; // 一行结束

        if (c >= 0x20 && c <= 0x7E) // 可见字符
        {
            if (g_ls.j < sizeof(g_line_buf) - 1)
            {
                g_line_buf[g_ls.j++] = c;
                g_line_buf[g_ls.j] = '\0';
            }
            else
            {
                return 0; // 太长
            }
        }
    }
    return 0;
}



/* ========== 小工具：rx len 快照/安全终止 ========== */
static inline int NTN_SnapshotRxLen(void)
{
    int len;
    __disable_irq();
    len = ntn_rx_len;
    __enable_irq();
    if (len < 0) len = 0;
    if (len >= (int)sizeof(ntn_rx_buf)) len = (int)sizeof(ntn_rx_buf) - 1;
    return len;
}

static inline void NTN_RxTerminateSafe(void)
{
    int len = NTN_SnapshotRxLen();
    ntn_rx_buf[len] = '\0';
}

static int NTN_ReadImei(char *out, int out_cap)
{
    if (!out || out_cap < 16) return 0;

    const char *cmd_list[] = {
        "AT+CGSN=1\r\n",
    };

    for (int k = 0; k < 2; k++)
    {
        ntn_rx_len = 0;
        memset(ntn_rx_buf, 0, sizeof(ntn_rx_buf));

        HAL_UART_Transmit(&huart1,   (uint8_t*)cmd_list[k], (uint16_t)strlen(cmd_list[k]), 500);
        HAL_UART_Transmit(&hlpuart1, (uint8_t*)cmd_list[k], (uint16_t)strlen(cmd_list[k]), 500);

        uint32_t deadline = HAL_GetTick() + 2000;
        while (HAL_GetTick() < deadline)
        {
            NTN_RxTerminateSafe();

            if (strstr((char*)ntn_rx_buf, "OK"))
            {
                char *p = (char*)ntn_rx_buf;
                while (*p)
                {
                    if (*p >= '0' && *p <= '9')
                    {
                        int j = 0;
                        while (p[j] >= '0' && p[j] <= '9' && j < 20) j++;

                        if (j >= 15)
                        {
                            int copy_len = (j < out_cap - 1) ? j : (out_cap - 1);
                            memcpy(out, p, copy_len);
                            out[copy_len] = '\0';

                            HAL_UART_Transmit(&hlpuart1, (uint8_t*)"IMEI: ", 6, 100);
                            HAL_UART_Transmit(&hlpuart1, (uint8_t*)out, strlen(out), 100);
                            HAL_UART_Transmit(&hlpuart1, (uint8_t*)"\r\n", 2, 100);

                            return 1;
                        }

                        p += j;
                    }
                    else
                    {
                        p++;
                    }
                }

                break;
            }

            if (strstr((char*)ntn_rx_buf, "ERROR") || strstr((char*)ntn_rx_buf, "+CME ERROR"))
            {
                break;
            }

            HAL_Delay(20);
        }
    }

    return 0;
}

static int NTN_EnsureImeiReady(void)
{
    if (g_device_imei[0] != '\0')
        return 1;

    if (NTN_ReadImei(g_device_imei, sizeof(g_device_imei)))
    {
        HAL_UART_Transmit(&hlpuart1, (uint8_t*)"IMEI: ", 6, 100);
        HAL_UART_Transmit(&hlpuart1, (uint8_t*)g_device_imei, (uint16_t)strlen(g_device_imei), 100);
        HAL_UART_Transmit(&hlpuart1, (uint8_t*)"\r\n", 2, 100);
        return 1;
    }

    HAL_UART_Transmit(&hlpuart1, (uint8_t*)"Read IMEI FAIL\r\n", 16, 100);
    return 0;
}

/* ========== 工具：bytes -> HEX（用静态 g_hex_buf） ========== */
static int NTN_BytesToHex_Into(const uint8_t* data, int len, char* out, int out_cap)
{
    static const char hex_tab[] = "0123456789ABCDEF";
    int need = len * 2;
    if (out_cap < need + 1) return 0;

    int j = 0;
    for (int i = 0; i < len; ++i)
    {
        uint8_t b = data[i];
        out[j++] = hex_tab[b >> 4];
        out[j++] = hex_tab[b & 0x0F];
    }
    out[j] = '\0';
    return 1;
}

/* ========== 关 socket（静态 cmd 缓冲） ========== */
static void NTN_CloseSocket(int sock_id)
{
    int n = snprintf(g_cmd_buf, sizeof(g_cmd_buf), "AT+NSOCL=%d\r\n", sock_id);
    HAL_UART_Transmit(&huart1,   (uint8_t*)g_cmd_buf, n, 200);
    HAL_UART_Transmit(&hlpuart1, (uint8_t*)g_cmd_buf, n, 200);
}

static void NTN_DropUdpSock(const char* reason)
{
    if (reason)
    {
        HAL_UART_Transmit(&hlpuart1, (uint8_t*)reason, (uint16_t)strlen(reason), 200);
        HAL_UART_Transmit(&hlpuart1, (uint8_t*)"\r\n", 2, 200);
    }

    if (g_udp_sock_ready && g_udp_sock >= 0)
    {
        NTN_CloseSocket(g_udp_sock);   // 只在明确要丢弃时才关
    }
    g_udp_sock_ready = 0;
    g_udp_sock = -1;
}

/* 确保 UDP socket 已创建（只创建一次） */
static int NTN_EnsureUdpSockReady(void)
{
    if (g_udp_sock_ready && g_udp_sock >= 0) return 1;

    // 清 rx，避免解析被旧数据干扰
    ntn_rx_len = 0;
    memset(ntn_rx_buf, 0, sizeof(ntn_rx_buf));

    const char *cmd1 = "AT+NSOCR=DGRAM,17,0,1\r\n";
    HAL_UART_Transmit(&huart1,   (uint8_t*)cmd1, (uint16_t)strlen(cmd1), 200);
    HAL_UART_Transmit(&hlpuart1, (uint8_t*)cmd1, (uint16_t)strlen(cmd1), 200);

    int timeout = (int)(HAL_GetTick() + 2500);
    while ((int)HAL_GetTick() < timeout)
    {
        NTN_RxTerminateSafe();

        // 遇到 busy/不允许：退避一下，不要清 cfg
        if (strstr((char*)ntn_rx_buf, "+CME ERROR:8007"))
        {
            HAL_Delay(1500);
            return 0;
        }
        if (strstr((char*)ntn_rx_buf, "+CME ERROR:8002"))
        {
            HAL_Delay(1500);
            return 0;
        }

        char *p = strstr((char*)ntn_rx_buf, "+NSOCR:");
        if (p)
        {
            g_udp_sock = atoi(p + 7);
            g_udp_sock_ready = 1;
            return 1;
        }
        HAL_Delay(10);
    }

    return 0;
}


/* ========== LoRa Queue ========== */
static int NTN_LoraQ_IsFull(void)
{
    uint8_t next = (uint8_t)((g_lora_q_w + 1) % LORA_Q_MAX);
    return next == g_lora_q_r;
}

static int NTN_LoraQ_IsEmpty(void)
{
    return g_lora_q_w == g_lora_q_r;
}

int NTN_EnqueueLoRa(const uint8_t *data, uint16_t len)
{
    if (!data || len == 0) return 0;

    if (len == 1 && data[0] == 0x00) return 0;
    if (len > 60) return 0;
    if (len > LORA_Q_PAYLOAD_MAX) return 0;

    __disable_irq();
    if (NTN_LoraQ_IsFull()) { __enable_irq(); return 0; }

    uint8_t w = g_lora_q_w;
    g_lora_q[w].src = 0x25;
    g_lora_q[w].len = len;
    memcpy(g_lora_q[w].data, data, len);

    g_lora_q_w = (uint8_t)((g_lora_q_w + 1) % LORA_Q_MAX);
    __enable_irq();
    return 1;
}

void NTN_FlushLoRaQueue(void)
{
    if (NTN_LoraQ_IsEmpty()) return;

    __disable_irq();
    uint8_t r = g_lora_q_r;
    g_flush_msg = g_lora_q[r];     /* 复制到静态区，避免栈上 TxMsg */
    __enable_irq();

    int ok = NTN_Send_Payload_WithSrc(g_flush_msg.src, g_flush_msg.data, g_flush_msg.len);
    if (!ok) return;

    __disable_irq();
    g_lora_q_r = (uint8_t)((g_lora_q_r + 1) % LORA_Q_MAX);
    __enable_irq();
}

/* ========== JSON -> cfg（尽量减少 sscanf 负担，但你这里先保留 sscanf，也不炸栈） ========== */
int NTN_ParseConfigJson(const char *json, NetConfig *cfg)
{
    char *p;

    memset(cfg, 0, sizeof(*cfg));

    p = strstr(json, "\"protocol\"");
    if (!p) return 0;
    p = strchr(p, ':');
    if (!p) return 0;
    p++;
    while (*p == ' ' || *p == '\"') p++;
    sscanf(p, "%7[^\"]", cfg->protocol);

    p = strstr(json, "\"ip\"");
    if (!p) return 0;
    p = strchr(p, ':');
    if (!p) return 0;
    p++;
    while (*p == ' ' || *p == '\"') p++;
    sscanf(p, "%31[^\"]", cfg->ip);

    p = strstr(json, "\"port\"");
    if (!p) return 0;
    p = strchr(p, ':');
    if (!p) return 0;
    p++;
    cfg->port = atoi(p);

    return 1;
}



//提取文本
static void NTN_CommitCfgFromLine(void)
{
    int protoCode = 0;
    char host[32] = {0};
    int port = 0;

    // 期望: "0,43.139.170.206,29848"
    if (sscanf(g_line_buf, "%d,%31[^,],%d", &protoCode, host, &port) != 3)
    {
        HAL_UART_Transmit(&hlpuart1, (uint8_t*)"CFG line parse fail\r\n", 21, 100);
        return;
    }

    memset(&g_ntn_cfg, 0, sizeof(g_ntn_cfg));
    strcpy(g_ntn_cfg.protocol, (protoCode == 1) ? "UDP" : "TCP");
    strncpy(g_ntn_cfg.ip, host, sizeof(g_ntn_cfg.ip) - 1);
    g_ntn_cfg.port = port;
    g_ntn_cfg_ready = 1;

    int n = snprintf(g_cmd_buf, sizeof(g_cmd_buf),
                     "CFG: %s %s:%d\r\n",
                     g_ntn_cfg.protocol, g_ntn_cfg.ip, g_ntn_cfg.port);
    HAL_UART_Transmit(&hlpuart1, (uint8_t*)g_cmd_buf, n, 100);
}


/* ========== 对外接口：确保配置一定就绪（带重试） ========== */
static void NTN_TryFetchConfigOnce(void);

int NTN_EnsureConfigReady(int max_retry, uint32_t wait_ms_each)
{
    for (int attempt = 0; attempt < max_retry; ++attempt)
    {
        if (g_ntn_cfg_ready) return 1;

        NTN_TryFetchConfigOnce();

        uint32_t deadline = HAL_GetTick() + wait_ms_each;
        while (HAL_GetTick() < deadline)
        {
            if (g_ntn_cfg_ready) return 1;
            HAL_Delay(50);
        }
    }

    HAL_UART_Transmit(&hlpuart1,
                      (uint8_t*)"CFG still not ready after retries\r\n",
                      36, 100);
    return 0;
}

/* UDP send cmd */
#define NTN_UDP_SEND_CMD "AT+NSOST"

/* ========== 统一入口：按 g_ntn_cfg 发送 data（TCP/UDP） ========== */
/* ========== 统一入口：按 g_ntn_cfg 发送 data（TCP/UDP） ========== */
int NTN_SendPayloadUsingCfg(const uint8_t* data, int len)
{
    if (!data || len <= 0) return 0;

    if (len > (NTN_MAX_SEND_PAYLOAD - 1))
    {
        Simple_Print("Payload too large\r\n");
        return 0;
    }

    /* 1) 先确保 cfg 就绪 */
    if (!NTN_EnsureConfigReady(3, 15000))
        return 0;

    int n = snprintf(g_cmd_buf, sizeof(g_cmd_buf),
                     "SEND using CFG: %s %s:%d, len=%d\r\n",
                     g_ntn_cfg.protocol, g_ntn_cfg.ip, g_ntn_cfg.port, len);
    HAL_UART_Transmit(&hlpuart1, (uint8_t*)g_cmd_buf, n, 100);

    /* 2) payload -> hex */
    if (!NTN_BytesToHex_Into(data, len, g_hex_buf, sizeof(g_hex_buf)))
    {
        HAL_UART_Transmit(&hlpuart1,
                          (uint8_t*)"Payload too large for HEX buffer\r\n",
                          35, 100);
        return 0;
    }

    int sock_id = -1;
    int timeout = 0;
    int found = 0;

    /* 清 rx */
    ntn_rx_len = 0;
    memset(ntn_rx_buf, 0, sizeof(ntn_rx_buf));

    /* =========================================================
     * TCP 分支
     * ========================================================= */
    if (strcmp(g_ntn_cfg.protocol, "TCP") == 0)
    {
        /* -----------------------------------------------------
         * A) 如果已有可复用 TCP socket，直接发
         * ----------------------------------------------------- */
        if (g_user_sock_ready && g_user_sock >= 0)
        {
            ntn_rx_len = 0;
            memset(ntn_rx_buf, 0, sizeof(ntn_rx_buf));

            n = snprintf(g_cmd_buf, sizeof(g_cmd_buf),
                         "AT+NSOSD=%d,%d,%s\r\n",
                         g_user_sock, len, g_hex_buf);

            HAL_UART_Transmit(&huart1,   (uint8_t*)g_cmd_buf, n, 500);
            HAL_UART_Transmit(&hlpuart1, (uint8_t*)g_cmd_buf, n, 500);

            HAL_UART_Transmit(&hlpuart1,
                              (uint8_t*)"SEND via existing TCP socket\r\n",
                              29, 100);

            timeout = (int)(HAL_GetTick() + 3000);
            found = 0;
            while ((int)HAL_GetTick() < timeout)
            {
                NTN_RxTerminateSafe();

                if (strstr((char*)ntn_rx_buf, "OK"))
                {
                    found = 1;
                    break;
                }

                if (strstr((char*)ntn_rx_buf, "+CME ERROR:8002"))
                {
                    HAL_UART_Transmit(&hlpuart1,
                                      (uint8_t*)"[TCP] existing socket broken\r\n",
                                      30, 100);
                    g_user_sock_ready = 0;
                    g_user_sock = -1;
                    return 0;
                }

                if (strstr((char*)ntn_rx_buf, "ERROR") ||
                    strstr((char*)ntn_rx_buf, "+CME ERROR"))
                {
                    HAL_UART_Transmit(&hlpuart1,
                                      (uint8_t*)"[TCP] existing socket send error\r\n",
                                      33, 100);
                    g_user_sock_ready = 0;
                    g_user_sock = -1;
                    return 0;
                }

                HAL_Delay(10);
            }

            if (found)
                return 1;

            HAL_UART_Transmit(&hlpuart1,
                              (uint8_t*)"[TCP] existing socket send timeout\r\n",
                              35, 100);
            g_user_sock_ready = 0;
            g_user_sock = -1;
            return 0;
        }

        /* -----------------------------------------------------
         * B) 没有复用 socket，新建 TCP socket
         * ----------------------------------------------------- */
        const char *cmd1 = "AT+NSOCR=STREAM,6,0,1\r\n";
        HAL_UART_Transmit(&huart1,   (uint8_t*)cmd1, (uint16_t)strlen(cmd1), 200);
        HAL_UART_Transmit(&hlpuart1, (uint8_t*)cmd1, (uint16_t)strlen(cmd1), 200);

        timeout = (int)(HAL_GetTick() + 3000);
        found = 0;
        while ((int)HAL_GetTick() < timeout)
        {
            NTN_RxTerminateSafe();

            char *p = (char*)ntn_rx_buf;
            char *last = NULL;
            while ((p = strstr(p, "+NSOCR:")) != NULL)
            {
                last = p;
                p += 7;
            }

            if (last)
            {
                char *line_end = strpbrk(last, "\r\n");
                if (!line_end)
                {
                    HAL_Delay(10);
                    continue;
                }

                sock_id = atoi(last + 7);

                int dbg = snprintf(g_cmd_buf, sizeof(g_cmd_buf),
                                   "[DBG] TCP parsed sock_id=%d\r\n", sock_id);
                HAL_UART_Transmit(&hlpuart1, (uint8_t*)g_cmd_buf, dbg, 200);

                found = 1;
                break;
            }

            HAL_Delay(10);
        }

        if (!found)
        {
            HAL_UART_Transmit(&hlpuart1,
                              (uint8_t*)"TCP: Parse socket FAIL\r\n",
                              24, 100);
            return 0;
        }

        /* 等 NSOCR 后面的 OK */
        timeout = (int)(HAL_GetTick() + 2000);
        found = 0;
        while ((int)HAL_GetTick() < timeout)
        {
            NTN_RxTerminateSafe();

            if (strstr((char*)ntn_rx_buf, "OK"))
            {
                found = 1;
                break;
            }

            if (strstr((char*)ntn_rx_buf, "ERROR") ||
                strstr((char*)ntn_rx_buf, "+CME ERROR"))
            {
                break;
            }

            HAL_Delay(10);
        }

        if (!found)
        {
            HAL_UART_Transmit(&hlpuart1,
                              (uint8_t*)"TCP: No OK after NSOCR\r\n",
                              24, 100);
            if (sock_id >= 0)
                NTN_CloseSocket(sock_id);
            return 0;
        }

        /* -----------------------------------------------------
         * C) Connect
         * ----------------------------------------------------- */
        ntn_rx_len = 0;
        memset(ntn_rx_buf, 0, sizeof(ntn_rx_buf));

        int dbg2 = snprintf(g_cmd_buf, sizeof(g_cmd_buf),
                            "[DBG] TCP connect using sock_id=%d ip=%s port=%d\r\n",
                            sock_id, g_ntn_cfg.ip, g_ntn_cfg.port);
        HAL_UART_Transmit(&hlpuart1, (uint8_t*)g_cmd_buf, dbg2, 200);

        n = snprintf(g_cmd_buf, sizeof(g_cmd_buf),
                     "AT+NSOCO=%d,%s,%d\r\n",
                     sock_id, g_ntn_cfg.ip, g_ntn_cfg.port);

        HAL_UART_Transmit(&huart1,   (uint8_t*)g_cmd_buf, n, 200);
        HAL_UART_Transmit(&hlpuart1, (uint8_t*)g_cmd_buf, n, 200);

        timeout = (int)(HAL_GetTick() + 20000);
        found = 0;
        while ((int)HAL_GetTick() < timeout)
        {
            NTN_RxTerminateSafe();

            if (strstr((char*)ntn_rx_buf, "OK"))
            {
                found = 1;
                break;
            }

            if (strstr((char*)ntn_rx_buf, "ERROR") ||
                strstr((char*)ntn_rx_buf, "+CME ERROR"))
            {
                break;
            }

            HAL_Delay(10);
        }

        if (!found)
        {
            HAL_UART_Transmit(&hlpuart1,
                              (uint8_t*)"TCP: Connect FAIL\r\n",
                              18, 100);

            HAL_UART_Transmit(&hlpuart1,
                              (uint8_t*)"TCP got: ",
                              9, 100);
            NTN_RxTerminateSafe();
            HAL_UART_Transmit(&hlpuart1,
                              ntn_rx_buf,
                              (uint16_t)strlen((char*)ntn_rx_buf),
                              300);
            HAL_UART_Transmit(&hlpuart1,
                              (uint8_t*)"\r\n",
                              2, 100);

            if (sock_id >= 0)
                NTN_CloseSocket(sock_id);

            /* 这里只丢 TCP socket，不清 cfg */
            g_user_sock_ready = 0;
            g_user_sock = -1;
            return 0;
        }

        /* -----------------------------------------------------
         * D) Send
         * ----------------------------------------------------- */
        ntn_rx_len = 0;
        memset(ntn_rx_buf, 0, sizeof(ntn_rx_buf));

        n = snprintf(g_cmd_buf, sizeof(g_cmd_buf),
                     "AT+NSOSD=%d,%d,%s\r\n",
                     sock_id, len, g_hex_buf);

        HAL_UART_Transmit(&huart1,   (uint8_t*)g_cmd_buf, n, 500);
        HAL_UART_Transmit(&hlpuart1, (uint8_t*)g_cmd_buf, n, 500);

        timeout = (int)(HAL_GetTick() + 5000);
        found = 0;
        while ((int)HAL_GetTick() < timeout)
        {
            NTN_RxTerminateSafe();

            if (strstr((char*)ntn_rx_buf, "OK"))
            {
                found = 1;
                break;
            }

            if (strstr((char*)ntn_rx_buf, "ERROR") ||
                strstr((char*)ntn_rx_buf, "+CME ERROR"))
            {
                break;
            }

            HAL_Delay(10);
        }

        if (!found)
        {
            HAL_UART_Transmit(&hlpuart1,
                              (uint8_t*)"TCP: No OK after NSOSD\r\n",
                              24, 100);

            HAL_UART_Transmit(&hlpuart1,
                              (uint8_t*)"TCP got: ",
                              9, 100);
            NTN_RxTerminateSafe();
            HAL_UART_Transmit(&hlpuart1,
                              ntn_rx_buf,
                              (uint16_t)strlen((char*)ntn_rx_buf),
                              300);
            HAL_UART_Transmit(&hlpuart1,
                              (uint8_t*)"\r\n",
                              2, 100);

            if (sock_id >= 0)
                NTN_CloseSocket(sock_id);

            g_user_sock_ready = 0;
            g_user_sock = -1;
            return 0;
        }

        /* -----------------------------------------------------
         * E) 成功后保存复用
         * ----------------------------------------------------- */
        g_user_sock = sock_id;
        g_user_sock_ready = 1;
        return 1;
    }

    /* =========================================================
     * UDP 分支
     * ========================================================= */
    if (strcmp(g_ntn_cfg.protocol, "UDP") == 0)
    {
        if (!NTN_EnsureUdpSockReady())
        {
            HAL_UART_Transmit(&hlpuart1,
                              (uint8_t*)"UDP: sock not ready\r\n",
                              21, 100);
            return 0;
        }

        sock_id = g_udp_sock;

        ntn_rx_len = 0;
        memset(ntn_rx_buf, 0, sizeof(ntn_rx_buf));

        n = snprintf(g_cmd_buf, sizeof(g_cmd_buf),
                     "%s=%d,%s,%d,%d,%s\r\n",
                     NTN_UDP_SEND_CMD,
                     sock_id, g_ntn_cfg.ip, g_ntn_cfg.port, len, g_hex_buf);

        HAL_UART_Transmit(&hlpuart1, (uint8_t*)g_cmd_buf, n, 500);
        HAL_UART_Transmit(&huart1,   (uint8_t*)g_cmd_buf, n, 500);

        timeout = (int)(HAL_GetTick() + 3000);
        found = 0;
        while ((int)HAL_GetTick() < timeout)
        {
            NTN_RxTerminateSafe();

            if (strstr((char*)ntn_rx_buf, "OK"))
            {
                found = 1;
                break;
            }

            if (strstr((char*)ntn_rx_buf, "+CME ERROR"))
            {
                NTN_DropUdpSock("[UDP] CME -> drop udp sock");
                return 0;
            }

            if (strstr((char*)ntn_rx_buf, "ERROR"))
            {
                break;
            }

            HAL_Delay(10);
        }

        if (!found)
        {
            HAL_UART_Transmit(&hlpuart1,
                              (uint8_t*)"UDP: Send FAIL\r\n",
                              16, 100);
            NTN_DropUdpSock("[UDP] send fail -> drop udp sock");
            return 0;
        }

        return 1;
    }

    HAL_UART_Transmit(&hlpuart1,
                      (uint8_t*)"Unknown cfg protocol\r\n",
                      22, 100);
    return 0;
}
/* ========== WithSrc 封装（不再用栈上 buf[300]） ========== */
int NTN_Send_Payload_WithSrc(uint8_t src, const uint8_t *data, uint16_t len)
{
    if (!data || len == 0) return 0;
    if ((uint32_t)len + 1 > sizeof(g_withsrc_buf))
    {
        HAL_UART_Transmit(&hlpuart1,
                          (uint8_t*)"NTN_Send_Payload_WithSrc: payload too large\r\n",
                          48, 100);
        return 0;
    }

    uint16_t pos = 0;
    g_withsrc_buf[pos++] = src;
    memcpy(&g_withsrc_buf[pos], data, len);
    pos += len;

    return NTN_SendPayloadUsingCfg(g_withsrc_buf, (int)pos);
}

/* ========== hello（用缓存发送） ========== */
int NTN_SendHelloUsingCfg(void)
{
    const uint8_t payload[] = "hello";
    return NTN_SendPayloadUsingCfg(payload, (int)strlen((const char*)payload));
}

/* ========== 发送 HTTP GET（全部用静态 buf，不上栈） ========== */
void NTN_SendHttpGet(const char *imei, const char *host, int socket_id)
{
    int http_len = snprintf(g_http_get, sizeof(g_http_get),
        "GET /api/device/config?imei=%s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Connection: close\r\n"
        "\r\n",
        imei, host
    );

    static const char hex_tab[] = "0123456789ABCDEF";
    int hex_len = 0;

    for (int i = 0; i < http_len && (hex_len + 2) < (int)sizeof(g_hex_buf); ++i)
    {
        uint8_t b = (uint8_t)g_http_get[i];
        g_hex_buf[hex_len++] = hex_tab[b >> 4];
        g_hex_buf[hex_len++] = hex_tab[b & 0x0F];
    }
    g_hex_buf[hex_len] = '\0';

    int cmd_len = snprintf(g_cmd_buf, sizeof(g_cmd_buf),
        "AT+NSOSD=%d,%d,%s\r\n",
        socket_id,
        http_len,
		g_hex_buf
    );

    HAL_UART_Transmit(&hlpuart1, (uint8_t*)g_cmd_buf, cmd_len, 200);
    HAL_UART_Transmit(&huart1,   (uint8_t*)g_cmd_buf, cmd_len, 200);
}

void NTN_SendCfgQuery(const char *imei, int socket_id)
{
    // "C,imei\n"
    int qlen = snprintf(g_http_get, sizeof(g_http_get), "C,%s\n", imei);

    static const char hex_tab[] = "0123456789ABCDEF";
    int hex_len = 0;
    for (int i = 0; i < qlen && (hex_len + 2) < (int)sizeof(g_hex_buf); ++i)
    {
        uint8_t b = (uint8_t)g_http_get[i];
        g_hex_buf[hex_len++] = hex_tab[b >> 4];
        g_hex_buf[hex_len++] = hex_tab[b & 0x0F];
    }
    g_hex_buf[hex_len] = '\0';

    int cmd_len = snprintf(g_cmd_buf, sizeof(g_cmd_buf),
        "AT+NSOSD=%d,%d,%s\r\n", socket_id, qlen, g_hex_buf);

    HAL_UART_Transmit(&huart1,   (uint8_t*)g_cmd_buf, cmd_len, 200);
    HAL_UART_Transmit(&hlpuart1, (uint8_t*)g_cmd_buf, cmd_len, 200);
}

static void NTN_SendCfgQueryUdp(const char *imei, int socket_id, const char *ip, int port)
{
    int qlen = snprintf(g_http_get, sizeof(g_http_get), "C,%s\n", imei);

    static const char hex_tab[] = "0123456789ABCDEF";
    int hex_len = 0;
    for (int i = 0; i < qlen && (hex_len + 2) < (int)sizeof(g_hex_buf); ++i)
    {
        uint8_t b = (uint8_t)g_http_get[i];
        g_hex_buf[hex_len++] = hex_tab[b >> 4];
        g_hex_buf[hex_len++] = hex_tab[b & 0x0F];
    }
    g_hex_buf[hex_len] = '\0';

    int cmd_len = snprintf(g_cmd_buf, sizeof(g_cmd_buf),
                           "AT+NSOST=%d,%s,%d,%d,%s\r\n",
                           socket_id, ip, port, qlen, g_hex_buf);

    HAL_UART_Transmit(&huart1,   (uint8_t*)g_cmd_buf, cmd_len, 200);
    HAL_UART_Transmit(&hlpuart1, (uint8_t*)g_cmd_buf, cmd_len, 200);
}


/* ========== 解析 NSORF 行并返回 hex field 指针（不拷贝 1200） ========== */
/* 返回 1=成功；hex_start 指向 HEX 起始；hex_len 为长度；left 为剩余（没有则 0） */
static int NTN_ParseNsorfLine(char *line, char **hex_start, int *hex_len, int *left_out)
{
    /* 期望:
       +NSORF:0,ip,port,len,HEX,left
       或 +NSORF:0,ip,port,len,HEX
    */

    /* 找到第 4 个逗号后的字段（HEX） */
    char *p = line;

    /* 必须以 +NSORF: 开头 */
    p = strstr(p, "+NSORF:");
    if (!p) return 0;

    int comma_cnt = 0;
    while (*p && comma_cnt < 4)
    {
        if (*p == ',') comma_cnt++;
        p++;
    }
    if (comma_cnt < 4) return 0;

    /* p 现在指向 HEX 字段开头 */
    char *hex = p;

    /* HEX 到下一个逗号或行尾 */
    char *comma = strchr(hex, ',');
    char *eol = strpbrk(hex, "\r\n");

    int left = 0;

    if (comma)
    {
        /* 有 left 字段 */
        *comma = '\0';
        char *left_str = comma + 1;

        /* left_str 到行尾结束 */
        if (eol) *eol = '\0';
        left = atoi(left_str);
    }
    else
    {
        /* 没有 left 字段 */
        if (eol) *eol = '\0';
        left = 0;
    }

    *hex_start = hex;
    *hex_len   = (int)strlen(hex);
    *left_out  = left;
    return 1;
}

static int NTN_FetchCfgByUdp(void)
{
    int n, found, timeout;

    HAL_UART_Transmit(&hlpuart1, (uint8_t*)"CFG via UDP\r\n", 13, 100);

    if (!NTN_EnsureImeiReady())
    {
        HAL_UART_Transmit(&hlpuart1, (uint8_t*)"CFG UDP IMEI not ready\r\n", 24, 100);
        return 0;
    }

    if (!NTN_EnsureUdpSockReady())
    {
        HAL_UART_Transmit(&hlpuart1, (uint8_t*)"CFG UDP sock not ready\r\n", 24, 100);
        return 0;
    }

    /* 1) 发查询 */
    ntn_rx_len = 0;
    memset(ntn_rx_buf, 0, sizeof(ntn_rx_buf));

    NTN_SendCfgQueryUdp(g_device_imei, g_udp_sock, "47.103.38.50", 29848);

    /* 2) 先等发送 OK */
    timeout = (int)(HAL_GetTick() + 3000);
    found = 0;
    while ((int)HAL_GetTick() < timeout)
    {
        NTN_RxTerminateSafe();

        if (strstr((char*)ntn_rx_buf, "OK"))
        {
            found = 1;
            break;
        }

        if (strstr((char*)ntn_rx_buf, "+CME ERROR") || strstr((char*)ntn_rx_buf, "ERROR"))
        {
            HAL_UART_Transmit(&hlpuart1, (uint8_t*)"CFG UDP send FAIL\r\n", 19, 100);
            NTN_DropUdpSock("[CFG-UDP] send fail");
            return 0;
        }

        HAL_Delay(10);
    }

    if (!found)
    {
        HAL_UART_Transmit(&hlpuart1, (uint8_t*)"CFG UDP send timeout\r\n", 22, 100);
        NTN_DropUdpSock("[CFG-UDP] send timeout");
        return 0;
    }

    /* 3) 等回包 */
    ntn_rx_len = 0;
    memset(ntn_rx_buf, 0, sizeof(ntn_rx_buf));

    timeout = (int)(HAL_GetTick() + 60000);
    found = 0;

    while ((int)HAL_GetTick() < timeout)
    {
        NTN_RxTerminateSafe();

        char *p = strstr((char*)ntn_rx_buf, "+NSONMI:");
        if (p)
        {
            /* 关键：先等这一行完整收齐，再解析 */
            char *line_end = strpbrk(p, "\r\n");
            if (!line_end)
            {
                HAL_Delay(10);
                continue;
            }

            int sock = -1;
            char src_ip[32] = {0};
            int src_port = 0;
            int avail = 0;

            if (sscanf(p, "+NSONMI:%d,%31[^,],%d,%d",
                       &sock, src_ip, &src_port, &avail) == 4 && avail > 0)
            {
                found = 1;
                HAL_UART_Transmit(&hlpuart1, (uint8_t*)"CFG UDP GOT NSONMI\r\n", 20, 100);

                int dbg = snprintf(g_cmd_buf, sizeof(g_cmd_buf),
                                   "NSONMI parsed: sock=%d ip=%s port=%d avail=%d\r\n",
                                   sock, src_ip, src_port, avail);
                HAL_UART_Transmit(&hlpuart1, (uint8_t*)g_cmd_buf, dbg, 200);

                /* 找第5个字段 HEX 起点 */
                int comma_cnt = 0;
                char *hex_start = p;
                while (*hex_start && comma_cnt < 4)
                {
                    if (*hex_start == ',') comma_cnt++;
                    hex_start++;
                }

                if (comma_cnt == 4 && *hex_start)
                {
                    int need_hex_len = avail * 2;
                    uint32_t hex_deadline = HAL_GetTick() + 8000;

                    while (HAL_GetTick() < hex_deadline)
                    {
                        NTN_RxTerminateSafe();

                        /* 重新定位，避免 buffer 增长后指针失效 */
                        p = strstr((char*)ntn_rx_buf, "+NSONMI:");
                        if (!p)
                        {
                            HAL_Delay(10);
                            continue;
                        }

                        char *cur_line_end = strpbrk(p, "\r\n");
                        if (!cur_line_end)
                        {
                            HAL_Delay(10);
                            continue;
                        }

                        int cc = 0;
                        hex_start = p;
                        while (*hex_start && cc < 4)
                        {
                            if (*hex_start == ',') cc++;
                            hex_start++;
                        }

                        if (cc == 4 && *hex_start)
                        {
                            int cur_hex_len = 0;
                            while (hex_start[cur_hex_len] &&
                                   hex_start[cur_hex_len] != '\r' &&
                                   hex_start[cur_hex_len] != '\n')
                            {
                                cur_hex_len++;
                            }

                            /* 必须等完整 HEX 到齐 */
                            if (cur_hex_len >= need_hex_len)
                            {
                                LineStream_Reset();

                                if (LineStream_FeedHex(hex_start, need_hex_len))
                                {
                                    NTN_CommitCfgFromLine();
                                    return g_ntn_cfg_ready ? 1 : 0;
                                }

                                HAL_UART_Transmit(&hlpuart1,
                                                  (uint8_t*)"CFG UDP line decode FAIL\r\n",
                                                  26, 100);
                                return 0;
                            }
                        }

                        HAL_Delay(10);
                    }
                }

                HAL_UART_Transmit(&hlpuart1, (uint8_t*)"CFG UDP NSONMI hex parse FAIL\r\n", 31, 100);
                return 0;
            }
        }

        HAL_Delay(10);
    }

    if (!found)
    {
        HAL_UART_Transmit(&hlpuart1, (uint8_t*)"CFG UDP no NSONMI\r\n", 19, 100);
        return 0;
    }

    return g_ntn_cfg_ready ? 1 : 0;
}

/* ========== 内部：单次拉配置 ========== */
int ntn_socket_id = -1;

/*********************** ADD-ONLY: Net Gate for Cellular (NB-IoT/Base-station SIM) ************************/




static void Gate_Print(const char *s)
{
    HAL_UART_Transmit(&hlpuart1, (uint8_t*)s, (uint16_t)strlen(s), 200);
}

// 发AT命令前清理rx，等待出现关键字或OK，遇到ERROR/CME则失败
static int NTN_Gate_SendWait(const char *cmd,
                            const char *must_contain,     // 可为NULL
                            uint32_t timeout_ms)
{
    // 清 rx，避免命中旧残留
    ntn_rx_len = 0;
    memset(ntn_rx_buf, 0, sizeof(ntn_rx_buf));

    HAL_UART_Transmit(&huart1,   (uint8_t*)cmd, (uint16_t)strlen(cmd), 500);
    HAL_UART_Transmit(&hlpuart1, (uint8_t*)cmd, (uint16_t)strlen(cmd), 500);

    uint32_t deadline = HAL_GetTick() + timeout_ms;
    while (HAL_GetTick() < deadline)
    {
        NTN_RxTerminateSafe();

        if (strstr((char*)ntn_rx_buf, "+CME ERROR") || strstr((char*)ntn_rx_buf, "ERROR"))
            return 0;

        if (must_contain)
        {
            // ✅ 查询类命令：必须同时命中前缀 + OK，避免 URC/旧残留误判
            if (strstr((char*)ntn_rx_buf, must_contain) && strstr((char*)ntn_rx_buf, "OK"))
                return 1;
        }
        else
        {
            // ✅ 无 must_contain：只要 OK 就算成功
            if (strstr((char*)ntn_rx_buf, "OK"))
                return 1;
        }

        HAL_Delay(20);
    }
    return 0;
}

// 解析 +CEREG: <n>,<stat> 的 stat；失败返回 -1
static int NTN_Gate_ParseCeregStat(void)
{
    NTN_RxTerminateSafe();
    char *p = strstr((char*)ntn_rx_buf, "+CEREG:");
    if (!p) return -1;

    int n = 0, stat = -1;
    if (sscanf(p, "+CEREG:%d,%d", &n, &stat) == 2) return stat;
    return -1;
}

// 检查 CGPADDR 输出是否包含 IPv4： +CGPADDR:<cid>,"x.x.x.x"
static int NTN_Gate_CgpaddrHasIpv4(char *out_ip, int out_ip_cap)
{
    NTN_RxTerminateSafe();

    char *p = (char*)ntn_rx_buf;
    while ((p = strstr(p, "+CGPADDR:")) != NULL)
    {
        // 找引号
        char *q = strchr(p, '\"');
        if (!q) { p += 8; continue; }

        // 读引号内字符串
        char ip[32] = {0};
        if (sscanf(q, "\"%31[0-9.]\"", ip) == 1)
        {
            // 粗验 IPv4 三个点
            int dots = 0;
            for (int i = 0; ip[i]; i++) if (ip[i] == '.') dots++;
            if (dots == 3)
            {
                if (out_ip && out_ip_cap > 0)
                {
                    strncpy(out_ip, ip, out_ip_cap - 1);
                    out_ip[out_ip_cap - 1] = 0;
                }
                return 1;
            }
        }

        p += 8;
    }
    return 0;
}

/*
 * Gate: 确保蜂窝承载 ready（基站卡/NB-IoT 卡）
 * 通过条件：
 *  1) AT OK
 *  2) +CPIN: READY
 *  3) +CFUN:1（否则 CFUN=1）
 *  4) +CEREG stat=1 或 5
 *  5) AT+CGPADDR 返回 IPv4
 *
 * 失败时：只打印原因并 return 0，不改你现有 socket/cfg 流程的代码结构
 */


static int NTN_TryCellularQuickProbe(void)
{
    Gate_Print("CELLULAR PROBE\r\n");

    // 1) AT
    if (!NTN_Gate_SendWait("AT\r\n", NULL, 1000))
        return 0;

    // 2) CPIN
    if (!NTN_Gate_SendWait("AT+CPIN?\r\n", "+CPIN:", 1500))
        return 0;

    NTN_RxTerminateSafe();
    if (!strstr((char*)ntn_rx_buf, "READY"))
        return 0;

    // 3) CFUN?
    if (!NTN_Gate_SendWait("AT+CFUN?\r\n", "+CFUN:", 1500))
        return 0;

    int cfun = -1;
    char *p = strstr((char*)ntn_rx_buf, "+CFUN:");
    if (p) sscanf(p, "+CFUN:%d", &cfun);

    if (cfun != 1)
    {
        if (!NTN_Gate_SendWait("AT+CFUN=1\r\n", "OK", 5000))
            return 0;
        HAL_Delay(8000);
    }

    // 4) 只给一个短窗口看是否像普通蜂窝
    (void)NTN_Gate_SendWait("AT+CEREG=2\r\n", "OK", 1000);

    uint32_t deadline = HAL_GetTick() + 15000;   // 只等15秒
    int stat = -1;

    while (HAL_GetTick() < deadline)
    {
        if (NTN_Gate_SendWait("AT+CEREG?\r\n", "+CEREG:", 2000))
        {
            stat = NTN_Gate_ParseCeregStat();
            if (stat == 1 || stat == 5)
                break;

        }
        HAL_Delay(1500);
    }

    if (!(stat == 1 || stat == 5))
        return 0;

    // 5) 再快速问 IP
    if (NTN_Gate_SendWait("AT+CGPADDR\r\n", "+CGPADDR:", 2000))
    {
        if (NTN_Gate_CgpaddrHasIpv4(NULL, 0))
            return 1;
    }

    return 0;
}

// 放在 NTN_SendTest() 之前（文件级别）
static void NTN_ModemReset_AndWaitReady(void)
{
    // 1) 让现有 socket/cfg 失效，避免复用旧连接
    NTN_Invalidate_UserSock_AndCfg("[NTN] reset: invalidate sock/cfg");

    // 2) 清 RX，避免后续误判
    ntn_rx_len = 0;
    memset(ntn_rx_buf, 0, sizeof(ntn_rx_buf));

    // 3) 发重启（NRB 常见于 NB 系列模组）
    const char *cmd = "AT+RESET\r\n";
    HAL_UART_Transmit(&huart1,   (uint8_t*)cmd, (uint16_t)strlen(cmd), 500);
    HAL_UART_Transmit(&hlpuart1, (uint8_t*)cmd, (uint16_t)strlen(cmd), 500);
    HAL_UART_Transmit(&hlpuart1, (uint8_t*)"[NTN] AT+RESET sent\r\n", 23, 200);

    // 4) 给它一点“硬重启”时间
    HAL_Delay(1500);

    // 5) 轮询 AT，直到 OK（不依赖 POWERON tick）
    uint32_t deadline = HAL_GetTick() + 20000;   // 20s
    int at_ready = 0;

    while (HAL_GetTick() < deadline)
    {
        if (NTN_Gate_SendWait("AT\r\n", NULL, 1000))
        {
            at_ready = 1;
            break;
        }
        HAL_Delay(300);
    }

    if (at_ready)
    {
        HAL_UART_Transmit(&hlpuart1,
                          (uint8_t*)"[NTN] AT ready after reset\r\n",
                          27, 200);
    }
    else
    {
        HAL_UART_Transmit(&hlpuart1,
                          (uint8_t*)"[NTN] reset wait AT timeout\r\n",
                          28, 200);
        // 这里你可以选择 return；也可以继续让 Gate 再试一次
        // return;
    }

    // 6) 再等一下：让 SIM / 协议栈 / 网络侧初始化更稳
    HAL_Delay(5000);

    // 7) 再清一次 RX，避免 reset URC 干扰后面的 Gate 解析
    ntn_rx_len = 0;
    memset(ntn_rx_buf, 0, sizeof(ntn_rx_buf));
}

static int NTN_PrepareSatelliteProfile(void)
{
    HAL_UART_Transmit(&hlpuart1, (uint8_t*)"MODE: TRY NTN\r\n", 15, 100);

    /* 6) 给协议栈一点时间起来 */
    HAL_Delay(2000);

    /* 7) APN / PDP */
    if (!NTN_Gate_SendWait("AT+CGDCONT=1,\"IP\",\"DATA.MONO\"\r\n", "OK", 3000))
    {
        HAL_UART_Transmit(&hlpuart1, (uint8_t*)"NTN: CGDCONT FAIL\r\n", 19, 100);
        return 0;
    }

    /* 8) NTN 测试参数 */
    if (!NTN_Gate_SendWait(
            "AT+NSET=\"NTN_TEST\",\"130733023:-28177446:30:2026:03:09:11:10:00:*\"\r\n",
            "OK", 5000))
    {
        HAL_UART_Transmit(&hlpuart1, (uint8_t*)"NTN: NTN_TEST FAIL\r\n", 20, 100);
        return 0;
    }

    /* 2) 配置 band */
     if (!NTN_Gate_SendWait("AT+NBAND=255,256\r\n", "OK", 3000))
     {
         HAL_UART_Transmit(&hlpuart1, (uint8_t*)"NTN: NBAND FAIL\r\n", 17, 100);
         return 0;
     }

    /* 9) 打开下发通知 */
    if (!NTN_Gate_SendWait("AT+NSONMI=2\r\n", "OK", 3000))
    {
        HAL_UART_Transmit(&hlpuart1, (uint8_t*)"NTN: NSONMI FAIL\r\n", 18, 100);
        return 0;
    }

    HAL_UART_Transmit(&hlpuart1, (uint8_t*)"NTN profile OK\r\n", 16, 100);
    return 1;
}

static int NTN_NtnWaitBearerReady(void)
{
    (void)NTN_Gate_SendWait("AT+CEREG=2\r\n", "OK", 1000);

    uint32_t reg_deadline = HAL_GetTick() + 900000;
    int stat = -1;

    while (HAL_GetTick() < reg_deadline)
    {
        if (NTN_Gate_SendWait("AT+CEREG?\r\n", "+CEREG:", 20000))
        {
            stat = NTN_Gate_ParseCeregStat();
            if (stat == 1 || stat == 5)
                break;
        }
        HAL_Delay(2000);
    }

    if (!(stat == 1 || stat == 5))
    {
        HAL_UART_Transmit(&hlpuart1, (uint8_t*)"NTN: CEREG not ready\r\n", 22, 100);
        return 0;
    }

    uint32_t ip_deadline = HAL_GetTick() + 60000;
    while (HAL_GetTick() < ip_deadline)
    {
        if (NTN_Gate_SendWait("AT+CGPADDR\r\n", "+CGPADDR:", 2000))
        {
            if (NTN_Gate_CgpaddrHasIpv4(NULL, 0))
            {
                HAL_UART_Transmit(&hlpuart1, (uint8_t*)"NTN: bearer ready\r\n", 19, 100);
                return 1;
            }
        }
        HAL_Delay(1500);
    }

    HAL_UART_Transmit(&hlpuart1, (uint8_t*)"NTN: no IP after config\r\n", 25, 100);
    return 0;
}

static void NTN_SwitchToCellularClean(void)
{
    HAL_UART_Transmit(&hlpuart1,
        (uint8_t*)"[SWITCH] NTN -> CELLULAR CLEAN\r\n", 35, 200);

    /* 1) 清 socket / cfg / udp */
    NTN_Invalidate_UserSock_AndCfg("[SWITCH] invalidate all");
    HAL_Delay(300);

    /* 2) 先做整机 reset（比 CFUN 更重） */
    HAL_UART_Transmit(&huart1,
        (uint8_t*)"AT+RESET\r\n", 10, 500);
    HAL_UART_Transmit(&hlpuart1,
        (uint8_t*)"AT+RESET\r\n", 10, 500);

    /* 3) 等模块重启起来 */
    HAL_Delay(3000);

    /* 4) 重新探测 AT，确认模组活了 */
    uint32_t deadline = HAL_GetTick() + 20000;
    while (HAL_GetTick() < deadline)
    {
        if (NTN_Gate_SendWait("AT\r\n", NULL, 1000))
            break;
        HAL_Delay(300);
    }

    /* 5) 再做一次无线功能重建 */
    NTN_Gate_SendWait("AT+CFUN=0\r\n", "OK", 5000);
    HAL_Delay(1000);

    NTN_Gate_SendWait("AT+CFUN=1\r\n", "OK", 5000);
    HAL_Delay(5000);

    /* 6) 改回普通卡 APN */
    NTN_Gate_SendWait("AT+CGDCONT=1,\"IP\",\"internet\"\r\n", "OK", 3000);

    HAL_UART_Transmit(&hlpuart1,
        (uint8_t*)"[SWITCH] CELLULAR CLEAN DONE\r\n", 33, 200);
}

void NTN_SendTest(void)
{
    g_ntn_cfg_ready = 0;
    int timeout = 0;
    int found = 0;

    /* STEP 1: 清空缓冲 */
    ntn_rx_len = 0;
    memset(ntn_rx_buf, 0, sizeof(ntn_rx_buf));
    HAL_Delay(200);  // 给 URC 一个窗口吐完
    ntn_rx_len = 0;
    memset(ntn_rx_buf, 0, sizeof(ntn_rx_buf));


    /* 1) 先快速探测普通基站卡 */
    /* 直接走卫星卡流程，不再判断普通卡 */
        HAL_UART_Transmit(&hlpuart1, (uint8_t*)"MODE: NTN ONLY\r\n", 16, 100);
        g_use_udp_first_for_cfg = 1;

        if (!NTN_PrepareSatelliteProfile())
        {
            HAL_UART_Transmit(&hlpuart1,
                (uint8_t*)"NTN prepare fail -> reset -> cellular\r\n", 39, 100);

            NTN_ModemReset_AndWaitReady();

            if (!NTN_TryCellularQuickProbe())
            {
                HAL_UART_Transmit(&hlpuart1,
                    (uint8_t*)"CELLULAR FAIL\r\n", 15, 100);
                return;
            }

            HAL_UART_Transmit(&hlpuart1,
                (uint8_t*)"MODE: CELLULAR READY\r\n", 22, 100);

            g_use_udp_first_for_cfg = 0;
            return;
        }

        if (!NTN_NtnWaitBearerReady())
        {
            HAL_UART_Transmit(&hlpuart1,
                (uint8_t*)"NTN bearer fail -> reset -> cellular\r\n", 38, 100);

            NTN_ModemReset_AndWaitReady();

            if (!NTN_TryCellularQuickProbe())
            {
                HAL_UART_Transmit(&hlpuart1,
                    (uint8_t*)"CELLULAR FAIL\r\n", 15, 100);
                return;
            }

            HAL_UART_Transmit(&hlpuart1,
                (uint8_t*)"MODE: CELLULAR READY\r\n", 22, 100);

            g_use_udp_first_for_cfg = 0;
            return;
        }

        HAL_UART_Transmit(&hlpuart1, (uint8_t*)"MODE: NTN READY\r\n", 17, 100);

        if (g_use_udp_first_for_cfg)
        {
            HAL_UART_Transmit(&hlpuart1, (uint8_t*)"CFG ORDER: UDP ONLY\r\n", 21, 100);

            if (NTN_FetchCfgByUdp())
            {
                HAL_UART_Transmit(&hlpuart1, (uint8_t*)"CFG via UDP OK\r\n", 16, 100);
                return;
            }

            HAL_UART_Transmit(&hlpuart1, (uint8_t*)"CFG via UDP FAIL\r\n", 18, 100);
            return;
        }

}

static void NTN_TryFetchConfigOnce(void)
{
    g_ntn_cfg_ready = 0;
    NTN_SendTest();
}
#endif
