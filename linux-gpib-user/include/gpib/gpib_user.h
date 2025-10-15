/* SPDX-License-Identifier: GPL-2.0 */

/***************************************************************************
 *    copyright		   : (C) 2002 by Frank Mori Hess
 ***************************************************************************/

#ifndef _GPIB_USER_H
#define _GPIB_USER_H

#include <linux/types.h>
#include "gpib.h"


/* IBERR error codes */
enum iberr_code {
	EDVR = 0,		/* system error */
	ECIC = 1,		/* not CIC */
	ENOL = 2,		/* no listeners */
	EADR = 3,		/* CIC and not addressed before I/O */
	EARG = 4,		/* bad argument to function call */
	ESAC = 5,		/* not SAC */
	EABO = 6,		/* I/O operation was aborted */
	ENEB = 7,		/* non-existent board (GPIB interface offline) */
	EDMA = 8,		/* DMA hardware error detected */
	EOIP = 10,		/* new I/O attempted with old I/O in progress  */
	ECAP = 11,		/* no capability for intended opeation */
	EFSO = 12,		/* file system operation error */
	EBUS = 14,		/* bus error */
	ESTB = 15,		/* lost serial poll bytes */
	ESRQ = 16,		/* SRQ stuck on */
	ECNF = 17,              /* Configuration file error */
	ETAB = 20	       /* Table Overflow */
};

/* Timeout values and meanings */
enum gpib_timeout {
	TNONE = 0,		/* Infinite timeout (disabled)	   */
	T10us = 1,		/* Timeout of 10 usec (ideal)	   */
	T30us = 2,		/* Timeout of 30 usec (ideal)	   */
	T100us = 3,		/* Timeout of 100 usec (ideal)	   */
	T300us = 4,		/* Timeout of 300 usec (ideal)	   */
	T1ms = 5,		/* Timeout of 1 msec (ideal)	   */
	T3ms = 6,		/* Timeout of 3 msec (ideal)	   */
	T10ms = 7,		/* Timeout of 10 msec (ideal)	   */
	T30ms = 8,		/* Timeout of 30 msec (ideal)	   */
	T100ms = 9,		/* Timeout of 100 msec (ideal)	   */
	T300ms = 10,	/* Timeout of 300 msec (ideal)	   */
	T1s = 11,		/* Timeout of 1 sec (ideal)	   */
	T3s = 12,		/* Timeout of 3 sec (ideal)	   */
	T10s = 13,		/* Timeout of 10 sec (ideal)	   */
	T30s = 14,		/* Timeout of 30 sec (ideal)	   */
	T100s = 15,		/* Timeout of 100 sec (ideal)	   */
	T300s = 16,		/* Timeout of 300 sec (ideal)	   */
	T1000s = 17		/* Timeout of 1000 sec (maximum)   */
};

/* Possible GPIB command messages */

enum cmd_byte {
	GTL = 0x1,      /* go to local                  */
	SDC = 0x4,      /* selected device clear        */
	PP_CONFIG = 0x5,
#ifndef PPC
	PPC = PP_CONFIG, /* parallel poll configure     */
#endif
	GET = 0x8,      /* group execute trigger        */
	TCT = 0x9,      /* take control                 */
	LLO = 0x11,     /* local lockout                */
	DCL = 0x14,     /* device clear                 */
	PPU = 0x15,     /* parallel poll unconfigure    */
	SPE = 0x18,     /* serial poll enable           */
	SPD = 0x19,     /* serial poll disable          */
	CFE = 0x1f, /* configure enable */
	LAD = 0x20,     /* value to be 'ored' in to obtain listen address */
	UNL = 0x3F,     /* unlisten                     */
	TAD = 0x40,     /* value to be 'ored' in to obtain talk address   */
	UNT = 0x5F,     /* untalk                       */
	SAD = 0x60,     /* my secondary address (base) */
	PPE = 0x60,     /* parallel poll enable (base)  */
	PPD = 0x70      /* parallel poll disable        */
};


