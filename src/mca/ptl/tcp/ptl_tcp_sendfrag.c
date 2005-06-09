/*
 * Copyright (c) 2004-2005 The Trustees of Indiana University.
 *                         All rights reserved.
 * Copyright (c) 2004-2005 The Trustees of the University of Tennessee.
 *                         All rights reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart, 
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * $COPYRIGHT$
 * 
 * Additional copyrights may follow
 * 
 * $HEADER$
 */
#include "ompi_config.h"
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#ifdef HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif
#include "include/ompi_socket_errno.h"
#include "include/types.h"
#include "datatype/datatype.h"
#include "mca/pml/base/pml_base_sendreq.h"
#include "ptl_tcp.h"
#include "ptl_tcp_peer.h"
#include "ptl_tcp_proc.h"
#include "ptl_tcp_sendfrag.h"

#define frag_header     frag_send.frag_base.frag_header
#define frag_owner      frag_send.frag_base.frag_owner
#define frag_peer       frag_send.frag_base.frag_peer
#define frag_convertor  frag_send.frag_base.frag_convertor


static void mca_ptl_tcp_send_frag_construct(mca_ptl_tcp_send_frag_t* frag);
static void mca_ptl_tcp_send_frag_destruct(mca_ptl_tcp_send_frag_t* frag);


ompi_class_t  mca_ptl_tcp_send_frag_t_class = {
    "mca_ptl_tcp_send_frag_t",
    OBJ_CLASS(mca_ptl_base_send_frag_t),
    (ompi_construct_t)mca_ptl_tcp_send_frag_construct,
    (ompi_destruct_t)mca_ptl_tcp_send_frag_destruct
};
                                                                                                           
/*
 * Placeholders for send fragment constructor/destructors.
 */

static void mca_ptl_tcp_send_frag_construct(mca_ptl_tcp_send_frag_t* frag)
{
   OMPI_DEBUG_ZERO(*frag);
   frag->free_after = 0;
}


static void mca_ptl_tcp_send_frag_destruct(mca_ptl_tcp_send_frag_t* frag)
{
}

extern mca_ptl_tcp_component_t mca_ptl_tcp_component;

void* mca_ptl_tcp_memalloc( size_t* length, void* data )
{
   if( (*length) > mca_ptl_tcp_component.tcp_frag_size )
      *length = mca_ptl_tcp_component.tcp_frag_size;
   return malloc( *length );
}

/*
 *  Initialize the fragment based on the current offset into the users
 *  data buffer, and the indicated size.
 */

int mca_ptl_tcp_send_frag_init(
    mca_ptl_tcp_send_frag_t* sendfrag,
    mca_ptl_base_peer_t* ptl_peer,
    mca_ptl_base_send_request_t* sendreq,
    size_t offset,
    size_t* size,
    int flags)
{
    /* message header */
    size_t size_in = *size, size_out, max_data;
    uint32_t iov_count;
    mca_ptl_base_header_t* hdr = &sendfrag->frag_header;

    sendfrag->free_after = 0;
    /* initialize convertor */
    if(size_in > 0) {
       ompi_convertor_t *convertor;
       int rc;

       convertor = &sendfrag->frag_convertor;
       ompi_convertor_clone( &sendreq->req_send.req_convertor, convertor, 1 );
       ompi_convertor_personalize( convertor, 0, &offset, mca_ptl_tcp_memalloc, NULL );
        /* if data is contigous convertor will return an offset
         * into users buffer - otherwise will return an allocated buffer 
         * that holds the packed data
         */
        sendfrag->frag_vec[1].iov_base = NULL;
        sendfrag->frag_vec[1].iov_len = size_in;
        iov_count = 1;
        max_data = size_in;
        if((rc = ompi_convertor_pack( convertor, &sendfrag->frag_vec[1], 
                                      &iov_count, &max_data, &(sendfrag->free_after) )) < 0) {
            return OMPI_ERROR;
        }
        /* adjust the freeAfter as the position zero is reserved for the header */
        sendfrag->free_after <<= 1;

        /* adjust size and request offset to reflect actual number of bytes packed by convertor */
        size_out = sendfrag->frag_vec[1].iov_len;
    } else {
        size_out = size_in;
    }

    if(offset == 0) {
        hdr->hdr_common.hdr_type = (flags & MCA_PTL_FLAGS_ACK) ? MCA_PTL_HDR_TYPE_MATCH : MCA_PTL_HDR_TYPE_RNDV;
        hdr->hdr_common.hdr_flags = flags;
        hdr->hdr_match.hdr_contextid = sendreq->req_send.req_base.req_comm->c_contextid;
        hdr->hdr_match.hdr_src = sendreq->req_send.req_base.req_comm->c_my_rank;
        hdr->hdr_match.hdr_dst = sendreq->req_send.req_base.req_peer;
        hdr->hdr_match.hdr_tag = sendreq->req_send.req_base.req_tag;
        hdr->hdr_match.hdr_msg_length = sendreq->req_send.req_bytes_packed;
        hdr->hdr_match.hdr_msg_seq = sendreq->req_send.req_base.req_sequence;
        hdr->hdr_rndv.hdr_frag_length = size_out;
        hdr->hdr_rndv.hdr_src_ptr.lval = 0; /* for VALGRIND/PURIFY - REPLACE WITH MACRO */
        hdr->hdr_rndv.hdr_src_ptr.pval = sendfrag;
        if(ptl_peer->peer_nbo) {
            MCA_PTL_BASE_RNDV_HDR_HTON(hdr->hdr_rndv);
        }
    } else {
        hdr->hdr_common.hdr_type = MCA_PTL_HDR_TYPE_FRAG;
        hdr->hdr_common.hdr_flags = flags;
        hdr->hdr_frag.hdr_frag_offset = offset;
        hdr->hdr_frag.hdr_src_ptr.lval = 0; /* for VALGRIND/PURIFY - REPLACE WITH MACRO */
        hdr->hdr_frag.hdr_src_ptr.pval = sendfrag;
        hdr->hdr_frag.hdr_dst_ptr = sendreq->req_peer_match;
        hdr->hdr_frag.hdr_frag_length = size_out;
        if(ptl_peer->peer_nbo) {
            MCA_PTL_BASE_FRAG_HDR_HTON(hdr->hdr_frag);
        }
    }

    /* fragment state */
    sendfrag->frag_owner = &ptl_peer->peer_ptl->super;
    sendfrag->frag_send.frag_request = sendreq;
    sendfrag->frag_send.frag_base.frag_addr = sendfrag->frag_vec[1].iov_base;
    sendfrag->frag_send.frag_base.frag_size = size_out;
    
    sendfrag->frag_peer = ptl_peer;
    sendfrag->frag_vec_ptr = sendfrag->frag_vec;
    sendfrag->frag_vec_cnt = (size_out == 0) ? 1 : 2;
    sendfrag->frag_vec[0].iov_base = (ompi_iov_base_ptr_t)hdr;
    sendfrag->frag_vec[0].iov_len = sizeof(mca_ptl_base_header_t);
    sendfrag->frag_progressed = 0;
    *size = size_out;
    return OMPI_SUCCESS;
}


