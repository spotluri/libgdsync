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

#include <gdsync.h>

#include "utils.hpp"
#include "memmgr.hpp"
#include "mem.hpp"
#include "objs.hpp"
#include "archutils.h"
#include "mlnxutils.h"

//-----------------------------------------------------------------------------

int gds_dbg_enabled()
{
        static int gds_dbg_is_enabled = -1;
        if (-1 == gds_dbg_is_enabled) {
                const char *env = getenv("GDS_ENABLE_DEBUG");
                if (env) {
                        int en = atoi(env);
                        gds_dbg_is_enabled = !!en;
                        //printf("GDS_ENABLE_DEBUG=%s\n", env);
                } else
                        gds_dbg_is_enabled = 0;
        }
        return gds_dbg_is_enabled;
}

//-----------------------------------------------------------------------------
// detect Async APIs

#if HAVE_DECL_CU_STREAM_MEM_OP_WRITE_VALUE_64
#warning "enabling write_64 extensions"
#define GDS_HAS_WRITE64     1
#else 
#define GDS_HAS_WRITE64     0
#endif

#if HAVE_DECL_CU_STREAM_MEM_OP_INLINE_COPY
#warning "enabling inline_copy extensions"
#define GDS_HAS_INLINE_COPY 1
#else 
#define GDS_HAS_INLINE_COPY 0
#endif

#if HAVE_DECL_CU_STREAM_BATCH_MEM_OP_CONSISTENCY_WEAK
#warning "enabling consistency extensions"
#define GDS_HAS_WEAK_API    1
#else
#define GDS_HAS_WEAK_API    0
#endif

#if HAVE_DECL_CU_STREAM_MEM_OP_MEMORY_BARRIER
#warning "enabling memory barrier extensions"
#define GDS_HAS_MEMBAR      1
#else
#define GDS_HAS_MEMBAR      0
#endif

// TODO: use corret value
// TODO: make it dependent upon the particular GPU
const size_t GDS_GPU_MAX_INLINE_SIZE = 256;

//-----------------------------------------------------------------------------

// Note: inlcpy has precedence
//bool gds_has_inlcpy = GDS_HAS_INLINE_COPY;
//bool gds_has_write64 = GDS_HAS_WRITE64;
//bool gds_has_weak_consistency = GDS_HAS_WEAK_API;
//bool gds_has_membar = GDS_HAS_MEMBAR;

static bool gpu_does_support_nor(gds_peer *peer) { return false; }

//-----------------------------------------------------------------------------

// BUG: this feature is GPU device dependent
static bool gds_enable_write64()
{
        static int gds_disable_write64 = -1;
        if (-1 == gds_disable_write64) {
                const char *env = getenv("GDS_DISABLE_WRITE64");
                if (env)
                        gds_disable_write64 = !!atoi(env);
                else
                        gds_disable_write64 = 0;
                gds_dbg("GDS_DISABLE_WRITE64=%d\n", gds_disable_write64);
        }
        // BUG: need to query device property for write64 capability
        //return GDS_HAS_WRITE64 && !gds_disable_write64;
        return false;
}

static bool gds_enable_inlcpy()
{
        static int gds_disable_inlcpy = -1;
        if (-1 == gds_disable_inlcpy) {
                const char *env = getenv("GDS_DISABLE_INLINECOPY");
                if (env)
                        gds_disable_inlcpy = !!atoi(env);
                else
                        gds_disable_inlcpy = 0;
                gds_dbg("GDS_DISABLE_INLINECOPY=%d\n", gds_disable_inlcpy);
        }
        return GDS_HAS_INLINE_COPY && !gds_disable_inlcpy;
}

static bool gds_simulate_write64()
{
        static int gds_simulate_write64 = -1;
        if (-1 == gds_simulate_write64) {
                const char *env = getenv("GDS_SIMULATE_WRITE64");
                if (env)
                        gds_simulate_write64 = !!atoi(env);
                else
                        gds_simulate_write64 = 0; // default
                gds_dbg("GDS_SIMULATE_WRITE64=%d\n", gds_simulate_write64);

                if (gds_simulate_write64 && gds_enable_inlcpy()) {
                        gds_warn("INLINECOPY has priority over SIMULATE_WRITE64, using the former\n");
                        gds_simulate_write64 = 0;
                }
        }
        // simulate 64-bits writes with inlcpy
        return GDS_HAS_INLINE_COPY && gds_simulate_write64;
}

static bool gds_enable_membar()
{
        static int gds_disable_membar = -1;
        if (-1 == gds_disable_membar) {
                const char *env = getenv("GDS_DISABLE_MEMBAR");
                if (env)
                        gds_disable_membar = !!atoi(env);
                else
                        gds_disable_membar = 0;
                gds_dbg("GDS_DISABLE_MEMBAR=%d\n", gds_disable_membar);
        }
        return GDS_HAS_MEMBAR && !gds_disable_membar;
}

static bool gds_enable_weak_consistency()
{
        static int gds_disable_weak_consistency = -1;
        if (-1 == gds_disable_weak_consistency) {
            const char *env = getenv("GDS_DISABLE_WEAK_CONSISTENCY");
            if (env)
                    gds_disable_weak_consistency = !!atoi(env);
            else
                    gds_disable_weak_consistency = 1; // disabled by default
            gds_dbg("GDS_DISABLE_WEAK_CONSISTENCY=%d\n", gds_disable_weak_consistency);
        }
        return GDS_HAS_WEAK_API && !gds_disable_weak_consistency;
}

//-----------------------------------------------------------------------------

static bool gds_enable_dump_memops()
{
        static int gds_enable_dump_memops = -1;
        if (-1 == gds_enable_dump_memops) {
            const char *env = getenv("GDS_ENABLE_DUMP_MEMOPS");
            if (env)
                    gds_enable_dump_memops = !!atoi(env);
            else
                    gds_enable_dump_memops = 0; // disabled by default
            gds_dbg("GDS_ENABLE_DUMP_MEMOPS=%d\n", gds_enable_dump_memops);
        }
        return gds_enable_dump_memops;
}

void gds_dump_param(CUstreamBatchMemOpParams *param)
{
        switch(param->operation) {
        case CU_STREAM_MEM_OP_WAIT_VALUE_32:
                gds_info("WAIT32 addr:%p alias:%p value:%08x flags:%08x\n",
                        (void*)param->waitValue.address,
                        (void*)param->writeValue.alias,
                        param->waitValue.value,
                        param->waitValue.flags);
                break;

        case CU_STREAM_MEM_OP_WRITE_VALUE_32:
                gds_info("WRITE32 addr:%p alias:%p value:%08x flags:%08x\n",
                        (void*)param->writeValue.address,
                        (void*)param->writeValue.alias,
                        param->writeValue.value,
                        param->writeValue.flags);
                break;

        case CU_STREAM_MEM_OP_FLUSH_REMOTE_WRITES:
                gds_dbg("FLUSH\n");
                break;

#if GDS_HAS_INLINE_COPY
        case CU_STREAM_MEM_OP_INLINE_COPY:
                gds_info("INLINECOPY addr:%p alias:%p src:%p len=%zu flags:%08x\n",
                        (void*)param->inlineCopy.address,
                        (void*)param->inlineCopy.alias,
                        (void*)param->inlineCopy.srcData,
                        param->inlineCopy.byteCount,
                        param->inlineCopy.flags);
                break;
#endif

#if GDS_HAS_MEMBAR
        case CU_STREAM_MEM_OP_MEMORY_BARRIER:
                gds_info("MEMORY_BARRIER flags:%08x\n",
                        param->memoryBarrier.flags);
                break;
#endif
        default:
                gds_err("unsupported operation=%d\n", param->operation);
                break;
        }
}

//-----------------------------------------------------------------------------

void gds_dump_params(unsigned int nops, CUstreamBatchMemOpParams *params)
{
        for (unsigned int n = 0; n < nops; ++n) {
                CUstreamBatchMemOpParams *param = params + n;
                gds_info("param[%d]:\n", n);
                gds_dump_param(param);
        }
}

//-----------------------------------------------------------------------------

