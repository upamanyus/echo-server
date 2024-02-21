#ifndef I10E_H_
#define I10E_H_

#include <linux/types.h>

#define EIMC (0x00888)
#define CTRL_addr (0x0000)
#define STATUS (0x0008)
#define CTRL_EXT (0x0018)
#define LINKS (0x042A4)
#define EEC (0x10010)
#define AUTOC (0x042A0)
#define FCTRL (0x05080)
#define MTA(n) (0x05200 + 4*n)

// 8.2.3.8
// valid for n = 0...63
#define RDBAL(n) (0x01000 + 0x40*n)
#define RDBAH(n) (0x01004 + 0x40*n)
#define RDLEN(n) (0x01008 + 0x40*n)
#define RDH(n) (0x01010 + 0x40*n)
#define RDT(n) (0x01018 + 0x40*n)
#define RXDCTL(n) (0x01028 + 0x40*n)
#define SRRCTL(n) (0x01014 + 0x40*n)
#define RDRXCTL (0x02F00)
#define RXPBSIZE(n) (0x03C00 + 4*n)
#define RXCTRL (0x03000)

// 8.2.3.9
#define DTXMXSZRQ (0x08100)
#define DMATXCTL (0x04A80)
#define DTXTCPFLGL (0x04A88)
#define DTXTCPFLGH (0x04A8C)
#define TDBAL(n) (0x06000+0x40*n)
#define TDBAH(n) (0x06004+0x40*n)
#define TDLEN(n) (0x06008+0x40*n)
#define TDH(n) (0x06010+0x40*n)
#define TDT(n) (0x06018+0x40*n)
#define TXDCTL(n) (0x06028+0x40*n)
#define TDWBAL(n) (0x06038+0x40*n)
#define TDWBAH(n) (0x0603C+0x40*n)

#define HLREG0 (0x04240)

typedef struct {
    __u64 buffer_address;
    __u16 length;
    __u16 checksum;
    __u16 status;
    __u16 errors;
    __u16 vlan_tag;
} rq_desc_t;

typedef struct {
    __u64 buffer_address;
    __u16 length;
    __u8 cso;
    __u8 cmd;
    __u8 sta_and_rsvd;
    __u8 css;
    __u16 vlan;
} tq_desc_t;

#endif // I10E_H_
