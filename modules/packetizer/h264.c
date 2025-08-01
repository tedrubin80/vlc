/*****************************************************************************
 * h264.c: h264/avc video packetizer
 *****************************************************************************
 * Copyright (C) 2001, 2002, 2006 VLC authors and VideoLAN
 *
 * Authors: Laurent Aimar <fenrir@via.ecp.fr>
 *          Eric Petit <titer@videolan.org>
 *          Gildas Bazin <gbazin@videolan.org>
 *          Derk-Jan Hartman <hartman at videolan dot org>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

/*****************************************************************************
 * Preamble
 *****************************************************************************/

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <vlc_common.h>
#include <vlc_plugin.h>
#include <vlc_sout.h>
#include <vlc_codec.h>
#include <vlc_block.h>

#include <vlc_block_helper.h>
#include <vlc_bits.h>
#include "h264_nal.h"
#include "h264_slice.h"
#include "hxxx_nal.h"
#include "hxxx_sei.h"
#include "hxxx_common.h"
#include "packetizer_helper.h"
#include "startcode_helper.h"

#include <limits.h>

/*****************************************************************************
 * Module descriptor
 *****************************************************************************/
static int  Open ( vlc_object_t * );
static void Close( vlc_object_t * );

vlc_module_begin ()
    set_subcategory( SUBCAT_SOUT_PACKETIZER )
    set_description( N_("H.264 video packetizer") )
    set_capability( "video packetizer", 50 )
    set_callbacks( Open, Close )
vlc_module_end ()


/****************************************************************************
 * Local prototypes
 ****************************************************************************/

typedef struct
{
    /* */
    packetizer_t packetizer;

    /* */
    struct
    {
        block_t *p_head;
        block_t **pp_append;
    } frame, leading;

    /* a new sps/pps can be transmitted outside of iframes */
    bool    b_new_sps;
    bool    b_new_pps;

    struct
    {
        block_t *p_block;
        h264_sequence_parameter_set_t *p_sps;
    } sps[H264_SPS_ID_MAX + 1];
    struct
    {
        block_t *p_block;
        h264_picture_parameter_set_t *p_pps;
    } pps[H264_PPS_ID_MAX + 1];
    struct
    {
        block_t *p_block;
    } spsext[H264_SPSEXT_ID_MAX + 1];
    const h264_sequence_parameter_set_t *p_active_sps;
    const h264_picture_parameter_set_t *p_active_pps;

    /* avcC data */
    uint8_t i_avcC_length_size;

    /* From SEI for current frame */
    uint8_t i_pic_struct;
    uint8_t i_dpb_output_delay;
    unsigned i_recovery_frame_cnt;

    /* Current Slice Header */
    h264_slice_t *p_slice;

    /* */
    int i_next_block_flags;
    bool b_recovered;
    unsigned i_recoveryfnum;
    unsigned i_recoverystartfnum;

    /* POC */
    h264_poc_context_t pocctx;
    struct
    {
        vlc_tick_t pts;
        int num;
    } prevdatedpoc;

    vlc_tick_t i_frame_pts;
    vlc_tick_t i_frame_dts;

    date_t dts;

    /* */
    cc_storage_t *p_ccs;
} decoder_sys_t;

#define BLOCK_FLAG_PRIVATE_AUD (1 << BLOCK_FLAG_PRIVATE_SHIFT)
#define BLOCK_FLAG_PRIVATE_SEI (2 << BLOCK_FLAG_PRIVATE_SHIFT)
#define BLOCK_FLAG_DROP        (4 << BLOCK_FLAG_PRIVATE_SHIFT)

static block_t *Packetize( decoder_t *, block_t ** );
static block_t *PacketizeAVC1( decoder_t *, block_t ** );
static block_t *GetCc( decoder_t *p_dec, decoder_cc_desc_t * );
static void PacketizeFlush( decoder_t * );

static void PacketizeReset( void *p_private, bool b_broken );
static block_t *PacketizeParse( void *p_private, bool *pb_ts_used, block_t * );
static int PacketizeValidate( void *p_private, block_t * );
static block_t * PacketizeDrain( void *p_private );

static block_t *ParseNALBlock( decoder_t *, bool *pb_ts_used, block_t * );
static inline block_t *ParseNALBlockW( void *opaque, bool *pb_ts_used, block_t *p_frag )
{
    return ParseNALBlock( (decoder_t *) opaque, pb_ts_used, p_frag );
}

static block_t *OutputPicture( decoder_t *p_dec );
static void ReleaseXPS( decoder_sys_t *p_sys );
static bool PutXPS( decoder_t *p_dec, uint8_t i_nal_type, block_t *p_frag );
static h264_slice_t * ParseSliceHeader( decoder_t *p_dec, const block_t *p_frag );
static bool ParseSeiCallback( const hxxx_sei_data_t *, void * );



/*****************************************************************************
 * Helpers
 *****************************************************************************/
static void LastAppendXPSCopy( const block_t *p_block, block_t ***ppp_last )
{
    if( !p_block )
        return;
    block_t *p_dup = block_Alloc( 4 + p_block->i_buffer );
    if( p_dup )
    {
        memcpy( &p_dup->p_buffer[0], annexb_startcode4, 4 );
        memcpy( &p_dup->p_buffer[4], p_block->p_buffer, p_block->i_buffer );
        block_ChainLastAppend( ppp_last, p_dup );
    }
}

static block_t * GatherSets( decoder_sys_t *p_sys, bool b_need_sps, bool b_need_pps )
{
    block_t *p_xpsnal = NULL;
    block_t **pp_xpsnal_tail = &p_xpsnal;
    for( int i = 0; i <= H264_SPS_ID_MAX && b_need_sps; i++ )
    {
        LastAppendXPSCopy( p_sys->sps[i].p_block, &pp_xpsnal_tail );
        /* 7.4.1.2.3,  shall be the next NAL unit after a sequence parameter set NAL unit
         * having the same value of seq_parameter_set_id */
        LastAppendXPSCopy( p_sys->spsext[i].p_block, &pp_xpsnal_tail );
    }
    for( int i = 0; i < H264_PPS_ID_MAX && b_need_pps; i++ )
        LastAppendXPSCopy( p_sys->pps[i].p_block, &pp_xpsnal_tail );
    return p_xpsnal;
}