static const int gpib_addr_max = 30;	/* max address for primary/secondary gpib addresses */
static const int gpib_sad_max = 31;	/* (0x1f) max address for secondary gpib addresses */

/* confine address to range 0 to 30. */
static inline unsigned int gpib_address_restrict(unsigned int addr)
{
	addr &= 0x1f;
	if (addr == 0x1f)
		addr = 0;
	return addr;
}

static inline __u8 MLA(unsigned int addr)
{
	return gpib_address_restrict(addr) | LAD;
}

static inline __u8 MTA(unsigned int addr)
{
	return gpib_address_restrict(addr) | TAD;
}

static inline __u8 MSA(unsigned int addr)
{
	return (addr & 0x1f) | SAD;
}

static inline __u8 PPE_byte(unsigned int dio_line, int sense)
{
	__u8 cmd;

	cmd = PPE;
	if (sense)
		cmd |= PPC_SENSE;
	cmd |= (dio_line - 1) & 0x7;
	return cmd;
}

/* mask of bits that actually matter in a command byte */
enum {
	gpib_command_mask = 0x7f,
};

static inline int is_PPE(__u8 command)
{
	return (command & 0x70) == 0x60;
}

static inline int is_PPD(__u8 command)
{
	return (command & 0x70) == 0x70;
}

static inline int in_addressed_command_group(__u8 command)
{
	return (command & 0x70) == 0x0;
}

static inline int in_universal_command_group(__u8 command)
{
	return (command & 0x70) == 0x10;
}

static inline int in_listen_address_group(__u8 command)
{
	return (command & 0x60) == 0x20;
}

static inline int in_talk_address_group(__u8 command)
{
	return (command & 0x60) == 0x40;
}

static inline int in_primary_command_group(__u8 command)
{
	return in_addressed_command_group(command) ||
		in_universal_command_group(command) ||
		in_listen_address_group(command) ||
		in_talk_address_group(command);
}

static inline int gpib_address_equal(unsigned int pad1, int sad1, unsigned int pad2, int sad2)
{
	if (pad1 == pad2) {
		if (sad1 == sad2)
			return 1;
		if (sad1 < 0 && sad2 < 0)
			return 1;
	}

	return 0;
}

static __inline__ uint8_t CFGn( unsigned int meters )
{
	return 0x6 | (meters & 0xf);
}
enum ibask_option {
	IBA_PAD = 0x1,
	IBA_SAD = 0x2,
	IBA_TMO = 0x3,
	IBA_EOT = 0x4,
	IBA_PPC = 0x5,  /* board only */
	IBA_READ_DR = 0x6,      /* device only */
	IBA_AUTOPOLL = 0x7,     /* board only */
	IBA_CICPROT = 0x8,      /* board only */
	IBA_IRQ = 0x9,  /* board only */
	IBA_SC = 0xa,   /* board only */
	IBA_SRE = 0xb,  /* board only */
	IBA_EOS_RD = 0xc,
	IBA_EOS_WRT = 0xd,
	IBA_EOS_CMP = 0xe,
	IBA_EOS_CHAR = 0xf,
	IBA_PP2 = 0x10, /* board only */
	IBA_TIMING = 0x11,      /* board only */
	IBA_DMA = 0x12, /* board only */
	IBA_READ_ADJUST = 0x13,
	IBA_WRITE_ADJUST = 0x14,
	IBA_EVENT_QUEUE = 0x15, /* board only */
	IBA_SPOLL_BIT = 0x16,   /* board only */
	IBA_SEND_LLO = 0x17,    /* board only */
	IBA_SPOLL_TIME = 0x18,  /* device only */
	IBA_PPOLL_TIME = 0x19,  /* board only */
	IBA_END_BIT_IS_NORMAL = 0x1a,
	IBA_UN_ADDR = 0x1b,     /* device only */
	IBA_HS_CABLE_LENGTH = 0x1f,     /* board only */
	IBA_IST = 0x20, /* board only */
	IBA_RSV = 0x21, /* board only */
	IBA_BNA = 0x200,        /* device only */
	/* linux-gpib extensions */
	IBA_7_BIT_EOS = 0x1000  /* board only. Returns 1 if board supports 7 bit eos compares*/
};

