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

#include "mp4dechwd_container.h"
#include "mp4dechwd_error_conceal.h"
#include "mp4dechwd_utils.h"
#include "mp4debug.h"

/*------------------------------------------------------------------------------
    2. External identifiers
------------------------------------------------------------------------------*/
/*------------------------------------------------------------------------------
    3. Module defines
------------------------------------------------------------------------------*/

enum
{
    MASK_BIT0 = 0x1,
    MASK_BIT1 = 0x2
};

enum
{
    EC_ABOVE,
    EC_ABOVELEFT,
    EC_ABOVERIGHT,
    EC_NOCANDO
};

/*------------------------------------------------------------------------------
    4. Module indentifiers
------------------------------------------------------------------------------*/

static void PConcealment(DecContainer * pDecCont, u32 mbNumber);

static void IConcealment(DecContainer * pDecCont, u32 mbNumber);

static void ITextureConcealment(DecContainer * pDecCont, u32 mbNumber);

static void MotionVectorConcealment(DecContainer *, u32);

/*------------------------------------------------------------------------------

   5.1  Function name: StrmDec_ErrorConcealment

        Purpose:

        Input:

        Output:

------------------------------------------------------------------------------*/
u32 StrmDec_ErrorConcealment(DecContainer * pDecContainer, u32 start,
    u32 end)
{

    extern const u8 asicPosNoRlc[6];
    u32 i, j;

    u32 vopCodingType = 0;
    u32 controlBits = 0;
    u32 *pCtrl;

    ASSERT(end <= pDecContainer->VopDesc.totalMbInVop);
    ASSERT(start <= end);

    MP4DEC_API_DEBUG(("ErrorConcealment # %d end \n", end));
    vopCodingType = pDecContainer->VopDesc.vopCodingType;

    /* qp to control word */
    controlBits = (u32) (31 & 0x1F) << ASICPOS_QP;
    /* set block type to inter */
    controlBits |= ((u32) 1 << ASICPOS_MBTYPE);

    controlBits |= ((u32) 1 << ASICPOS_CONCEAL);

    controlBits |= ((u32) 1 << ASICPOS_MBNOTCODED);

    for(j = 0; j < 6; j++)
    {
        controlBits |= (1 << asicPosNoRlc[j]);
    }

    for(i = start; i <= end; i++)
    {

        /* pointer to control */
        pCtrl = pDecContainer->MbSetDesc.pCtrlDataAddr + i * NBR_OF_WORDS_MB;
        /* video package boundary */
        if((i == pDecContainer->StrmStorage.vpMbNumber) &&
           (pDecContainer->StrmStorage.shortVideo == HANTRO_FALSE))
        {
            controlBits |= (1 << ASICPOS_VPBI);
        }

        /* write control bits */
        *pCtrl = controlBits;

        if( ( (vopCodingType == PVOP) ||
              (pDecContainer->StrmStorage.validVopHeader == HANTRO_FALSE) ) &&
            pDecContainer->VopDesc.vopNumber)
        {
            PConcealment(pDecContainer, i);
        }
        else if(vopCodingType == IVOP)
        {
            IConcealment(pDecContainer, i);
            ITextureConcealment(pDecContainer, i);
        }

    }

    /* update number of concealed blocks */
    pDecContainer->StrmStorage.numErrMbs += end - start + 1;

    return (EC_OK);
}

/*------------------------------------------------------------------------------

   5.2  Function name:PConcealment

        Purpose:

        Input:

        Output:

------------------------------------------------------------------------------*/

static void PConcealment(DecContainer * pDecContainer, u32 mbNumber)
{
    MotionVectorConcealment(pDecContainer, mbNumber);

    pDecContainer->MBDesc[mbNumber].errorStatus |= 0x80;
    pDecContainer->MBDesc[mbNumber].typeOfMB = MB_INTER;

}

/*------------------------------------------------------------------------------

   5.4  Function name:IConcealment;

        Purpose:

        Input:

        Output:

------------------------------------------------------------------------------*/

static void IConcealment(DecContainer * pDecContainer, u32 mbNumber)
{

    pDecContainer->MBDesc[mbNumber].errorStatus |= 0x80;
    pDecContainer->MBDesc[mbNumber].typeOfMB = MB_INTRA;

}

/*------------------------------------------------------------------------------

   5.5  Function name: ITextureConcealment

        Purpose: i-vop texture concealment

        Input:

        Output:

------------------------------------------------------------------------------*/
static void ITextureConcealment(DecContainer * pDecContainer, u32 mbNumber)
{

    u32 *pCtrl;
    u32 tmp = 0;

    /* pointer to last word of control bits of first block */
    pCtrl = pDecContainer->MbSetDesc.pCtrlDataAddr + mbNumber * NBR_OF_WORDS_MB;

    tmp = (((u32) 1 << ASICPOS_ACPREDFLAG) | ((u32) 1 << ASICPOS_MBTYPE));

    *pCtrl &= ~(tmp);

    pDecContainer->MBDesc[mbNumber].errorStatus |= 0x80;

}

/*------------------------------------------------------------------------------

   5.6  Function name: MotionVectorConcealment

        Purpose:

        Input:

        Output:

------------------------------------------------------------------------------*/
static void MotionVectorConcealment(DecContainer * pDecContainer,
                                    u32 mbNumber)
{

    *(pDecContainer->MbSetDesc.pMvDataAddr + mbNumber*NBR_MV_WORDS_MB) = 0;
    *(pDecContainer->MbSetDesc.pMvDataAddr + mbNumber*NBR_MV_WORDS_MB + 1) = 0;
    *(pDecContainer->MbSetDesc.pMvDataAddr + mbNumber*NBR_MV_WORDS_MB + 2) = 0;
    *(pDecContainer->MbSetDesc.pMvDataAddr + mbNumber*NBR_MV_WORDS_MB + 3) = 0;

}