static int gds_fill_membar(CUstreamBatchMemOpParams *param, int flags)
{
        int retcode = 0;
#if GDS_HAS_MEMBAR
        if (flags & GDS_MEMBAR_FLUSH_REMOTE) {
                param->operation = CU_STREAM_MEM_OP_FLUSH_REMOTE_WRITES;
                param->flushRemoteWrites.flags = 0;
                gds_dbg("op=%d flush_remote flags=%08x\n",
                        param->operation,
                        param->flushRemoteWrites.flags);
        } else {
                if (flags & GDS_MEMBAR_DEFAULT) {
                        param->operation = CU_STREAM_MEM_OP_MEMORY_BARRIER;
                        param->memoryBarrier.flags = CU_STREAM_MEMORY_BARRIER_DEFAULT;
                } else if (flags & GDS_MEMBAR_SYS) {
                        param->operation = CU_STREAM_MEM_OP_MEMORY_BARRIER;
                        param->memoryBarrier.flags = CU_STREAM_MEMORY_BARRIER_SYS;
                } else {
                        gds_err("error, unsupported membar\n");
                        retcode = EINVAL;
                        goto out;
                }
                gds_dbg("op=%d membar flags=%08x\n",
                        param->operation,
                        param->memoryBarrier.flags);
        }
out:
#else
        gds_err("error, inline copy is unsupported\n");
        retcode = EINVAL;
#endif
        return retcode;
}

//-----------------------------------------------------------------------------

static int gds_fill_inlcpy(CUstreamBatchMemOpParams *param, CUdeviceptr addr, void *data, size_t n_bytes, int flags)
{
        int retcode = 0;
#if GDS_HAS_INLINE_COPY
        CUdeviceptr dev_ptr = addr;

        assert(addr);
        assert(n_bytes > 0);
        // TODO:
        //  verify address requirements of inline_copy
        //assert((((unsigned long)addr) & 0x3) == 0); 

        bool need_barrier       = (flags  & GDS_IMMCOPY_POST_TAIL_FLUSH  ) ? true : false;

        param->operation = CU_STREAM_MEM_OP_INLINE_COPY;
        param->inlineCopy.byteCount = n_bytes;
        param->inlineCopy.srcData = data;
        param->inlineCopy.address = dev_ptr;
        param->inlineCopy.flags = CU_STREAM_INLINE_COPY_NO_MEMORY_BARRIER;
        if (need_barrier)
                param->inlineCopy.flags = 0;
        gds_dbg("op=%d addr=%p src=%p size=%zd flags=%08x\n",
                param->operation,
                (void*)param->inlineCopy.address,
                param->inlineCopy.srcData,
                param->inlineCopy.byteCount,
                param->inlineCopy.flags);
#else
        gds_err("error, inline copy is unsupported\n");
        retcode = EINVAL;
#endif
        return retcode;
}

static int gds_fill_inlcpy(CUstreamBatchMemOpParams *param, void *ptr, void *data, size_t n_bytes, int flags)
{
        int retcode = 0;
        CUdeviceptr dev_ptr = 0;
        retcode = gds_map_mem(ptr, n_bytes, memtype_from_flags(flags), &dev_ptr);
        if (retcode) {
                gds_err("could not lookup %p\n", ptr);
                goto out;
        }

        retcode = gds_fill_inlcpy(param, dev_ptr, data, n_bytes, flags);
out:
        return retcode;
}

//-----------------------------------------------------------------------------

static void gds_enable_barrier_for_inlcpy(CUstreamBatchMemOpParams *param)
{
#if GDS_HAS_INLINE_COPY
        assert(param->operation == CU_STREAM_MEM_OP_INLINE_COPY);
        param->inlineCopy.flags &= ~CU_STREAM_INLINE_COPY_NO_MEMORY_BARRIER;
#endif
}

//-----------------------------------------------------------------------------

static int gds_fill_poke(CUstreamBatchMemOpParams *param, CUdeviceptr addr, uint32_t value, int flags)
{
        int retcode = 0;
        CUdeviceptr dev_ptr = addr;

        // TODO: convert into errors
        assert(addr);
        assert((((unsigned long)addr) & 0x3) == 0); 

        bool need_barrier = (flags  & GDS_POKE_POST_PRE_BARRIER ) ? true : false;

        param->operation = CU_STREAM_MEM_OP_WRITE_VALUE_32;
        param->writeValue.address = dev_ptr;
        param->writeValue.value = value;
        param->writeValue.flags = CU_STREAM_WRITE_VALUE_NO_MEMORY_BARRIER;
        if (need_barrier)
                param->writeValue.flags = 0;
        gds_dbg("op=%d addr=%p value=%08x flags=%08x\n",
                param->operation,
                (void*)param->writeValue.address,
                param->writeValue.value,
                param->writeValue.flags);

        return retcode;
}

static int gds_fill_poke(CUstreamBatchMemOpParams *param, uint32_t *ptr, uint32_t value, int flags)
{
        int retcode = 0;
        CUdeviceptr dev_ptr = 0;

        gds_dbg("addr=%p value=%08x flags=%08x\n", ptr, value, flags);

        retcode = gds_map_mem(ptr, sizeof(*ptr), memtype_from_flags(flags), &dev_ptr);
        if (retcode) {
                gds_err("error %d while looking up %p\n", retcode, ptr);
                goto out;
        }

        retcode = gds_fill_poke(param, dev_ptr, value, flags);
out:
        return retcode;
}

//-----------------------------------------------------------------------------

static int gds_fill_poll(CUstreamBatchMemOpParams *param, CUdeviceptr ptr, uint32_t magic, int cond_flag, int flags)
{
        int retcode = 0;
        const char *cond_str = NULL;
        CUdeviceptr dev_ptr = ptr;

        assert(ptr);
        assert((((unsigned long)ptr) & 0x3) == 0);

        bool need_flush = (flags & GDS_POLL_POST_FLUSH) ? true : false;

        param->operation = CU_STREAM_MEM_OP_WAIT_VALUE_32;
        param->waitValue.address = dev_ptr;
        param->waitValue.value = magic;
        switch(cond_flag) {
        case GDS_POLL_COND_GEQ:
                param->waitValue.flags = CU_STREAM_WAIT_VALUE_GEQ;
                cond_str = "CU_STREAM_WAIT_VALUE_GEQ";
                break;
        case GDS_POLL_COND_EQ:
                param->waitValue.flags = CU_STREAM_WAIT_VALUE_EQ;
                cond_str = "CU_STREAM_WAIT_VALUE_EQ";
                break;
        case GDS_POLL_COND_AND:
                param->waitValue.flags = CU_STREAM_WAIT_VALUE_AND;
                cond_str = "CU_STREAM_WAIT_VALUE_AND";
                break;
        default: 
                gds_err("invalid wait condition flag\n");
                retcode = EINVAL;
                goto out;
        }
        if (need_flush)
                param->waitValue.flags |= CU_STREAM_WAIT_VALUE_FLUSH;
        gds_dbg("op=%d addr=%p value=%08x cond=%s flags=%08x\n",
                param->operation,
                (void*)param->waitValue.address,
                param->waitValue.value,
                cond_str,
                param->waitValue.flags);
out:
        return retcode;
}

int gds_fill_poll(CUstreamBatchMemOpParams *param, uint32_t *ptr, uint32_t magic, int cond_flag, int flags)
{
        int retcode = 0;
        CUdeviceptr dev_ptr = 0;

        gds_dbg("addr=%p value=%08x cond=%08x flags=%08x\n", ptr, magic, cond_flag, flags);

        retcode = gds_map_mem(ptr, sizeof(*ptr), memtype_from_flags(flags), &dev_ptr);
        if (retcode) {
                gds_err("could not lookup %p\n", ptr);
                goto out;
        }

        retcode = gds_fill_poll(param, dev_ptr, magic, cond_flag, flags);
out:
        return retcode;
}

//-----------------------------------------------------------------------------

