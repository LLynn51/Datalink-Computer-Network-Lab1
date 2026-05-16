#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "protocol.h"
#include "datalink.h"

#define DATA_TIMER 2000
#define ACK_TIMER 150

/*
 * protocol.c has 128 data timers (0..127).  Keep the sequence space inside
 * that range so every outstanding frame can own a valid timer.
 */
#define MAX_SEQ_LIMIT 127
#define DEFAULT_SR_WINDOW_SIZE 4

#define inc(k) do { if ((k) < max_seq_num) (k)++; else (k) = 0; } while (0)
#define between(a,b,c) (((a) <= (b) && (b) < (c)) || ((c) < (a) && (a) <= (b)) || ((b) < (c) && (c) < (a)))

static int max_seq_num = DEFAULT_SR_WINDOW_SIZE * 2 - 1;
static int sr_window_size = DEFAULT_SR_WINDOW_SIZE;
static int no_nak = 1;

struct FRAME {
    unsigned char kind;
    unsigned char ack;
    unsigned char seq;
    unsigned char data[PKT_LEN];
    unsigned int padding;
};

static unsigned char send_window_l = 0;
static unsigned char send_window_r = 0;
static unsigned char frame_expected = 0;
static int nbuffered = 0;
static int phl_ready = 0;

static unsigned char send_buffer[MAX_SEQ_LIMIT + 1][PKT_LEN];
static unsigned char recv_buffer[MAX_SEQ_LIMIT + 1][PKT_LEN];
static int acked[MAX_SEQ_LIMIT + 1];
static int arrived[MAX_SEQ_LIMIT + 1];

static unsigned char last_in_order_ack(void)
{
    return frame_expected == 0 ? (unsigned char)max_seq_num : (unsigned char)(frame_expected - 1);
}

static void trace_window_state(const char *action)
{
    dbg_event("[%s] SendWin:[%d,%d) nb=%d | RecvBase:%d | W=%d\n",
              action, send_window_l, send_window_r, nbuffered,
              frame_expected, sr_window_size);
}

static void put_frame(unsigned char *frame, int len)
{
    *(unsigned int *)(frame + len) = crc32(frame, len);
    send_frame(frame, len + 4);
    phl_ready = 0;
}

static void send_ack_frame(unsigned char ack)
{
    struct FRAME s;

    memset(&s, 0, sizeof(s));
    s.kind = FRAME_ACK;
    s.ack = ack;

    dbg_frame("Send ACK  %d\n", s.ack);
    put_frame((unsigned char *)&s, 2);
}

static void send_nak_frame(void)
{
    struct FRAME s;

    memset(&s, 0, sizeof(s));
    s.kind = FRAME_NAK;
    s.ack = last_in_order_ack();

    dbg_frame("Send NAK %d\n", s.ack);
    put_frame((unsigned char *)&s, 2);
}

static void send_data_frame(unsigned char frame_nr)
{
    struct FRAME s;

    memset(&s, 0, sizeof(s));
    s.kind = FRAME_DATA;
    s.seq = frame_nr;
    s.ack = last_in_order_ack();
    memcpy(s.data, send_buffer[frame_nr], PKT_LEN);

    dbg_frame("Send DATA %d %d, ID %d\n", s.seq, s.ack, *(unsigned short *)s.data);
    put_frame((unsigned char *)&s, 3 + PKT_LEN);
    start_timer(frame_nr, DATA_TIMER);
}

static void acknowledge_frame(unsigned char ack)
{
    if (nbuffered > 0 && between(send_window_l, ack, send_window_r)) {
        acked[ack] = 1;
        stop_timer(ack);
    }

    while (nbuffered > 0 && acked[send_window_l]) {
        acked[send_window_l] = 0;
        stop_timer(send_window_l);
        nbuffered--;
        inc(send_window_l);
    }
}

static int in_receive_window(unsigned char seq)
{
    unsigned char right = frame_expected;
    int i;

    for (i = 0; i < sr_window_size; i++) {
        inc(right);
    }

    return between(frame_expected, seq, right);
}

