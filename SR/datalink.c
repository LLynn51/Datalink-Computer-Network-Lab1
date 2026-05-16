/**
 * @ Author: LLynn51 (GBN Original), SR adaptation
 * @ Create Time: 2026-05-15
 * @ Description: 选择重传(SR)协议实现
 *   =========== 与GBN协议的关键差异标记为 /* [SR] */ ===========
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "protocol.h"
#include "datalink.h"

/* ================================================================
 * [SR] 定时器参数（与GBN版本一致）
 * ================================================================ */
#define DATA_TIMER  2000
#define ACK_TIMER 150

/* ================================================================
 * [SR] 常量与宏定义（与GBN版本一致）
 * ================================================================ */
#define ABSOLUTE_MAX_SEQ_NUM 255
static int no_nak = 1;
#define inc(k) if((k) < max_seq_num) (k)++; else (k) = 0;
#define between(a,b,c) (((a)<=(b) && (b)<(c)) || ((c)<(a) && (a)<=(b)) || ((b)<(c) && (c)<(a)))
int max_seq_num=7;
/* [SR] SR协议窗口大小限制：W ≤ (MAX_SEQ+1)/2，以避免序列号歧义
 *   例如max_seq_num=7时，序列号空间为0~7共8个，SR窗口≤4 */
int sr_window_size = 4;  // 在main()中根据max_seq_num动态计算

/* ================================================================
 * [SR] 帧结构定义（与GBN版本完全一致）
 * ================================================================ */
struct FRAME {
    unsigned char kind;  // FRAME_DATA=1, FRAME_ACK=2, FRAME_NAK=3
    unsigned char ack;   // [SR] 独立ACK：确认号表示具体收到哪一帧（非累积）
    unsigned char seq;   // 当前发送帧的序号
    unsigned char data[PKT_LEN]; // 有效载荷256字节
    unsigned int  padding;
};

/* ================================================================
 * [SR] 发送方变量（与GBN版本一致）
 * ================================================================ */
static unsigned char send_window_l = 0, send_window_r = 0;
static unsigned char buffer[ABSOLUTE_MAX_SEQ_NUM+1][PKT_LEN], nbuffered = 0;
/* [SR] 发送方新增：记录发送窗口中哪些帧已被确认 */
static int acked[ABSOLUTE_MAX_SEQ_NUM + 1] = {0};
static int phl_ready = 0;

/* ================================================================
 * [SR] 接收方变量 —— SR核心差异：使用完整接收窗口
 *   GBN: 只有一个变量 frame_expected，接收窗口大小=1
 *   SR:  维护接收窗口[recv_window_l, recv_window_r)，大小=sr_window_size
 * ================================================================ */
static unsigned char recv_window_l = 0;  // 接收窗口左沿（下一个期望的有序帧）
static unsigned char recv_window_r = 4;  // [SR] 接收窗口右沿（初始化为sr_window_size）
/* [SR] 接收方新增：标记接收窗口中哪些帧已到达（用于缓存乱序帧） */
static int arrived[ABSOLUTE_MAX_SEQ_NUM + 1] = {0};
/* [SR] 接收方新增：乱序帧缓存区 */
static unsigned char recv_buffer[ABSOLUTE_MAX_SEQ_NUM + 1][PKT_LEN];

/* ================================================================
 * 以下辅助函数与GBN版本结构相同，但ACK语义已改为独立确认
 * ================================================================ */

static void trace_window_state(const char* action) {
    /* [SR] 相比GBN，额外打印接收窗口右沿、SR窗口大小 */
    dbg_event("[%s] SendWin:[%d,%d) nb=%d | RecvWin:[%d,%d) | sr_win=%d\n",
            action, send_window_l, send_window_r, nbuffered,
            recv_window_l, recv_window_r, sr_window_size);
}

static void put_frame(unsigned char *frame, int len)
{
    *(unsigned int *)(frame + len) = crc32(frame, len);
    send_frame(frame, len + 4);
    phl_ready = 0;
}

/* [SR] send_nak_frame: 与GBN结构相同，但NAK的语义变为请求单个缺失帧 */
static void send_nak_frame(void)
{
    struct FRAME s;
    s.kind = FRAME_NAK;
    if(recv_window_l == 0) s.ack = max_seq_num;
    else s.ack = recv_window_l - 1;
    dbg_frame("Send NAK %d\n", s.ack);
    put_frame((unsigned char *)&s, 2);
}