static void ActivateSets( decoder_t *p_dec, const h264_sequence_parameter_set_t *p_sps,
                                            const h264_picture_parameter_set_t *p_pps )
{
    decoder_sys_t *p_sys = p_dec->p_sys;

    p_sys->p_active_pps = p_pps;
    p_sys->p_active_sps = p_sps;

    if( p_sps )
    {
        uint8_t pl[2];
        if( h264_get_sps_profile_tier_level( p_sps, pl, &pl[1] ) )
        {
            p_dec->fmt_out.i_profile = pl[0];
            p_dec->fmt_out.i_level = pl[1];
        }

        (void) h264_get_picture_size( p_sps,
                                      &p_dec->fmt_out.video.i_x_offset,
                                      &p_dec->fmt_out.video.i_y_offset,
                                      &p_dec->fmt_out.video.i_width,
                                      &p_dec->fmt_out.video.i_height,
                                      &p_dec->fmt_out.video.i_visible_width,
                                      &p_dec->fmt_out.video.i_visible_height );

        h264_get_aspect_ratio( p_sps,
                              &p_dec->fmt_out.video.i_sar_num,
                              &p_dec->fmt_out.video.i_sar_den );

        if( !p_dec->fmt_out.video.i_frame_rate ||
            !p_dec->fmt_out.video.i_frame_rate_base )
        {
            /* on first run == if fmt_in does not provide frame rate info */
            /* If we have frame rate info in the stream */
            unsigned nd[2];
            if( h264_get_frame_rate( p_sps, nd, &nd[1] ) )
                date_Change( &p_sys->dts, nd[0], nd[1] );
            /* else use the default num/den */
            p_dec->fmt_out.video.i_frame_rate = p_sys->dts.i_divider_num >> 1; /* num_clock_ts == 2 */
            p_dec->fmt_out.video.i_frame_rate_base = p_sys->dts.i_divider_den;
        }

        if( p_dec->fmt_in->video.primaries == COLOR_PRIMARIES_UNDEF )
        {
            h264_get_colorimetry( p_sps, &p_dec->fmt_out.video.primaries,
                                  &p_dec->fmt_out.video.transfer,
                                  &p_dec->fmt_out.video.space,
                                  &p_dec->fmt_out.video.color_range );
        }

        if( p_dec->fmt_out.i_extra == 0 && p_pps )
        {
            block_t *p_xpsblocks = GatherSets( p_sys, true, true );
            if( p_xpsblocks )
            {
                size_t i_total;
                block_ChainProperties( p_xpsblocks, NULL, &i_total, NULL );
                p_dec->fmt_out.p_extra = malloc( i_total );
                if( p_dec->fmt_out.p_extra )
                {
                    p_dec->fmt_out.i_extra = i_total;
                    block_ChainExtract( p_xpsblocks, p_dec->fmt_out.p_extra, i_total );
                }
                block_ChainRelease( p_xpsblocks );
            }
        }
    }
}

static void DropStoredNAL( decoder_sys_t *p_sys )
{
    block_ChainRelease( p_sys->frame.p_head );
    block_ChainRelease( p_sys->leading.p_head );
    p_sys->frame.p_head = NULL;
    p_sys->frame.pp_append = &p_sys->frame.p_head;
    p_sys->leading.p_head = NULL;
    p_sys->leading.pp_append = &p_sys->leading.p_head;
}

/*****************************************************************************
 * Open: probe the packetizer and return score
 * When opening after demux, the packetizer is only loaded AFTER the decoder
 * That means that what you set in fmt_out is ignored by the decoder in this special case
 *****************************************************************************/
