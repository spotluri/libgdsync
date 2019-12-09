/* Copyright (c) 2016, NVIDIA CORPORATION. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *  * Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *  * Neither the name of NVIDIA CORPORATION nor the names of its
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#if HAVE_CONFIG_H
#  include <config.h>
#endif /* HAVE_CONFIG_H */

#include <unistd.h>
#include <string.h>
#include <assert.h>

#include "gdsync.h"
#include "gdsync/mlx5.h"
#include "utils.hpp"
#include "memmgr.hpp"
//#include "mem.hpp"
#include "objs.hpp"
#include "utils.hpp"

#if 0
                union { uint64_t qw; uint32_t dw[2]; } db_val;
                db_val.qw = 0;
                db_val.dw[0] = desc->data32;
                db_val.dw[1] = desc->data32;
                mlx5_i->db_value = db_val.qw;
#endif

//-----------------------------------------------------------------------------

int gds_mlx5_get_send_descs(gds_mlx5_send_info_t *mlx5_i, const gds_send_request_t *request)
{
        int retcode = 0;
        size_t n_ops = request->commit.entries;
        peer_op_wr *op = request->commit.storage;
        size_t n = 0;

        memset(mlx5_i, 0, sizeof(*mlx5_i));

        for (; op && n < n_ops; op = op->next, ++n) {
                switch(op->type) {
                case IBV_EXP_PEER_OP_FENCE: {
                        gds_dbg("OP_FENCE: fence_flags=%" PRIu64 "\n", op->wr.fence.fence_flags);
                        uint32_t fence_op = (op->wr.fence.fence_flags & (IBV_EXP_PEER_FENCE_OP_READ|IBV_EXP_PEER_FENCE_OP_WRITE));
                        uint32_t fence_from = (op->wr.fence.fence_flags & (IBV_EXP_PEER_FENCE_FROM_CPU|IBV_EXP_PEER_FENCE_FROM_HCA));
                        uint32_t fence_mem = (op->wr.fence.fence_flags & (IBV_EXP_PEER_FENCE_MEM_SYS|IBV_EXP_PEER_FENCE_MEM_PEER));
                        if (fence_op == IBV_EXP_PEER_FENCE_OP_READ) {
                                gds_dbg("nothing to do for read fences\n");
                                break;
                        }
                        if (fence_from != IBV_EXP_PEER_FENCE_FROM_HCA) {
                                gds_err("unexpected from fence\n");
                                retcode = EINVAL;
                                break;
                        }
                        if (fence_mem == IBV_EXP_PEER_FENCE_MEM_PEER) {
                                gds_dbg("using light membar\n");
                                mlx5_i->membar = 1;
                        }
                        else if (fence_mem == IBV_EXP_PEER_FENCE_MEM_SYS) {
                                gds_dbg("using heavy membar\n");
                                mlx5_i->membar_full = 1;
                        }
                        else {
                                gds_err("unsupported fence combination\n");
                                retcode = EINVAL;
                                break;
                        }
                        break;
                }
                case IBV_EXP_PEER_OP_STORE_DWORD: {
                        CUdeviceptr dev_ptr = range_from_id(op->wr.dword_va.target_id)->dptr + 
                                op->wr.dword_va.offset;
                        uint32_t data = op->wr.dword_va.data;
                        gds_dbg("OP_STORE_DWORD dev_ptr=%" PRIx64 " data=%08x\n", (uint64_t)dev_ptr, data);
                        if (n != 0) {
                                gds_err("store DWORD is not 1st op\n");
                                retcode = EINVAL;
                                break;
                        }
                        mlx5_i->dbrec_ptr = (uint32_t*)dev_ptr;
                        mlx5_i->dbrec_value = data;
                        break;
                }
                case IBV_EXP_PEER_OP_STORE_QWORD: {
                        CUdeviceptr dev_ptr = range_from_id(op->wr.qword_va.target_id)->dptr +
                                op->wr.qword_va.offset;
                        uint64_t data = op->wr.qword_va.data;
                        gds_dbg("OP_STORE_QWORD dev_ptr=%" PRIx64 " data=%" PRIx64 "\n", (uint64_t)dev_ptr, (uint64_t)data);
                        if (n != 2) {
                                gds_err("store QWORD is not 3rd op\n");
                                retcode = EINVAL;
                                break;
                        }
                        mlx5_i->db_ptr = (uint64_t*)dev_ptr;
                        mlx5_i->db_value = data;
                        break;
                }
                case IBV_EXP_PEER_OP_COPY_BLOCK: {
                        CUdeviceptr dev_ptr = range_from_id(op->wr.copy_op.target_id)->dptr +
                                op->wr.copy_op.offset;
                        size_t len = op->wr.copy_op.len;
                        void *src = op->wr.copy_op.src;
                        gds_dbg("send inline detected\n");
                        if (len < 8 || len > 64) {
                                gds_err("unexpected len %zu\n", len);
                                retcode = EINVAL;
                                break;
                        }
                        mlx5_i->db_ptr = (uint64_t*)dev_ptr;
                        mlx5_i->db_value = *(uint64_t*)src; 
                        break;
                }
                case IBV_EXP_PEER_OP_POLL_AND_DWORD:
                case IBV_EXP_PEER_OP_POLL_GEQ_DWORD:
                case IBV_EXP_PEER_OP_POLL_NOR_DWORD: {
                        gds_err("unexpected polling op in send request\n");
                        retcode = EINVAL;
                        break;
                }
                default:
                        gds_err("undefined peer op type %d\n", op->type);
                        retcode = EINVAL;
                        break;
                }

                if (retcode) {
                        gds_err("error in fill func at entry n=%zu\n", n);
                        break;
                }
        }
        return retcode;
}