/* [SR] send_data_frame: 结构与GBN一致，但捎带ACK语义变为独立确认 */
static void send_data_frame(unsigned char frame_nr)
{
    struct FRAME s;
    s.kind = FRAME_DATA;
    s.seq = frame_nr;
    /* [SR] 捎带ACK：确认最后收到的有序帧（独立ACK语义） */
    if(recv_window_l == 0) s.ack = max_seq_num;
    else s.ack = recv_window_l - 1;
    memcpy(s.data, buffer[s.seq], PKT_LEN);
    dbg_frame("Send DATA %d %d, ID %d\n", s.seq, s.ack, *(short *)s.data);
    put_frame((unsigned char *)&s, 3 + PKT_LEN);
    start_timer(frame_nr, DATA_TIMER);
}

static void send_ack_frame(void)
{
    struct FRAME s;
    s.kind = FRAME_ACK;
    if(recv_window_l == 0) s.ack = max_seq_num;
    else s.ack = recv_window_l - 1;
    dbg_frame("Send ACK  %d\n", s.ack);
    put_frame((unsigned char *)&s, 2);
}

int main(int argc, char **argv)
{
    int event, arg;
    struct FRAME f;
    int len = 0;

    /* ================================================================
     * [SR] 命令行参数解析：-w 设置max_seq_num，SR窗口自动为(max_seq_num+1)/2
     * ================================================================ */
    for(int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-w") == 0 && i + 1 < argc) {
            max_seq_num = atoi(argv[i + 1]);
            argv[i][0] = '\0';
            argv[i+1][0] = '\0';
            if (max_seq_num <= 0 || max_seq_num > ABSOLUTE_MAX_SEQ_NUM) {
                max_seq_num = 7;
            }
        }
    }

    /* [SR] SR窗口大小必须≤(MAX_SEQ+1)/2，否则会产生序列号歧义 */
    sr_window_size = (max_seq_num + 1) / 2;
    /* [SR] 初始化接收窗口右沿 */
    recv_window_r = sr_window_size;
    /* [SR] 初始化acked和arrived数组 */
    for (int i = 0; i <= ABSOLUTE_MAX_SEQ_NUM; i++) {
        acked[i] = 0;
        arrived[i] = 0;
    }

    protocol_init(argc, argv);

    lprintf("Designed by LLynn51 (GBN) / SR adaptation, build: " __DATE__"  "__TIME__"\n");
    lprintf("Protocol: Selective Repeat | Piggybacking: Enabled | NAK: Enabled | MAX_SEQ:%d | SR_Window:%d\n",
            max_seq_num, sr_window_size);

    disable_network_layer();

    for (;;) {
        event = wait_for_event(&arg);

        switch (event) {
        /* ================================================================
         * 网络层就绪：与GBN相同（使用sr_window_size控制流量）
         * ================================================================ */
        case NETWORK_LAYER_READY:
            get_packet(buffer[send_window_r]);
            nbuffered++;
            send_data_frame(send_window_r);
            inc(send_window_r);
            stop_ack_timer();
            trace_window_state("NETWORK_LAYER_READY");
            break;

        /* ================================================================
         * 物理层就绪：与GBN完全相同
         * ================================================================ */
        case PHYSICAL_LAYER_READY:
            phl_ready = 1;
            break;

        /* ================================================================
         * [SR] 收到帧：这是SR与GBN差异最大的部分
         *   GBN: 只接受frame_expected帧，乱序则丢弃+发NAK，使用累积ACK
         *   SR:  接受接收窗口内任意帧，缓存乱序帧，使用独立ACK
         * ================================================================ */
        case FRAME_RECEIVED:
            len = recv_frame((unsigned char *)&f, sizeof f);
            if (len < 5 || crc32((unsigned char *)&f, len) != 0) {
                dbg_event("**** Receiver Error, Bad CRC Checksum\n");
                break;
            }

            /* ---- [SR] 收到ACK帧：独立ACK处理（GBN是累积ACK） ---- */
            if (f.kind == FRAME_ACK) {
                dbg_frame("Recv ACK  %d\n", f.ack);
                /* [SR] 只有一个帧被确认，而非GBN的全部累积确认 */
                if (between(send_window_l, f.ack, send_window_r)) {
                    acked[f.ack] = 1;
                }
            }

            /* ---- [SR] 收到NAK帧：仅重传缺失的那一帧（GBN重传全部） ---- */
            if (f.kind == FRAME_NAK) {
                unsigned char missing = f.ack;
                inc(missing);
                dbg_frame("**** Recv NAK %d, retransmit frame %d\n", f.ack, missing);
                /* [SR] 只重传missing那一帧，不连带重传后续所有帧 */
                if (between(send_window_l, missing, send_window_r)) {
                    send_data_frame(missing);
                }
            }

            /* ---- [SR] 收到数据帧：SR与GBN的核心差异所在 ---- */
            if (f.kind == FRAME_DATA) {
                dbg_frame("Recv DATA %d %d, ID %d\n", f.seq, f.ack, *(short *)f.data);

                /* [SR] 只要帧序号落在接收窗口[recv_window_l, recv_window_r)内就接受
                 *   GBN版本此处仅接受f.seq==frame_expected的帧 */
                if (between(recv_window_l, f.seq, recv_window_r)) {
                    /* [SR] 缓存乱序帧（避免重复接收同一帧） */
                    if (!arrived[f.seq]) {
                        arrived[f.seq] = 1;
                        memcpy(recv_buffer[f.seq], f.data, PKT_LEN);
                    }
                    /* [SR] 按序交付：从recv_window_l开始，连续交付所有已到达的帧 */
                    while (arrived[recv_window_l]) {
                        put_packet(recv_buffer[recv_window_l], PKT_LEN);
                        arrived[recv_window_l] = 0;
                        inc(recv_window_l);
                        inc(recv_window_r);  /* 接收窗口同步滑动 */
                    }
                    start_ack_timer(ACK_TIMER);
                    no_nak = 1;
                } else if (no_nak) {
                    /* [SR] 帧不在接收窗口内，发送NAK请求缺失帧 */
                    dbg_event("**** Frame %d outside recv window [%d,%d), sending NAK\n",
                              f.seq, recv_window_l, recv_window_r);
                    send_nak_frame();
                    no_nak = 0;
                }
            }

            /* ---- [SR] 发送窗口滑动：只滑动连续ACK的帧（GBN是累积滑动） ---- */
            /* [SR] 处理捎带ACK（数据帧中携带的ACK信息）及纯ACK帧 */
            if (f.kind == FRAME_DATA && between(send_window_l, f.ack, send_window_r)) {
                /* [SR] 数据帧中的捎带独立ACK */
                acked[f.ack] = 1;
            }
            /* [SR] 从窗口左端开始，连续滑动已被确认的帧 */
            while (acked[send_window_l] && nbuffered > 0) {
                acked[send_window_l] = 0;
                nbuffered--;
                stop_timer(send_window_l);
                inc(send_window_l);
            }
            trace_window_state("FRAME_RECEIVED");
            break;

        /* ================================================================
         * ACK超时：与GBN相同（发送纯ACK帧）
         * ================================================================ */
        case ACK_TIMEOUT:
            dbg_event("**** ACK_TIMER expired, send pure ACK.\n");
            send_ack_frame();
            trace_window_state("ACK_TIMEOUT");
            break;

        /* ================================================================
         * [SR] 数据超时：SR与GBN的最大差异之一
         *   GBN: 从send_window_l开始重传窗口内全部帧
         *   SR:  只重传超时(arg)的那一帧！
         * ================================================================ */
        case DATA_TIMEOUT:
            dbg_event("**** DATA %d timeout, retransmit ONLY this frame\n", arg);
            /* [SR] 关键差异：仅重传超时的单个帧，不连坐重传后续帧 */
            if (between(send_window_l, (unsigned char)arg, send_window_r)) {
                send_data_frame((unsigned char)arg);
            }
            trace_window_state("DATA_TIMEOUT");
            break;
        }

        /* ================================================================
         * [SR] 流量控制：使用sr_window_size代替max_seq_num
         *   GBN: nbuffered < max_seq_num（W=7）
         *   SR:  nbuffered < sr_window_size（W=4，因为W≤(MAX_SEQ+1)/2）
         * ================================================================ */
        if (nbuffered < sr_window_size && phl_ready)
            enable_network_layer();
        else
            disable_network_layer();
    }
}