static int Open( vlc_object_t *p_this )
{
    decoder_t     *p_dec = (decoder_t*)p_this;
    decoder_sys_t *p_sys;
    int i;

    const bool b_avc = (p_dec->fmt_in->i_original_fourcc == VLC_FOURCC( 'a', 'v', 'c', '1' ));

    if( p_dec->fmt_in->i_codec != VLC_CODEC_H264 )
        return VLC_EGENERIC;
    if( b_avc && p_dec->fmt_in->i_extra < 7 )
        return VLC_EGENERIC;

    /* Allocate the memory needed to store the decoder's structure */
    if( ( p_dec->p_sys = p_sys = malloc( sizeof(decoder_sys_t) ) ) == NULL )
    {
        return VLC_ENOMEM;
    }

    p_sys->p_ccs = cc_storage_new();
    if( unlikely(!p_sys->p_ccs) )
    {
        free( p_sys );
        return VLC_ENOMEM;
    }

    packetizer_Init( &p_sys->packetizer,
                     annexb_startcode3, 3, startcode_FindAnnexB,
                     annexb_startcode3, 1, 5,
                     PacketizeReset, PacketizeParse, PacketizeValidate, PacketizeDrain,
                     p_dec );

    p_sys->p_slice = NULL;
    p_sys->frame.p_head = NULL;
    p_sys->frame.pp_append = &p_sys->frame.p_head;
    p_sys->leading.p_head = NULL;
    p_sys->leading.pp_append = &p_sys->leading.p_head;
    p_sys->b_new_sps = false;
    p_sys->b_new_pps = false;

    for( i = 0; i <= H264_SPS_ID_MAX; i++ )
    {
        p_sys->sps[i].p_sps = NULL;
        p_sys->sps[i].p_block = NULL;
    }
    p_sys->p_active_sps = NULL;
    for( i = 0; i <= H264_PPS_ID_MAX; i++ )
    {
        p_sys->pps[i].p_pps = NULL;
        p_sys->pps[i].p_block = NULL;
    }
    p_sys->p_active_pps = NULL;
    for( i = 0; i <= H264_SPSEXT_ID_MAX; i++ )
        p_sys->spsext[i].p_block = NULL;
    p_sys->i_recovery_frame_cnt = UINT_MAX;

    p_sys->i_next_block_flags = 0;
    p_sys->b_recovered = false;
    p_sys->i_recoveryfnum = UINT_MAX;
    p_sys->i_frame_dts = VLC_TICK_INVALID;
    p_sys->i_frame_pts = VLC_TICK_INVALID;
    p_sys->i_dpb_output_delay = 0;

    /* POC */
    h264_poc_context_init( &p_sys->pocctx );
    p_sys->prevdatedpoc.pts = VLC_TICK_INVALID;

    date_Init( &p_sys->dts, 30000 * 2, 1001 );

    /* Setup properties */
    es_format_Copy( &p_dec->fmt_out, p_dec->fmt_in );
    p_dec->fmt_out.i_codec = VLC_CODEC_H264;
    p_dec->fmt_out.b_packetized = true;

    if( p_dec->fmt_in->video.i_frame_rate_base &&
        p_dec->fmt_in->video.i_frame_rate &&
        p_dec->fmt_in->video.i_frame_rate <= UINT_MAX / 2 )
    {
        date_Change( &p_sys->dts, p_dec->fmt_in->video.i_frame_rate * 2,
                                  p_dec->fmt_in->video.i_frame_rate_base );
    }

    if( b_avc )
    {
        /* This type of stream is produced by mp4 and matroska
         * when we want to store it in another streamformat, you need to convert
         * The fmt_in.p_extra should ALWAYS contain the avcC
         * The fmt_out.p_extra should contain all the SPS and PPS with 4 byte startcodes */
        if( h264_isavcC( p_dec->fmt_in->p_extra, p_dec->fmt_in->i_extra ) )
        {
            free( p_dec->fmt_out.p_extra );
            size_t i_size;
            p_dec->fmt_out.p_extra = h264_avcC_to_AnnexB_NAL( p_dec->fmt_in->p_extra,
                                                              p_dec->fmt_in->i_extra,
                                                             &i_size,
                                                             &p_sys->i_avcC_length_size );
            p_dec->fmt_out.i_extra = i_size;
            p_sys->b_recovered = !!p_dec->fmt_out.i_extra;

            if(!p_dec->fmt_out.p_extra)
            {
                msg_Err( p_dec, "Invalid AVC extradata");
                Close( p_this );
                return VLC_EGENERIC;
            }
        }
        else
        {
            msg_Err( p_dec, "Invalid or missing AVC extradata");
            Close( p_this );
            return VLC_EGENERIC;
        }

        /* Set callback */
        p_dec->pf_packetize = PacketizeAVC1;
    }
    else
    {
        /* This type of stream contains data with 3 of 4 byte startcodes
         * The fmt_in.p_extra MAY contain SPS/PPS with 4 byte startcodes
         * The fmt_out.p_extra should be the same */

        /* Set callback */
        p_dec->pf_packetize = Packetize;
    }

    /* */
    if( p_dec->fmt_out.i_extra > 0 )
    {
        packetizer_Header( &p_sys->packetizer,
                           p_dec->fmt_out.p_extra, p_dec->fmt_out.i_extra );
    }

    if( b_avc )
    {
        /* FIXME: that's not correct for every AVC */
        if( !p_sys->b_new_pps || !p_sys->b_new_sps )
        {
            msg_Err( p_dec, "Invalid or missing SPS %d or PPS %d in AVC extradata",
                     p_sys->b_new_sps, p_sys->b_new_pps );
            Close( p_this );
            return VLC_EGENERIC;
        }

        msg_Dbg( p_dec, "Packetizer fed with AVC, nal length size=%d",
                         p_sys->i_avcC_length_size );
    }

    /* CC are the same for H264/AVC in T35 sections (ETSI TS 101 154)  */
    p_dec->pf_get_cc = GetCc;
    p_dec->pf_flush = PacketizeFlush;

    return VLC_SUCCESS;
}

/*****************************************************************************
 * Close: clean up the packetizer
 *****************************************************************************/
static void Close( vlc_object_t *p_this )
{
    decoder_t *p_dec = (decoder_t*)p_this;
    decoder_sys_t *p_sys = p_dec->p_sys;

    DropStoredNAL( p_sys );
    ReleaseXPS( p_sys );

    if( p_sys->p_slice )
        h264_slice_release( p_sys->p_slice );

    packetizer_Clean( &p_sys->packetizer );

    cc_storage_delete( p_sys->p_ccs );

    free( p_sys );
}

static void PacketizeFlush( decoder_t *p_dec )
{
    decoder_sys_t *p_sys = p_dec->p_sys;

    packetizer_Flush( &p_sys->packetizer );
}

/****************************************************************************
 * Packetize: the whole thing
 * Search for the startcodes 3 or more bytes
 * Feed ParseNALBlock ALWAYS with 4 byte startcode prepended NALs
 ****************************************************************************/
static block_t *Packetize( decoder_t *p_dec, block_t **pp_block )
{
    decoder_sys_t *p_sys = p_dec->p_sys;

    return packetizer_Packetize( &p_sys->packetizer, pp_block );
}

/****************************************************************************
 * PacketizeAVC1: Takes VCL blocks of data and creates annexe B type NAL stream
 * Will always use 4 byte 0 0 0 1 startcodes
 * Will prepend a SPS and PPS before each keyframe
 ****************************************************************************/
static block_t *PacketizeAVC1( decoder_t *p_dec, block_t **pp_block )
{
    decoder_sys_t *p_sys = p_dec->p_sys;

    return PacketizeXXC1( p_dec, p_dec->obj.logger,
                          p_sys->i_avcC_length_size, pp_block,
                          ParseNALBlockW, PacketizeDrain );
}

/*****************************************************************************
 * GetCc:
 *****************************************************************************/
static block_t *GetCc( decoder_t *p_dec, decoder_cc_desc_t *p_desc )
{
    decoder_sys_t *p_sys = p_dec->p_sys;
    return cc_storage_get_current( p_sys->p_ccs, p_desc );
}

