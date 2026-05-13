/**
 * @ Author: LLynn51
 * @ Create Time: 2026-05-10 21:49:19
 * @ Modified by: LLynn51
 * @ Modified time: 2026-05-13 16:20:04
 * @ Description:
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "protocol.h"
#include "datalink.h"

#define DATA_TIMER  2000
#define ACK_TIMER 150

#define ABSOLUTE_MAX_SEQ_NUM 255 // 由于seq类型为unsigned char，故最大窗口大小为255，否则会报错
// no_nak 标识当前是否可以发送NAK帧。为1表示可以发送，为0表示已经发送过了。
static int no_nak = 1;
// 用于序号循环递增
#define inc(k) if((k) < max_seq_num) (k)++; else (k) = 0; 
// 判断序列号a是否在[b,c)窗口内，应对序列号循环的情况
#define between(a,b,c) (((a)<=(b) && (b)<(c)) || ((c)<(a) && (a)<=(b)) || ((b)<(c) && (c)<(a)))
int max_seq_num=7; // 默认最大窗口大小为7 

struct FRAME { 
    unsigned char kind; // 帧的类型，有 FRAME_DATA=1,FRAME_ACK=2,FRAME_NAK=3 这三种类型
    unsigned char ack; // 对于数据帧，是期望收到的ACK帧的序号；对于ACK和NAK帧是上一个成功接收到的ACK帧的序号。
    unsigned char seq; // 当前发送的帧的序号
    unsigned char data[PKT_LEN]; // 有效载荷，PKT_LEN=256
    unsigned int  padding; // 上述内容加在一起不足256字节时的填充
};

// 发送窗口的左沿和右沿，分别代表期待接收ack的最早帧和下一个要接收的数据帧
static unsigned char send_window_l = 0,send_window_r=0;
static unsigned char buffer[ABSOLUTE_MAX_SEQ_NUM+1][PKT_LEN], nbuffered=0;
// 期望收到的ack帧的序号
static unsigned char frame_expected = 0;
static int phl_ready = 0;

// 实现软件协议跟踪
static void trace_window_state(const char* action) {
    dbg_event("[%s] Sender Window: L=%d, R=%d, nbuffered=%d | Receiver expects: %d\n", 
            action, send_window_l, send_window_r, nbuffered, frame_expected);
}

static void put_frame(unsigned char *frame, int len)
{
    *(unsigned int *)(frame + len) = crc32(frame, len);
    send_frame(frame, len + 4);
    phl_ready = 0;
}

// send_nak_frame 在接收到错误序号的帧的时候及时返回NAK帧提前终止发送、开始重传
static void send_nak_frame(void)
{
    struct FRAME s;
    s.kind = FRAME_NAK;

    // NAK序号为发送方最后一次成功发送的包
    if(frame_expected == 0) s.ack = max_seq_num;
    else s.ack = frame_expected - 1; 

    dbg_frame("Send NAK %d\n", s.ack);
    put_frame((unsigned char *)&s, 2);
}


static void send_data_frame(unsigned char frame_nr)//接收参数frame_nr表示本次调用函数要发送的帧序号
{
    struct FRAME s;

    s.kind = FRAME_DATA;
    s.seq = frame_nr;

    if(frame_expected==0)s.ack=max_seq_num;
    else s.ack = frame_expected - 1;

    memcpy(s.data, buffer[s.seq], PKT_LEN);

    dbg_frame("Send DATA %d %d, ID %d\n", s.seq, s.ack, *(short *)s.data);

    put_frame((unsigned char *)&s, 3 + PKT_LEN);
    start_timer(frame_nr, DATA_TIMER);
}

static void send_ack_frame(void)
{
    struct FRAME s;

    s.kind = FRAME_ACK;

    if(frame_expected==0)s.ack=max_seq_num;
    else s.ack=frame_expected-1;

    dbg_frame("Send ACK  %d\n", s.ack);

    put_frame((unsigned char *)&s, 2);
}

int main(int argc, char **argv)
{
    int event, arg;
    struct FRAME f;
    int len = 0;
    
    //从命令行接收窗口大小（默认为7）
    for(int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-w") == 0 && i + 1 < argc) {
            max_seq_num = atoi(argv[i + 1]);
            // 避免后续底层库解析出错
            argv[i][0] = '\0'; 
            argv[i+1][0] = '\0';
            // 简单校验
            if (max_seq_num <= 0 || max_seq_num > ABSOLUTE_MAX_SEQ_NUM) {
                max_seq_num = 7;
            }
        }
    }

    protocol_init(argc, argv); 

    lprintf("Designed by LLynn, build: " __DATE__"  "__TIME__"\n");
    lprintf("Protocol Options: Piggybacking (Enabled), NAK (Enabled),Window size:%d",max_seq_num);

    disable_network_layer();

    for (;;) {
        event = wait_for_event(&arg);

        switch (event) {
        // 当前端口网络层准备好时，发送捎带ACK帧并取消计时器
        case NETWORK_LAYER_READY:
            get_packet(buffer[send_window_r]);
            nbuffered++;
            send_data_frame(send_window_r);
            inc(send_window_r);
            stop_ack_timer();
            trace_window_state("NETWORK_LAYER_READY, appended new packet.");
            break;

        case PHYSICAL_LAYER_READY:
            phl_ready = 1;
            //由于PHYSICAL_LAYER_READY事件触发及其频繁，故不打印相关信息
            break;

        
        //当前端口作为接收方的情况
        case FRAME_RECEIVED: 
            len = recv_frame((unsigned char *)&f, sizeof f);
            // 检查文件是否损坏。如果损坏不需要重传，只是不发ack，直接等待超时处理。
            if (len < 5 || crc32((unsigned char *)&f, len) != 0) {
                dbg_event("**** Receiver Error, Bad CRC Checksum\n");
                break;
            }

            // 作为发送方正常接收到ACK帧的情况
            if (f.kind == FRAME_ACK) dbg_frame("Recv ACK  %d\n", f.ack);

            
            // 作为发送方收到NAK帧的情况
            if (f.kind == FRAME_NAK){
                // 计算需要重发的帧的序号
                unsigned char missing = f.ack;
                inc(missing);

                dbg_frame("**** Recv NAK %d, immediate resending\n",f.ack);
                // 像超时重传一样，从NAK帧好开始进行超时重传
                unsigned char temp = missing;
                for (int i = 0; i < nbuffered; i++) {
                    if (between(send_window_l, temp, send_window_r)) {
                        send_data_frame(temp);
                        inc(temp);
                    }
                }
            }

            // 接收方接收到无损数据帧时，如果是预期中按序到达的数据帧，
            // 就右移接收窗口，并等待捎带帧一起发送ACK帧，直到ACK计时器超时
            if (f.kind == FRAME_DATA) {
                dbg_frame("Recv DATA %d %d, ID %d\n", f.seq, f.ack, *(short *)f.data);
                if (f.seq == frame_expected) {
                    put_packet(f.data, len - 7);
                    inc(frame_expected);
                    trace_window_state("DATA_TIMEOUT, resending");
                    start_ack_timer(ACK_TIMER);
                    no_nak=1;
                }else if (no_nak) {// 如果接收到无损数据帧，但序号不对，就发送带有期望数据帧序号的nak帧
                    dbg_event("**** Out of order frame %d (expected %d), sending NAK", f.seq, frame_expected);
                    send_nak_frame(); 
                    no_nak = 0; // 将no_nak的状态标记为不可发送
                }
            }


            // 当发送端接收到的ack帧落在发送窗口中时，说明该ack帧对应的数据帧被正常接收
            // 停止计时，并右移发送窗口的左边界
            while (between(send_window_l, f.ack, send_window_r)) {
                nbuffered--;
                stop_timer(send_window_l);
                inc(send_window_l);
            }
            trace_window_state("FRAME_RECEIVED, window slides after ack");
            break; 


        // 作为接收方的情况
        case ACK_TIMEOUT: // ACK计时器超时时没有可以捎带ACK的数据帧，直接发送纯ACK
            dbg_event("**** ACK_TIMER expired, send pure ACK.");
            send_ack_frame();
            trace_window_state("ACK_TIMER expired, send pure ACK.");
            break;
        

        // 作为发送方的情况
        // 若计时器超时后仍没有收到第arg帧，需要重发不在其之前的所有帧（从第arg帧到发送窗口最右端的帧）
        case DATA_TIMEOUT:
            dbg_event("**** DATA %d timeout, FRAME not in front of it needs to be send again\n", arg); 
            unsigned char temp = send_window_l; //从未确认的第一个包开始
            for (int i = 0; i < nbuffered; i++) {
                send_data_frame(temp);
                inc(temp);
            }
            trace_window_state("DATA_TIMEOUT, resending");
            break;
        
        }
        // 当缓冲区尚未塞满时，允许网络层继续向数据链路层发送数据包
        if (nbuffered < max_seq_num && phl_ready)
            enable_network_layer();
        else
            disable_network_layer();
   }
}
