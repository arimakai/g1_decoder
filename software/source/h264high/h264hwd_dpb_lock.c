/*------------------------------------------------------------------------------
--       Copyright (c) 2015-2017, VeriSilicon Inc. All rights reserved        --
--         Copyright (c) 2011-2014, Google Inc. All rights reserved.          --
--         Copyright (c) 2007-2010, Hantro OY. All rights reserved.           --
--                                                                            --
-- This software is confidential and proprietary and may be used only as      --
--   expressly authorized by VeriSilicon in a written licensing agreement.    --
--                                                                            --
--         This entire notice must be reproduced on all copies                --
--                       and may not be removed.                              --
--                                                                            --
--------------------------------------------------------------------------------
-- Redistribution and use in source and binary forms, with or without         --
-- modification, are permitted provided that the following conditions are met:--
--   * Redistributions of source code must retain the above copyright notice, --
--       this list of conditions and the following disclaimer.                --
--   * Redistributions in binary form must reproduce the above copyright      --
--       notice, this list of conditions and the following disclaimer in the  --
--       documentation and/or other materials provided with the distribution. --
--   * Neither the names of Google nor the names of its contributors may be   --
--       used to endorse or promote products derived from this software       --
--       without specific prior written permission.                           --
--------------------------------------------------------------------------------
-- THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"--
-- AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE  --
-- IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE --
-- ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE  --
-- LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR        --
-- CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF       --
-- SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS   --
-- INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN    --
-- CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)    --
-- ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE --
-- POSSIBILITY OF SUCH DAMAGE.                                                --
--------------------------------------------------------------------------------
------------------------------------------------------------------------------*/

#include "h264decapi.h"
#include "h264hwd_dpb_lock.h"
#include "h264hwd_dpb.h"
#include "h264hwd_storage.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>

#define FB_UNALLOCATED      0x00U
#define FB_FREE             0x01U
#define FB_ALLOCATED        0x02U
#define FB_OUTPUT           0x04U
#define FB_TEMP_OUTPUT      0x08U

#define FB_HW_ONGOING       0x30U


#ifdef DPB_LOCK_TRACE
#define DPB_TRACE(fmt, ...) fprintf(stderr, "%s(): " fmt,\
                                    __func__, ## __VA_ARGS__)
#else
#define DPB_TRACE(...) do {} while(0)
#endif

u32 InitList(FrameBufferList *fbList)
{
    (void) DWLmemset(fbList, 0, sizeof(*fbList));

    sem_init(&fbList->out_count_sem, 0, 0);
    pthread_mutex_init(&fbList->out_count_mutex, NULL);
    /* CV to be signaled when output  queue is empty */
    pthread_cond_init(&fbList->out_empty_cv, NULL );

    pthread_mutex_init(&fbList->ref_count_mutex, NULL );
    /* CV to be signaled when a buffer is not referenced anymore */
    pthread_cond_init(&fbList->ref_count_cv, NULL );

    /* this CV is used to signal the HW has finished processing a picture
     * that is needed for output ( FB_OUTPUT | FB_HW_ONGOING )
     */
    pthread_cond_init(&fbList->hw_rdy_cv, NULL);

    fbList->bInitialized = 1;

    return 0;
}

void ReleaseList(FrameBufferList *fbList)
{
    int i;
    if (!fbList->bInitialized)
        return;

    for(i = 0; i < MAX_FRAME_BUFFER_NUMBER; i++)
    {
        /* we shall clean our stuff graciously */
        assert(fbList->fbStat[i].nRefCount == 0);
    }

    assert(fbList->freeBuffers == 0);

    fbList->bInitialized = 0;

    pthread_mutex_destroy(&fbList->ref_count_mutex);
    pthread_cond_destroy(&fbList->ref_count_cv);

    pthread_mutex_destroy(&fbList->out_count_mutex);
    pthread_cond_destroy(&fbList->out_empty_cv);
    pthread_cond_destroy(&fbList->hw_rdy_cv);

    sem_destroy(&fbList->out_count_sem);
}

u32 AllocateIdUsed(FrameBufferList *fbList, const void * data)
{
    u32 id = 0;

    /* find first unallocated ID */
    do
    {
        if (fbList->fbStat[id].bUsed == FB_UNALLOCATED)
            break;
        id++;
    } while (id < MAX_FRAME_BUFFER_NUMBER);

    if (id >= MAX_FRAME_BUFFER_NUMBER)
        return FB_NOT_VALID_ID;

    fbList->fbStat[id].bUsed = FB_ALLOCATED;
    fbList->fbStat[id].nRefCount = 0;
    fbList->fbStat[id].data = data;

    return id;
}