/****************************************************************************
 * Helpers
 ****************************************************************************/
static void ResetOutputVariables( decoder_sys_t *p_sys )
{
    p_sys->i_frame_dts = VLC_TICK_INVALID;
    p_sys->i_frame_pts = VLC_TICK_INVALID;
    if( p_sys->p_slice )
        h264_slice_release( p_sys->p_slice );
    p_sys->p_slice = NULL;
    p_sys->b_new_sps = false;
    p_sys->b_new_pps = false;
    /* From SEI */
    p_sys->i_dpb_output_delay = 0;
    p_sys->i_pic_struct = UINT8_MAX;
    p_sys->i_recovery_frame_cnt = UINT_MAX;
}

static void PacketizeReset( void *p_private, bool b_flush )
{
    decoder_t *p_dec = p_private;
    decoder_sys_t *p_sys = p_dec->p_sys;

    if( b_flush || !p_sys->p_slice )
    {
        DropStoredNAL( p_sys );
        ResetOutputVariables( p_sys );
        p_sys->p_active_pps = NULL;
        p_sys->p_active_sps = NULL;
        p_sys->b_recovered = false;
        p_sys->i_recoveryfnum = UINT_MAX;
        /* POC */
        h264_poc_context_init( &p_sys->pocctx );
        p_sys->prevdatedpoc.pts = VLC_TICK_INVALID;
    }
    p_sys->i_next_block_flags = BLOCK_FLAG_DISCONTINUITY;
    date_Set( &p_sys->dts, VLC_TICK_INVALID );
}
static block_t *PacketizeParse( void *p_private, bool *pb_ts_used, block_t *p_block )
{
    decoder_t *p_dec = p_private;

    /* Remove trailing 0 bytes */
    while( p_block->i_buffer > 5 && p_block->p_buffer[p_block->i_buffer-1] == 0x00 )
        p_block->i_buffer--;

    return ParseNALBlock( p_dec, pb_ts_used, p_block );
}
static int PacketizeValidate( void *p_private, block_t *p_au )
{
    VLC_UNUSED(p_private);
    VLC_UNUSED(p_au);
    return VLC_SUCCESS;
}

static block_t * PacketizeDrain( void *p_private )
{
    decoder_t *p_dec = p_private;
    decoder_sys_t *p_sys = p_dec->p_sys;

    if( !p_sys->p_slice )
        return NULL;

    block_t *p_out = OutputPicture( p_dec );
    if( p_out && (p_out->i_flags & BLOCK_FLAG_DROP) )
    {
        block_Release( p_out );
        p_out = NULL;
    }

    return p_out;
}

/*****************************************************************************
 * ParseNALBlock: parses annexB type NALs
 * All p_frag blocks are required to start with 0 0 0 1 4-byte startcode
 *****************************************************************************/