//-----------------------------------------------------------------------------

int gds_mlx5_get_send_info(int count, const gds_send_request_t *requests, gds_mlx5_send_info_t *mlx5_infos)
{
        int retcode = 0;

	for (int j=0; j<count; j++) {
                gds_mlx5_send_info *mlx5_i = mlx5_infos + j;
                const gds_send_request_t *request = requests + j;
                retcode = gds_mlx5_get_send_descs(mlx5_i, request);
                if (retcode) {
                        gds_err("error %d while retrieving descriptors for %dth request\n", retcode, j);
                        break;
                }
                gds_dbg("mlx5_i: dbrec={%p,%08x} db={%p,%" PRIx64 "}\n",
                        mlx5_i->dbrec_ptr, mlx5_i->dbrec_value, mlx5_i->db_ptr, mlx5_i->db_value);
	}

	return retcode;
}

//-----------------------------------------------------------------------------

int gds_mlx5_get_wait_descs(gds_mlx5_wait_info_t *mlx5_i, const gds_wait_request_t *request)
{
        int retcode = 0;
        size_t n_ops = request->peek.entries;
        peer_op_wr *op = request->peek.storage;
        size_t n = 0;

        memset(mlx5_i, 0, sizeof(*mlx5_i));

        for (; op && n < n_ops; op = op->next, ++n) {
                switch(op->type) {
                case IBV_EXP_PEER_OP_FENCE: {
                        gds_dbg("OP_FENCE: fence_flags=%" PRIu64 "\n", op->wr.fence.fence_flags);
                        uint32_t fence_op = (op->wr.fence.fence_flags & (IBV_EXP_PEER_FENCE_OP_READ|IBV_EXP_PEER_FENCE_OP_WRITE));
                        uint32_t fence_from = (op->wr.fence.fence_flags & (IBV_EXP_PEER_FENCE_FROM_CPU|IBV_EXP_PEER_FENCE_FROM_HCA));
                        uint32_t fence_mem = (op->wr.fence.fence_flags & (IBV_EXP_PEER_FENCE_MEM_SYS|IBV_EXP_PEER_FENCE_MEM_PEER));
                        if (fence_op == IBV_EXP_PEER_FENCE_OP_READ) {
                                gds_dbg("nothing to do for read fences\n");
                                break;
                        }
                        if (fence_from != IBV_EXP_PEER_FENCE_FROM_HCA) {
                                gds_err("unexpected from fence\n");
                                retcode = EINVAL;
                                break;
                        }
                        gds_err("unsupported fence combination\n");
                        retcode = EINVAL;
                        break;
                }
                case IBV_EXP_PEER_OP_STORE_DWORD: {
                        CUdeviceptr dev_ptr = range_from_id(op->wr.dword_va.target_id)->dptr + 
                                op->wr.dword_va.offset;
                        uint32_t data = op->wr.dword_va.data;
                        gds_dbg("OP_STORE_DWORD dev_ptr=%" PRIx64 " data=%08x\n", (uint64_t)dev_ptr, data);
                        if (n != 1) {
                                gds_err("store DWORD is not 2nd op\n");
                                retcode = EINVAL;
                                break;
                        }
                        mlx5_i->flag_ptr = (uint32_t*)dev_ptr;
                        mlx5_i->flag_value = data;
                        break;
                }
                case IBV_EXP_PEER_OP_STORE_QWORD: {
                        CUdeviceptr dev_ptr = range_from_id(op->wr.qword_va.target_id)->dptr +
                                op->wr.qword_va.offset;
                        uint64_t data = op->wr.qword_va.data;
                        gds_dbg("OP_STORE_QWORD dev_ptr=%" PRIx64 " data=%" PRIx64 "\n", (uint64_t)dev_ptr, (uint64_t)data);
                        gds_err("unsupported QWORD op\n");
                        retcode = EINVAL;
                        break;
                }
                case IBV_EXP_PEER_OP_COPY_BLOCK: {
                        CUdeviceptr dev_ptr = range_from_id(op->wr.copy_op.target_id)->dptr +
                                op->wr.copy_op.offset;
                        size_t len = op->wr.copy_op.len;
                        void *src = op->wr.copy_op.src;
                        gds_err("unsupported COPY_BLOCK\n");
                        retcode = EINVAL;
                        break;
                }
                case IBV_EXP_PEER_OP_POLL_AND_DWORD:
                case IBV_EXP_PEER_OP_POLL_GEQ_DWORD:
                case IBV_EXP_PEER_OP_POLL_NOR_DWORD: {
                        CUdeviceptr dev_ptr = range_from_id(op->wr.dword_va.target_id)->dptr + 
                                op->wr.dword_va.offset;
                        uint32_t data = op->wr.dword_va.data;

                        gds_dbg("OP_POLL_DWORD dev_ptr=%" PRIx64 " data=%08x\n", (uint64_t)dev_ptr, data);

                        mlx5_i->cqe_ptr = (uint32_t *)dev_ptr;
                        mlx5_i->cqe_value = data;

                        switch(op->type) {
                        case IBV_EXP_PEER_OP_POLL_NOR_DWORD:
                                // GPU SMs can always do NOR
                                mlx5_i->cond = GDS_WAIT_COND_NOR;
                                break;
                        case IBV_EXP_PEER_OP_POLL_GEQ_DWORD:
                                mlx5_i->cond = GDS_WAIT_COND_GEQ;
                                break;
                        case IBV_EXP_PEER_OP_POLL_AND_DWORD:
                                mlx5_i->cond = GDS_WAIT_COND_AND;
                                break;
                        default:
                                gds_err("unexpected op type\n");
                                retcode = EINVAL;
                                goto err;
                        }
                        break;
                }
                default:
                        gds_err("undefined peer op type %d\n", op->type);
                        retcode = EINVAL;
                        break;
                }
        err:
                if (retcode) {
                        gds_err("error in fill func at entry n=%zu\n", n);
                        break;
                }
        }
        return retcode;
}

