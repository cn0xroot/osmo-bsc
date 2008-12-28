#ifndef _GSM_04_11_H
#define _GSM_04_11_H

/* GSM TS 04.11  definitions */

/* Chapter 8.1.2 (refers to GSM 04.07 Chapter 11.2.3.1.1 */
#define GSM411_PDISC_SMS	0x09

/* Chapter 8.1.3 */
#define GSM411_MT_CP_DATA	0x01
#define GSM411_MT_CP_ACK	0x04
#define GSM411_MT_CP_ERROR	0x10

/* Chapter 8.2.2 */
#define GSM411_MT_RP_DATA_MO	0x00
#define GSM411_MT_RP_DATA_MT	0x01
#define GSM411_MT_RP_ACK_MO	0x02
#define GSM411_MT_RP_ACK_MT	0x03
#define GSM411_MT_RP_ERROR_MO	0x04
#define GSM411_MT_RP_ERROR_MT	0x04
#define GSM411_MT_RP_SMMA_MO	0x05

/* Chapter 8.1.1 */
struct gsm411_rp_data_hdr {
  u_int8_t msg_type;
	u_int8_t msg_ref;
	u_int8_t data[0];
} __attribute__ ((packed));

struct msgb;

int gsm0411_rcv_sms(struct msgb *msg);

#endif