int gds_stream_batch_ops(CUstream stream, int nops, CUstreamBatchMemOpParams *params, int flags)
{
        CUresult result = CUDA_SUCCESS;
        int retcode = 0;
        unsigned int cuflags = 0;
#if GDS_HAS_WEAK_API
        cuflags |= gds_enable_weak_consistency() ? CU_STREAM_BATCH_MEM_OP_CONSISTENCY_WEAK : 0;
#endif
        gds_dbg("nops=%d flags=%08x\n", nops, cuflags);
#if 0
        // temporarily disabled, see below
        if (gds_enable_dump_memops()) {
                gds_info("nops=%d flags=%08x\n", nops, cuflags);
                gds_dump_params(nops, params);
        }
#endif
        if (nops > 256) {
                gds_warn("batch size might be too big, stream=%p nops=%d params=%p flags=%08x\n", stream, nops, params, flags);
                //return EINVAL;
        }

#if 0
        int batch_size = 1;
        {
                int j = 100;
                while (--j) {
                        result = cuStreamQuery(stream);
                        //result = cuStreamSynchronize(stream);
                        if (CUDA_SUCCESS != result && CUDA_ERROR_NOT_READY != result) {
                                const char *err_str = NULL;
                                cuGetErrorString(result, &err_str);
                                gds_err("got CUDA result %d (%s) while pre-checking stream\n", result, err_str);
                                retcode = gds_curesult_to_errno(result);
                                goto out;
                        }
                        usleep(100);
                }
        }
        gds_dbg("chopping batch with max batch_size=%d\n", batch_size);
        for(int i=0; i < nops; i += batch_size) {
                result = cuStreamBatchMemOp(stream, batch_size, params+i, cuflags);
                if (CUDA_SUCCESS != result) {
                        const char *err_str = NULL;
                        cuGetErrorString(result, &err_str);
                        gds_err("got CUDA result %d (%s) while submitting batch operations:\n", result, err_str);
                        retcode = gds_curesult_to_errno(result);
                }
                if (!retcode) { 
                        int j = 100;
                        while (--j) {
                                result = cuStreamQuery(stream);
                                //result = cuStreamSynchronize(stream);
                                if (CUDA_SUCCESS != result && CUDA_ERROR_NOT_READY != result) {
                                        const char *err_str = NULL;
                                        cuGetErrorString(result, &err_str);
                                        gds_err("got CUDA result %d (%s) while checking stream\n", result, err_str);
                                        retcode = gds_curesult_to_errno(result);
                                        break;
                                }
                                usleep(100);
                        }
                }
                // moved here to be able to dump .alias field too
                if (retcode || gds_enable_dump_memops()) {
                        gds_info("last submited batch was: stream=0x%x nops=%d flags=%08x\n", stream, batch_size, cuflags);
                        gds_dump_params(batch_size, params+i);
                }
                if (retcode)
                        goto out;
        }
#else
        result = cuStreamBatchMemOp(stream, nops, params, cuflags);
	if (CUDA_SUCCESS != result) {
                const char *err_str = NULL;
                cuGetErrorString(result, &err_str);
		gds_err("got CUDA result %d (%s) while submitting batch operations:\n", result, err_str);
                retcode = gds_curesult_to_errno(result);
                gds_err("nops=%d flags=%08x\n", nops, cuflags);
                gds_dump_params(nops, params);
                goto out;
	}
        // moved here to be able to dump .alias field too
        if (gds_enable_dump_memops()) {
                gds_info("nops=%d flags=%08x\n", nops, cuflags);
                gds_dump_params(nops, params);
        }
#if 0
        // optional debugging aid
        result = cuStreamQuery(stream);
        if (CUDA_SUCCESS != result && CUDA_ERROR_NOT_READY != result) {
                const char *err_str = NULL;
                cuGetErrorString(result, &err_str);
		gds_err("got CUDA result %d (%s) while checking stream\n", result, err_str);
                retcode = gds_curesult_to_errno(result);
                goto out;
        }
#endif
#endif

out:        
        return retcode;
}

//-----------------------------------------------------------------------------

int gds_stream_post_poll_dword(CUstream stream, uint32_t *ptr, uint32_t magic, int cond_flags, int flags)
{
        int retcode = 0;
	CUstreamBatchMemOpParams param[1];
        retcode = gds_fill_poll(param, ptr, magic, cond_flags, flags);
        if (retcode) {
                gds_err("error in fill_poll\n");
                goto out;
        }
        retcode = gds_stream_batch_ops(stream, 1, param, 0);
        if (retcode) {
                gds_err("error in batch_ops\n");
                goto out;
        }
out:
        return retcode;
}

//-----------------------------------------------------------------------------

int gds_stream_post_poke_dword(CUstream stream, uint32_t *ptr, uint32_t value, int flags)
{
        int retcode = 0;
	CUstreamBatchMemOpParams param[1];
        retcode = gds_fill_poke(param, ptr, value, flags);
        if (retcode) {
                gds_err("error in fill_poke\n");
                goto out;
        }
        retcode = gds_stream_batch_ops(stream, 1, param, 0);
        if (retcode) {
                gds_err("error in batch_ops\n");
                goto out;
        }
out:
        return retcode;
}

//-----------------------------------------------------------------------------

int gds_stream_post_inline_copy(CUstream stream, void *ptr, void *src, size_t nbytes, int flags)
{
        int retcode = 0;
	CUstreamBatchMemOpParams param[1];

        retcode = gds_fill_inlcpy(param, ptr, src, nbytes, flags);
        if (retcode) {
                gds_err("error in fill_poke\n");
                goto out;
        }
        retcode = gds_stream_batch_ops(stream, 1, param, 0);
        if (retcode) {
                gds_err("error in batch_ops\n");
                goto out;
        }
out:
        return retcode;
}

//-----------------------------------------------------------------------------

/*
  A) plain+membar:
  WR32
  MEMBAR
  WR32
  WR32

  B) plain:
  WR32
  WR32+PREBARRIER
  WR32

  C) sim64+membar:
  WR32
  MEMBAR
  INLCPY 8B

  D) sim64:
  INLCPY 4B + POSTBARRIER
  INLCPY 8B

  E) inlcpy+membar:
  WR32
  MEMBAR
  INLCPY XB

  F) inlcpy:
  INLCPY 4B + POSTBARRIER
  INLCPY 128B
*/

static inline uint32_t gds_qword_lo(uint64_t v) {
        return (uint32_t)(v);
}
static inline uint32_t gds_qword_hi(uint64_t v) {
        return (uint32_t)(v >> 32);
}

enum {
        GDS_POST_OPS_DISCARD_WAIT_FLUSH = 1<<0
};