static block_t *ParseNALBlock( decoder_t *p_dec, bool *pb_ts_used, block_t *p_frag )
{
    decoder_sys_t *p_sys = p_dec->p_sys;
    block_t *p_pic = NULL;

    const enum h264_nal_unit_type_e i_nal_type = h264_getNALType( &p_frag->p_buffer[4] );
    const vlc_tick_t i_frag_dts = p_frag->i_dts;
    const vlc_tick_t i_frag_pts = p_frag->i_pts;
    bool b_au_end = p_frag->i_flags & BLOCK_FLAG_AU_END;
    p_frag->i_flags &= ~BLOCK_FLAG_AU_END;

    if( p_sys->p_slice && (!p_sys->p_active_pps || !p_sys->p_active_sps) )
    {
        msg_Warn( p_dec, "waiting for SPS/PPS" );

        /* Reset context */
        DropStoredNAL( p_sys );
        ResetOutputVariables( p_sys );
        cc_storage_reset( p_sys->p_ccs );
    }

    switch( i_nal_type )
    {
        /*** Slices ***/
        case H264_NAL_SLICE:
        case H264_NAL_SLICE_DPA:
        case H264_NAL_SLICE_DPB:
        case H264_NAL_SLICE_DPC:
        case H264_NAL_SLICE_IDR:
        {
            h264_slice_t *p_newslice;

            if( i_nal_type == H264_NAL_SLICE_IDR )
            {
                p_sys->b_recovered = true;
                p_sys->i_recovery_frame_cnt = UINT_MAX;
                p_sys->i_recoveryfnum = UINT_MAX;
            }

            if( (p_newslice = ParseSliceHeader( p_dec, p_frag )) )
            {
                /* Only IDR carries the id, to be propagated */
                h264_slice_copy_idr_id( p_sys->p_slice, p_newslice );

                bool b_new_picture = h264_IsFirstVCLNALUnit( p_sys->p_slice, p_newslice );
                if( b_new_picture )
                {
                    /* Parse SEI for that frame now we should have matched SPS/PPS */
                    for( block_t *p_sei = p_sys->leading.p_head; p_sei; p_sei = p_sei->p_next )
                    {
                        if( (p_sei->i_flags & BLOCK_FLAG_PRIVATE_SEI) == 0 )
                            continue;
                        HxxxParse_AnnexB_SEI( p_sei->p_buffer, p_sei->i_buffer,
                                              1 /* nal header */, ParseSeiCallback, p_dec );
                    }

                    if( p_sys->p_slice )
                        p_pic = OutputPicture( p_dec );
                }

                /* */
                h264_slice_release( p_sys->p_slice );
                p_sys->p_slice = p_newslice;
            }
            else
            {
                p_sys->p_active_pps = NULL;
                /* Fragment will be discarded later on */
            }

            block_ChainLastAppend( &p_sys->frame.pp_append, p_frag );
        } break;

        /*** Prefix NALs ***/

        case H264_NAL_AU_DELIMITER:
            if( p_sys->p_slice )
                p_pic = OutputPicture( p_dec );

            /* clear junk if no pic, we're always the first nal */
            DropStoredNAL( p_sys );

            p_frag->i_flags |= BLOCK_FLAG_PRIVATE_AUD;

            block_ChainLastAppend( &p_sys->leading.pp_append, p_frag );
        break;

        case H264_NAL_SPS:
        case H264_NAL_PPS:
            if( p_sys->p_slice )
                p_pic = OutputPicture( p_dec );

            /* Stored for insert on keyframes */
            if( i_nal_type == H264_NAL_SPS )
                p_sys->b_new_sps |= PutXPS( p_dec, i_nal_type, p_frag );
            else
                p_sys->b_new_pps |= PutXPS( p_dec, i_nal_type, p_frag );
        break;

        case H264_NAL_SEI:
            if( p_sys->p_slice )
                p_pic = OutputPicture( p_dec );

            p_frag->i_flags |= BLOCK_FLAG_PRIVATE_SEI;
            block_ChainLastAppend( &p_sys->leading.pp_append, p_frag );
        break;

        case H264_NAL_SPS_EXT:
            PutXPS( p_dec, i_nal_type, p_frag );
            if( p_sys->p_slice )
                p_pic = OutputPicture( p_dec );
            break;

        case H264_NAL_PREFIX: /* first slice/VCL associated data */
        case H264_NAL_SUBSET_SPS:
        case H264_NAL_DEPTH_PS:
        case H264_NAL_RESERVED_17:
        case H264_NAL_RESERVED_18:
            if( p_sys->p_slice )
                p_pic = OutputPicture( p_dec );

            block_ChainLastAppend( &p_sys->leading.pp_append, p_frag );
        break;

        /*** Suffix NALs ***/

        case H264_NAL_END_OF_SEQ:
        case H264_NAL_END_OF_STREAM:
            /* Early end of packetization */
            block_ChainLastAppend( &p_sys->frame.pp_append, p_frag );

            /* important for still pictures/menus */
            p_sys->i_next_block_flags |= BLOCK_FLAG_END_OF_SEQUENCE;
            if( p_sys->p_slice )
                p_pic = OutputPicture( p_dec );
        break;

        case H264_NAL_SLICE_WP: // post
        case H264_NAL_UNKNOWN:
        case H264_NAL_FILLER_DATA:
        case H264_NAL_SLICE_EXT:
        case H264_NAL_SLICE_3D_EXT:
        case H264_NAL_RESERVED_22:
        case H264_NAL_RESERVED_23:
        default: /* others 24..31, including unknown */
            block_ChainLastAppend( &p_sys->frame.pp_append, p_frag );
        break;
    }

    *pb_ts_used = false;
    if( p_sys->i_frame_dts == VLC_TICK_INVALID &&
        p_sys->i_frame_pts == VLC_TICK_INVALID )
    {
        p_sys->i_frame_dts = i_frag_dts;
        p_sys->i_frame_pts = i_frag_pts;
        *pb_ts_used = true;
        if( i_frag_dts != VLC_TICK_INVALID )
            date_Set( &p_sys->dts, i_frag_dts );
    }

    if( p_sys->p_slice && b_au_end && !p_pic )
    {
        p_pic = OutputPicture( p_dec );
    }

    if( p_pic && (p_pic->i_flags & BLOCK_FLAG_DROP) )
    {
        block_Release( p_pic );
        p_pic = NULL;
    }

    return p_pic;
}