u32 AllocateIdFree(FrameBufferList *fbList, const void * data)
{
    u32 id = 0;

    /* find first unallocated ID */
    do
    {
        if (fbList->fbStat[id].bUsed == FB_UNALLOCATED)
            break;
        id++;
    } while (id < MAX_FRAME_BUFFER_NUMBER);

    if (id >= MAX_FRAME_BUFFER_NUMBER)
        return FB_NOT_VALID_ID;

    fbList->freeBuffers++;

    fbList->fbStat[id].bUsed = FB_FREE;
    fbList->fbStat[id].nRefCount = 0;
    fbList->fbStat[id].data = data;
    return id;
}

void ReleaseId(FrameBufferList *fbList, u32 id)
{
    assert(id < MAX_FRAME_BUFFER_NUMBER);

    /* it is "bad" to release referenced or unallocated buffers */
    assert(fbList->fbStat[id].nRefCount == 0);
    assert(fbList->fbStat[id].bUsed != FB_UNALLOCATED);

    if (id >= MAX_FRAME_BUFFER_NUMBER)
        return;

    if(fbList->fbStat[id].bUsed == FB_FREE)
    {
        assert(fbList->freeBuffers > 0);
        fbList->freeBuffers--;
    }

    fbList->fbStat[id].bUsed = FB_UNALLOCATED;
    fbList->fbStat[id].nRefCount = 0;
    fbList->fbStat[id].data = NULL;
}

void * GetDataById(FrameBufferList *fbList, u32 id)
{
    assert(id < MAX_FRAME_BUFFER_NUMBER);
    assert(fbList->fbStat[id].bUsed != FB_UNALLOCATED);

    return (void*) fbList->fbStat[id].data;
}

u32 GetIdByData(FrameBufferList *fbList, const void *data)
{
    u32 id = 0;
    assert(data);

    do
    {
        if (fbList->fbStat[id].data == data)
            break;
        id++;
    } while (id < MAX_FRAME_BUFFER_NUMBER);

    return id < MAX_FRAME_BUFFER_NUMBER ? id : FB_NOT_VALID_ID;
}
void IncrementRefUsage(FrameBufferList *fbList, u32 id)
{
    pthread_mutex_lock(&fbList->ref_count_mutex);
    fbList->fbStat[id].nRefCount++;
    DPB_TRACE("id = %d rc = %d\n", id, fbList->fbStat[id].nRefCount);
    pthread_mutex_unlock(&fbList->ref_count_mutex);
}

void DecrementRefUsage(FrameBufferList *fbList, u32 id)
{
    FrameBufferStatus *bs = fbList->fbStat + id;

    pthread_mutex_lock(&fbList->ref_count_mutex);
    assert(bs->nRefCount > 0);
    bs->nRefCount--;

    if (bs->nRefCount == 0)
    {
        if (bs->bUsed == FB_FREE)
        {
            fbList->freeBuffers++;
            DPB_TRACE("FREE id = %d\n", id);
        }
        /* signal that this buffer is not referenced anymore */
        pthread_cond_signal(&fbList->ref_count_cv);
    }
    else if (bs->bUsed == FB_FREE)
    {
        DPB_TRACE("Free buffer id = %d still referenced\n", id);
    }

    DPB_TRACE("id = %d rc = %d\n", id, fbList->fbStat[id].nRefCount);
    pthread_mutex_unlock(&fbList->ref_count_mutex);
}

void MarkHWOutput(FrameBufferList *fbList, u32 id, u32 type)
{

    pthread_mutex_lock(&fbList->ref_count_mutex);

    assert( fbList->fbStat[id].bUsed & FB_ALLOCATED );
    assert( fbList->fbStat[id].bUsed ^ type );

    fbList->fbStat[id].nRefCount++;
    fbList->fbStat[id].bUsed |= type;

    DPB_TRACE("id = %d rc = %d\n", id, fbList->fbStat[id].nRefCount);

    pthread_mutex_unlock(&fbList->ref_count_mutex);
}