/*
 * The socket is setup as non-blocking, writes are handled asynchronously,
 * with event callbacks when the socket is ready for writes.
 */

bool mca_ptl_tcp_send_frag_handler(mca_ptl_tcp_send_frag_t* frag, int sd)
{
    int cnt=-1;
    size_t i, num_vecs;

    /* non-blocking write, but continue if interrupted */
    while(cnt < 0) {
        cnt = writev(sd, frag->frag_vec_ptr, frag->frag_vec_cnt);
        if(cnt < 0) {
            switch(ompi_socket_errno) {
            case EINTR:
                continue;
            case EWOULDBLOCK:
                /* ompi_output(0, "mca_ptl_tcp_send_frag_handler: EWOULDBLOCK\n"); */
                return false;
	    case EFAULT:
                ompi_output( 0, "mca_ptl_tcp_send_frag_handler: writev error (%p, %d)\n\t%s(%d)\n",
                             frag->frag_vec_ptr[0].iov_base, frag->frag_vec_ptr[0].iov_len,
                             strerror(ompi_socket_errno), frag->frag_vec_cnt );
            default:
                {
                ompi_output(0, "mca_ptl_tcp_send_frag_handler: writev failed with errno=%d", ompi_socket_errno);
                mca_ptl_tcp_peer_close(frag->frag_peer);
                return false;
                }
            }
        }
    }

#if MCA_PTL_TCP_STATISTICS
    ((mca_ptl_tcp_module_t*)frag->frag_owner)->ptl_bytes_sent += cnt;
    ((mca_ptl_tcp_module_t*)frag->frag_owner)->ptl_send_handler++;
#endif

    /* if the write didn't complete - update the iovec state */
    num_vecs = frag->frag_vec_cnt;
    for(i=0; i<num_vecs; i++) {
        if(cnt >= (int)frag->frag_vec_ptr->iov_len) {
            cnt -= frag->frag_vec_ptr->iov_len;
            if( frag->free_after & 1 ) {
               free( frag->frag_saved_vec.iov_base );
            }
            frag->frag_vec_ptr++;
            frag->frag_vec_cnt--;
            frag->frag_saved_vec = *frag->frag_vec_ptr;
            frag->free_after >>= 1;
        } else {
            frag->frag_vec_ptr->iov_base = (ompi_iov_base_ptr_t)
                (((unsigned char*)frag->frag_vec_ptr->iov_base) + cnt);
            frag->frag_vec_ptr->iov_len -= cnt;
            break;
        }
    }
    return (frag->frag_vec_cnt == 0);
}

