/*
* Copyright (c) 2020 - 2025 Renesas Electronics Corporation and/or its affiliates
*
* SPDX-License-Identifier: BSD-3-Clause
*/

/***********************************************************************************************************************
 * Includes
 **********************************************************************************************************************/
#include "r_rsip_primitive.h"
#include "r_rsip_reg.h"
#include "r_rsip_util.h"

/***********************************************************************************************************************
 * Functions
 **********************************************************************************************************************/

rsip_ret_t r_rsip_pe5u (const uint32_t InData_Msg[], uint32_t MAX_CNT)
{
    uint32_t iLoop = 0U;

    WAIT_STS(REG_0118H, 0, 1);

    WR1_PROG(REG_0014H, 0x00020064U);

    for (iLoop = 0U; iLoop < MAX_CNT; iLoop++)
    {
        WAIT_STS(REG_0014H, 31, 1);
        WR1_PROG(REG_002CH, InData_Msg[iLoop]);
    }

    WR1_PROG(REG_0014H, 0x00000000U);
    WAIT_STS(REG_0118H, 8, 0);
    WR1_PROG(REG_0040H, 0x00001600U);

    r_rsip_func101(bswap_32big(0xc9558f0bU),
                   bswap_32big(0xbcdc3dddU),
                   bswap_32big(0xe83d1e7dU),
                   bswap_32big(0x71310ed6U));

    return RSIP_RET_PASS;
}