void ClearHWOutput(FrameBufferList *fbList, u32 id, u32 type)
{
    FrameBufferStatus *bs = fbList->fbStat + id;

    pthread_mutex_lock(&fbList->ref_count_mutex);

    assert(bs->bUsed & (FB_HW_ONGOING | FB_ALLOCATED));

    bs->nRefCount--;
    bs->bUsed &= ~type;

    if (bs->nRefCount == 0)
    {
        if (bs->bUsed == FB_FREE)
        {
            fbList->freeBuffers++;
            DPB_TRACE("FREE id = %d\n", id);
        }
        /* signal that this buffer is not referenced anymore */
        pthread_cond_signal(&fbList->ref_count_cv);
    }

    if((bs->bUsed & FB_HW_ONGOING) == 0 &&  (bs->bUsed & FB_OUTPUT))
        /* signal that this buffer is done by HW */
        pthread_cond_signal(&fbList->hw_rdy_cv);

    DPB_TRACE("id = %d rc = %d\n", id, fbList->fbStat[id].nRefCount);

    pthread_mutex_unlock(&fbList->ref_count_mutex);
}

/* Mark a buffer as a potential (temporal) output. Output has to be marked
 * permanent by FinalizeOutputAll or reverted to non-output by
 * RemoveTempOutputAll.
 */
void MarkTempOutput(FrameBufferList *fbList, u32 id)
{
    DPB_TRACE(" id = %d\n", id);
    pthread_mutex_lock(&fbList->ref_count_mutex);

    assert( fbList->fbStat[id].bUsed & FB_ALLOCATED);

    fbList->fbStat[id].nRefCount++;
    fbList->fbStat[id].bUsed |= FB_TEMP_OUTPUT;

    pthread_mutex_unlock(&fbList->ref_count_mutex);
}

/* Mark all temp output as valid output */
void FinalizeOutputAll(FrameBufferList *fbList)
{
    i32 i;
    pthread_mutex_lock(&fbList->ref_count_mutex);

    for (i = 0; i < MAX_FRAME_BUFFER_NUMBER; i++)
    {
        if (fbList->fbStat[i].bUsed & FB_TEMP_OUTPUT)
        {
            /* mark permanent output */
            fbList->fbStat[i].bUsed |= FB_OUTPUT;
            /* clean TEMP flag from output */
            fbList->fbStat[i].bUsed &= ~FB_TEMP_OUTPUT;

            DPB_TRACE("id = %d\n", i);
        }
    }

    pthread_mutex_unlock(&fbList->ref_count_mutex);
}

void ClearOutput(FrameBufferList *fbList, u32 id)
{
    FrameBufferStatus *bs = fbList->fbStat + id;

    pthread_mutex_lock(&fbList->ref_count_mutex);

    assert(bs->bUsed & (FB_OUTPUT | FB_TEMP_OUTPUT));

    assert(bs->nRefCount > 0);
    bs->nRefCount--;

    bs->bUsed &= ~(FB_OUTPUT | FB_TEMP_OUTPUT);

    if (bs->nRefCount == 0)
    {
        if (bs->bUsed == FB_FREE)
        {
            fbList->freeBuffers++;
            DPB_TRACE("FREE id = %d\n", id);
        }
        /* signal that this buffer is not referenced anymore */
        pthread_cond_signal(&fbList->ref_count_cv);
    }
    else if(bs->bUsed == FB_FREE)
    {
        DPB_TRACE("Free buffer id = %d still referenced\n", id);
    }

    DPB_TRACE("id = %d rc = %d\n", id, fbList->fbStat[id].nRefCount);
    pthread_mutex_unlock(&fbList->ref_count_mutex);
}

u32 PopFreeBuffer(FrameBufferList *fbList)
{
    u32 i = 0;
    FrameBufferStatus *bs = fbList->fbStat;
    do
    {
        if (bs->bUsed == FB_FREE && bs->nRefCount == 0)
        {
            bs->bUsed = FB_ALLOCATED;
            break;
        }
        bs++;
        i++;
    } while (i < MAX_FRAME_BUFFER_NUMBER);

    assert(i < MAX_FRAME_BUFFER_NUMBER);

    fbList->freeBuffers--;

    DPB_TRACE("id = %d\n", i);

    return i;
}

void PushFreeBuffer(FrameBufferList *fbList, u32 id)
{
    assert(id < MAX_FRAME_BUFFER_NUMBER);
    assert(fbList->fbStat[id].bUsed & FB_ALLOCATED);

    pthread_mutex_lock(&fbList->ref_count_mutex);

    DPB_TRACE("id = %d\n", id);

    fbList->fbStat[id].bUsed &= ~FB_ALLOCATED;
    fbList->fbStat[id].bUsed |= FB_FREE;

    if (fbList->fbStat[id].nRefCount == 0)
    {
        fbList->freeBuffers++;
        DPB_TRACE("FREE id = %d\n", id);

        /* signal that this buffer is not referenced anymore */
        pthread_cond_signal(&fbList->ref_count_cv);
    }
    else
        DPB_TRACE("Free buffer id = %d still referenced\n", id);

    pthread_mutex_unlock(&fbList->ref_count_mutex);
}

