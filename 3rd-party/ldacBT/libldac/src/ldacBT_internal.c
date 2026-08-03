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

#include "ldacBT_internal.h"



enum {
    /* 2-DH5 */
                     LDACBT_2DH5_02,  LDACBT_2DH5_03,  LDACBT_2DH5_04,  LDACBT_2DH5_05,
    LDACBT_2DH5_06,  LDACBT_2DH5_07,  LDACBT_2DH5_08,  LDACBT_2DH5_09,  LDACBT_2DH5_10,
    LDACBT_2DH5_11,  LDACBT_2DH5_12,  LDACBT_2DH5_13,  LDACBT_2DH5_14,
};

#define LDACBT_NO_DEF_ -1
DECLFUNC const LDACBT_EQMID_PROPERTY tbl_ldacbt_eqmid_property[] = {
    /* kbps,    ID               , label, ID for 2DH5     */
    /* 990 */ { LDACBT_EQMID_HQ, "HQ" , LDACBT_2DH5_02 },
    /* 660 */ { LDACBT_EQMID_SQ, "SQ" , LDACBT_2DH5_03 },
    /* 492 */ { LDACBT_EQMID_Q0, "Q0" , LDACBT_2DH5_04 },
    /* 396 */ { LDACBT_EQMID_Q1, "Q1" , LDACBT_2DH5_05 },
    /* 330 */ { LDACBT_EQMID_MQ, "MQ" , LDACBT_2DH5_06 },
    /* 282 */ { LDACBT_EQMID_Q2, "Q2" , LDACBT_2DH5_07 },
    /* 246 */ { LDACBT_EQMID_Q3, "Q3" , LDACBT_2DH5_08 },
    /* 216 */ { LDACBT_EQMID_Q4, "Q4" , LDACBT_2DH5_09 },
    /* 198 */ { LDACBT_EQMID_Q5, "Q5" , LDACBT_2DH5_10 },
    /* 180 */ { LDACBT_EQMID_Q6, "Q6" , LDACBT_2DH5_11 },
    /* 162 */ { LDACBT_EQMID_Q7, "Q7" , LDACBT_2DH5_12 },
    /* 150 */ { LDACBT_EQMID_Q8, "Q8" , LDACBT_2DH5_13 },
    /* 138 */ { LDACBT_EQMID_END, "Q9" , LDACBT_2DH5_14 },
};

/* LDAC config table
 *  - NFRM/PCKT must be less than 16.
 */
DECLFUNC const LDACBT_CONFIG tbl_ldacbt_config[] = {
/*
 *   index          , NFRM , LDAC  , FRM   
 *                  , ---- ,  FRM  ,  LEN  
 *                  , PCKT ,   LEN ,    /CH
 *                  ,      , [byte], [byte]
 */
    { LDACBT_2DH5_02,     2,    330,   165},
    { LDACBT_2DH5_03,     3,    220,   110},
    { LDACBT_2DH5_04,     4,    164,    82},
    { LDACBT_2DH5_05,     5,    132,    66},
    { LDACBT_2DH5_06,     6,    110,    55},
    { LDACBT_2DH5_07,     7,     94,    47},
    { LDACBT_2DH5_08,     8,     82,    41},
    { LDACBT_2DH5_09,     9,     72,    36},
    { LDACBT_2DH5_10,    10,     66,    33},
    { LDACBT_2DH5_11,    11,     60,    30},
    { LDACBT_2DH5_12,    12,     54,    27},
    { LDACBT_2DH5_13,    13,     50,    25},
    { LDACBT_2DH5_14,    14,     46,    23},
};


