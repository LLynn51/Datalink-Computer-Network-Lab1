/**
 * @ Author: LLynn51 (GBN Original), SR adaptation
 * @ Create Time: 2026-05-10 21:49:31
 * @ Description: 数据链路层帧格式定义（GBN与SR共用相同帧格式）
 */


/* FRAME kind */
#define FRAME_DATA 1
#define FRAME_ACK  2
#define FRAME_NAK  3

/*
    DATA Frame（GBN与SR格式一致）
    +=========+========+========+===============+========+
    | KIND(1) | SEQ(1) | ACK(1) | DATA(240~256) | CRC(4) |
    +=========+========+========+===============+========+

    ACK Frame（GBN与SR格式一致，但ACK语义不同：SR为独立ACK）
    +=========+========+========+
    | KIND(1) | ACK(1) | CRC(4) |
    +=========+========+========+

    NAK Frame（GBN与SR格式一致，但NAK语义不同：SR为单个帧重传请求）
    +=========+========+========+
    | KIND(1) | ACK(1) | CRC(4) |
    +=========+========+========+
*/