static void deliver_ordered_frames(void)
{
    while (arrived[frame_expected]) {
        put_packet(recv_buffer[frame_expected], PKT_LEN);
        arrived[frame_expected] = 0;
        inc(frame_expected);
    }
}

static void parse_window_option(int argc, char **argv)
{
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-w") == 0 && i + 1 < argc) {
            sr_window_size = atoi(argv[i + 1]);
            break;
        }
    }

    if (sr_window_size <= 0)
        sr_window_size = DEFAULT_SR_WINDOW_SIZE;
    if (sr_window_size > (MAX_SEQ_LIMIT + 1) / 2)
        sr_window_size = (MAX_SEQ_LIMIT + 1) / 2;

    max_seq_num = sr_window_size * 2 - 1;
}

int main(int argc, char **argv)
{
    int event;
    int arg;
    int len;
    struct FRAME f;

    parse_window_option(argc, argv);
    memset(acked, 0, sizeof(acked));
    memset(arrived, 0, sizeof(arrived));

    protocol_init(argc, argv);

    lprintf("Designed by LLynn51 / fixed SR, build: " __DATE__ "  " __TIME__ "\n");
    lprintf("Protocol: Selective Repeat | per-frame ACK | NAK | MAX_SEQ:%d | Window:%d\n",
            max_seq_num, sr_window_size);

    disable_network_layer();

    for (;;) {
        event = wait_for_event(&arg);

        switch (event) {
        case NETWORK_LAYER_READY:
            get_packet(send_buffer[send_window_r]);
            nbuffered++;
            send_data_frame(send_window_r);
            inc(send_window_r);
            stop_ack_timer();
            trace_window_state("NETWORK_LAYER_READY");
            break;

        case PHYSICAL_LAYER_READY:
            phl_ready = 1;
            break;

        case FRAME_RECEIVED:
            len = recv_frame((unsigned char *)&f, sizeof(f));
            if (len < 5 || crc32((unsigned char *)&f, len) != 0) {
                dbg_event("**** Bad CRC, NAK expected frame %d\n", frame_expected);
                if (no_nak) {
                    send_nak_frame();
                    no_nak = 0;
                }
                break;
            }

            if (f.kind == FRAME_ACK) {
                dbg_frame("Recv ACK  %d\n", f.ack);
                acknowledge_frame(f.ack);
            } else if (f.kind == FRAME_NAK) {
                unsigned char missing = f.ack;

                inc(missing);
                dbg_frame("**** Recv NAK %d, retransmit DATA %d\n", f.ack, missing);
                if (between(send_window_l, missing, send_window_r) && !acked[missing])
                    send_data_frame(missing);
            } else if (f.kind == FRAME_DATA) {
                int was_expected;

                dbg_frame("Recv DATA %d %d, ID %d\n", f.seq, f.ack, *(unsigned short *)f.data);

                was_expected = (f.seq == frame_expected);
                if (in_receive_window(f.seq)) {
                    if (!arrived[f.seq]) {
                        arrived[f.seq] = 1;
                        memcpy(recv_buffer[f.seq], f.data, PKT_LEN);
                    }

                    send_ack_frame(f.seq);

                    if (!was_expected && no_nak) {
                        send_nak_frame();
                        no_nak = 0;
                    }

                    deliver_ordered_frames();
                    if (was_expected)
                        no_nak = 1;
                } else {
                    send_ack_frame(f.seq);
                }
            }

            trace_window_state("FRAME_RECEIVED");
            break;

        case ACK_TIMEOUT:
            dbg_event("**** ACK timer expired\n");
            send_ack_frame(last_in_order_ack());
            trace_window_state("ACK_TIMEOUT");
            break;

        case DATA_TIMEOUT:
            dbg_event("**** DATA %d timeout, retransmit\n", arg);
            if (between(send_window_l, (unsigned char)arg, send_window_r) && !acked[(unsigned char)arg])
                send_data_frame((unsigned char)arg);
            trace_window_state("DATA_TIMEOUT");
            break;
        }

        if (nbuffered < sr_window_size && phl_ready)
            enable_network_layer();
        else
            disable_network_layer();
    }
}