static block_t *OutputPicture( decoder_t *p_dec )
{
    decoder_sys_t *p_sys = p_dec->p_sys;
    block_t *p_pic = NULL;
    block_t **pp_pic_last = &p_pic;

    if( unlikely(!p_sys->frame.p_head) )
    {
        assert( p_sys->frame.p_head );
        DropStoredNAL( p_sys );
        ResetOutputVariables( p_sys );
        cc_storage_reset( p_sys->p_ccs );
        return NULL;
    }

    /* Bind matched/referred PPS and SPS */
    const h264_picture_parameter_set_t *p_pps = p_sys->p_active_pps;
    const h264_sequence_parameter_set_t *p_sps = p_sys->p_active_sps;
    if( !p_pps || !p_sps )
    {
        DropStoredNAL( p_sys );
        ResetOutputVariables( p_sys );
        cc_storage_reset( p_sys->p_ccs );
        return NULL;
    }

    if( !p_sys->b_recovered && p_sys->i_recoveryfnum == UINT_MAX &&
        p_sys->i_recovery_frame_cnt == UINT_MAX && h264_get_slice_type(p_sys->p_slice) == H264_SLICE_TYPE_I )
    {
        /* No way to recover using SEI, just sync on I Slice */
        p_sys->b_recovered = true;
    }

    bool b_need_sps_pps = h264_get_slice_type(p_sys->p_slice) == H264_SLICE_TYPE_I &&
                          p_sys->p_active_pps && p_sys->p_active_sps;

    const unsigned i_frame_num = h264_get_frame_num(p_sys->p_slice);

    /* Handle SEI recovery */
    if ( !p_sys->b_recovered && p_sys->i_recovery_frame_cnt != UINT_MAX &&
         p_sys->i_recoveryfnum == UINT_MAX )
    {
        p_sys->i_recoveryfnum = i_frame_num + p_sys->i_recovery_frame_cnt;
        p_sys->i_recoverystartfnum = i_frame_num;
        b_need_sps_pps = true; /* SPS/PPS must be inserted for SEI recovery */
        msg_Dbg( p_dec, "Recovering using SEI, prerolling %u reference pics", p_sys->i_recovery_frame_cnt );
    }

    if( p_sys->i_recoveryfnum != UINT_MAX )
    {
        assert(p_sys->b_recovered == false);
        const unsigned maxFrameNum = h264_get_max_frame_num( p_sps );

        if( ( p_sys->i_recoveryfnum > maxFrameNum &&
              i_frame_num < p_sys->i_recoverystartfnum &&
              i_frame_num >= p_sys->i_recoveryfnum % maxFrameNum ) ||
            ( p_sys->i_recoveryfnum <= maxFrameNum &&
              i_frame_num >= p_sys->i_recoveryfnum ) )
        {
            p_sys->i_recoveryfnum = UINT_MAX;
            p_sys->b_recovered = true;
            msg_Dbg( p_dec, "Recovery from SEI recovery point complete" );
        }
    }

    /* Gather PPS/SPS if required */
    block_t *p_xpsnal = GatherSets( p_sys, b_need_sps_pps|p_sys->b_new_sps,
                                           b_need_sps_pps|p_sys->b_new_pps );

    /* Now rebuild NAL Sequence, inserting PPS/SPS if any */
    if( p_sys->leading.p_head &&
       (p_sys->leading.p_head->i_flags & BLOCK_FLAG_PRIVATE_AUD) )
    {
        block_t *p_au = p_sys->leading.p_head;
        p_sys->leading.p_head = p_au->p_next;
        p_au->p_next = NULL;
        block_ChainLastAppend( &pp_pic_last, p_au );
    }

    if( p_xpsnal )
        block_ChainLastAppend( &pp_pic_last, p_xpsnal );

    if( p_sys->leading.p_head )
        block_ChainLastAppend( &pp_pic_last, p_sys->leading.p_head );

    assert( p_sys->frame.p_head );
    if( p_sys->frame.p_head )
        block_ChainLastAppend( &pp_pic_last, p_sys->frame.p_head );

    /* Reset chains, now empty */
    p_sys->frame.p_head = NULL;
    p_sys->frame.pp_append = &p_sys->frame.p_head;
    p_sys->leading.p_head = NULL;
    p_sys->leading.pp_append = &p_sys->leading.p_head;

    p_pic = block_ChainGather( p_pic );

    if( !p_pic )
    {
        ResetOutputVariables( p_sys );
        cc_storage_reset( p_sys->p_ccs );
        return NULL;
    }

    /* clear up flags gathered */
    p_pic->i_flags &= ~BLOCK_FLAG_PRIVATE_MASK;

    /* for PTS Fixup, interlaced fields (multiple AU/block) */
    int tFOC = 0, bFOC = 0, PictureOrderCount = 0;
    h264_compute_poc( p_sps, p_sys->p_slice, &p_sys->pocctx, &PictureOrderCount, &tFOC, &bFOC );

    unsigned i_num_clock_ts = h264_get_num_ts( p_sps, p_sys->p_slice, p_sys->i_pic_struct, tFOC, bFOC );

    if( !h264_is_frames_only( p_sps ) && p_sys->i_pic_struct != UINT8_MAX )
    {
        switch( p_sys->i_pic_struct )
        {
        /* Top and Bottom field slices */
        case 1:
        case 2:
            p_pic->i_flags |= BLOCK_FLAG_SINGLE_FIELD;
            p_pic->i_flags |= h264_slice_top_field(p_sys->p_slice) ? BLOCK_FLAG_TOP_FIELD_FIRST
                                                                   : BLOCK_FLAG_BOTTOM_FIELD_FIRST;
            break;
        /* Each of the following slices contains multiple fields */
        case 3:
            p_pic->i_flags |= BLOCK_FLAG_TOP_FIELD_FIRST;
            break;
        case 4:
            p_pic->i_flags |= BLOCK_FLAG_BOTTOM_FIELD_FIRST;
            break;
        case 5:
            p_pic->i_flags |= BLOCK_FLAG_TOP_FIELD_FIRST;
            break;
        case 6:
            p_pic->i_flags |= BLOCK_FLAG_BOTTOM_FIELD_FIRST;
            break;
        default:
            break;
        }
    }

    /* set dts/pts to current block timestamps */
    p_pic->i_dts = p_sys->i_frame_dts;
    p_pic->i_pts = p_sys->i_frame_pts;

    /* Fixup missing timestamps after split (multiple AU/block)*/
    if( p_pic->i_dts == VLC_TICK_INVALID )
        p_pic->i_dts = date_Get( &p_sys->dts );

    if( h264_get_slice_type( p_sys->p_slice ) == H264_SLICE_TYPE_I )
        p_sys->prevdatedpoc.pts = VLC_TICK_INVALID;

    if( p_pic->i_pts == VLC_TICK_INVALID )
    {
        if( p_sys->prevdatedpoc.pts != VLC_TICK_INVALID &&
            date_Get( &p_sys->dts ) != VLC_TICK_INVALID )
        {
            date_t pts = p_sys->dts;
            date_Set( &pts, p_sys->prevdatedpoc.pts );

            int diff = tFOC - p_sys->prevdatedpoc.num;
            if( diff > 0 )
                date_Increment( &pts, diff );
            else
                date_Decrement( &pts, -diff );

            p_pic->i_pts = date_Get( &pts );
            /* non monotonically increasing dts on some videos 33333 33333...35000 */
            if( p_pic->i_pts < p_pic->i_dts )
                p_pic->i_pts = p_pic->i_dts;
        }
        /* In case there's no PTS at all */
        else if( h264_CanSwapPTSWithDTS( p_sys->p_slice, p_sps ) )
        {
            p_pic->i_pts = p_pic->i_dts;
        }
        else if( h264_get_slice_type( p_sys->p_slice ) == H264_SLICE_TYPE_I &&
                 date_Get( &p_sys->dts ) != VLC_TICK_INVALID )
        {
            /* Hell no PTS on IDR. We're totally blind */
            date_t pts = p_sys->dts;
            date_Increment( &pts, 2 );
            p_pic->i_pts = date_Get( &pts );
        }
    }
    else if( p_pic->i_dts == VLC_TICK_INVALID &&
             h264_CanSwapPTSWithDTS( p_sys->p_slice, p_sps ) )
    {
        p_pic->i_dts = p_pic->i_pts;
        if( date_Get( &p_sys->dts ) == VLC_TICK_INVALID )
            date_Set( &p_sys->dts, p_pic->i_pts );
    }

    if( p_pic->i_pts != VLC_TICK_INVALID )
    {
        p_sys->prevdatedpoc.pts = p_pic->i_pts;
        p_sys->prevdatedpoc.num = PictureOrderCount;
    }

    if( p_pic->i_length == 0 )
    {
        date_t next = p_sys->dts;
        date_Increment( &next, i_num_clock_ts );
        p_pic->i_length = date_Get( &next ) - date_Get( &p_sys->dts );
    }

#if 0
    msg_Err(p_dec, "F/BOC %d/%d POC %d %d rec %d flags %x ref%d fn %d fp %d %ld pts %ld len %ld",
                    tFOC, bFOC, PictureOrderCount,
                    h264_get_slice_type(p_sys->p_slice), p_sys->b_recovered, p_pic->i_flags,
                    h264_get_nal_ref_idc(p_sys->p_slice), h264_get_frame_num(p_sys->p_slice),
                    h264_is_field_pic(p_sys->p_slice),
                    p_pic->i_pts - p_pic->i_dts, p_pic->i_pts % VLC_TICK_FROM_SEC(100), p_pic->i_length);
#endif

    /* save for next pic fixups */
    if( date_Get( &p_sys->dts ) != VLC_TICK_INVALID )
    {
        if( p_sys->i_next_block_flags & BLOCK_FLAG_DISCONTINUITY )
            date_Set( &p_sys->dts, VLC_TICK_INVALID );
        else
            date_Increment( &p_sys->dts, i_num_clock_ts );
    }

    if( p_pic )
    {
        p_pic->i_flags |= p_sys->i_next_block_flags;
        p_sys->i_next_block_flags = 0;
    }

    switch( h264_get_slice_type( p_sys->p_slice ) )
    {
        case H264_SLICE_TYPE_P:
            p_pic->i_flags |= BLOCK_FLAG_TYPE_P;
            break;
        case H264_SLICE_TYPE_B:
            p_pic->i_flags |= BLOCK_FLAG_TYPE_B;
            break;
        case H264_SLICE_TYPE_I:
            p_pic->i_flags |= BLOCK_FLAG_TYPE_I;
        default:
            break;
    }

    if( !p_sys->b_recovered )
    {
        if( p_sys->i_recoveryfnum != UINT_MAX ) /* recovering from SEI */
            p_pic->i_flags |= BLOCK_FLAG_PREROLL;
        else
            p_pic->i_flags |= BLOCK_FLAG_DROP;
    }

    p_pic->i_flags &= ~BLOCK_FLAG_PRIVATE_AUD;

    /* reset after output */
    ResetOutputVariables( p_sys );

    /* CC */
    cc_storage_commit( p_sys->p_ccs, p_pic );

    return p_pic;
}