/* Clear LDAC handle parameters */
DECLFUNC void ldacBT_param_clear(HANDLE_LDAC_BT hLdacBT)
{
    int ich;
    if( hLdacBT == NULL ) { return ; }
    hLdacBT->proc_mode = LDACBT_PROCMODE_UNSET;
    hLdacBT->error_code = LDACBT_ERR_NONE;
    hLdacBT->error_code_api = LDACBT_ERR_NONE;

    hLdacBT->frm_samples = 0;
    hLdacBT->sfid = UNSET;
    hLdacBT->pcm.sf = UNSET;
    hLdacBT->tx.mtu = UNSET;
    hLdacBT->tx.tx_size = UNSET;
    hLdacBT->tx.pkt_hdr_sz = UNSET;
    hLdacBT->frmlen_tx = UNSET;
    hLdacBT->tx.nfrm_in_pkt = UNSET;
    hLdacBT->pcm.ch = 0;
    hLdacBT->pcm.fmt = LDACBT_SMPL_FMT_S24;
    hLdacBT->nshift = 0;
    hLdacBT->frmlen = UNSET;
    hLdacBT->frm_status = 0;
    hLdacBT->bitrate = 0;
    /* for alter frame length */
    hLdacBT->tgt_nfrm_in_pkt = UNSET;
    hLdacBT->tgt_frmlen = UNSET;
    hLdacBT->tgt_eqmid = UNSET;
    hLdacBT->stat_alter_op = LDACBT_ALTER_OP__NON;

    hLdacBT->cm = UNSET;
    hLdacBT->cci = UNSET;
    hLdacBT->eqmid = UNSET;
    hLdacBT->transport = UNSET;

    clear_data_ldac( hLdacBT->ldac_trns_frm_buf.buf, sizeof(hLdacBT->ldac_trns_frm_buf.buf));
    hLdacBT->ldac_trns_frm_buf.used = 0;
    hLdacBT->ldac_trns_frm_buf.nfrm_in = 0;

    clear_data_ldac( hLdacBT->pcmring.buf, sizeof(hLdacBT->pcmring.buf));
    hLdacBT->pcmring.wp = 0;
    hLdacBT->pcmring.rp = 0;
    hLdacBT->pcmring.nsmpl = 0;
/* work buffer for I/O */
    for( ich = 0; ich < LDAC_PRCNCH; ich++ ){
        hLdacBT->ap_pcm[ich] = &hLdacBT->a_pcm[ ich * LDACBT_MAX_LSU * LDACBT_PCM_WLEN_MAX ];
    }
    hLdacBT->pp_pcm = hLdacBT->ap_pcm;
    clear_data_ldac( hLdacBT->a_pcm, LDAC_PRCNCH * LDACBT_MAX_LSU * LDACBT_PCM_WLEN_MAX );

}

/* get ldaclib error code */
DECLFUNC int ldacBT_check_ldaclib_error_code(HANDLE_LDAC_BT hLdacBT)
{
    HANDLE_LDAC hData;
    int error_code, internal_error_code;

    if( hLdacBT == NULL ){ return LDACBT_E_FAIL; }
    if( (hData = hLdacBT->hLDAC) == NULL ){ return LDACBT_E_FAIL; }

    ldaclib_get_error_code(hData, &error_code);

    ldaclib_get_internal_error_code(hData, &internal_error_code);

    hLdacBT->error_code = error_code << 10 | internal_error_code;

    return LDACBT_S_OK;
}