u32 GetFreePicBuffer(FrameBufferList *fbList, u32 old_id)
{
    u32 id;

    pthread_mutex_lock(&fbList->ref_count_mutex);

    /* Wait until a free buffer is available or "old_id"
     * buffer is not referenced anymore */
    while (fbList->freeBuffers == 0 && fbList->fbStat[old_id].nRefCount != 0)
    {
        DPB_TRACE("NO FREE PIC BUFFER\n");
        pthread_cond_wait(&fbList->ref_count_cv, &fbList->ref_count_mutex);
    }

    if (fbList->fbStat[old_id].nRefCount == 0)
    {
        /*  our old buffer is not referenced anymore => reuse it */
        id = old_id;
    }
    else
    {
        id = PopFreeBuffer(fbList);
    }

    DPB_TRACE("id = %d\n", id);

    pthread_mutex_unlock(&fbList->ref_count_mutex);

    return id;
}

u32 GetFreeBufferCount(FrameBufferList *fbList)
{
    u32 freeBuffers;
    pthread_mutex_lock(&fbList->ref_count_mutex);
    freeBuffers = fbList->freeBuffers;
    pthread_mutex_unlock(&fbList->ref_count_mutex);

    return freeBuffers;
}

void SetFreePicBuffer(FrameBufferList *fbList, u32 id)
{
    PushFreeBuffer(fbList, id);
}

void IncrementDPBRefCount(dpbStorage_t *dpb)
{
    u32 i;
    DPB_TRACE("\n");
    for (i = 0; i < dpb->dpbSize; i++)
    {
        IncrementRefUsage(dpb->fbList, dpb->buffer[i].memIdx);
        dpb->refId[i] = dpb->buffer[i].memIdx;
    }
}

void DecrementDPBRefCount(dpbStorage_t *dpb)
{
    u32 i;
    DPB_TRACE("\n");
    for (i = 0; i < dpb->dpbSize; i++)
    {
        DecrementRefUsage(dpb->fbList, dpb->refId[i]);
    }
}

u32 IsBufferReferenced(FrameBufferList *fbList, u32 id)
{
    int nRefCount;
    DPB_TRACE(" %d ? refCount = %d\n", id, fbList->fbStat[id].nRefCount);
    pthread_mutex_lock(&fbList->ref_count_mutex);
    nRefCount = fbList->fbStat[id].nRefCount;
    pthread_mutex_unlock(&fbList->ref_count_mutex);

    return nRefCount != 0 ? 1 : 0;
}

u32 IsBufferOutput(FrameBufferList *fbList, u32 id)
{
    u32 bOutput;
    pthread_mutex_lock(&fbList->ref_count_mutex);
    bOutput = fbList->fbStat[id].bUsed & FB_OUTPUT ? 1 : 0;
    pthread_mutex_unlock(&fbList->ref_count_mutex);

    return bOutput;
}

void MarkOutputPicCorrupt(FrameBufferList *fbList, u32 id, u32 errors)
{
    i32 i, rdId;

    pthread_mutex_lock(&fbList->out_count_mutex);

    rdId = fbList->rdId;

    for(i = 0; i < fbList->numOut; i++)
    {
        if(fbList->outFifo[rdId].memIdx == id)
        {
            DPB_TRACE("id = %d\n", id);
            fbList->outFifo[rdId].pic.nbrOfErrMBs = errors;
            break;
        }

        rdId = (rdId + 1) % MAX_FRAME_BUFFER_NUMBER;
    }

    pthread_mutex_unlock(&fbList->out_count_mutex);
}