static int gds_post_ops(size_t n_ops, struct peer_op_wr *op, CUstreamBatchMemOpParams *params, int &idx, int post_flags = 0)
{
        int retcode = 0;
        size_t n = 0;
        bool prev_was_fence = false;
        bool use_inlcpy_for_dword = false;

        gds_dbg("n_ops=%zu idx=%d\n", n_ops, idx);

        // divert the request to the same engine handling 64bits
        // to avoid out-of-order execution
        // caveat: can't use membar if inlcpy is used for 4B writes (to simulate 8B writes)
        if (gds_enable_inlcpy()) {
                if (!gds_enable_membar())
                        use_inlcpy_for_dword = true; // F
        }
        if (gds_simulate_write64()) {
                if (!gds_enable_membar()) {
                        gds_warn_once("enabling use_inlcpy_for_dword\n");
                        use_inlcpy_for_dword = true; // D
                }
        }

        for (; op && n < n_ops; op = op->next, ++n) {
                //int flags = 0;
                gds_dbg("op[%zu] type:%08x\n", n, op->type);
                switch(op->type) {
                case IBV_PEER_OP_FENCE: {
                        gds_dbg("OP_FENCE: fence_flags=%"PRIu64"\n", op->wr.fence.fence_flags);
                        uint32_t fence_op = (op->wr.fence.fence_flags & (IBV_EXP_PEER_FENCE_OP_READ|IBV_EXP_PEER_FENCE_OP_WRITE));
                        uint32_t fence_from = (op->wr.fence.fence_flags & (IBV_EXP_PEER_FENCE_FROM_CPU|IBV_EXP_PEER_FENCE_FROM_HCA));
                        uint32_t fence_mem = (op->wr.fence.fence_flags & (IBV_EXP_PEER_FENCE_MEM_SYS|IBV_EXP_PEER_FENCE_MEM_PEER));

                        if (fence_op == IBV_EXP_PEER_FENCE_OP_READ) {
                                gds_dbg("nothing to do for read fences\n");
                                //retcode = EINVAL;
                                break;
                        }
                        else {
                                if (!gds_enable_membar()) {
                                        if (use_inlcpy_for_dword) {
                                                assert(idx-1 >= 0);
                                                gds_dbg("patching previous param\n");
                                                gds_enable_barrier_for_inlcpy(params+idx-1);
                                        }
                                        else {
                                                gds_dbg("recording fence event\n");
                                                prev_was_fence = true;
                                        }
                                        //retcode = 0;
                                }
                                else {
                                        if (fence_from != IBV_EXP_PEER_FENCE_FROM_HCA) {
                                                gds_err("unexpected from fence\n");
                                                retcode = EINVAL;
                                                break;
                                        }
                                        int flags = 0;
                                        if (fence_mem == IBV_EXP_PEER_FENCE_MEM_PEER) {
                                                gds_dbg("using light membar\n");
                                                flags = GDS_MEMBAR_DEFAULT;
                                        }
                                        else if (fence_mem == IBV_EXP_PEER_FENCE_MEM_SYS) {
                                                gds_dbg("using heavy membar\n");
                                                flags = GDS_MEMBAR_SYS;
                                        }
                                        else {
                                                gds_err("unsupported fence combination\n");
                                                retcode = EINVAL;
                                                break;
                                        }
                                        retcode = gds_fill_membar(params+idx, flags);
                                        ++idx;
                                }
                        }
                        break;
                }
                case IBV_PEER_OP_STORE_DWORD: {
                        CUdeviceptr dev_ptr = range_from_id(op->wr.dword_va.target_id)->dptr + 
                                op->wr.dword_va.offset;
                        uint32_t data = op->wr.dword_va.data;
                        int flags = 0;
                        gds_dbg("OP_STORE_DWORD dev_ptr=%llx data=%"PRIx32"\n", dev_ptr, data);
                        if (use_inlcpy_for_dword) { // F || D
                                // membar may be out of order WRT inlcpy
                                if (gds_enable_membar()) {
                                        gds_err("invalid feature combination, inlcpy + membar\n");
                                        retcode = EINVAL;
                                        break;
                                }
                                // tail flush is set when following fence is met
                                //  flags |= GDS_IMMCOPY_POST_TAIL_FLUSH;
                                retcode = gds_fill_inlcpy(params+idx, dev_ptr, &data, sizeof(data), flags);
                                ++idx;
                        }
                        else {  // A || B || C || E
                                // can't guarantee ordering of write32+inlcpy unless
                                // a membar is there
                                // TODO: fix driver when !weak
                                if (gds_enable_inlcpy() && !gds_enable_membar()) {
                                        gds_err("invalid feature combination, inlcpy needs membar\n");
                                        retcode = EINVAL;
                                        break;
                                }
                                if (prev_was_fence) {
                                        gds_dbg("using PRE_BARRIER as fence\n");
                                        flags |= GDS_POKE_POST_PRE_BARRIER;
                                        prev_was_fence = false;
                                }
                                retcode = gds_fill_poke(params+idx, dev_ptr, data, flags);
                                ++idx;
                        }
                        break;
                }
                case IBV_PEER_OP_STORE_QWORD: {
                        CUdeviceptr dev_ptr = range_from_id(op->wr.qword_va.target_id)->dptr +
                                op->wr.qword_va.offset;
                        uint64_t data = op->wr.qword_va.data;
                        int flags = 0;
                        gds_dbg("OP_STORE_QWORD dev_ptr=%llx data=%"PRIx64"\n", dev_ptr, data);
                        // C || D
                        if (gds_enable_write64()) {
                                gds_err("write64 is not supported\n");
                                retcode = EINVAL;
                                break;
                        }

                        // simulate 64-bit poke by inline copy

                        if (gds_simulate_write64()){
                                if (!gds_enable_membar()) {
                                        gds_err("invalid feature combination, inlcpy needs membar\n");
                                        retcode = EINVAL;
                                        break;
                                }

                                // tail flush is never useful here
                                //flags |= GDS_IMMCOPY_POST_TAIL_FLUSH;
                                retcode = gds_fill_inlcpy(params+idx, dev_ptr, &data, sizeof(data), flags);
                                ++idx;
                        }
                        else {
                                uint32_t datalo = gds_qword_lo(op->wr.qword_va.data);
                                uint32_t datahi = gds_qword_hi(op->wr.qword_va.data);

                                if (prev_was_fence) {
                                        gds_dbg("enabling PRE_BARRIER\n");
                                        flags |= GDS_POKE_POST_PRE_BARRIER;
                                        prev_was_fence = false;
                                }
                                retcode = gds_fill_poke(params+idx, dev_ptr, datalo, flags);
                                ++idx;

                                // get rid of the barrier, if there
                                flags &= ~GDS_POKE_POST_PRE_BARRIER;

                                // advance to next DWORD
                                dev_ptr += sizeof(uint32_t);
                                retcode = gds_fill_poke(params+idx, dev_ptr, datahi, flags);
                                ++idx;
                        }

                        break;
                }
                case IBV_PEER_OP_COPY_BLOCK: {
                        CUdeviceptr dev_ptr = range_from_id(op->wr.copy_op.target_id)->dptr +
                                op->wr.copy_op.offset;
                        size_t len = op->wr.copy_op.len;
                        void *src = op->wr.copy_op.src;
                        int flags = 0;
                        gds_dbg("OP_COPY_BLOCK dev_ptr=%llx src=%p len=%zu\n", dev_ptr, src, len);
                        // catching any other size here
                        if (!gds_enable_inlcpy()) {
                                gds_err("inline copy is not supported\n");
                                retcode = EINVAL;
                                break;
                        }
                        // IB Verbs bug
                        assert(len <= GDS_GPU_MAX_INLINE_SIZE);
                        //if (desc->need_flush) {
                        //        flags |= GDS_IMMCOPY_POST_TAIL_FLUSH;
                        //}
                        retcode = gds_fill_inlcpy(params+idx, dev_ptr, src, len, flags);
                        ++idx;
                        break;
                }
                case IBV_PEER_OP_POLL_AND_DWORD:
                case IBV_PEER_OP_POLL_GEQ_DWORD:
                case IBV_PEER_OP_POLL_NOR_DWORD: {
                        int poll_cond;
                        CUdeviceptr dev_ptr = range_from_id(op->wr.dword_va.target_id)->dptr + 
                                op->wr.dword_va.offset;
                        uint32_t data = op->wr.dword_va.data;
                        // TODO: properly handle a following fence instead of blidly flushing
                        int flags = 0;
                        if (!(post_flags & GDS_POST_OPS_DISCARD_WAIT_FLUSH))
                                flags |= GDS_POLL_POST_FLUSH;

                        gds_dbg("OP_POLL_DWORD dev_ptr=%llx data=%"PRIx32"\n", dev_ptr, data);

                        switch(op->type) {
                        case IBV_PEER_OP_POLL_NOR_DWORD:
                                //poll_cond = GDS_POLL_COND_NOR;
                                // TODO: lookup and pass peer down
                                assert(gpu_does_support_nor(NULL));
                                retcode = -EINVAL;
                                goto out;
                                break;
                        case IBV_PEER_OP_POLL_GEQ_DWORD:
                                poll_cond = GDS_POLL_COND_GEQ;
                                break;
                        case IBV_PEER_OP_POLL_AND_DWORD:
                                poll_cond = GDS_POLL_COND_AND;
                                break;
                        default:
                                assert(!"cannot happen");
                                retcode = EINVAL;
                                goto out;
                        }
                        retcode = gds_fill_poll(params+idx, dev_ptr, data, poll_cond, flags);
                        ++idx;                        
                        break;
                }
                default:
                        gds_err("undefined peer op type %d\n", op->type);
                        retcode = EINVAL;
                        break;
                }
                if (retcode) {
                        gds_err("error in fill func at entry n=%zu (idx=%d)\n", n, idx);
                        goto out;
                }
        }

        assert(n_ops == n);

out:
        return retcode;
}

//-----------------------------------------------------------------------------

