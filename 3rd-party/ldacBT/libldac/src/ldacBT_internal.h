/*
 * Copyright (C) 2013 - 2016 Sony Corporation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef _LDACBT_INTERNAL_H_
#define _LDACBT_INTERNAL_H_

#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include "struct_ldac.h"

#ifdef    __cplusplus
extern "C" {
#endif

/* Function declaration */
#define DECLFUNC static

/* Limit for alter EQMID process */
#define LDACBT_LIMIT_ALTER_EQMID_PRIORITY LDACBT_EQMID_MQ


#include "ldaclib.h"
#include "ldacBT.h"
#include "ldacBT_ex.h"

/* macro value */
/* The size of LDAC transport header. Unit:Byte. */
#define LDACBT_FRMHDRBYTES LDAC_FRMHDRBYTES
/* The Maximum number of frames that can transrate in one packet.(LDAC A2DP spec) */
#define LDACBT_NFRM_TX_MAX 15
/* Lowest Common Multiple of (2,3,4)Bytes * 2ch * 256samples */
#define LDACBT_ENC_PCM_BUF_SZ 6144 
/* The maximum pcm word length allowed. Unit:Byte */
#define LDACBT_PCM_WLEN_MAX 4
/* The size of LDACBT_TRANSPORT_FRM_BUF's buffer. Unit:Byte  */
#define LDACBT_ENC_STREAM_BUF_SZ 1024
/* The size of rtp header and so on. Unit:Byte */
/*  = sizeof(struct rtp_header) + sizeof(struct rtp_payload) + 1 (scms-t). */
#define LDACBT_TX_HEADER_SIZE 18
/* The MTU size required for LDAC A2DP streaming. */
#define LDACBT_MTU_REQUIRED  679
/* usbpods: floor actually enforced — see ldacBT_assert_mtu(). 679 rejects the
   very common 672-byte-MTU link, on which LDAC must simply pack fewer frames
   per packet. 200 still leaves room for a whole frame at every EQMID. */
#define LDACBT_MTU_MIN_USBPODS 200
#define LDACBT_MTU_3DH5 (990+LDACBT_TX_HEADER_SIZE)

/* The state for alter operation */
#define LDACBT_ALTER_OP__NON 0
#define LDACBT_ALTER_OP__ACTIVE 1
#define LDACBT_ALTER_OP__STANDBY 2
#define LDACBT_ALTER_OP__FLASH 9

/* other */
#ifndef LDACBT_S_OK
#define LDACBT_S_OK (0)
#endif
#ifndef LDACBT_E_FAIL
#define LDACBT_E_FAIL (-1)
#endif
#ifndef FALSE
#define FALSE  0
#endif
#ifndef TRUE
#define TRUE   1
#endif
#ifndef UNSET
#define UNSET -1
#endif
#define LDACBT_GET_LDACLIB_ERROR_CODE   9999

/* The index for A2DP packets */
enum {
    ___DH1,    ___DH3,    ___DH5, /* basic rate */
    _2_DH1,    _2_DH3,    _2_DH5, /* EDR2M */
    _3_DH1,    _3_DH3,    _3_DH5, /* EDR3M */
};

/* The state for LDACBT handle processing mode. */
typedef enum {
    LDACBT_PROCMODE_UNSET = -1,
    LDACBT_PROCMODE_ENCODE = 1,
    LDACBT_PROCMODE_DECODE = 2,
} LDACBT_PROCMODE;

/* Structs */
/* The structure for the property of EQMID. */
typedef struct _st_ldacbt_eqmid_property
{
    int  eqmid;
    char strModeName[4];
    int  id_for_2DH5;
} LDACBT_EQMID_PROPERTY, * P_LDACBT_EQMID_PROPERTY;

/* The structure for the configuration of LDAC. */
typedef struct _st_ldacbt_config 
{
    int id;
    int nfrm_in_pkt; /* number of ldac frame in packet */
    int frmlen;      /* ldac frame length */
    int frmlen_1ch;  /* ldac frame length per channel */
} LDACBT_CONFIG, * P_LDACBT_CONFIG;

/* The structure for the pcm information. */
typedef struct _ldacbt_pcm_info {
    int sf; /* sampling frequency */
    int ch; /* number of channel */
    int wl;
    LDACBT_SMPL_FMT_T fmt; /* sample format */
} LDACBT_PCM_INFO;