static int CmpXPS( const block_t *p_ref, const block_t *p_nal )
{
    return p_ref == NULL ||
           p_ref->i_buffer != p_nal->i_buffer ||
           memcmp( p_ref->p_buffer, p_nal->p_buffer, p_nal->i_buffer );
}

#define wrap_h264_xps_decode(funcname ) \
    static void *funcname ## _wrapper ( const uint8_t *a, size_t b, bool c ) \
    { return funcname(a,b,c); }

wrap_h264_xps_decode(h264_decode_sps)
wrap_h264_xps_decode(h264_decode_pps)

#define wrap_h264_xps_release(funcname, typecast) \
    static void funcname ## _wrapper ( void *a ) { funcname((typecast *)a); }

wrap_h264_xps_release(h264_release_sps, h264_sequence_parameter_set_t)
wrap_h264_xps_release(h264_release_pps, h264_picture_parameter_set_t)

static void ReleaseXPS( decoder_sys_t *p_sys )
{
    for( int i = 0; i <= H264_SPS_ID_MAX; i++ )
    {
        if( !p_sys->sps[i].p_block )
            continue;
        block_Release( p_sys->sps[i].p_block );
        h264_release_sps( p_sys->sps[i].p_sps );
    }
    for( int i = 0; i <= H264_PPS_ID_MAX; i++ )
    {
        if( !p_sys->pps[i].p_block )
            continue;
        block_Release( p_sys->pps[i].p_block );
        h264_release_pps( p_sys->pps[i].p_pps );
    }
    for( int i = 0; i <= H264_SPSEXT_ID_MAX; i++ )
    {
        if( p_sys->spsext[i].p_block )
            block_Release( p_sys->spsext[i].p_block );
    }
    p_sys->p_active_sps = NULL;
    p_sys->p_active_pps = NULL;
}

static bool PutXPS( decoder_t *p_dec, uint8_t i_nal_type, block_t *p_frag )
{
    decoder_sys_t *p_sys = p_dec->p_sys;

    uint8_t i_id;
    if( !hxxx_strip_AnnexB_startcode( (const uint8_t **)&p_frag->p_buffer,
                                      &p_frag->i_buffer ) ||
        !h264_get_xps_id( p_frag->p_buffer, p_frag->i_buffer, &i_id ) )
    {
        block_Release( p_frag );
        return false;
    }

    const char * rgsz_types[3] = {"SPS", "PPS", "SPSEXT"};
    const char *psz_type;
    block_t **pp_block_dst;
    /* all depend on pp_xps_dst */
    void **pp_xps_dst = NULL;
    const void **pp_active = NULL; /* optional */
    void * (* pf_decode_xps)(const uint8_t *, size_t, bool) = NULL;
    void   (* pf_release_xps)(void *) = NULL;

    switch( i_nal_type )
    {
        case H264_NAL_SPS:
            psz_type = rgsz_types[0];
            pp_active = (const void **) &p_sys->p_active_sps;
            pp_block_dst = &p_sys->sps[i_id].p_block;
            pp_xps_dst = (void **) &p_sys->sps[i_id].p_sps;
            pf_decode_xps = h264_decode_sps_wrapper;
            pf_release_xps = h264_release_sps_wrapper;
            break;
        case H264_NAL_PPS:
            psz_type = rgsz_types[1];
            pp_active = (const void **) &p_sys->p_active_pps;
            pp_block_dst = &p_sys->pps[i_id].p_block;
            pp_xps_dst = (void **) &p_sys->pps[i_id].p_pps;
            pf_decode_xps = h264_decode_pps_wrapper;
            pf_release_xps = h264_release_pps_wrapper;
            break;
        case H264_NAL_SPS_EXT:
            psz_type = rgsz_types[2];
            pp_block_dst = &p_sys->spsext[i_id].p_block;
            break;
        default:
            block_Release( p_frag );
            return false;
    }

    if( !CmpXPS( *pp_block_dst, p_frag ) )
    {
        block_Release( p_frag );
        return false;
    }

    msg_Dbg( p_dec, "found NAL_%s (id=%" PRIu8 ")", psz_type, i_id );

    if( pp_xps_dst != NULL )
    {
        void *p_xps = pf_decode_xps( p_frag->p_buffer, p_frag->i_buffer, true );
        if( !p_xps )
        {
            block_Release( p_frag );
            return false;
        }
        if( *pp_xps_dst )
        {
            if( pp_active && *pp_active == *pp_xps_dst )
                *pp_active = NULL;
            pf_release_xps( *pp_xps_dst );
        }
        *pp_xps_dst = p_xps;
    }

    if( *pp_block_dst )
        block_Release( *pp_block_dst );
    *pp_block_dst = p_frag;

    return true;
}