enum ibconfig_option {
	IBC_PAD = 0x1,
	IBC_SAD = 0x2,
	IBC_TMO = 0x3,
	IBC_EOT = 0x4,
	IBC_PPC = 0x5,  /* board only */
	IBC_READDR = 0x6,       /* device only */
	IBC_AUTOPOLL = 0x7,     /* board only */
	IBC_CICPROT = 0x8,      /* board only */
	IBC_IRQ = 0x9,  /* board only */
	IBC_SC = 0xa,   /* board only */
	IBC_SRE = 0xb,  /* board only */
	IBC_EOS_RD = 0xc,
	IBC_EOS_WRT = 0xd,
	IBC_EOS_CMP = 0xe,
	IBC_EOS_CHAR = 0xf,
	IBC_PP2 = 0x10, /* board only */
	IBC_TIMING = 0x11,      /* board only */
	IBC_DMA = 0x12, /* board only */
	IBC_READ_ADJUST = 0x13,
	IBC_WRITE_ADJUST = 0x14,
	IBC_EVENT_QUEUE = 0x15, /* board only */
	IBC_SPOLL_BIT = 0x16,   /* board only */
	IBC_SEND_LLO = 0x17,    /* board only */
	IBC_SPOLL_TIME = 0x18,  /* device only */
	IBC_PPOLL_TIME = 0x19,  /* board only */
	IBC_END_BIT_IS_NORMAL = 0x1a,
	IBC_UN_ADDR = 0x1b,     /* device only */
	IBC_HS_CABLE_LENGTH = 0x1f,     /* board only */
	IBC_IST = 0x20, /* board only */
	IBC_RSV = 0x21, /* board only */
	IBC_BNA = 0x200 /* device only */
};

enum t1_delays {
	T1_DELAY_2000ns = 1,
	T1_DELAY_500ns = 2,
	T1_DELAY_350ns = 3
};

enum gpib_stb {
	IB_STB_RQS = 0x40, /* IEEE 488.1 & 2  */
	IB_STB_ESB = 0x20, /* IEEE 488.2 only */
	IB_STB_MAV = 0x10        /* IEEE 488.2 only */
};

/*  Compatibility defines due to removal of CamelCase identifiers in kernel code */

/* GPIB Bus Control Lines bit vector */
#define ValidDAV  VALID_DAV
#define ValidNDAC VALID_NDAC
#define ValidNRFD VALID_NRFD
#define ValidIFC  VALID_IFC
#define ValidREN  VALID_REN
#define ValidSRQ  VALID_SRQ
#define ValidATN  VALID_ATN
#define ValidEOI  VALID_EOI
#define ValidALL  VALID_ALL
#define BusDAV    BUS_DAV
#define BusNDAC   BUS_NDAC
#define BusNRFD   BUS_NRFD
#define BusIFC    BUS_IFC
#define BusREN    BUS_REN
#define BusSRQ    BUS_SRQ
#define BusATN    BUS_ATN
#define BusEOI    BUS_EOI

#define PPConfig PP_CONFIG