/* The structure for the A2DP streaming. */
typedef struct _ldacbt_tx_info {
    int mtu;
    int tx_size;     /* size for ldac stream */
    int pkt_type;    /* packet type */
    int pkt_hdr_sz;  /* packet header size */
    int nfrm_in_pkt; /* number of ldac frame in packet */
} LDACBT_TX_INFO;
/* The structure for the ldac_transport_frame sequence. */
typedef struct _ldacbt_transport_frame_buf {
    unsigned char buf[LDACBT_ENC_STREAM_BUF_SZ];
    int used;
    int nfrm_in;
} LDACBT_TRANSPORT_FRM_BUF;
/* The structure of ring buffer for the input PCM. */
typedef struct _ldacbt_pcm_ring_buf {
    char buf[LDACBT_ENC_PCM_BUF_SZ]; 
    int wp;
    int rp;
    int nsmpl;
} LDACBT_PCM_RING_BUF;

/* The LDACBT handle. */
typedef struct _st_ldacbt_handle {
    HANDLE_LDAC hLDAC;
    LDACBT_PROCMODE proc_mode;
    int error_code;
    int error_code_api;
/* common */
    /* pcm */
    LDACBT_PCM_INFO    pcm;
    /* tx */
    LDACBT_TX_INFO     tx;
    /* ldaclib config */
    int frm_samples;  /* frame samples */
    int sfid;         /* sampling frequency index */
    int nshift;
    int flg_encode_flushed;
    int frm_status;
    int frmlen;    /* Frame Length */
    int frmlen_tx; /* Frame Length with transport header */
    int bitrate;

    int eqmid;     /* Encode Quality Mode Index */
    /* for alter frame length */
    int tgt_eqmid;      /* target Encode Quality Mode Index */
    int tgt_nfrm_in_pkt;/* target number of frame in packet */
    int tgt_frmlen;     /* target frame length */
    int stat_alter_op;  /* status of alter operation */

    int cm; /* Channel Mode */
    int cci; /* Channel Config Index */
    int transport;   /* Transport Stream ( with frame header) */
    /* buffer for "ldac_transport_frame" sequence */
    LDACBT_TRANSPORT_FRM_BUF ldac_trns_frm_buf;
    /* buffer for input pcm */
    LDACBT_PCM_RING_BUF pcmring;

/* work buffer for LDACLIB I/O */
    char **pp_pcm;
    char *ap_pcm[LDAC_PRCNCH];
    char a_pcm[LDAC_PRCNCH * LDACBT_MAX_LSU * LDACBT_PCM_WLEN_MAX];
} STRUCT_LDACBT_HANDLE;



/* subfunctions */
DECLFUNC void ldacBT_param_clear(HANDLE_LDAC_BT hLdacBT);
DECLFUNC int  ldacBT_check_ldaclib_error_code(HANDLE_LDAC_BT hLdacBT);
DECLFUNC int  ldacBT_assert_cm( int cm );
DECLFUNC int  ldacBT_assert_cci( int cci );
DECLFUNC int  ldacBT_assert_sample_format( LDACBT_SMPL_FMT_T fmt );
DECLFUNC int  ldacBT_assert_pcm_sampling_freq( int sf );
DECLFUNC int  ldacBT_assert_mtu( int mtu );
DECLFUNC int  ldacBT_assert_eqmid( int eqmid );
DECLFUNC void ldacBT_set_eqmid_core( HANDLE_LDAC_BT hLdacBT, int eqmid );
DECLFUNC void ldacBT_prepare_pcm_encode( void *pbuff, char **ap_pcm, int nsmpl, int nch,
                     LDACBT_SMPL_FMT_T fmt );
DECLFUNC int  ldacBT_frmlen_to_bitrate( int frmlen, int flgFrmHdr, int sf, int frame_samples );
DECLFUNC int  ldacBT_cm_to_cci( int cm );
DECLFUNC int  ldacBT_cci_to_cm( int cci );
DECLFUNC int  ldacBT_get_altered_eqmid ( HANDLE_LDAC_BT hLdacBT, int priority );
DECLFUNC int  ldacBT_get_eqmid_from_frmlen( int frmlen, int nch, int flgFrmHdr, int pktType );
DECLFUNC int  ldacBT_update_frmlen(HANDLE_LDAC_BT hLdacBT, int frmlen);
DECLFUNC P_LDACBT_EQMID_PROPERTY ldacBT_get_eqmid_conv_tbl ( int ldac_bt_mode );
DECLFUNC P_LDACBT_CONFIG ldacBT_get_config( int ldac_bt_mode, int pkt_type );

#ifdef    __cplusplus
}
#endif
#endif /* _LDACBT_INTERNAL_H_ */