/* Assertions. */
DECLFUNC int ldacBT_assert_cm( int cm )
{
    if( (cm != LDACBT_CHANNEL_MODE_STEREO )
        && (cm != LDACBT_CHANNEL_MODE_DUAL_CHANNEL )
        && (cm != LDACBT_CHANNEL_MODE_MONO )
    ){
        return LDACBT_ERR_ASSERT_CHANNEL_MODE;
    }
    return LDACBT_ERR_NONE;
}
UNUSED_ATTR DECLFUNC int ldacBT_assert_cci( int cci )
{
    if( (cci != LDAC_CCI_STEREO )
        && (cci != LDAC_CCI_DUAL_CHANNEL )
        && (cci != LDAC_CCI_MONO )
    ){
        return LDACBT_ERR_ASSERT_CHANNEL_CONFIG;
    }
    return LDACBT_ERR_NONE;
}
DECLFUNC int ldacBT_assert_sample_format( LDACBT_SMPL_FMT_T fmt )
{
#ifndef _32BIT_FIXED_POINT
    if( (fmt != LDACBT_SMPL_FMT_S16)
        && (fmt != LDACBT_SMPL_FMT_S24)
        && (fmt != LDACBT_SMPL_FMT_S32)
        && (fmt != LDACBT_SMPL_FMT_F32)
    ){
#else /* _32BIT_FIXED_POINT */
    if( (fmt != LDACBT_SMPL_FMT_S16)
        && (fmt != LDACBT_SMPL_FMT_S24)
        && (fmt != LDACBT_SMPL_FMT_S32)
    ){
#endif /* _32BIT_FIXED_POINT */
        return LDACBT_ERR_ILL_SMPL_FORMAT;
    }
    return LDACBT_ERR_NONE;
}
DECLFUNC int ldacBT_assert_pcm_sampling_freq( int sampling_freq )
{
    if( (sampling_freq != 1*44100) && (sampling_freq != 1*48000)
        && (sampling_freq != 2*44100) && (sampling_freq != 2*48000)
    ){
        return LDACBT_ERR_ILL_SAMPLING_FREQ;
    }
    return LDACBT_ERR_NONE;
}
/* usbpods: LDACBT_MTU_REQUIRED (679) assumes the link offers a 679-byte
   A2DP packet. Plenty of sinks negotiate the DEFAULT 672-byte L2CAP MTU, and
   there libldac's own packing (tx_size 661) + our 1-byte frame-count header +
   the 12-byte RTP header comes to 674 — so BTstack refused every send with
   MEMORY_CAPACITY_EXCEEDED and LDAC was silently, totally dead. The caller
   now passes the REAL usable MTU so the existing tx_size clamp packs fewer
   frames per packet instead; that only needs this floor lowered. Sony's
   constant is a recommendation about 2-DH5 efficiency, not a codec limit. */
DECLFUNC int ldacBT_assert_mtu( int mtu )
{
    if( mtu < LDACBT_MTU_MIN_USBPODS ){
        return LDACBT_ERR_ILL_MTU_SIZE;
    }
    return LDACBT_ERR_NONE;
}
DECLFUNC int ldacBT_assert_eqmid( int eqmid )
{
    if( (eqmid == LDACBT_EQMID_HQ) || (eqmid == LDACBT_EQMID_SQ) || (eqmid == LDACBT_EQMID_MQ)){
        return LDACBT_ERR_NONE;
    }

    return LDACBT_ERR_ILL_EQMID;
}

/* LDAC set Encode Quality Mode index core */
DECLFUNC void ldacBT_set_eqmid_core( HANDLE_LDAC_BT hLdacBT, int eqmid )
{
    /* "eqmid" must be checked before calling this function. */
    /* just update tgt_eqmid */
    P_LDACBT_CONFIG pCfg;
    pCfg = ldacBT_get_config( eqmid, hLdacBT->tx.pkt_type );
    hLdacBT->tgt_eqmid = eqmid;
    hLdacBT->tgt_frmlen = hLdacBT->pcm.ch * pCfg->frmlen_1ch;
    hLdacBT->tgt_frmlen -= LDACBT_FRMHDRBYTES;
    hLdacBT->tgt_nfrm_in_pkt = pCfg->nfrm_in_pkt;

}

/* Split LR interleaved PCM into buffer that for LDAC encode. */
DECLFUNC void ldacBT_prepare_pcm_encode( void *pbuff, char **ap_pcm, int nsmpl, int nch,
                     LDACBT_SMPL_FMT_T fmt )
{
    int i;
    if( nch == 2 ){
        if( fmt == LDACBT_SMPL_FMT_S16 ){
            short *p_pcm_16 = (short *)pbuff;
            short *p_lch_16 = (short *)ap_pcm[0];
            short *p_rch_16 = (short *)ap_pcm[1];
            for (i = 0; i < nsmpl; i++) {
                *p_lch_16++ = p_pcm_16[0];
                *p_rch_16++ = p_pcm_16[1];
                p_pcm_16+=2;
            }
        }
        else if( fmt == LDACBT_SMPL_FMT_S24 ){
            char *p_pcm_8 = (char *)pbuff;
            char *p_lch_8 = (char *)ap_pcm[0];
            char *p_rch_8 = (char *)ap_pcm[1];
#if __BYTE_ORDER == __LITTLE_ENDIAN
            for (i = 0; i < nsmpl; i++) {
                *p_lch_8++ = p_pcm_8[0];
                *p_lch_8++ = p_pcm_8[1];
                *p_lch_8++ = p_pcm_8[2];
                p_pcm_8+=3;
                *p_rch_8++ = p_pcm_8[0];
                *p_rch_8++ = p_pcm_8[1];
                *p_rch_8++ = p_pcm_8[2];
                p_pcm_8+=3;
            }
#else   /* __BYTE_ORDER */
#error unsupported byte order
#endif  /* #if __BYTE_ORDER == __LITTLE_ENDIAN */
        }
        else if ( fmt == LDACBT_SMPL_FMT_S32 ){
            char *p_pcm_8 = (char *)pbuff;
            char *p_lch_8 = (char *)ap_pcm[0];
            char *p_rch_8 = (char *)ap_pcm[1];
#if __BYTE_ORDER == __LITTLE_ENDIAN
            for (i = 0; i < nsmpl; i++) {
                *p_lch_8++ = p_pcm_8[0]; *p_lch_8++ = p_pcm_8[1]; *p_lch_8++ = p_pcm_8[2]; *p_lch_8++ = p_pcm_8[3];
                p_pcm_8+=4;
                *p_rch_8++ = p_pcm_8[0]; *p_rch_8++ = p_pcm_8[1]; *p_rch_8++ = p_pcm_8[2]; *p_rch_8++ = p_pcm_8[3];
                p_pcm_8+=4;
            }
#else   /* __BYTE_ORDER */
#error unsupported byte order
#endif  /* #if __BYTE_ORDER == __LITTLE_ENDIAN */
        }
        else if ( fmt == LDACBT_SMPL_FMT_F32 ){
            float *p_pcm = (float *)pbuff;
            float *p_lch = (float *)ap_pcm[0];
            float *p_rch = (float *)ap_pcm[1];
            for (i = 0; i < nsmpl; i++) {
                *p_lch++ = p_pcm[0];
                p_pcm++;
                *p_rch++ = p_pcm[0];
                p_pcm++;
            }
        }
        else{;} /* never be happend */
    }
    else if( nch == 1 ){
        switch(fmt){
          case LDACBT_SMPL_FMT_S16:
            copy_data_ldac( pbuff, ap_pcm[0], 2*nsmpl );
            break;
          case LDACBT_SMPL_FMT_S24:
            copy_data_ldac( pbuff, ap_pcm[0], 3*nsmpl );
            break;
          case LDACBT_SMPL_FMT_S32:
          case LDACBT_SMPL_FMT_F32:
            copy_data_ldac( pbuff, ap_pcm[0], 4*nsmpl );
            break;
          default:
            break;
        }
    }
}


/* update framelength */
DECLFUNC int ldacBT_update_frmlen(HANDLE_LDAC_BT hLdacBT, int frmlen)
{
    int status, sf, ch, fl, fl_per_ch;
    int nbasebands, grad_mode, grad_qu_l, grad_qu_h, grad_ofst_l, grad_ofst_h, abc_flag;
    LDACBT_TX_INFO *ptx;
    LDAC_RESULT result;
    status = LDACBT_E_FAIL;

    if( hLdacBT == NULL ){
        return LDACBT_E_FAIL;
    }
    sf = hLdacBT->pcm.sf; /* sampling frequency */
    ch = hLdacBT->pcm.ch; /* number of channels */
    ptx = &hLdacBT->tx;

ldac_setup_AGAIN:
    /* update LDAC parameters. */
    
    
    if( frmlen == UNSET ){
        goto ldac_setup_END;
    }

    /* check & update frameLength */
    ldaclib_get_encode_frame_length( hLdacBT->hLDAC, &fl );
    if( fl == 0 ){ // This meens that the handle was not initialized yet. Shall not happen.
        
        goto ldac_setup_END;
    }
    else if( frmlen == fl ){
        /* No need to update frame length. Just update bitrate information. */
        status = LDACBT_S_OK;
        hLdacBT->bitrate = ldacBT_frmlen_to_bitrate( fl, 1, sf, hLdacBT->frm_samples );
        goto ldac_setup_END;
    }

    /* Time to update the frame_length. */
    /* Get ldac encoding information for frame_length. */
    fl_per_ch = (frmlen+LDACBT_FRMHDRBYTES) / ch;
    result = ldaclib_get_encode_setting( fl_per_ch, hLdacBT->sfid, &nbasebands,
                     &grad_mode, &grad_qu_l, &grad_qu_h, &grad_ofst_l, &grad_ofst_h, &abc_flag);
    if (LDAC_FAILED(result)) {
        goto ldac_setup_END;
    }
    /* Set Encoding Information */
    result = ldaclib_set_encode_info( hLdacBT->hLDAC, nbasebands, grad_mode,
                               grad_qu_l, grad_qu_h, grad_ofst_l, grad_ofst_h, abc_flag);
    if (LDAC_FAILED(result)) {
        ldacBT_check_ldaclib_error_code(hLdacBT);
        goto ldac_setup_END;
    }

    if( !LDAC_SUCCEEDED(ldaclib_set_encode_frame_length( hLdacBT->hLDAC, frmlen ))){
        goto ldac_setup_END;
    }

    /* Update parameters in handle. */
    hLdacBT->frmlen = frmlen;
    hLdacBT->frmlen_tx = LDACBT_FRMHDRBYTES + frmlen;
    ptx->nfrm_in_pkt = ptx->tx_size / hLdacBT->frmlen_tx;
    if( ptx->nfrm_in_pkt > LDACBT_NFRM_TX_MAX ){
        ptx->nfrm_in_pkt = LDACBT_NFRM_TX_MAX;
    }
    else if( ptx->nfrm_in_pkt < 2 ){
        /* Not allowed 1frame/packet transportation for current version of LDAC A2DP */
        if( frmlen <= (ptx->tx_size / 2 - LDACBT_FRMHDRBYTES)){
            goto ldac_setup_END;
        }
        frmlen = ptx->tx_size / 2 - LDACBT_FRMHDRBYTES;
        goto ldac_setup_AGAIN;
    }
    /* Update bitrate and EQMID. */
    hLdacBT->bitrate = ldacBT_frmlen_to_bitrate( frmlen, 1, sf, hLdacBT->frm_samples );
    hLdacBT->eqmid = ldacBT_get_eqmid_from_frmlen( frmlen, ch, hLdacBT->transport, ptx->pkt_type );
    if( hLdacBT->tgt_eqmid == UNSET){
        hLdacBT->eqmid = UNSET;
    }
    status = LDACBT_S_OK;

ldac_setup_END:
    return status;
}

/* Get channel_config_index from channel_mode.
 * The argument cm, channel_mode, must be checked by function ldacBT_assert_cm() before calling this
 * function.
 */
DECLFUNC int  ldacBT_cm_to_cci( int cm )
{
    if( cm == LDACBT_CHANNEL_MODE_STEREO ){
        return LDAC_CCI_STEREO;
    }
    else if( cm == LDACBT_CHANNEL_MODE_DUAL_CHANNEL ){
        return LDAC_CCI_DUAL_CHANNEL;
    }
    else/* if( cm == LDACBT_CHANNEL_MODE_MONO )*/{
        return LDAC_CCI_MONO;
    }
}

/* Get channel_mode from channel_config_index.
 * The argument cci, channel_config_index, must be checked by the function ldacBT_assert_cci() before
 * calling this function.
 */
UNUSED_ATTR DECLFUNC int  ldacBT_cci_to_cm( int cci )
{
    if( cci == LDAC_CCI_STEREO ){
        return LDACBT_CHANNEL_MODE_STEREO;
    }
    else if( cci == LDAC_CCI_DUAL_CHANNEL ){
        return LDACBT_CHANNEL_MODE_DUAL_CHANNEL;
    }
    else/* if( cci == LDAC_CCI_MONO )*/{
        return LDACBT_CHANNEL_MODE_MONO;
    }
}

/* Get bitrate from frame length */
DECLFUNC int ldacBT_frmlen_to_bitrate( int frmlen, int flgFrmHdr, int sf, int frame_samples )
{
    int bitrate;
    if( (frmlen == UNSET) || (flgFrmHdr == UNSET) || (sf == UNSET) || (frame_samples == UNSET) ){
        return LDACBT_E_FAIL;
    }
    if( flgFrmHdr ){
        frmlen += LDACBT_FRMHDRBYTES;
    }
    bitrate  = frmlen * sf / frame_samples / (1000 / 8);
    return bitrate;
}

/* Get Encode Quality Mode index property */
DECLFUNC P_LDACBT_EQMID_PROPERTY ldacBT_get_eqmid_conv_tbl ( int eqmid )
{
    int i, tbl_size;
    P_LDACBT_EQMID_PROPERTY pEqmIdProp;

    pEqmIdProp = (P_LDACBT_EQMID_PROPERTY)tbl_ldacbt_eqmid_property;
    tbl_size = (int)(sizeof(tbl_ldacbt_eqmid_property)/sizeof(tbl_ldacbt_eqmid_property[0]));
    /* Search current eqmid */
    for( i = 0; i < tbl_size; ++i, ++pEqmIdProp ){
        if( pEqmIdProp->eqmid == eqmid ){
            return pEqmIdProp;
        }
    }
    return NULL;
}

/* Get altered Encode Quality Mode index */
DECLFUNC int ldacBT_get_altered_eqmid ( HANDLE_LDAC_BT hLdacBT, int priority )
{
    int i, eqmid_0, eqmid_1, eqmid_new, tbl_size;
    if( priority == 0 ){ return LDACBT_E_FAIL; }
    switch( hLdacBT->tx.pkt_type ){
      case _2_DH5:
        break;
      default:
        return LDACBT_E_FAIL;
    }
    tbl_size = (int)(sizeof(tbl_ldacbt_eqmid_property)/sizeof(tbl_ldacbt_eqmid_property[0]));
    /* Search target eqmid */
    for( i = 0; i < tbl_size; ++i ){
        if( tbl_ldacbt_eqmid_property[i].eqmid == hLdacBT->tgt_eqmid ){
            break;
        }
    }
    eqmid_0 = i;
    eqmid_1 = eqmid_0 - priority;
    if( eqmid_1 < 0 ){ return LDACBT_E_FAIL; }
    if( eqmid_1 >= tbl_size ){ return LDACBT_E_FAIL; }

    eqmid_new = tbl_ldacbt_eqmid_property[eqmid_1].eqmid;
    for( i = 0; i < tbl_size; ++i ){
        if( tbl_ldacbt_eqmid_property[i].eqmid == LDACBT_LIMIT_ALTER_EQMID_PRIORITY ){
            break;
        }
    }
    if( eqmid_1 > i ){ return LDACBT_E_FAIL; }
    return eqmid_new;

}

/* Get LDAC bitrate info from Encode Quality Mode Index */
DECLFUNC P_LDACBT_CONFIG ldacBT_get_config( int ldac_bt_mode, int pkt_type )
{
    int i, tbl_size, ldac_mode_id;
    P_LDACBT_EQMID_PROPERTY pEqmIdProp;
    P_LDACBT_CONFIG pCfg;

    if( (pEqmIdProp = ldacBT_get_eqmid_conv_tbl( ldac_bt_mode )) == NULL ){
        return NULL;
    }

    if( pkt_type == _2_DH5 ){ ldac_mode_id = pEqmIdProp->id_for_2DH5;}
    else{
        return NULL;
    }

    pCfg = (P_LDACBT_CONFIG)tbl_ldacbt_config;
    tbl_size = (int)(sizeof(tbl_ldacbt_config)/sizeof(tbl_ldacbt_config[0]));
    for( i = 0; i < tbl_size; ++i, ++pCfg ){
        if( ldac_mode_id == pCfg->id ){
            return pCfg;
        }
    }
    return NULL; /* not found */
}

/* Get Encode Quality Mode Index from framelength */
DECLFUNC int ldacBT_get_eqmid_from_frmlen( int frmlen, int nch, int flgFrmHdr, int pktType )
{
    int i, n, eqmid;
    P_LDACBT_CONFIG pCfg;

    if( flgFrmHdr ){
        frmlen += LDACBT_FRMHDRBYTES;
    }
    if( nch > 0 ){
        frmlen /= nch;
    }

    eqmid = LDACBT_EQMID_END;
    n = (int)(sizeof(tbl_ldacbt_eqmid_property)/sizeof(tbl_ldacbt_eqmid_property[0]));
    for( i = 0; i < n; ++i ){
        if( (pCfg = ldacBT_get_config( tbl_ldacbt_eqmid_property[i].eqmid, pktType )) != NULL ){
            if( frmlen >= pCfg->frmlen_1ch){
                eqmid = tbl_ldacbt_eqmid_property[i].eqmid;
                break;
            }
        }
    }
    return eqmid;
}