/* ibask options */
#define	IbaPAD            IBA_PAD
#define	IbaSAD		  IBA_SAD
#define	IbaTMO		  IBA_TMO
#define	IbaEOT		  IBA_EOT
#define	IbaPPC		  IBA_PPC
#define	IbaREADDR	  IBA_READ_DR
#define	IbaAUTOPOLL	  IBA_AUTOPOLL
#define	IbaCICPROT	  IBA_CICPROT
#define	IbaIRQ		  IBA_IRQ
#define	IbaSC		  IBA_SC
#define	IbaSRE		  IBA_SRE
#define	IbaEOSrd	  IBA_EOS_RD
#define	IbaEOSwrt	  IBA_EOS_WRT
#define	IbaEOScmp	  IBA_EOS_CMP
#define	IbaEOSchar	  IBA_EOS_CHAR
#define	IbaPP2		  IBA_PP2
#define	IbaTIMING	  IBA_TIMING
#define	IbaDMA		  IBA_DMA
#define	IbaReadAdjust	  IBA_READ_ADJUST
#define	IbaWriteAdjust	  IBA_WRITE_ADJUST
#define	IbaEventQueue	  IBA_EVENT_QUEUE
#define	IbaSPollBit	  IBA_SPOLL_BIT
#define	IbaSpollBit	  IBA_SPOLL_BIT
#define	IbaSendLLO	  IBA_SEND_LLO
#define	IbaSPollTime	  IBA_SPOLL_TIME
#define	IbaPPollTime	  IBA_PPOLL_TIME
#define	IbaEndBitIsNormal IBA_END_BIT_IS_NORMAL
#define	IbaUnAddr	  IBA_UN_ADDR
#define	IbaHSCableLength  IBA_HS_CABLE_LENGTH
#define	IbaIst		  IBA_IST
#define	IbaRsv		  IBA_RSV
#define	IbaBNA		  IBA_BNA
#define Iba7BitEOS        IBA_7_BIT_EOS
/* ibconfig options */
#define	IbcPAD            IBC_PAD
#define	IbcSAD		  IBC_SAD
#define	IbcTMO		  IBC_TMO
#define	IbcEOT		  IBC_EOT
#define	IbcPPC		  IBC_PPC
#define	IbcREADDR	  IBC_READDR
#define	IbcAUTOPOLL	  IBC_AUTOPOLL
#define	IbcCICPROT	  IBC_CICPROT
#define	IbcIRQ		  IBC_IRQ
#define	IbcSC		  IBC_SC
#define	IbcSRE		  IBC_SRE
#define	IbcEOSrd	  IBC_EOS_RD
#define	IbcEOSwrt	  IBC_EOS_WRT
#define	IbcEOScmp	  IBC_EOS_CMP
#define	IbcEOSchar	  IBC_EOS_CHAR
#define	IbcPP2		  IBC_PP2
#define	IbcTIMING	  IBC_TIMING
#define	IbcDMA		  IBC_DMA
#define	IbcReadAdjust	  IBC_READ_ADJUST
#define	IbcWriteAdjust	  IBC_WRITE_ADJUST
#define	IbcEventQueue	  IBC_EVENT_QUEUE
#define	IbcSPollBit	  IBC_SPOLL_BIT
#define	IbcSpollBit	  IBC_SPOLL_BIT
#define	IbcSendLLO	  IBC_SEND_LLO
#define	IbcSPollTime	  IBC_SPOLL_TIME
#define	IbcPPollTime	  IBC_PPOLL_TIME
#define	IbcEndBitIsNormal IBC_END_BIT_IS_NORMAL
#define	IbcUnAddr	  IBC_UN_ADDR
#define	IbcHSCableLength  IBC_HS_CABLE_LENGTH
#define	IbcIst		  IBC_IST
#define	IbcRsv		  IBC_RSV
#define	IbcBNA		  IBC_BNA

/* gpib events */
#define	EventNone   EVENT_NONE
#define	EventDevTrg EVENT_DEV_TRG
#define	EventDevClr EVENT_DEV_CLR
#define	EventIFC    EVENT_IFC

/* standard status byte bits */
#define	IbStbRQS IB_STB_RQS
#define	IbStbESB IB_STB_ESB
#define	IbStbMAV IB_STB_MAV


#endif	/* _GPIB_USER_H */