static void GetSPSPPS( uint8_t i_pps_id, void *priv,
                       const h264_sequence_parameter_set_t **pp_sps,
                       const h264_picture_parameter_set_t **pp_pps )
{
    decoder_sys_t *p_sys = priv;

    *pp_pps = p_sys->pps[i_pps_id].p_pps;
    if( *pp_pps == NULL )
        *pp_sps = NULL;
    else
        *pp_sps = p_sys->sps[h264_get_pps_sps_id(*pp_pps)].p_sps;
}

static h264_slice_t * ParseSliceHeader( decoder_t *p_dec, const block_t *p_frag )
{
    decoder_sys_t *p_sys = p_dec->p_sys;

    const uint8_t *p_stripped = p_frag->p_buffer;
    size_t i_stripped = p_frag->i_buffer;

    if( !hxxx_strip_AnnexB_startcode( &p_stripped, &i_stripped ) || i_stripped < 2 )
        return NULL;

    h264_slice_t *p_slice = h264_decode_slice( p_stripped, i_stripped, GetSPSPPS, p_sys );
    if( !p_slice )
        return NULL;

    const h264_sequence_parameter_set_t *p_sps;
    const h264_picture_parameter_set_t *p_pps;
    GetSPSPPS( h264_get_slice_pps_id( p_slice ), p_sys, &p_sps, &p_pps );
    if( unlikely( !p_sps || !p_pps) )
    {
        h264_slice_release( p_slice );
        return NULL;
    }

    ActivateSets( p_dec, p_sps, p_pps );

    return p_slice;
}

static bool ParseSeiCallback( const hxxx_sei_data_t *p_sei_data, void *cbdata )
{
    decoder_t *p_dec = (decoder_t *) cbdata;
    decoder_sys_t *p_sys = p_dec->p_sys;

    switch( p_sei_data->i_type )
    {
        /* Look for pic timing */
        case HXXX_SEI_PIC_TIMING:
        {
            const h264_sequence_parameter_set_t *p_sps = p_sys->p_active_sps;
            if( unlikely( p_sps == NULL ) )
            {
                assert( p_sps );
                break;
            }

            h264_decode_sei_pic_timing( p_sei_data->p_bs, p_sps,
                                       &p_sys->i_pic_struct,
                                       &p_sys->i_dpb_output_delay );
        } break;

            /* Look for user_data_registered_itu_t_t35 */
        case HXXX_SEI_USER_DATA_REGISTERED_ITU_T_T35:
        {
            if( p_sei_data->itu_t35.type == HXXX_ITU_T35_TYPE_CC )
            {
                cc_storage_append( p_sys->p_ccs, true, p_sei_data->itu_t35.u.cc.p_data,
                                                       p_sei_data->itu_t35.u.cc.i_data );
            }
        } break;

        case HXXX_SEI_FRAME_PACKING_ARRANGEMENT:
        {
            if( p_dec->fmt_in->video.multiview_mode == MULTIVIEW_2D )
            {
                video_multiview_mode_t mode;
                switch( p_sei_data->frame_packing.type )
                {
                    case FRAME_PACKING_INTERLEAVED_CHECKERBOARD:
                        mode = MULTIVIEW_STEREO_CHECKERBOARD; break;
                    case FRAME_PACKING_INTERLEAVED_COLUMN:
                        mode = MULTIVIEW_STEREO_COL; break;
                    case FRAME_PACKING_INTERLEAVED_ROW:
                        mode = MULTIVIEW_STEREO_ROW; break;
                    case FRAME_PACKING_SIDE_BY_SIDE:
                        mode = MULTIVIEW_STEREO_SBS; break;
                    case FRAME_PACKING_TOP_BOTTOM:
                        mode = MULTIVIEW_STEREO_TB; break;
                    case FRAME_PACKING_TEMPORAL:
                        mode = MULTIVIEW_STEREO_FRAME; break;
                    case FRAME_PACKING_TILED:
                    default:
                        mode = MULTIVIEW_2D; break;
                }
                p_dec->fmt_out.video.multiview_mode = mode;
            }
        } break;

            /* Look for SEI recovery point */
        case HXXX_SEI_RECOVERY_POINT:
        {
            h264_sei_recovery_point_t reco;
            if( !p_sys->b_recovered &&
                h264_decode_sei_recovery_point( p_sei_data->p_bs, &reco ) )
            {
                msg_Dbg( p_dec, "Seen SEI recovery point, %u recovery frames", reco.i_frames );
                p_sys->i_recovery_frame_cnt = reco.i_frames;
            }
        } break;

        default:
            /* Will skip */
            break;
    }

    return true;
}

