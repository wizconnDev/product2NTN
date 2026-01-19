/*********************** toNTN.c (stack-safe / stream JSON) ************************/
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
}


/* ========== JSON 流式提取（避免 http_hex_total[1500]） ========== */
typedef struct {
    uint8_t started;
    int     depth;
    size_t  j;
} JsonStreamState;

static JsonStreamState g_js;
static char g_json_buf[256];  /* 够解析 {"protocol":"TCP","ip":"x","port":yyyy} */

static void JsonStream_Reset(void)
{
    g_js.started = 0;
    g_js.depth   = 0;
    g_js.j       = 0;
    g_json_buf[0] = '\0';
}

static int hex_val(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

/* 把一段 HEX（仅 0-9A-F）流式喂给 JSON 提取器，成功提到完整 JSON 返回 1 */
static int JsonStream_FeedHex(const char *hex, int hex_len)
{
    for (int i = 0; i + 1 < hex_len; i += 2)
    {
        int vh = hex_val(hex[i]);
        int vl = hex_val(hex[i+1]);
        if (vh < 0 || vl < 0) continue;

        char c = (char)((vh << 4) | vl);

        if (!g_js.started)
        {
            if (c == '{')
            {
                g_js.started = 1;
                g_js.depth = 1;
                if (g_js.j < sizeof(g_json_buf) - 1)
                {
                    g_json_buf[g_js.j++] = '{';
                    g_json_buf[g_js.j] = '\0';
                }
            }
            continue;
        }

        /* started==1: 记录 JSON */
        if (g_js.j < sizeof(g_json_buf) - 1)
        {
            g_json_buf[g_js.j++] = c;
            g_json_buf[g_js.j] = '\0';
        }
        else
        {
            /* JSON 太长，直接判失败 */
            return 0;
        }

        if (c == '{') g_js.depth++;
        else if (c == '}')
        {
            g_js.depth--;
            if (g_js.depth == 0)
            {
                /* 完整 JSON 收到 */
                return 1;
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

/* 这个函数现在直接喂 JSON 流式提取器 + parse cfg，不再需要 http_hex_total */
static void NTN_CommitCfgFromJson(void)
{
    NetConfig cfg;

    HAL_UART_Transmit(&hlpuart1, (uint8_t*)"JSON: ", 6, 100);
    HAL_UART_Transmit(&hlpuart1, (uint8_t*)g_json_buf, (uint16_t)strlen(g_json_buf), 100);
    HAL_UART_Transmit(&hlpuart1, (uint8_t*)"\r\n", 2, 100);

    if (!NTN_ParseConfigJson(g_json_buf, &cfg))
    {
        HAL_UART_Transmit(&hlpuart1, (uint8_t*)"JSON parse fail\r\n", 17, 100);
        return;
    }

    g_ntn_cfg = cfg;
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
int NTN_SendPayloadUsingCfg(const uint8_t* data, int len)
{
    if (!data || len <= 0) return 0;
    if (len > (NTN_MAX_SEND_PAYLOAD - 1))   /* 给 WithSrc 留空间也安全 */
    {
        Simple_Print("Payload too large\r\n");
        return 0;
    }

    /* ===== 已有可复用 socket，直接发（不再用栈上 hex/cmd）===== */
    if (g_user_sock_ready && g_user_sock >= 0)
    {
        if (!NTN_BytesToHex_Into(data, len, g_hex_buf, sizeof(g_hex_buf)))
        {
            Simple_Print("HEX buffer too small\r\n");
            return 0;
        }

        int n = snprintf(g_cmd_buf, sizeof(g_cmd_buf),
                         "AT+NSOSD=%d,%d,%s\r\n",
                         g_user_sock, len, g_hex_buf);

        // ✅ 清 rx，避免误判旧残留
        ntn_rx_len = 0;
        memset(ntn_rx_buf, 0, sizeof(ntn_rx_buf));

        HAL_UART_Transmit(&huart1,   (uint8_t*)g_cmd_buf, n, 1000);
        HAL_UART_Transmit(&hlpuart1, (uint8_t*)g_cmd_buf, n, 1000);

        Simple_Print("SEND via existing socket\r\n");

        // ✅ 等 OK 或 CME ERROR
        int timeout = (int)(HAL_GetTick() + 3000);
        while ((int)HAL_GetTick() < timeout)
        {
            NTN_RxTerminateSafe();

            // 命中 CME ERROR:8002 → 认为 socket 断了
            if (strstr((char*)ntn_rx_buf, "+CME ERROR:8002"))
            {
                NTN_Invalidate_UserSock_AndCfg("[NTN] CME8002 -> drop sock, refetch cfg");
                return 0;
            }

            // 其他 ERROR 也当失败处理
            if (strstr((char*)ntn_rx_buf, "ERROR") || strstr((char*)ntn_rx_buf, "+CME ERROR"))
            {
                NTN_Invalidate_UserSock_AndCfg("[NTN] SEND ERROR -> drop sock, refetch cfg");
                return 0;
            }

            if (strstr((char*)ntn_rx_buf, "OK"))
            {
                return 1;
            }

            HAL_Delay(10);
        }

        // 超时也认为 socket 不稳定：丢弃复用 socket
        NTN_Invalidate_UserSock_AndCfg("[NTN] SEND TIMEOUT -> drop sock, refetch cfg");
        return 0;
    }


    /* 1) 确保 cfg 就绪 */
    if (!NTN_EnsureConfigReady(3, 15000)) return 0;

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
    int timeout, found;

    /* 清 rx */
    ntn_rx_len = 0;
    memset(ntn_rx_buf, 0, sizeof(ntn_rx_buf));

    /* ========== TCP ========== */
    if (strcmp(g_ntn_cfg.protocol, "TCP") == 0)
    {
        /* A) NSOCR */
        const char *cmd1 = "AT+NSOCR=STREAM,6,0,1\r\n";
        HAL_UART_Transmit(&huart1,   (uint8_t*)cmd1, (uint16_t)strlen(cmd1), 200);
        HAL_UART_Transmit(&hlpuart1, (uint8_t*)cmd1, (uint16_t)strlen(cmd1), 200);

        timeout = (int)(HAL_GetTick() + 2000);
        found = 0;
        while ((int)HAL_GetTick() < timeout)
        {
            NTN_RxTerminateSafe();
            char *p = strstr((char*)ntn_rx_buf, "+NSOCR:");
            if (p)
            {
                sock_id = atoi(p + 6);
                found = 1;
                break;
            }
            HAL_Delay(1);
        }
        if (!found)
        {
            HAL_UART_Transmit(&hlpuart1, (uint8_t*)"TCP: Parse socket FAIL\r\n", 24, 100);
            g_ntn_cfg_ready = 0;
            return 0;
        }

        /* 等 OK */
        timeout = (int)(HAL_GetTick() + 2000);
        while ((int)HAL_GetTick() < timeout)
        {
            NTN_RxTerminateSafe();
            if (strstr((char*)ntn_rx_buf, "OK")) break;
            HAL_Delay(1);
        }
        NTN_RxTerminateSafe();
        if (!strstr((char*)ntn_rx_buf, "OK"))
        {
            HAL_UART_Transmit(&hlpuart1, (uint8_t*)"TCP: No OK after NSOCR\r\n", 24, 100);
            NTN_CloseSocket(sock_id);
            g_user_sock_ready = 0;
            g_user_sock = -1;
            g_ntn_cfg_ready = 0;
            return 0;
        }

        /* B) NSOCO */
        ntn_rx_len = 0;
        memset(ntn_rx_buf, 0, sizeof(ntn_rx_buf));

        n = snprintf(g_cmd_buf, sizeof(g_cmd_buf),
                     "AT+NSOCO=%d,%s,%d\r\n",
                     sock_id, g_ntn_cfg.ip, g_ntn_cfg.port);
        HAL_UART_Transmit(&huart1,   (uint8_t*)g_cmd_buf, n, 200);
        HAL_UART_Transmit(&hlpuart1, (uint8_t*)g_cmd_buf, n, 200);

        timeout = (int)(HAL_GetTick() + 5000);
        found = 0;
        while ((int)HAL_GetTick() < timeout)
        {
            NTN_RxTerminateSafe();
            if (strstr((char*)ntn_rx_buf, "OK")) { found = 1; break; }
            if (strstr((char*)ntn_rx_buf, "ERROR")) break;
            HAL_Delay(10);
        }
        if (!found)
        {
            HAL_UART_Transmit(&hlpuart1, (uint8_t*)"TCP: Connect FAIL -> invalidate cfg\r\n", 37, 100);
            NTN_CloseSocket(sock_id);
            g_ntn_cfg_ready = 0;
            return 0;
        }

        /* C) NSOSD */
        ntn_rx_len = 0;
        memset(ntn_rx_buf, 0, sizeof(ntn_rx_buf));

        n = snprintf(g_cmd_buf, sizeof(g_cmd_buf),
                     "AT+NSOSD=%d,%d,%s\r\n",
                     sock_id, len, g_hex_buf);
        HAL_UART_Transmit(&hlpuart1, (uint8_t*)g_cmd_buf, n, 500);
        HAL_UART_Transmit(&huart1,   (uint8_t*)g_cmd_buf, n, 500);

        timeout = (int)(HAL_GetTick() + 3000);
        found = 0;
        while ((int)HAL_GetTick() < timeout)
        {
            NTN_RxTerminateSafe();
            if (strstr((char*)ntn_rx_buf, "OK")) { found = 1; break; }
            HAL_Delay(10);
        }
        if (!found)
        {
            HAL_UART_Transmit(&hlpuart1, (uint8_t*)"TCP: No OK after NSOSD -> invalidate cfg\r\n", 43, 100);
            NTN_CloseSocket(sock_id);
            g_ntn_cfg_ready = 0;
            return 0;
        }

        /* 成功：保留 socket 复用 */
        g_user_sock = sock_id;
        g_user_sock_ready = 1;
        return 1;
    }

    /* ========== UDP ========== */
    if (strcmp(g_ntn_cfg.protocol, "UDP") == 0)
    {
        const char *cmd1 = "AT+NSOCR=DGRAM,17,0,1\r\n";
        HAL_UART_Transmit(&huart1,   (uint8_t*)cmd1, (uint16_t)strlen(cmd1), 200);
        HAL_UART_Transmit(&hlpuart1, (uint8_t*)cmd1, (uint16_t)strlen(cmd1), 200);

        timeout = (int)(HAL_GetTick() + 2000);
        found = 0;
        while ((int)HAL_GetTick() < timeout)
        {
            NTN_RxTerminateSafe();
            char *p = strstr((char*)ntn_rx_buf, "+NSOCR:");
            if (p)
            {
                sock_id = atoi(p + 6);
                found = 1;
                break;
            }
            HAL_Delay(1);
        }
        if (!found)
        {
            HAL_UART_Transmit(&hlpuart1, (uint8_t*)"UDP: Parse socket FAIL\r\n", 24, 100);
            g_ntn_cfg_ready = 0;
            return 0;
        }

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
            if (strstr((char*)ntn_rx_buf, "OK")) { found = 1; break; }
            if (strstr((char*)ntn_rx_buf, "ERROR")) break;
            HAL_Delay(10);
        }

        if (!found)
        {
            HAL_UART_Transmit(&hlpuart1, (uint8_t*)"UDP: Send FAIL -> invalidate cfg\r\n", 34, 100);
            NTN_CloseSocket(sock_id);
            g_ntn_cfg_ready = 0;
            return 0;
        }

        NTN_CloseSocket(sock_id);
        return 1;
    }

    HAL_UART_Transmit(&hlpuart1, (uint8_t*)"Unknown protocol in cfg\r\n", 26, 100);
    g_ntn_cfg_ready = 0;
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
void NTN_SendHttpGet(const char *guid, const char *host, int socket_id)
{
    int http_len = snprintf(g_http_get, sizeof(g_http_get),
        "GET /api/device/config?guid=%s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Connection: close\r\n"
        "\r\n",
        guid, host
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

/* ========== 内部：单次拉配置 ========== */
int ntn_socket_id = -1;

void NTN_SendTest(void)
{
    g_ntn_cfg_ready = 0;
    g_user_sock_ready = 0;
    g_user_sock = -1;

    /* STEP 1: 清空缓冲 */
    ntn_rx_len = 0;
    memset(ntn_rx_buf, 0, sizeof(ntn_rx_buf));

    /* STEP 2: NSOCR */
    const char *cmd1 = "AT+NSOCR=STREAM,6,0,1\r\n";
    HAL_UART_Transmit(&huart1,   (uint8_t*)cmd1, (uint16_t)strlen(cmd1), 200);
    HAL_UART_Transmit(&hlpuart1, (uint8_t*)cmd1, (uint16_t)strlen(cmd1), 200);

    /* STEP 3: parse socket */
    int timeout = (int)(HAL_GetTick() + 2000);
    int found = 0;
    while ((int)HAL_GetTick() < timeout)
    {
        NTN_RxTerminateSafe();
        char *p = strstr((char*)ntn_rx_buf, "+NSOCR:");
        if (p)
        {
            ntn_socket_id = atoi(p + 6);
            found = 1;
            break;
        }
        HAL_Delay(1);
    }
    if (!found)
    {
        HAL_UART_Transmit(&hlpuart1, (uint8_t*)"Parse socket FAIL\r\n", 19, 100);
        return;
    }

    int n = snprintf(g_cmd_buf, sizeof(g_cmd_buf), "SOCKET ID = %d\r\n", ntn_socket_id);
    HAL_UART_Transmit(&hlpuart1, (uint8_t*)g_cmd_buf, n, 100);

    /* STEP 4: wait OK */
    timeout = (int)(HAL_GetTick() + 2000);
    while ((int)HAL_GetTick() < timeout)
    {
        NTN_RxTerminateSafe();
        if (strstr((char*)ntn_rx_buf, "OK")) break;
        HAL_Delay(1);
    }
    NTN_RxTerminateSafe();
    if (!strstr((char*)ntn_rx_buf, "OK"))
    {
        HAL_UART_Transmit(&hlpuart1, (uint8_t*)"No OK after NSOCR\r\n", 19, 100);
        return;
    }

    /* STEP 5: clear rx */
    ntn_rx_len = 0;
    memset(ntn_rx_buf, 0, sizeof(ntn_rx_buf));

    /* STEP 6: NSOCO (你的 cfg server 仍是 47.103.38.50:80) */
    n = snprintf(g_cmd_buf, sizeof(g_cmd_buf),
                 "AT+NSOCO=%d,%s,%d\r\n",
                 ntn_socket_id, "47.103.38.50", 80);
    HAL_UART_Transmit(&huart1,   (uint8_t*)g_cmd_buf, n, 200);
    HAL_UART_Transmit(&hlpuart1, (uint8_t*)g_cmd_buf, n, 200);

    /* STEP 7: wait connect OK */
    timeout = (int)(HAL_GetTick() + 3000);
    found = 0;
    while ((int)HAL_GetTick() < timeout)
    {
        NTN_RxTerminateSafe();
        if (strstr((char*)ntn_rx_buf, "OK")) { found = 1; break; }
        if (strstr((char*)ntn_rx_buf, "ERROR")) break;
        HAL_Delay(1);
    }
    if (!found)
    {
        HAL_UART_Transmit(&hlpuart1, (uint8_t*)"TCP Connect FAIL\r\n", 18, 100);
        return;
    }
    HAL_UART_Transmit(&hlpuart1, (uint8_t*)"TCP Connected OK\r\n", 18, 100);

    /* STEP 8: send GET */
    ntn_rx_len = 0;
    memset(ntn_rx_buf, 0, sizeof(ntn_rx_buf));
    NTN_SendHttpGet("NTN001", "47.103.38.50", ntn_socket_id);

    /* STEP 9: wait +NSONMI */
    ntn_rx_len = 0;
    memset(ntn_rx_buf, 0, sizeof(ntn_rx_buf));

    timeout = (int)(HAL_GetTick() + 15000);
    found = 0;

    while ((int)HAL_GetTick() < timeout)
    {
        NTN_RxTerminateSafe();
        char *p = strstr((char*)ntn_rx_buf, "+NSONMI:");
        if (p)
        {
            /* 轻量解析 sock,avail */
            int sock = -1, avail = 0;
            if (sscanf(p, "+NSONMI:%d,%d", &sock, &avail) == 2 && avail > 0)
            {
                found = 1;
                HAL_UART_Transmit(&hlpuart1, (uint8_t*)"GOT NSONMI\r\n", 12, 100);

                ntn_rx_len = 0;
                memset(ntn_rx_buf, 0, sizeof(ntn_rx_buf));

                n = snprintf(g_cmd_buf, sizeof(g_cmd_buf), "AT+NSORF=%d,%d\r\n", sock, avail);
                HAL_UART_Transmit(&huart1,   (uint8_t*)g_cmd_buf, n, 200);
                HAL_UART_Transmit(&hlpuart1, (uint8_t*)g_cmd_buf, n, 200);
                break;
            }
        }
        HAL_Delay(10);
    }

    if (!found)
    {
        HAL_UART_Transmit(&hlpuart1, (uint8_t*)"DBG: No NSONMI\r\n", 16, 100);
        return;
    }

    /* STEP 10: NSORF loop（流式找 JSON，不再拼 http_hex_total） */
    JsonStream_Reset();

    int remain = 0;
    uint32_t nsorf_deadline = HAL_GetTick() + 10000;

    do
    {
        /* 等到收到完整一行 +NSORF */
        uint32_t line_deadline = HAL_GetTick() + 3000;
        char *p = NULL;

        while (HAL_GetTick() < line_deadline)
        {
            NTN_RxTerminateSafe();
            p = strstr((char*)ntn_rx_buf, "+NSORF:");
            if (p)
            {
                char *lf = strchr(p, '\n');
                if (lf) break;
            }
            HAL_Delay(10);
        }

        if (!p)
        {
            HAL_UART_Transmit(&hlpuart1, (uint8_t*)"No NSORF line\r\n", 15, 100);
            return;
        }

        /* 解析 HEX 字段指针（不拷贝 1200） */
        char *hex_start = NULL;
        int hex_len = 0;
        int left = 0;

        if (!NTN_ParseNsorfLine(p, &hex_start, &hex_len, &left))
        {
            HAL_UART_Transmit(&hlpuart1, (uint8_t*)"NSORF parse FAIL\r\n", 17, 100);
            return;
        }

        /* 喂给 JSON 提取器 */
        if (hex_start && hex_len > 0)
        {
            int ok_json = JsonStream_FeedHex(hex_start, hex_len);
            if (ok_json)
            {
                NTN_CommitCfgFromJson();
                break; /* 已拿到 JSON，直接结束 */
            }
        }

        remain = left;

        /* 还有剩余就继续拉 */
        if (remain > 0 && HAL_GetTick() < nsorf_deadline)
        {
            ntn_rx_len = 0;
            memset(ntn_rx_buf, 0, sizeof(ntn_rx_buf));

            int pull = (remain > 512) ? 512 : remain;
            n = snprintf(g_cmd_buf, sizeof(g_cmd_buf),
                         "AT+NSORF=%d,%d\r\n",
                         ntn_socket_id, pull);
            HAL_UART_Transmit(&huart1,   (uint8_t*)g_cmd_buf, n, 200);
            HAL_UART_Transmit(&hlpuart1, (uint8_t*)g_cmd_buf, n, 200);
        }

        HAL_Delay(50);

    } while (remain > 0 && HAL_GetTick() < nsorf_deadline);

    /* 如果 cfg 没拿到：提示 */
    if (!g_ntn_cfg_ready)
    {
        HAL_UART_Transmit(&hlpuart1, (uint8_t*)"JSON not found in NSORF stream\r\n", 31, 100);
    }

    /* 你原来这里的逻辑：没 ready 就关 socket */
    if (!g_user_sock_ready)
    {
        NTN_CloseSocket(ntn_socket_id);
    }
}

static void NTN_TryFetchConfigOnce(void)
{
    g_ntn_cfg_ready = 0;
    NTN_SendTest();
}