//-----------------------------------------------------------------------------

int gds_mlx5_get_wait_info(int count, const gds_wait_request_t *requests, gds_mlx5_wait_info_t *mlx5_infos)
{
        int retcode = 0;

	for (int j=0; j<count; j++) {
                gds_mlx5_wait_info *mlx5_i = mlx5_infos + j;
                const gds_wait_request_t *request = requests + j;
                retcode = gds_mlx5_get_wait_descs(mlx5_i, request);
                if (retcode) {
                        gds_err("error %d while retrieving descriptors for %dth request\n", retcode, j);
                        break;
                }
                gds_dbg("wait[%d] cqe_ptr=%p cqe_value=0x%08x flag_ptr=%p flag_value=0x%08x\n", 
                        j, mlx5_i->cqe_ptr, mlx5_i->cqe_value, mlx5_i->flag_ptr, mlx5_i->flag_value);
        }

        return retcode;
}

//-----------------------------------------------------------------------------

int gds_mlx5_get_dword_wait_info(uint32_t *ptr, uint32_t value, int flags, gds_mlx5_dword_wait_info_t *mlx5_info)
{
        int retcode = 0;
        CUdeviceptr dev_ptr = 0;

        assert(NULL != ptr);
        //assert((((unsigned long)ptr) & 0x3) == 0);
        memset(mlx5_info, 0, sizeof(&mlx5_info));

        retcode = gds_map_mem(ptr, sizeof(*ptr), memtype_from_flags(flags), &dev_ptr);
        if (retcode) {
                gds_err("error %d while mapping addr %p\n", retcode, ptr);
                goto out;
        }

        gds_dbg("dev_ptr=%llx value=%08x\n", dev_ptr, value);
        mlx5_info->ptr = (uint32_t*)dev_ptr;
        mlx5_info->value = value;
out:
        return retcode;
}

//-----------------------------------------------------------------------------

void gds_mlx5_get_send_device_info(gds_mlx5_send_info_t* mlx5_sinfo, gdsync::send_desc_t *sinfo)
{
    assert (sinfo != NULL);
    assert (mlx5_sinfo != NULL);

    sinfo->dbrec.ptr   = mlx5_sinfo->dbrec_ptr;
    sinfo->dbrec.value = mlx5_sinfo->dbrec_value;
    sinfo->db.ptr      = mlx5_sinfo->db_ptr;
    sinfo->db.value    = mlx5_sinfo->db_value;
}

//-----------------------------------------------------------------------------

void gds_mlx5_get_wait_device_info(gds_mlx5_wait_info_t* mlx5_winfo, gdsync::wait_desc_t *sinfo)
{
    assert(winfo != NULL); 
    assert(mlx5_winfo != NULL); 
		    
    winfo->sema_cond  = mlx5_winfo.cond;
    winfo->sema.ptr   = mlx5_winfo.cqe_ptr;
    winfo->sema.value = mlx5_winfo.cqe_value;
    winfo->flag.ptr   = mlx5_winfo.flag_ptr;
    winfo->flag.value = mlx5_winfo.flag_value;

out:
    return ret;
}

//-----------------------------------------------------------------------------

/*
 * Local variables:
 *  c-indent-level: 8
 *  c-basic-offset: 8
 *  tab-width: 8
 *  indent-tabs-mode: nil
 * End:
 */