int gds_post_pokes(CUstream stream, int count, gds_send_request_t *info, uint32_t *dw, uint32_t val)
{
        int retcode = 0;
        int poke_count = 0;
        int idx = 0;

        assert(info);

        for (int i = 0; i < count; i++) {
                poke_count += info[i].commit.entries + 2;
        }

        CUstreamBatchMemOpParams params[poke_count+1];

	for (int j=0; j<count; j++) {
                gds_dbg("peer_commit:%d idx=%d\n", j, idx);
                retcode = gds_post_ops(info[j].commit.entries, info[j].commit.storage, params, idx);
                if (retcode) {
                        goto out;
                }
        }
        assert(idx < poke_count);

        if (dw) {
                // assume host mem
                retcode = gds_fill_poke(params + idx, dw, val, GDS_MEMORY_HOST);
                if (retcode) {
                        gds_err("error %d at tracking entry\n", retcode);
                        goto out;
                }
                ++idx;
        }

        retcode = gds_stream_batch_ops(stream, idx, params, 0);
        if (retcode) {
                gds_err("error %d in stream_batch_ops\n", retcode);
                goto out;
        }
out:

	return retcode;
}

//-----------------------------------------------------------------------------

static int gds_post_ops_on_cpu(size_t n_descs, struct peer_op_wr *op)
{
        int retcode = 0;
        size_t n = 0;

        for (; op && n < n_descs; op = op->next, ++n) {
                //int flags = 0;
                gds_dbg("op[%zu] type:%08x\n", n, op->type);
                switch(op->type) {
                case IBV_PEER_OP_FENCE: {
                        gds_dbg("fence_flags=%"PRIu64"\n", op->wr.fence.fence_flags);
                        uint32_t fence_op = (op->wr.fence.fence_flags & (IBV_EXP_PEER_FENCE_OP_READ|IBV_EXP_PEER_FENCE_OP_WRITE));
                        uint32_t fence_from = (op->wr.fence.fence_flags & (IBV_EXP_PEER_FENCE_FROM_CPU|IBV_EXP_PEER_FENCE_FROM_HCA));
                        uint32_t fence_mem = (op->wr.fence.fence_flags & (IBV_EXP_PEER_FENCE_MEM_SYS|IBV_EXP_PEER_FENCE_MEM_PEER));

                        if (fence_op == IBV_EXP_PEER_FENCE_OP_READ) {
                                gds_warnc(1, "nothing to do for read fences\n");
                                //retcode = EINVAL;
                                break;
                        }
                        else {
                                if (fence_from != IBV_EXP_PEER_FENCE_FROM_HCA) {
                                        gds_err("unexpected from %08x fence, expected FROM_HCA\n", fence_from);
                                        retcode = EINVAL;
                                        break;
                                }
                                if (fence_mem == IBV_EXP_PEER_FENCE_MEM_PEER) {
                                        gds_dbg("using light membar\n");
                                        wmb();
                                }
                                else if (fence_mem == IBV_EXP_PEER_FENCE_MEM_SYS) {
                                        gds_dbg("using heavy membar\n");
                                        wmb();
                                }
                                else {
                                        gds_err("unsupported fence combination\n");
                                        retcode = EINVAL;
                                        break;
                                }
                        }
                        break;
                }
                case IBV_PEER_OP_STORE_DWORD: {
                        uint32_t *ptr = (uint32_t*)((ptrdiff_t)range_from_id(op->wr.dword_va.target_id)->va + op->wr.dword_va.offset);
                        uint32_t data = op->wr.dword_va.data;
                        // A || B || C || E
                        ACCESS_ONCE(*ptr) = data;
                        gds_dbg("%p <- %08x\n", ptr, data);
                        break;
                }
                case IBV_PEER_OP_STORE_QWORD: {
                        uint64_t *ptr = (uint64_t*)((ptrdiff_t)range_from_id(op->wr.qword_va.target_id)->va + op->wr.qword_va.offset);
                        uint64_t data = op->wr.qword_va.data;
                        ACCESS_ONCE(*ptr) = data;
                        gds_dbg("%p <- %016"PRIx64"\n", ptr, data);
                        break;
                }
                case IBV_PEER_OP_COPY_BLOCK: {
                        uint64_t *ptr = (uint64_t*)((ptrdiff_t)range_from_id(op->wr.copy_op.target_id)->va + op->wr.copy_op.offset);
                        uint64_t *src = (uint64_t*)op->wr.copy_op.src;
                        size_t n_bytes = op->wr.copy_op.len;
                        gds_bf_copy(ptr, src, n_bytes);
                        gds_dbg("%p <- %p len=%zu\n", ptr, src, n_bytes);
                        break;
                }
                case IBV_PEER_OP_POLL_AND_DWORD:
                case IBV_PEER_OP_POLL_GEQ_DWORD:
                case IBV_PEER_OP_POLL_NOR_DWORD: {
                        gds_err("polling is not supported\n");
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
                        goto out;
                }
        }

        assert(n_descs == n);

out:
        return retcode;
}

//-----------------------------------------------------------------------------

int gds_post_pokes_on_cpu(int count, gds_send_request_t *info, uint32_t *dw, uint32_t val)
{
        int retcode = 0;
        int idx = 0;

        assert(info);

	for (int j=0; j<count; j++) {
                gds_dbg("peer_commit:%d idx=%d\n", j, idx);
                retcode = gds_post_ops_on_cpu(info[j].commit.entries, info[j].commit.storage);
                if (retcode) {
                        goto out;
                }
        }

        if (dw) {
                wmb();
                ACCESS_ONCE(*dw) = val;
        }

out:
	return retcode;
}

//-----------------------------------------------------------------------------

int gds_stream_post_polls_and_pokes(CUstream stream,
				    size_t n_polls, uint32_t *ptrs[], uint32_t magics[], int cond_flags[], int poll_flags[], 
				    size_t n_pokes, uint32_t *poke_ptrs[], uint32_t poke_values[], int poke_flags[])
{
	int retcode = 0;
	size_t idx = 0;
	CUstreamBatchMemOpParams params[n_polls + n_pokes];
        gds_dbg("n_polls=%zu n_pokes=%zu\n", n_polls, n_pokes);

	for (size_t j = 0; j < n_polls; ++j, ++idx) {
                uint32_t *ptr = ptrs[j];
                uint32_t magic = magics[j];
                int cond_flag = cond_flags[j];
                int flags = poll_flags[j];
                gds_dbg("poll %zu: addr=%p value=%08x cond=%d flags=%08x\n", j, ptr, magic, cond_flag, flags);
                retcode = gds_fill_poll(params+idx, ptr, magic, cond_flag, flags);
                if (retcode) {
                        gds_err("error in fill_poll at entry %zu\n", j);
                        goto out;
                }
	}

        for (size_t j = 0; j < n_pokes; ++j, ++idx) {
                uint32_t *addr = poke_ptrs[j];
                uint32_t value = poke_values[j];
                int flags = poke_flags[j];
                gds_dbg("poke %zu: addr=%p value=%08x flags=%08x\n", j, addr, value, flags);
                retcode = gds_fill_poke(params+idx, addr, value, flags);
                if (retcode) {
                        gds_err("error in fill_poll at entry %zu\n", j);
                        goto out;
                }
        }

        assert(idx == n_polls + n_pokes);

        retcode = gds_stream_batch_ops(stream, idx, params, 0);
        if (retcode) {
                gds_err("error in batch_ops\n");
                goto out;
        }

out:
	return retcode;
}

//-----------------------------------------------------------------------------

int gds_stream_post_polls_and_immediate_copies(CUstream stream, 
                                               size_t n_polls, uint32_t *ptrs[], uint32_t magics[], int cond_flags[], int poll_flags[], 
                                               size_t n_imms, void *imm_ptrs[], void *imm_datas[], size_t imm_bytes[], int imm_flags[])
{
	int retcode = 0;
	size_t idx = 0;
	CUstreamBatchMemOpParams params[n_polls + n_imms];

	for (size_t j = 0; j < n_polls; ++j, ++idx) {
                uint32_t *ptr = ptrs[j];
                uint32_t magic = magics[j];
                int cond_flag = cond_flags[j];
                int flags = poll_flags[j];

                retcode = gds_fill_poll(params+idx, ptr, magic, cond_flag, flags);
                if (retcode) {
                        gds_err("error in fill_poll at entry %zu\n", j);
                        goto out;
                }
	}

	for (size_t j = 0; j < n_imms; ++j, ++idx) {
                void *ptr = imm_ptrs[j];
                void *data = imm_datas[j];
                size_t n_bytes = imm_bytes[j];
                int flags = imm_flags[j];
                retcode = gds_fill_inlcpy(params+idx, ptr, data, n_bytes, flags);
                if (retcode) {
                        gds_err("error in fill_inlcpy at entry %zu\n", j);
                        goto out;
                }
        }
        assert(idx == n_polls + n_imms);

        retcode = gds_stream_batch_ops(stream, idx, params, 0);
        if (retcode) {
                gds_err("error in batch_ops\n");
                goto out;
        }

out:
	return retcode;
}

//-----------------------------------------------------------------------------

static void gds_dump_ops(struct peer_op_wr *op, size_t count)
{
        size_t n = 0;
        for (; op; op = op->next, ++n) {
                gds_dbg("op[%zu] type:%d\n", n, op->type);
                switch(op->type) {
                case IBV_PEER_OP_FENCE: {
                        gds_dbg("FENCE flags=%"PRIu64"\n", op->wr.fence.fence_flags);
                        break;
                }
                case IBV_PEER_OP_STORE_DWORD: {
                        CUdeviceptr dev_ptr = range_from_id(op->wr.dword_va.target_id)->dptr + 
                                op->wr.dword_va.offset;
                        gds_dbg("STORE_QWORD data:%x target_id:%"PRIx64" offset:%zu dev_ptr=%llx\n",
                                op->wr.dword_va.data, op->wr.dword_va.target_id,
                                op->wr.dword_va.offset, dev_ptr);
                        break;
                }
                case IBV_PEER_OP_STORE_QWORD: {
                        CUdeviceptr dev_ptr = range_from_id(op->wr.qword_va.target_id)->dptr +
                                op->wr.qword_va.offset;
                        gds_dbg("STORE_QWORD data:%"PRIx64" target_id:%"PRIx64" offset:%zu dev_ptr=%llx\n",
                                op->wr.qword_va.data, op->wr.qword_va.target_id,
                                op->wr.qword_va.offset, dev_ptr);
                        break;
                }
                case IBV_PEER_OP_COPY_BLOCK: {
                        CUdeviceptr dev_ptr = range_from_id(op->wr.copy_op.target_id)->dptr +
                                op->wr.copy_op.offset;
                        gds_dbg("COPY_BLOCK src:%p len:%zu target_id:%"PRIx64" offset:%zu dev_ptr=%llx\n",
                                op->wr.copy_op.src, op->wr.copy_op.len,
                                op->wr.copy_op.target_id, op->wr.copy_op.offset,
                                dev_ptr);
                        break;
                }
                case IBV_PEER_OP_POLL_AND_DWORD:
                case IBV_PEER_OP_POLL_NOR_DWORD: {
                        CUdeviceptr dev_ptr = range_from_id(op->wr.dword_va.target_id)->dptr + 
                                op->wr.dword_va.offset;
                        gds_dbg("%s data:%08x target_id:%"PRIx64" offset:%zu dev_ptr=%llx\n", 
                                (op->type==IBV_PEER_OP_POLL_AND_DWORD) ? "POLL_AND_DW" : "POLL_NOR_SDW",
                                op->wr.dword_va.data, 
                                op->wr.dword_va.target_id, 
                                op->wr.dword_va.offset, 
                                dev_ptr);
                        break;
                }
                default:
                        gds_err("undefined peer op type %d\n", op->type);
                        break;
                }
        }

        assert(count == n);
}

//-----------------------------------------------------------------------------

void gds_dump_wait_request(gds_wait_request_t *request, size_t count)
{
        for (size_t j=0; j<count; ++j) {
                struct ibv_exp_peer_peek *peek = &request[j].peek;
                gds_dbg("req[%zu] entries:%u whence:%u offset:%u peek_id:%"PRIx64" comp_mask:%08x\n", 
                        j, peek->entries, peek->whence, peek->offset, 
                        peek->peek_id, peek->comp_mask);
                gds_dump_ops(peek->storage, peek->entries);
        }
}

//-----------------------------------------------------------------------------

int gds_stream_post_wait_cq_multi(CUstream stream, int count, gds_wait_request_t *request, uint32_t *dw, uint32_t val)
{
        int retcode = 0;
        int n_mem_ops = 0;
        int idx = 0;

        assert(request);

        for (int i = 0; i < count; i++) {
                n_mem_ops += request[i].peek.entries;
        }

        gds_dbg("count=%d dw=%p val=%08x space for n_mem_ops=%d\n", count, dw, val, n_mem_ops);

        CUstreamBatchMemOpParams params[n_mem_ops+1];

	for (int j=0; j<count; j++) {
                gds_dbg("peek request:%d\n", j);
                retcode = gds_post_ops(request[j].peek.entries, request[j].peek.storage, params, idx);
                if (retcode) {
                        goto out;
                }
        }
        gds_dbg("idx=%d\n", idx);
	assert(idx <= n_mem_ops);

        if (dw) {
                // assume host mem
                retcode = gds_fill_poke(params + idx, dw, val, GDS_MEMORY_HOST);
                if (retcode) {
                        gds_err("error %d at tracking entry\n", retcode);
                        goto out;
                }
                ++idx;
        }

        retcode = gds_stream_batch_ops(stream, idx, params, 0);
        if (retcode) {
                gds_err("error %d in stream_batch_ops\n", retcode);
                goto out;
        }
out:
        return retcode;
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------

// If NULL returned then buffer will be allocated in system memory
// by ibverbs driver.
static struct ibv_peer_buf *gds_buf_alloc(ibv_peer_buf_alloc_attr *attr)
{
        assert(attr);
        gds_peer *peer = peer_from_id(attr->peer_id);
        assert(peer);

        gds_dbg("alloc mem peer:{type=%d gpu_id=%d} attr{len=%zu dir=%d alignment=%d peer_id=%"PRIx64"}\n",
                peer->alloc_type, peer->gpu_id, attr->length, attr->dir, attr->alignment, attr->peer_id);

        return peer->buf_alloc(peer->alloc_type, attr->length, attr->dir, attr->alignment, peer->alloc_flags);
}

static int gds_buf_release(struct ibv_peer_buf *pb)
{
        gds_dbg("freeing pb=%p\n", pb);
        gds_buf *buf = static_cast<gds_buf*>(pb);
        gds_peer *peer = buf->peer;
        peer->free(buf);
        return 0;
}

static uint64_t gds_register_va(void *start, size_t length, uint64_t peer_id, struct ibv_exp_peer_buf *pb)
{
        gds_peer *peer = peer_from_id(peer_id);
        gds_range *range = NULL;

        gds_dbg("start=%p length=%zu peer_id=%"PRIx64" peer_buf=%p\n", start, length, peer_id, pb);

        if (IBV_EXP_PEER_IOMEMORY == pb) {
                // register as IOMEM
                range = peer->register_range(start, length, GDS_MEMORY_IO);
        }
        else if (pb) {
                gds_buf *buf = static_cast<gds_buf*>(pb);
                // should have been allocated via gds_buf_alloc
                // assume GDR mapping already created
                // associate range to peer_buf
                range = peer->range_from_buf(buf, start, length);
        }
        else {
                // register as SYSMEM
                range = peer->register_range(start, length, GDS_MEMORY_HOST);
        }
        if (!range) {
                gds_err("error while registering range, returning 0 as error value\n");
                return 0;
        }
        return range_to_id(range);
}

static int gds_unregister_va(uint64_t registration_id, uint64_t peer_id)
{
        gds_peer *peer = peer_from_id(peer_id);
        gds_range *range = range_from_id(registration_id);
        gds_dbg("peer=%p range=%p\n", peer, range);
        peer->unregister(range);
        return 0;
}

static void gds_init_peer(gds_peer *peer, int gpu_id)
{
        assert(peer);

        peer->gpu_id = gpu_id;
        peer->gpu_dev = 0;
        peer->gpu_ctx = 0;
}

static void gds_init_peer_attr(gds_peer_attr *attr, gds_peer *peer)
{
        assert(peer);

        peer->alloc_type = gds_peer::NONE;
        peer->alloc_flags = 0;

        attr->peer_id = peer_to_id(peer);
        attr->buf_alloc = gds_buf_alloc;
        attr->buf_release = gds_buf_release;
        attr->register_va = gds_register_va;
        attr->unregister_va = gds_unregister_va;

        attr->caps = ( IBV_EXP_PEER_OP_STORE_DWORD_CAP    | 
                       IBV_EXP_PEER_OP_STORE_QWORD_CAP    | 
                       IBV_EXP_PEER_OP_FENCE_CAP          | 
                       IBV_EXP_PEER_OP_POLL_AND_DWORD_CAP );

        if (gpu_does_support_nor(peer))
                attr->caps |= IBV_EXP_PEER_OP_POLL_NOR_DWORD_CAP;
        else
                attr->caps |= IBV_EXP_PEER_OP_POLL_GEQ_DWORD_CAP;

        if (gds_enable_inlcpy()) {
                attr->caps |= IBV_EXP_PEER_OP_COPY_BLOCK_CAP;
        }
        else if (gds_enable_write64() || gds_simulate_write64()) {
                attr->caps |= IBV_EXP_PEER_OP_STORE_QWORD_CAP;
        }
        gds_dbg("caps=%016lx\n", attr->caps);
        attr->peer_dma_op_map_len = GDS_GPU_MAX_INLINE_SIZE;
        attr->comp_mask = IBV_EXP_PEER_DIRECT_VERSION;
        attr->version = 1;

        gds_dbg("peer_attr: peer_id=%"PRIx64"\n", attr->peer_id);
}

//-----------------------------------------------------------------------------

static gds_peer gpu_peer[max_gpus];
static gds_peer_attr gpu_peer_attr[max_gpus];
static bool gpu_registered[max_gpus];

int gds_register_peer_ex(struct ibv_context *context, unsigned gpu_id, gds_peer **p_peer, gds_peer_attr **p_peer_attr)
{
        int ret = 0;

        gds_dbg("GPU %u: registering peer\n", gpu_id);
        
        if (gpu_id >= max_gpus) {
                gds_err("invalid gpu_id %d\n", gpu_id);
                return EINVAL;
        }

        gds_peer *peer = &gpu_peer[gpu_id];
        gds_peer_attr *peer_attr = &gpu_peer_attr[gpu_id];

        if (gpu_registered[gpu_id]) {
                gds_dbg("gds_peer for GPU %d already initialized\n", gpu_id);
        } else {
                gds_init_peer(peer, gpu_id);
                gds_init_peer_attr(peer_attr, peer);
                gpu_registered[gpu_id] = true;
        }

        if (p_peer)
                *p_peer = peer;

        if (p_peer_attr)
                *p_peer_attr = peer_attr;

        return ret;
}

int gds_register_peer(struct ibv_context *context, unsigned gpu_id)
{
        return gds_register_peer_ex(context, gpu_id, NULL, NULL);
}

//-----------------------------------------------------------------------------

struct ibv_cq *
gds_create_cq(struct ibv_context *context, int cqe,
              void *cq_context, struct ibv_comp_channel *channel,
              int comp_vector, int gpu_id, gds_alloc_cq_flags_t flags)
{
        int ret = 0;
        struct ibv_cq *cq = NULL;

        gds_dbg("cqe=%d gpu_id=%d cq_flags=%08x\n", cqe, gpu_id, flags);

        // TODO: add support for res_domain

        gds_peer *peer = NULL;
        gds_peer_attr *peer_attr = NULL;
        ret = gds_register_peer_ex(context, gpu_id, &peer, &peer_attr);
        if (ret) {
                gds_err("error %d while registering GPU peer\n", ret);
                return NULL;
        }
        assert(peer);
        assert(peer_attr);

        peer->alloc_type = gds_peer::CQ;
        peer->alloc_flags = flags;

        ibv_create_cq_attr_ex attr;
        attr.comp_mask = IBV_CREATE_CQ_ATTR_PEER_DIRECT;
        attr.flags = 0; // see ibv_exp_cq_create_flags
        attr.res_domain = NULL;
        attr.peer_direct_attrs = peer_attr;

        int old_errno = errno;
        cq = ibv_exp_create_cq(context, cqe, cq_context, channel, comp_vector, &attr);
        if (!cq) {
                gds_err("error %d in ibv_exp_create_cq, old errno %d\n", errno, old_errno);
        }

        return cq;
}

//-----------------------------------------------------------------------------

struct gds_qp *gds_create_qp(struct ibv_pd *pd, struct ibv_context *context, gds_qp_init_attr_t *qp_attr, int gpu_id, int flags)
{
        int ret = 0;
        struct gds_qp *gqp = NULL;
        struct ibv_qp *qp = NULL;
        struct ibv_cq *rx_cq = NULL, *tx_cq = NULL;
        gds_peer *peer = NULL;
        gds_peer_attr *peer_attr = NULL;
        int old_errno = errno;

        gds_dbg("pd=%p context=%p gpu_id=%d flags=%08x errno=%d\n", pd, context, gpu_id, flags, errno);
        assert(pd);
        assert(context);
        assert(qp_attr);

        gqp = (struct gds_qp*)calloc(1, sizeof(struct gds_qp));
        if (!gqp) {
                gds_err("cannot allocate memory\n");
                return NULL;
        }

        gds_dbg("creating TX CQ\n");
        tx_cq = gds_create_cq(context, qp_attr->cap.max_send_wr, NULL, NULL, 0, gpu_id, 
                              (flags & GDS_CREATE_QP_TX_CQ_ON_GPU) ? 
                              GDS_ALLOC_CQ_ON_GPU : GDS_ALLOC_CQ_DEFAULT);
	if (!tx_cq) {
                ret = errno;
		gds_err("error %d while creating TX CQ, old_errno=%d\n", ret, old_errno);
		goto err;
	}

        gds_dbg("creating RX CQ\n");
        rx_cq = gds_create_cq(context, qp_attr->cap.max_recv_wr, NULL, NULL, 0, gpu_id, 
                              (flags & GDS_CREATE_QP_RX_CQ_ON_GPU) ? 
                              GDS_ALLOC_CQ_ON_GPU : GDS_ALLOC_CQ_DEFAULT);
	if (!rx_cq) {
                ret = errno;
                gds_err("error %d while creating RX CQ\n", ret);
		goto err_free_tx_cq;
	}

        qp_attr->send_cq = tx_cq;
        qp_attr->recv_cq = rx_cq;

        qp_attr->pd = pd;
        qp_attr->comp_mask |= IBV_QP_INIT_ATTR_PD;

        // disable overflow checks in ibv_poll_cq(), as GPU might invalidate
        // the CQE without updating the tracking variables
        if (flags & GDS_CREATE_QP_GPU_INVALIDATE_RX_CQ) {
                gds_warn("IGNORE_RQ_OVERFLOW\n");
                qp_attr->exp_create_flags |= IBV_EXP_QP_CREATE_IGNORE_RQ_OVERFLOW;
                qp_attr->comp_mask |= IBV_EXP_QP_INIT_ATTR_CREATE_FLAGS;
        }
        if (flags & GDS_CREATE_QP_GPU_INVALIDATE_TX_CQ) {
                gds_warn("IGNORE_SQ_OVERFLOW\n");
                qp_attr->exp_create_flags |= IBV_EXP_QP_CREATE_IGNORE_SQ_OVERFLOW;
                qp_attr->comp_mask |= IBV_EXP_QP_INIT_ATTR_CREATE_FLAGS;
        }

        gds_dbg("before gds_register_peer_ex\n");

        ret = gds_register_peer_ex(context, gpu_id, &peer, &peer_attr);
        if (ret) {
                gds_err("error %d in gds_register_peer_ex\n", ret);
                goto err_free_cqs;
        }

        peer->alloc_type = gds_peer::WQ;
        peer->alloc_flags = GDS_ALLOC_WQ_DEFAULT | GDS_ALLOC_DBREC_DEFAULT;
        if (flags & GDS_CREATE_QP_WQ_ON_GPU) {
                gds_err("error, QP WQ on GPU is not supported yet\n");
                goto err_free_cqs;
        }
        if (flags & GDS_CREATE_QP_WQ_DBREC_ON_GPU) {
                gds_warn("QP WQ DBREC on GPU\n");
                peer->alloc_flags |= GDS_ALLOC_DBREC_ON_GPU;
        }        
        qp_attr->comp_mask |= IBV_QP_INIT_ATTR_PEER_DIRECT;
        qp_attr->peer_direct_attrs = peer_attr;

        qp = ibv_create_qp_ex(context, qp_attr);
        if (!qp)  {
                ret = EINVAL;
                gds_err("error in ibv_create_qp_ex\n");
                goto err_free_cqs;
	}

        gqp->qp = qp;
        gqp->send_cq.cq = qp->send_cq;
        gqp->send_cq.curr_offset = 0;
        gqp->recv_cq.cq = qp->recv_cq;
        gqp->recv_cq.curr_offset = 0;

        gds_dbg("created gds_qp=%p\n", gqp);

        return gqp;

err_free_qp:
        gds_dbg("destroying QP\n");
        ibv_destroy_qp(qp);

err_free_cqs:
        gds_dbg("destroying RX CQ\n");
	ret = ibv_destroy_cq(rx_cq);
        if (ret) {
                gds_err("error %d destroying RX CQ\n", ret);
        }

err_free_tx_cq:
        gds_dbg("destroying TX CQ\n");
	ret = ibv_destroy_cq(tx_cq);
        if (ret) {
                gds_err("error %d destroying TX CQ\n", ret);
        }

err:
        free(gqp);

        return NULL;
}

//-----------------------------------------------------------------------------

int gds_destroy_qp(struct gds_qp *qp)
{
        int retcode = 0;
        int ret;
        assert(qp);

        assert(qp->qp);
        ret = ibv_destroy_qp(qp->qp);
        if (ret) {
                gds_err("error %d in destroy_qp\n", ret);
                retcode = ret;
        }

        assert(qp->send_cq.cq);
        ret = ibv_destroy_cq(qp->send_cq.cq);
        if (ret) {
                gds_err("error %d in destroy_cq send_cq\n", ret);
                retcode = ret;
        }

        assert(qp->recv_cq.cq);
        ret = ibv_destroy_cq(qp->recv_cq.cq);
        if (ret) {
                gds_err("error %d in destroy_cq recv_cq\n", ret);
                retcode = ret;
        }

        free(qp);

        return retcode;
}

//-----------------------------------------------------------------------------

int gds_query_param(gds_param_t param, int *value)
{
        int ret = 0;
        if (!value)
                return EINVAL;

        switch (param) {
        case GDS_PARAM_VERSION:
                *value = (GDS_API_MAJOR_VERSION << 16)|GDS_API_MINOR_VERSION;
                break;
        default:
                ret = EINVAL;
                break;
        };
        return ret;
}

//-----------------------------------------------------------------------------

static bool no_network_descs_after_entry(size_t n_descs, gds_descriptor_t *descs, size_t idx)
{
        bool ret = true;
        size_t i;
        for(i = idx+1; i < n_descs; ++i) {
                gds_descriptor_t *desc = descs + i;
                switch(desc->tag) {
                case GDS_TAG_SEND:
                case GDS_TAG_WAIT:
                        ret = false;
                        goto out;
                case GDS_TAG_WAIT_VALUE32:
                case GDS_TAG_WRITE_VALUE32:
                        break;
                default:
                        gds_err("invalid tag\n");
                        ret = EINVAL;
                        goto out;
                }
        }
out:
        return ret;
}

static int get_wait_info(size_t n_descs, gds_descriptor_t *descs, size_t &n_waits, size_t &last_wait)
{
        int ret = 0;
        size_t i;
        for(i = 0; i < n_descs; ++i) {
                gds_descriptor_t *desc = descs + i;
                switch(desc->tag) {
                case GDS_TAG_WAIT:
                        ++n_waits;
                        last_wait = i;
                        break;
                case GDS_TAG_SEND:
                case GDS_TAG_WAIT_VALUE32:
                case GDS_TAG_WRITE_VALUE32:
                        break;
                default:
                        gds_err("invalid tag\n");
                        ret = EINVAL;
                }
        }
        return ret;
}

static size_t calc_n_mem_ops(size_t n_descs, gds_descriptor_t *descs)
{
        size_t n_mem_ops = 0;
        size_t i;
        for(i = 0; i < n_descs; ++i) {
                gds_descriptor_t *desc = descs + i;
                switch(desc->tag) {
                case GDS_TAG_SEND:
                        n_mem_ops += desc->send->commit.entries + 2; // extra space, ugly
                        break;
                case GDS_TAG_WAIT:
                        n_mem_ops += desc->wait->peek.entries + 2; // ditto
                        break;
                case GDS_TAG_WAIT_VALUE32:
                case GDS_TAG_WRITE_VALUE32:
                        n_mem_ops += 2; // ditto
                        break;
                default:
                        gds_err("invalid tag\n");
                }
        }
        return n_mem_ops;
}

int gds_stream_post_descriptors(CUstream stream, size_t n_descs, gds_descriptor_t *descs)
{
        size_t i;
        int idx = 0;
        int ret = 0;
        int retcode = 0;
        size_t n_mem_ops = 0;
        size_t n_waits = 0;
        size_t last_wait = 0;
        bool move_flush = false;

        n_mem_ops = calc_n_mem_ops(n_descs, descs);
        get_wait_info(n_descs, descs, n_waits, last_wait);

        gds_dbg("n_descs=%zu n_waits=%zu n_mem_ops=%zu\n", n_descs, n_waits, n_mem_ops);

        // move flush to last wait in the whole batch
        if (n_waits && no_network_descs_after_entry(n_descs, descs, last_wait)) {
                gds_dbg("optimizing FLUSH to last wait i=%zu\n", last_wait);
                move_flush = true;
        }
        // alternatively, remove flush for wait is next op is a wait too

        CUstreamBatchMemOpParams params[n_mem_ops];

        for(i = 0; i < n_descs; ++i) {
                gds_descriptor_t *desc = descs + i;
                switch(desc->tag) {
                case GDS_TAG_SEND: {
                        gds_send_request_t *sreq = desc->send;
                        retcode = gds_post_ops(sreq->commit.entries, sreq->commit.storage, params, idx);
                        if (retcode) {
                                gds_err("error %d in gds_post_ops\n", retcode);
                                ret = retcode;
                                goto out;
                        }
                        // TODO: fix late checking
                        //assert(idx <= n_mem_ops);
                        if (idx >= n_mem_ops) {
                                gds_err("idx=%d is past allocation (%zu)\n", idx, n_mem_ops);
                                assert(!"corrupted heap");
                        }
                        break;
                }
                case GDS_TAG_WAIT: {
                        gds_wait_request_t *wreq = desc->wait;
                        int flags = 0;
                        if (move_flush && i != last_wait)
                                flags = GDS_POST_OPS_DISCARD_WAIT_FLUSH;
                        retcode = gds_post_ops(wreq->peek.entries, wreq->peek.storage, params, idx, flags);
                        if (retcode) {
                                gds_err("error %d in gds_post_ops\n", retcode);
                                ret = retcode;
                                goto out;
                        }
                        // TODO: fix late checking
                        assert(idx <= n_mem_ops);
                        break;
                }
                case GDS_TAG_WAIT_VALUE32:
                        retcode = gds_fill_poll(params+idx, desc->value32.ptr, desc->value32.value, desc->value32.cond_flags, desc->value32.flags);
                        if (retcode) {
                                gds_err("error %d in gds_fill_poll\n", retcode);
                                ret = retcode;
                                goto out;
                        }
                        ++idx;
                        break;
                case GDS_TAG_WRITE_VALUE32:
                        retcode = gds_fill_poke(params+idx, desc->value32.ptr, desc->value32.value, desc->value32.flags);
                        if (retcode) {
                                gds_err("error %d in gds_fill_poll\n", retcode);
                                ret = retcode;
                                goto out;
                        }
                        ++idx;
                        break;
                default:
                        assert(0);
                        break;
                }
        }
        retcode = gds_stream_batch_ops(stream, idx, params, 0);
        if (retcode) {
                gds_err("error in batch_ops\n");
                goto out;
        }

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