void PushOutputPic(FrameBufferList *fbList, const H264DecPicture *pic, u32 id)
{
    if (pic != NULL )
    {
        pthread_mutex_lock(&fbList->out_count_mutex);

        assert(IsBufferOutput(fbList, id));

        while(fbList->numOut == MAX_FRAME_BUFFER_NUMBER)
        {
            /* make sure we do not overflow the output */
            pthread_cond_signal(&fbList->out_empty_cv);
        }

        /* push to tail */
        fbList->outFifo[fbList->wrId].pic = *pic;
        fbList->outFifo[fbList->wrId].memIdx = id;
        fbList->numOut++;

        assert(fbList->numOut <= MAX_FRAME_BUFFER_NUMBER);

        fbList->wrId++;
        if (fbList->wrId >= MAX_FRAME_BUFFER_NUMBER)
            fbList->wrId = 0;

        pthread_mutex_unlock(&fbList->out_count_mutex);
    }

    if (pic != NULL)
        DPB_TRACE("numOut = %d\n",fbList->numOut);
    else
        DPB_TRACE("EOS\n");

    /* pic == NULL signals the end of decoding in which case we just need to
     * wake-up the output thread (potentially sleeping) */
    sem_post(&fbList->out_count_sem);
}

u32 PeekOutputPic(FrameBufferList *fbList, H264DecPicture *pic)
{
    u32 memIdx;

    sem_wait(&fbList->out_count_sem);

    pthread_mutex_lock(&fbList->out_count_mutex);
    if (!fbList->numOut)
    {
        pthread_mutex_unlock(&fbList->out_count_mutex);
        DPB_TRACE("Output empty, EOS\n");
        return 0;
    }
    pthread_mutex_unlock(&fbList->out_count_mutex);

    memIdx = fbList->outFifo[fbList->rdId].memIdx;

    pthread_mutex_lock(&fbList->ref_count_mutex);

    while((fbList->fbStat[memIdx].bUsed & FB_HW_ONGOING) != 0)
        pthread_cond_wait(&fbList->hw_rdy_cv, &fbList->ref_count_mutex);

    pthread_mutex_unlock(&fbList->ref_count_mutex);

    /* pop from head */
    DWLmemcpy(pic, &fbList->outFifo[fbList->rdId].pic, sizeof(H264DecPicture));

    DPB_TRACE("id = %d\n", fbList->outFifo[fbList->rdId].memIdx);

    fbList->lastOut.id = memIdx;
    fbList->lastOut.desc = pic;

    /* we remove it form the FIFO just after it was consumed by App */

    return 1;
}

u32 PopOutputPic(FrameBufferList *fbList, const H264DecPicture *pic)
{
    assert(fbList->lastOut.desc == pic);

    if (fbList->lastOut.desc != pic)
        return 1;

    pthread_mutex_lock(&fbList->out_count_mutex);
    fbList->rdId++;
    if (fbList->rdId >= MAX_FRAME_BUFFER_NUMBER)
        fbList->rdId = 0;
    fbList->numOut--;
    if (fbList->numOut == 0)
    {
        pthread_cond_signal(&fbList->out_empty_cv);
    }
    pthread_mutex_unlock(&fbList->out_count_mutex);

    ClearOutput(fbList, fbList->lastOut.id);

    return 0;
}

void RemoveTempOutputAll(FrameBufferList *fbList)
{
    i32 i;

    for (i = 0; i < MAX_FRAME_BUFFER_NUMBER; i++)
    {
        if (fbList->fbStat[i].bUsed & FB_TEMP_OUTPUT)
        {
            ClearOutput(fbList, i);
        }
    }
}

u32 IsOutputEmpty(FrameBufferList *fbList)
{
    u32 numOut;
    pthread_mutex_lock(&fbList->out_count_mutex);
    numOut = fbList->numOut;
    pthread_mutex_unlock(&fbList->out_count_mutex);

    return numOut == 0 ? 1 : 0;
}

void WaitOutputEmpty(FrameBufferList *fbList)
{
    if (!fbList->bInitialized)
        return;

    pthread_mutex_lock(&fbList->out_count_mutex);
    while (fbList->numOut != 0)
    {
        pthread_cond_wait(&fbList->out_empty_cv, &fbList->out_count_mutex);
    }
    pthread_mutex_unlock(&fbList->out_count_mutex);
}

void WaitListNotInUse(FrameBufferList *fbList)
{
    int i;

    DPB_TRACE("\n");

    if (!fbList->bInitialized)
        return;

    for (i = 0; i < MAX_FRAME_BUFFER_NUMBER; i++)
    {
        pthread_mutex_lock(&fbList->ref_count_mutex);
        /* Wait until all buffers are not referenced */
        while (fbList->fbStat[i].nRefCount != 0)
        {
            pthread_cond_wait(&fbList->ref_count_cv, &fbList->ref_count_mutex);
        }
        pthread_mutex_unlock(&fbList->ref_count_mutex);
    }
}
