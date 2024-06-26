/*
* Copyright (c) 2020 - 2024 Renesas Electronics Corporation and/or its affiliates
*
* SPDX-License-Identifier: BSD-3-Clause
*/

/***********************************************************************************************************************
 * Includes
 **********************************************************************************************************************/
#include "r_rsip_private.h"
#include "r_rsip_primitive.h"
#include "r_rsip_wrapper.h"
#include "r_rsip_reg.h"
#include "r_rsip_addr.h"
#include "r_rsip_util.h"
#include "r_rsip_public.h"
#include "r_rsip_cfg.h"

/***********************************************************************************************************************
 * Macro definitions
 **********************************************************************************************************************/

/* For AES-ECB/CBC/CTR */
#define RSIP_PRV_CMD_AES_CIPHER_ECB_ENCRYPT                 (0U)
#define RSIP_PRV_CMD_AES_CIPHER_ECB_DECRYPT                 (1U)
#define RSIP_PRV_CMD_AES_CIPHER_CBC_ENCRYPT                 (2U)
#define RSIP_PRV_CMD_AES_CIPHER_CBC_DECRYPT                 (3U)
#define RSIP_PRV_CMD_AES_CIPHER_CTR_CRYPT                   (4U)

/* For AES-CCM/GCMC */
#define RSIP_PRV_KEY_TYPE_AES_CCM_GCM_GENERAL_KEY           (0U)
#define RSIP_PRV_KEY_TYPE_AES_CCM_GCM_TLS_KEY               (1U)
#define RSIP_PRV_KEY_TYPE_AES_CCM_GCM_DLMS_COSEM_KEY        (2U)
#define RSIP_PRV_KEY_TYPE_AES_CCM_GCM_IAR_KEY               (3U)
#define RSIP_PRV_KEY_TYPE_AES_CCM_GCM_TLS_1_3_KEY           (4U)

/* For AES-CMAC. These commands do not require byte swapping */
#define RSIP_PRV_CMD_AES_CMAC_GENERATE_WITHOUT_REMAINDER    (0U)
#define RSIP_PRV_CMD_AES_CMAC_GENERATE_WITH_REMAINDER       (1U)
#define RSIP_PRV_CMD_AES_CMAC_VERIFY_WITHOUT_REMAINDER      (2U)
#define RSIP_PRV_CMD_AES_CMAC_VERIFY_WITH_REMAINDER         (3U)

/* For RSA */
/*
 * Maximum retry count of RSA key generation derived from FIPS186-4 B.3.3. 4.7 and 5.8 (1024, 2048, 3072)
 * Maximum retry count of RSA key generation derived from FIPS186-5 A.1.3. 4.7 and 5.8 (4096)
 */
#define RSIP_PRV_MAX_RETRY_COUNT_KEY_GEN_RSA_2048           (2 * (5 * 2048 / 2))
#define RSIP_PRV_MAX_RETRY_COUNT_KEY_GEN_RSA_3072           (2 * (5 * 3072 / 2))
#define RSIP_PRV_MAX_RETRY_COUNT_KEY_GEN_RSA_4096           (5 * 4096 + 10 * 4096)

/* For RFC3394 Key Wrap */
#define RSIP_PRV_WORD_SIZE_RFC3394_WRAPPED_KEY_AES_128      (6U)
#define RSIP_PRV_WORD_SIZE_RFC3394_WRAPPED_KEY_AES_256      (10U)

/* For ECC */
#define RSIP_PRV_WORD_SIZE_SIGNATURE_ECC_521                (40U)
#define RSIP_PRV_BYTE_SIZE_SIGNATURE_PAD_ECC_521            (14U)

/***********************************************************************************************************************
 * Typedef definitions
 **********************************************************************************************************************/

typedef enum e_rsip_aes_mode_extended
{
    RSIP_AES_MODE_EXTENDED_ECB_ENCRYPT,
    RSIP_AES_MODE_EXTENDED_ECB_DECRYPT,
    RSIP_AES_MODE_EXTENDED_CBC_ENCRYPT,
    RSIP_AES_MODE_EXTENDED_CBC_DECRYPT,
    RSIP_AES_MODE_EXTENDED_CTR_CRYPT
} rsip_aes_mode_extended_t;

static const uint32_t gs_cmd_aes_mode[] =
{
    [RSIP_AES_MODE_EXTENDED_ECB_ENCRYPT] = RSIP_PRV_CMD_AES_CIPHER_ECB_ENCRYPT,
    [RSIP_AES_MODE_EXTENDED_ECB_DECRYPT] = RSIP_PRV_CMD_AES_CIPHER_ECB_DECRYPT,
    [RSIP_AES_MODE_EXTENDED_CBC_ENCRYPT] = RSIP_PRV_CMD_AES_CIPHER_CBC_ENCRYPT,
    [RSIP_AES_MODE_EXTENDED_CBC_DECRYPT] = RSIP_PRV_CMD_AES_CIPHER_CBC_DECRYPT,
    [RSIP_AES_MODE_EXTENDED_CTR_CRYPT]   = RSIP_PRV_CMD_AES_CIPHER_CTR_CRYPT
};

typedef enum e_rsip_oem_cmd
{
    RSIP_OEM_CMD_INVALID = 0,
    RSIP_OEM_CMD_AES128  = 5,
    RSIP_OEM_CMD_AES192,
    RSIP_OEM_CMD_AES256,
    RSIP_OEM_CMD_AES128_XTS,
    RSIP_OEM_CMD_AES256_XTS,
    RSIP_OEM_CMD_RSA1024_PUBLIC,
    RSIP_OEM_CMD_RSA1024_PRIVATE,
    RSIP_OEM_CMD_RSA2048_PUBLIC,
    RSIP_OEM_CMD_RSA2048_PRIVATE,
    RSIP_OEM_CMD_RSA3072_PUBLIC,
    RSIP_OEM_CMD_RSA3072_PRIVATE,
    RSIP_OEM_CMD_RSA4096_PUBLIC,
    RSIP_OEM_CMD_RSA4096_PRIVATE,
    RSIP_OEM_CMD_ECC_P192_PUBLIC,
    RSIP_OEM_CMD_ECC_P192_PRIVATE,
    RSIP_OEM_CMD_ECC_P224_PUBLIC,
    RSIP_OEM_CMD_ECC_P224_PRIVATE,
    RSIP_OEM_CMD_ECC_SECP256R1_PUBLIC,
    RSIP_OEM_CMD_ECC_SECP256R1_PRIVATE,
    RSIP_OEM_CMD_ECC_SECP384R1_PUBLIC,
    RSIP_OEM_CMD_ECC_SECP384R1_PRIVATE,
    RSIP_OEM_CMD_HMAC_SHA224,
    RSIP_OEM_CMD_HMAC_SHA256,
    RSIP_OEM_CMD_ECC_BRAINPOOLP256R1_PUBLIC,
    RSIP_OEM_CMD_ECC_BRAINPOOLP256R1_PRIVATE,
    RSIP_OEM_CMD_ECC_BRAINPOOLP384R1_PUBLIC,
    RSIP_OEM_CMD_ECC_BRAINPOOLP384R1_PRIVATE,
    RSIP_OEM_CMD_ECC_BRAINPOOLP512R1_PUBLIC,
    RSIP_OEM_CMD_ECC_BRAINPOOLP512R1_PRIVATE,
    RSIP_OEM_CMD_ECC_SECP256K1_PUBLIC,
    RSIP_OEM_CMD_ECC_SECP256K1_PRIVATE,
    RSIP_OEM_CMD_ECC_SECP521R1_PUBLIC,
    RSIP_OEM_CMD_ECC_SECP521R1_PRIVATE,
    RSIP_OEM_CMD_ECC_EDWARDS25519_PUBLIC,
    RSIP_OEM_CMD_ECC_EDWARDS25519_PRIVATE,
    RSIP_OEM_CMD_HMAC_SHA384,
    RSIP_OEM_CMD_HMAC_SHA512,
    RSIP_OEM_CMD_HMAC_SHA512_224,
    RSIP_OEM_CMD_HMAC_SHA512_256,
    RSIP_OEM_CMD_RAS_2048_PUBLIC_FOR_TLS = 0xfe,
    RSIP_OEM_CMD_NUM
} rsip_oem_cmd_t;

typedef enum e_rsip_rfc3394_key_wrap_type
{
    RSIP_RFC3394_KEY_WRAP_TYPE_AES128,
    RSIP_RFC3394_KEY_WRAP_TYPE_AES192,
    RSIP_RFC3394_KEY_WRAP_TYPE_AES256
} rsip_rfc3394_key_wrap_type_t;

/***********************************************************************************************************************
 * Private function prototypes
 **********************************************************************************************************************/

static rsip_ret_t select_rfc3394_key_wrap_mode (const rsip_key_type_t key_type,
                                                uint32_t              WrappedKeyType[]);
static rsip_ret_t select_hmac_key_index_size (const rsip_wrapped_key_t * p_wrapped_key);

/***********************************************************************************************************************
 * Private global variables
 **********************************************************************************************************************/

/***********************************************************************************************************************
 * Global variables
 **********************************************************************************************************************/

/***********************************************************************************************************************
 * Functions
 **********************************************************************************************************************/

rsip_ret_t r_rsip_wrapper_pf0_secp256r1 (const uint32_t InData_KeyIndex[],
                                         const uint32_t InData_MsgDgst[],
                                         uint32_t       OutData_Signature[])
{
    const uint32_t   InData_Cmd[1]      = {bswap_32big(RSIP_ECC_CMD_P256)};
    const uint32_t * InData_DomainParam = DomainParam_NIST_P256;
    uint32_t         cv_type            = bswap_32big(RSIP_ECC_CURVE_TYPE_NIST);

    return r_rsip_pf0(&cv_type, InData_Cmd, InData_KeyIndex, InData_MsgDgst, InData_DomainParam, OutData_Signature);
}

rsip_ret_t r_rsip_wrapper_pf1_secp256r1 (const uint32_t InData_KeyIndex[],
                                         const uint32_t InData_MsgDgst[],
                                         const uint32_t InData_Signature[])
{
    const uint32_t   InData_Cmd[1]      = {bswap_32big(RSIP_ECC_CMD_P256)};
    const uint32_t * InData_DomainParam = DomainParam_NIST_P256;
    uint32_t         cv_type            = bswap_32big(RSIP_ECC_CURVE_TYPE_NIST);

    return r_rsip_pf1(&cv_type, InData_Cmd, InData_KeyIndex, InData_MsgDgst, InData_Signature, InData_DomainParam);
}

rsip_ret_t r_rsip_wrapper_pf4_secp256r1 (uint32_t OutData_PubKeyIndex[], uint32_t OutData_PrivKeyIndex[])
{
    uint32_t         InData_CurveType[1] = {RSIP_ECC_CURVE_TYPE_NIST};
    uint32_t         InData_Cmd[1]       = {RSIP_ECC_CMD_P256};
    uint32_t const * p_domain_param      = DomainParam_NIST_P256;

    return r_rsip_pf4(InData_CurveType, InData_Cmd, p_domain_param, OutData_PubKeyIndex, OutData_PrivKeyIndex);
}

rsip_ret_t r_rsip_wrapper_pf5_secp384r1 (const uint32_t InData_KeyIndex[],
                                         const uint32_t InData_MsgDgst[],
                                         uint32_t       OutData_Signature[])
{
    const uint32_t * InData_DomainParam = DomainParam_NIST_P384;
    uint32_t         cv_type            = bswap_32big(RSIP_ECC_CURVE_TYPE_NIST);

    return r_rsip_pf5(&cv_type, InData_KeyIndex, InData_MsgDgst, InData_DomainParam, OutData_Signature);
}

rsip_ret_t r_rsip_wrapper_pf6_secp384r1 (const uint32_t InData_KeyIndex[],
                                         const uint32_t InData_MsgDgst[],
                                         const uint32_t InData_Signature[])
{
    const uint32_t * InData_DomainParam = DomainParam_NIST_P384;
    uint32_t         cv_type            = bswap_32big(RSIP_ECC_CURVE_TYPE_NIST);

    return r_rsip_pf6(&cv_type, InData_KeyIndex, InData_MsgDgst, InData_Signature, InData_DomainParam);
}

rsip_ret_t r_rsip_wrapper_pf9_secp384r1 (uint32_t OutData_PubKeyIndex[], uint32_t OutData_PrivKeyIndex[])
{
    uint32_t         InData_CurveType[1] = {RSIP_ECC_CURVE_TYPE_NIST};
    uint32_t const * p_domain_param      = DomainParam_NIST_P384;

    return r_rsip_pf9(InData_CurveType, p_domain_param, OutData_PubKeyIndex, OutData_PrivKeyIndex);
}


rsip_ret_t r_rsip_wrapper_p11_secp521r1 (const uint32_t InData_KeyIndex[],
                                         const uint32_t InData_MsgDgst[],
                                         uint32_t       OutData_Signature[])
{
    rsip_ret_t rsip_ret;
    const uint32_t * InData_DomainParam = DomainParam_NIST_P521;
    uint32_t Signature[RSIP_PRV_WORD_SIZE_SIGNATURE_ECC_521] = {0};
    const uint32_t signature_r_pad_byte = RSIP_PRV_BYTE_SIZE_SIGNATURE_PAD_ECC_521;
    const uint32_t signature_s_pad_byte = RSIP_PRV_BYTE_SIZE_SIGNATURE_PAD_ECC_521
                                         + RSIP_PRV_BYTE_SIZE_ECC_521_PARAM
                                         + RSIP_PRV_BYTE_SIZE_SIGNATURE_PAD_ECC_521;

    rsip_ret = r_rsip_p11(InData_KeyIndex, InData_MsgDgst, InData_DomainParam, Signature);
    if (RSIP_RET_PASS == rsip_ret)
    {
        memcpy((uint8_t*)OutData_Signature, 
               (uint8_t*)Signature + signature_r_pad_byte,
               RSIP_PRV_BYTE_SIZE_ECC_521_PARAM);
        memcpy((uint8_t*)OutData_Signature + RSIP_PRV_BYTE_SIZE_ECC_521_PARAM,
               (uint8_t*)Signature + signature_s_pad_byte,
               RSIP_PRV_BYTE_SIZE_ECC_521_PARAM);
    }

    return rsip_ret;
}

rsip_ret_t r_rsip_wrapper_p12_secp521r1 (const uint32_t InData_KeyIndex[],
                                         const uint32_t InData_MsgDgst[],
                                         const uint32_t InData_Signature[])
{
    const uint32_t * InData_DomainParam = DomainParam_NIST_P521;
    uint32_t Signature[RSIP_PRV_WORD_SIZE_SIGNATURE_ECC_521] = {0};
    const uint32_t signature_r_pad_byte = RSIP_PRV_BYTE_SIZE_SIGNATURE_PAD_ECC_521;
    const uint32_t signature_s_pad_byte = RSIP_PRV_BYTE_SIZE_SIGNATURE_PAD_ECC_521
                                         + RSIP_PRV_BYTE_SIZE_ECC_521_PARAM
                                         + RSIP_PRV_BYTE_SIZE_SIGNATURE_PAD_ECC_521;

    memcpy((uint8_t*)Signature + signature_r_pad_byte, (uint8_t*)InData_Signature, RSIP_PRV_BYTE_SIZE_ECC_521_PARAM);
    memcpy((uint8_t*)Signature + signature_s_pad_byte, (uint8_t*)InData_Signature + RSIP_PRV_BYTE_SIZE_ECC_521_PARAM, 
           RSIP_PRV_BYTE_SIZE_ECC_521_PARAM);

    return r_rsip_p12(InData_KeyIndex, InData_MsgDgst, Signature, InData_DomainParam);
}

rsip_ret_t r_rsip_wrapper_p13_secp521r1 (uint32_t OutData_PubKeyIndex[], uint32_t OutData_PrivKeyIndex[])
{
    uint32_t const * p_domain_param      = DomainParam_NIST_P521;

    return r_rsip_p13(p_domain_param, OutData_PubKeyIndex, OutData_PrivKeyIndex);
}

rsip_ret_t r_rsip_wrapper_p2b_rsa2048 (uint32_t OutData_PubKeyIndex[], uint32_t OutData_PrivKeyIndex[])
{
    return r_rsip_p2b(RSIP_PRV_MAX_RETRY_COUNT_KEY_GEN_RSA_2048, OutData_PubKeyIndex, OutData_PrivKeyIndex);
}

rsip_ret_t r_rsip_wrapper_p3a_rsa3072 (uint32_t OutData_PubKeyIndex[], uint32_t OutData_PrivKeyIndex[])
{
    return r_rsip_p3a(RSIP_PRV_MAX_RETRY_COUNT_KEY_GEN_RSA_3072, OutData_PubKeyIndex, OutData_PrivKeyIndex);
}

rsip_ret_t r_rsip_wrapper_p3b_rsa4096 (uint32_t OutData_PubKeyIndex[], uint32_t OutData_PrivKeyIndex[])
{
    return r_rsip_p3b(RSIP_PRV_MAX_RETRY_COUNT_KEY_GEN_RSA_4096, OutData_PubKeyIndex, OutData_PrivKeyIndex);
}

rsip_ret_t r_rsip_wrapper_p6f_aes128 (const uint32_t InData_IV[],
                                      const uint32_t InData_InstData[],
                                      uint32_t       OutData_KeyIndex[])
{
    uint32_t CMD[1] = {bswap_32big(RSIP_OEM_CMD_AES128)};
    uint32_t LC[1]  = {0};
    LC[0]          = R_PSCU->DLMMON;
    INST_DATA_SIZE = RSIP_OEM_KEY_SIZE_AES128_INST_DATA_WORD;

    return r_rsip_p6f(LC, CMD, InData_IV, InData_InstData, OutData_KeyIndex);
}

rsip_ret_t r_rsip_wrapper_p6f_aes256 (const uint32_t InData_IV[],
                                      const uint32_t InData_InstData[],
                                      uint32_t       OutData_KeyIndex[])
{
    uint32_t CMD[1] = {bswap_32big(RSIP_OEM_CMD_AES256)};
    uint32_t LC[1]  = {0};
    LC[0]          = R_PSCU->DLMMON;
    INST_DATA_SIZE = RSIP_OEM_KEY_SIZE_AES256_INST_DATA_WORD;

    return r_rsip_p6f(LC, CMD, InData_IV, InData_InstData, OutData_KeyIndex);
}

rsip_ret_t r_rsip_wrapper_p6f_aes128xts (const uint32_t InData_IV[],
                                         const uint32_t InData_InstData[],
                                         uint32_t       OutData_KeyIndex[])
{
    uint32_t CMD[1] = {bswap_32big(RSIP_OEM_CMD_AES128_XTS)};
    uint32_t LC[1]  = {0};
    LC[0]          = R_PSCU->DLMMON;
    INST_DATA_SIZE = RSIP_OEM_KEY_SIZE_AES128_XTS_INST_DATA_WORD;

    return r_rsip_p6f(LC, CMD, InData_IV, InData_InstData, OutData_KeyIndex);
}

rsip_ret_t r_rsip_wrapper_p6f_aes256xts (const uint32_t InData_IV[],
                                         const uint32_t InData_InstData[],
                                         uint32_t       OutData_KeyIndex[])
{
    uint32_t CMD[1] = {bswap_32big(RSIP_OEM_CMD_AES256_XTS)};
    uint32_t LC[1]  = {0};
    LC[0]          = R_PSCU->DLMMON;
    INST_DATA_SIZE = RSIP_OEM_KEY_SIZE_AES256_XTS_INST_DATA_WORD;

    return r_rsip_p6f(LC, CMD, InData_IV, InData_InstData, OutData_KeyIndex);
}

rsip_ret_t r_rsip_wrapper_p6f_secp256r1_pub (const uint32_t InData_IV[],
                                             const uint32_t InData_InstData[],
                                             uint32_t       OutData_KeyIndex[])
{
    uint32_t CMD[1] = {bswap_32big(RSIP_OEM_CMD_ECC_SECP256R1_PUBLIC)};
    uint32_t LC[1]  = {0};
    LC[0]          = R_PSCU->DLMMON;
    INST_DATA_SIZE = RSIP_OEM_KEY_SIZE_ECC_SECP256R1_PUBLIC_KEY_INST_DATA_WORD;

    return r_rsip_p6f(LC, CMD, InData_IV, InData_InstData, OutData_KeyIndex);
}

rsip_ret_t r_rsip_wrapper_p6f_secp256r1_priv (const uint32_t InData_IV[],
                                              const uint32_t InData_InstData[],
                                              uint32_t       OutData_KeyIndex[])
{
    uint32_t CMD[1] = {bswap_32big(RSIP_OEM_CMD_ECC_SECP256R1_PRIVATE)};
    uint32_t LC[1]  = {0};
    LC[0]          = R_PSCU->DLMMON;
    INST_DATA_SIZE = RSIP_OEM_KEY_SIZE_ECC_SECP256R1_PRIVATE_KEY_INST_DATA_WORD;

    return r_rsip_p6f(LC, CMD, InData_IV, InData_InstData, OutData_KeyIndex);
}

rsip_ret_t r_rsip_wrapper_p6f_secp384r1_pub (const uint32_t InData_IV[],
                                             const uint32_t InData_InstData[],
                                             uint32_t       OutData_KeyIndex[])
{
    uint32_t CMD[1] = {bswap_32big(RSIP_OEM_CMD_ECC_SECP384R1_PUBLIC)};
    uint32_t LC[1]  = {0};
    LC[0]          = R_PSCU->DLMMON;
    INST_DATA_SIZE = RSIP_OEM_KEY_SIZE_ECC_SECP384R1_PUBLIC_KEY_INST_DATA_WORD;

    return r_rsip_p6f(LC, CMD, InData_IV, InData_InstData, OutData_KeyIndex);
}

rsip_ret_t r_rsip_wrapper_p6f_secp384r1_priv (const uint32_t InData_IV[],
                                              const uint32_t InData_InstData[],
                                              uint32_t       OutData_KeyIndex[])
{
    uint32_t CMD[1] = {bswap_32big(RSIP_OEM_CMD_ECC_SECP384R1_PRIVATE)};
    uint32_t LC[1]  = {0};
    LC[0]          = R_PSCU->DLMMON;
    INST_DATA_SIZE = RSIP_OEM_KEY_SIZE_ECC_SECP384R1_PRIVATE_KEY_INST_DATA_WORD;

    return r_rsip_p6f(LC, CMD, InData_IV, InData_InstData, OutData_KeyIndex);
}

rsip_ret_t r_rsip_wrapper_p6f_secp521r1_pub (const uint32_t InData_IV[],
                                             const uint32_t InData_InstData[],
                                             uint32_t       OutData_KeyIndex[])
{
    uint32_t CMD[1] = {bswap_32big(RSIP_OEM_CMD_ECC_SECP521R1_PUBLIC)};
    uint32_t LC[1]  = {0};
    LC[0]          = R_PSCU->DLMMON;
    INST_DATA_SIZE = RSIP_OEM_KEY_SIZE_ECC_SECP521R1_PUBLIC_KEY_INST_DATA_WORD;

    return r_rsip_p6f(LC, CMD, InData_IV, InData_InstData, OutData_KeyIndex);
}

rsip_ret_t r_rsip_wrapper_p6f_secp521r1_priv (const uint32_t InData_IV[],
                                              const uint32_t InData_InstData[],
                                              uint32_t       OutData_KeyIndex[])
{
    uint32_t CMD[1] = {bswap_32big(RSIP_OEM_CMD_ECC_SECP521R1_PRIVATE)};
    uint32_t LC[1]  = {0};
    LC[0]          = R_PSCU->DLMMON;
    INST_DATA_SIZE = RSIP_OEM_KEY_SIZE_ECC_SECP521R1_PRIVATE_KEY_INST_DATA_WORD;

    return r_rsip_p6f(LC, CMD, InData_IV, InData_InstData, OutData_KeyIndex);
}

rsip_ret_t r_rsip_wrapper_p6f_rsa2048_pub (const uint32_t InData_IV[],
                                           const uint32_t InData_InstData[],
                                           uint32_t       OutData_KeyIndex[])
{
    uint32_t CMD[1] = {bswap_32big(RSIP_OEM_CMD_RSA2048_PUBLIC)};
    uint32_t LC[1]  = {0};
    LC[0]          = R_PSCU->DLMMON;
    INST_DATA_SIZE = RSIP_OEM_KEY_SIZE_RSA2048_PUBLIC_KEY_INST_DATA_WORD;

    return r_rsip_p6f(LC, CMD, InData_IV, InData_InstData, OutData_KeyIndex);
}

rsip_ret_t r_rsip_wrapper_p6f_rsa2048_priv (const uint32_t InData_IV[],
                                            const uint32_t InData_InstData[],
                                            uint32_t       OutData_KeyIndex[])
{
    uint32_t CMD[1] = {bswap_32big(RSIP_OEM_CMD_RSA2048_PRIVATE)};
    uint32_t LC[1]  = {0};
    LC[0]          = R_PSCU->DLMMON;
    INST_DATA_SIZE = RSIP_OEM_KEY_SIZE_RSA2048_PRIVATE_KEY_INST_DATA_WORD;

    return r_rsip_p6f(LC, CMD, InData_IV, InData_InstData, OutData_KeyIndex);
}

rsip_ret_t r_rsip_wrapper_p6f_rsa3072_pub (const uint32_t InData_IV[],
                                           const uint32_t InData_InstData[],
                                           uint32_t       OutData_KeyIndex[])
{
    uint32_t CMD[1] = {bswap_32big(RSIP_OEM_CMD_RSA3072_PUBLIC)};
    uint32_t LC[1]  = {0};
    LC[0]          = R_PSCU->DLMMON;
    INST_DATA_SIZE = RSIP_OEM_KEY_SIZE_RSA3072_PUBLIC_KEY_INST_DATA_WORD;

    return r_rsip_p6f(LC, CMD, InData_IV, InData_InstData, OutData_KeyIndex);
}

rsip_ret_t r_rsip_wrapper_p6f_rsa3072_priv (const uint32_t InData_IV[],
                                            const uint32_t InData_InstData[],
                                            uint32_t       OutData_KeyIndex[])
{
    uint32_t CMD[1] = {bswap_32big(RSIP_OEM_CMD_RSA3072_PRIVATE)};
    uint32_t LC[1]  = {0};
    LC[0]          = R_PSCU->DLMMON;
    INST_DATA_SIZE = RSIP_OEM_KEY_SIZE_RSA3072_PRIVATE_KEY_INST_DATA_WORD;

    return r_rsip_p6f(LC, CMD, InData_IV, InData_InstData, OutData_KeyIndex);
}

rsip_ret_t r_rsip_wrapper_p6f_rsa4096_pub (const uint32_t InData_IV[],
                                           const uint32_t InData_InstData[],
                                           uint32_t       OutData_KeyIndex[])
{
    uint32_t CMD[1] = {bswap_32big(RSIP_OEM_CMD_RSA4096_PUBLIC)};
    uint32_t LC[1]  = {0};
    LC[0]          = R_PSCU->DLMMON;
    INST_DATA_SIZE = RSIP_OEM_KEY_SIZE_RSA4096_PUBLIC_KEY_INST_DATA_WORD;

    return r_rsip_p6f(LC, CMD, InData_IV, InData_InstData, OutData_KeyIndex);
}

rsip_ret_t r_rsip_wrapper_p6f_rsa4096_priv (const uint32_t InData_IV[],
                                            const uint32_t InData_InstData[],
                                            uint32_t       OutData_KeyIndex[])
{
    uint32_t CMD[1] = {bswap_32big(RSIP_OEM_CMD_RSA4096_PRIVATE)};
    uint32_t LC[1]  = {0};
    LC[0]          = R_PSCU->DLMMON;
    INST_DATA_SIZE = RSIP_OEM_KEY_SIZE_RSA4096_PRIVATE_KEY_INST_DATA_WORD;

    return r_rsip_p6f(LC, CMD, InData_IV, InData_InstData, OutData_KeyIndex);
}

rsip_ret_t r_rsip_wrapper_p6f_hmacsha256 (const uint32_t InData_IV[],
                                          const uint32_t InData_InstData[],
                                          uint32_t       OutData_KeyIndex[])
{
    uint32_t CMD[1] = {bswap_32big(RSIP_OEM_CMD_HMAC_SHA256)};
    uint32_t LC[1]  = {0};
    LC[0]          = R_PSCU->DLMMON;
    INST_DATA_SIZE = RSIP_OEM_KEY_SIZE_HMAC_SHA256_KEY_INST_DATA_WORD;

    return r_rsip_p6f(LC, CMD, InData_IV, InData_InstData, OutData_KeyIndex);
}

rsip_ret_t r_rsip_wrapper_p6f_hmacsha384 (const uint32_t InData_IV[],
                                          const uint32_t InData_InstData[],
                                          uint32_t       OutData_KeyIndex[])
{
    uint32_t CMD[1] = {bswap_32big(RSIP_OEM_CMD_HMAC_SHA384)};
    uint32_t LC[1]  = {0};
    LC[0]          = R_PSCU->DLMMON;
    INST_DATA_SIZE = RSIP_OEM_KEY_SIZE_HMAC_SHA384_KEY_INST_DATA_WORD;

    return r_rsip_p6f(LC, CMD, InData_IV, InData_InstData, OutData_KeyIndex);
}

rsip_ret_t r_rsip_wrapper_p6f_hmacsha512 (const uint32_t InData_IV[],
                                          const uint32_t InData_InstData[],
                                          uint32_t       OutData_KeyIndex[])
{
    uint32_t CMD[1] = {bswap_32big(RSIP_OEM_CMD_HMAC_SHA512)};
    uint32_t LC[1]  = {0};
    LC[0]          = R_PSCU->DLMMON;
    INST_DATA_SIZE = RSIP_OEM_KEY_SIZE_HMAC_SHA512_KEY_INST_DATA_WORD;

    return r_rsip_p6f(LC, CMD, InData_IV, InData_InstData, OutData_KeyIndex);
}

rsip_ret_t r_rsip_wrapper_p75i (const rsip_wrapped_key_t * p_wrapped_key,
                                const uint32_t InData_HashType[],
                                const uint32_t InData_MsgLen[])
{
    rsip_ret_t rsip_ret = select_hmac_key_index_size(p_wrapped_key);
    if (RSIP_RET_PASS == rsip_ret)
    {
        rsip_ret = r_rsip_p75i((uint32_t *) p_wrapped_key->value, InData_HashType, InData_MsgLen);
    }

    return rsip_ret;
}

rsip_ret_t r_rsip_wrapper_p75r (const rsip_wrapped_key_t * p_wrapped_key,
                                const uint32_t InData_HashType[],
                                const uint32_t InData_State[])
{
    rsip_ret_t rsip_ret = select_hmac_key_index_size(p_wrapped_key);
    if (RSIP_RET_PASS == rsip_ret)
    {
        rsip_ret = r_rsip_p75r((uint32_t *) p_wrapped_key->value, InData_HashType, InData_State);
    }

    return rsip_ret;
}

rsip_ret_t r_rsip_wrapper_p8f_aes128 (const uint32_t        InData_KeyIndex[],
                                      const rsip_key_type_t key_type,
                                      const uint32_t        InData_WrappedKeyIndex[],
                                      uint32_t              OutData_Text[])
{
    uint32_t InData_Cmd[1] = {bswap_32big(0)};
    uint32_t InData_WrappedKeyType[1];

    rsip_ret_t rsip_ret = select_rfc3394_key_wrap_mode(key_type, InData_WrappedKeyType);
    if (RSIP_RET_PASS == rsip_ret)
    {
        rsip_ret = r_rsip_p8f(InData_Cmd, InData_KeyIndex, InData_WrappedKeyType, InData_WrappedKeyIndex, OutData_Text);
    }

    return rsip_ret;
}

rsip_ret_t r_rsip_wrapper_p8f_aes256 (const uint32_t        InData_KeyIndex[],
                                      const rsip_key_type_t key_type,
                                      const uint32_t        InData_WrappedKeyIndex[],
                                      uint32_t              OutData_Text[])
{
    uint32_t InData_Cmd[1] = {bswap_32big(1)};
    uint32_t InData_WrappedKeyType[1];

    rsip_ret_t rsip_ret = select_rfc3394_key_wrap_mode(key_type, InData_WrappedKeyType);
    if (RSIP_RET_PASS == rsip_ret)
    {
        rsip_ret = r_rsip_p8f(InData_Cmd, InData_KeyIndex, InData_WrappedKeyType, InData_WrappedKeyIndex, OutData_Text);
    }

    return rsip_ret;
}

rsip_ret_t r_rsip_wrapper_p90_aes128 (const uint32_t        InData_KeyIndex[],
                                      const rsip_key_type_t key_type,
                                      const uint32_t        InData_Text[],
                                      uint32_t              OutData_KeyIndex[])
{
    uint32_t InData_KeyType[1] = {bswap_32big(0)};
    uint32_t InData_Cmd[1]     = {bswap_32big(0)};
    uint32_t InData_WrappedKeyType[1];

    rsip_ret_t rsip_ret = select_rfc3394_key_wrap_mode(key_type, InData_WrappedKeyType);
    if (RSIP_RET_PASS == rsip_ret)
    {
        rsip_ret = r_rsip_p90(InData_KeyType, InData_Cmd, InData_KeyIndex, InData_WrappedKeyType, InData_Text, OutData_KeyIndex);
    }

    return rsip_ret;
}

rsip_ret_t r_rsip_wrapper_p90_aes256 (const uint32_t        InData_KeyIndex[],
                                      const rsip_key_type_t key_type,
                                      const uint32_t        InData_Text[],
                                      uint32_t              OutData_KeyIndex[])
{
    uint32_t InData_KeyType[1] = {bswap_32big(0)};
    uint32_t InData_Cmd[1]     = {bswap_32big(1)};
    uint32_t InData_WrappedKeyType[1];

    rsip_ret_t rsip_ret = select_rfc3394_key_wrap_mode(key_type, InData_WrappedKeyType);
    if (RSIP_RET_PASS == rsip_ret)
    {
        rsip_ret = r_rsip_p90(InData_KeyType, InData_Cmd, InData_KeyIndex, InData_WrappedKeyType, InData_Text, OutData_KeyIndex);
    }

    return rsip_ret;
}

rsip_ret_t r_rsip_wrapper_p47i_aes128ecb_encrypt (const uint32_t InData_KeyIndex[], const uint32_t InData_IV[])
{
    uint32_t key_type = bswap_32big(0);
    uint32_t cmd      = bswap_32big(gs_cmd_aes_mode[RSIP_AES_MODE_EXTENDED_ECB_ENCRYPT]);

    return r_rsip_p47i(&key_type, &cmd, InData_KeyIndex, InData_IV);
}

rsip_ret_t r_rsip_wrapper_p47i_aes128ecb_decrypt (const uint32_t InData_KeyIndex[], const uint32_t InData_IV[])
{
    uint32_t key_type = bswap_32big(0);
    uint32_t cmd      = bswap_32big(gs_cmd_aes_mode[RSIP_AES_MODE_EXTENDED_ECB_DECRYPT]);

    return r_rsip_p47i(&key_type, &cmd, InData_KeyIndex, InData_IV);
}

rsip_ret_t r_rsip_wrapper_p47i_aes128cbc_encrypt (const uint32_t InData_KeyIndex[], const uint32_t InData_IV[])
{
    uint32_t key_type = bswap_32big(0);
    uint32_t cmd      = bswap_32big(gs_cmd_aes_mode[RSIP_AES_MODE_EXTENDED_CBC_ENCRYPT]);

    return r_rsip_p47i(&key_type, &cmd, InData_KeyIndex, InData_IV);
}

rsip_ret_t r_rsip_wrapper_p47i_aes128cbc_decrypt (const uint32_t InData_KeyIndex[], const uint32_t InData_IV[])
{
    uint32_t key_type = bswap_32big(0);
    uint32_t cmd      = bswap_32big(gs_cmd_aes_mode[RSIP_AES_MODE_EXTENDED_CBC_DECRYPT]);

    return r_rsip_p47i(&key_type, &cmd, InData_KeyIndex, InData_IV);
}

rsip_ret_t r_rsip_wrapper_p47i_aes128ctr_crypt (const uint32_t InData_KeyIndex[], const uint32_t InData_IV[])
{
    uint32_t key_type = bswap_32big(0);
    uint32_t cmd      = bswap_32big(gs_cmd_aes_mode[RSIP_AES_MODE_EXTENDED_CTR_CRYPT]);

    return r_rsip_p47i(&key_type, &cmd, InData_KeyIndex, InData_IV);
}

rsip_ret_t r_rsip_wrapper_p50i_aes256ecb_encrypt (const uint32_t InData_KeyIndex[], const uint32_t InData_IV[])
{
    uint32_t key_type = bswap_32big(0);
    uint32_t cmd      = bswap_32big(gs_cmd_aes_mode[RSIP_AES_MODE_EXTENDED_ECB_ENCRYPT]);

    return r_rsip_p50i(&key_type, &cmd, InData_KeyIndex, InData_IV);
}

rsip_ret_t r_rsip_wrapper_p50i_aes256ecb_decrypt (const uint32_t InData_KeyIndex[], const uint32_t InData_IV[])
{
    uint32_t key_type = bswap_32big(0);
    uint32_t cmd      = bswap_32big(gs_cmd_aes_mode[RSIP_AES_MODE_EXTENDED_ECB_DECRYPT]);

    return r_rsip_p50i(&key_type, &cmd, InData_KeyIndex, InData_IV);
}

rsip_ret_t r_rsip_wrapper_p50i_aes256cbc_encrypt (const uint32_t InData_KeyIndex[], const uint32_t InData_IV[])
{
    uint32_t key_type = bswap_32big(0);
    uint32_t cmd      = bswap_32big(gs_cmd_aes_mode[RSIP_AES_MODE_EXTENDED_CBC_ENCRYPT]);

    return r_rsip_p50i(&key_type, &cmd, InData_KeyIndex, InData_IV);
}

rsip_ret_t r_rsip_wrapper_p50i_aes256cbc_decrypt (const uint32_t InData_KeyIndex[], const uint32_t InData_IV[])
{
    uint32_t key_type = bswap_32big(0);
    uint32_t cmd      = bswap_32big(gs_cmd_aes_mode[RSIP_AES_MODE_EXTENDED_CBC_DECRYPT]);

    return r_rsip_p50i(&key_type, &cmd, InData_KeyIndex, InData_IV);
}

rsip_ret_t r_rsip_wrapper_p50i_aes256ctr_crypt (const uint32_t InData_KeyIndex[], const uint32_t InData_IV[])
{
    uint32_t key_type = bswap_32big(0);
    uint32_t cmd      = bswap_32big(gs_cmd_aes_mode[RSIP_AES_MODE_EXTENDED_CTR_CRYPT]);

    return r_rsip_p50i(&key_type, &cmd, InData_KeyIndex, InData_IV);
}

rsip_ret_t r_rsip_wrapper_p29i_aes128gcm_encrypt (const uint32_t * InData_KeyIndex, const uint32_t * InData_IV)
{
    uint32_t InData_KeyType[1]  = {bswap_32big(RSIP_PRV_KEY_TYPE_AES_CCM_GCM_GENERAL_KEY)};
    uint32_t InData_DataType[1] = {0};
    uint32_t InData_Cmd[1]      = {0};
    uint32_t InData_SeqNum[2]   = {0};

    return r_rsip_p29i(InData_KeyType, InData_DataType, InData_Cmd, InData_KeyIndex, InData_IV, InData_SeqNum);
}

rsip_ret_t r_rsip_wrapper_p32i_aes128gcm_decrypt (const uint32_t * InData_KeyIndex, const uint32_t * InData_IV)
{
    uint32_t InData_KeyType[1]  = {bswap_32big(RSIP_PRV_KEY_TYPE_AES_CCM_GCM_GENERAL_KEY)};
    uint32_t InData_DataType[1] = {0};
    uint32_t InData_Cmd[1]      = {0};
    uint32_t InData_SeqNum[2]   = {0};

    return r_rsip_p32i(InData_KeyType, InData_DataType, InData_Cmd, InData_KeyIndex, InData_IV, InData_SeqNum);
}

rsip_ret_t r_rsip_wrapper_p34i_aes256gcm_encrypt (const uint32_t * InData_KeyIndex, const uint32_t * InData_IV)
{
    uint32_t InData_KeyType[1] = {bswap_32big(RSIP_PRV_KEY_TYPE_AES_CCM_GCM_GENERAL_KEY)};

    return r_rsip_p34i(InData_KeyType, InData_KeyIndex, InData_IV);
}

rsip_ret_t r_rsip_wrapper_p36i_aes256gcm_decrypt (const uint32_t * InData_KeyIndex, const uint32_t * InData_IV)
{
    uint32_t InData_KeyType[1] = {bswap_32big(RSIP_PRV_KEY_TYPE_AES_CCM_GCM_GENERAL_KEY)};

    return r_rsip_p36i(InData_KeyType, InData_KeyIndex, InData_IV);
}

rsip_ret_t r_rsip_wrapper_p95i_aes128ccm_encrypt (const uint32_t * InData_KeyIndex,
                                                  const uint32_t * InData_TextLen,
                                                  const uint32_t * InData_IV,
                                                  const uint32_t * InData_Header,
                                                  uint32_t         Header_Len)
{
    uint32_t InData_KeyType[1]  = {bswap_32big(RSIP_PRV_KEY_TYPE_AES_CCM_GCM_GENERAL_KEY)};
    uint32_t InData_DataType[1] = {0};
    uint32_t InData_Cmd[1]      = {0};
    uint32_t InData_SeqNum[2]   = {0};

    return r_rsip_p95i(InData_KeyType,
                       InData_DataType,
                       InData_Cmd,
                       InData_TextLen,
                       InData_KeyIndex,
                       InData_IV,
                       InData_Header,
                       InData_SeqNum,
                       Header_Len);
}

rsip_ret_t r_rsip_wrapper_p95f_aes128ccm_encrypt (const uint32_t * InData_Text,
                                                  const uint32_t * InData_TextLen,
                                                  uint32_t       * OutData_Text,
                                                  uint32_t       * OutData_MAC)
{
    (void) InData_TextLen;

    return r_rsip_p95f(InData_Text, OutData_Text, OutData_MAC);
}

rsip_ret_t r_rsip_wrapper_p98i_aes128ccm_decrypt (const uint32_t * InData_KeyIndex,
                                                  const uint32_t * InData_TextLen,
                                                  const uint32_t * InData_MACLength,
                                                  const uint32_t * InData_IV,
                                                  const uint32_t * InData_Header,
                                                  uint32_t         Header_Len)
{
    uint32_t InData_KeyType[1]  = {bswap_32big(RSIP_PRV_KEY_TYPE_AES_CCM_GCM_GENERAL_KEY)};
    uint32_t InData_DataType[1] = {0};
    uint32_t InData_Cmd[1]      = {0};
    uint32_t InData_SeqNum[2]   = {0};

    return r_rsip_p98i(InData_KeyType,
                       InData_DataType,
                       InData_Cmd,
                       InData_TextLen,
                       InData_MACLength,
                       InData_KeyIndex,
                       InData_IV,
                       InData_Header,
                       InData_SeqNum,
                       Header_Len);
}

rsip_ret_t r_rsip_wrapper_p98f_aes128ccm_decrypt (const uint32_t * InData_Text,
                                                  const uint32_t * InData_TextLen,
                                                  const uint32_t * InData_MAC,
                                                  const uint32_t * InData_MACLength,
                                                  uint32_t       * OutData_Text)
{
    (void) InData_TextLen;
    (void) InData_MACLength;

    return r_rsip_p98f(InData_Text, InData_MAC, OutData_Text);
}

rsip_ret_t r_rsip_wrapper_pa1i_aes256ccm_encrypt (const uint32_t * InData_KeyIndex,
                                                  const uint32_t * InData_TextLen,
                                                  const uint32_t * InData_IV,
                                                  const uint32_t * InData_Header,
                                                  uint32_t         Header_Len)
{
    uint32_t InData_KeyType[1] = {bswap_32big(RSIP_PRV_KEY_TYPE_AES_CCM_GCM_GENERAL_KEY)};
    (void) InData_TextLen;

    return r_rsip_pa1i(InData_KeyType, InData_KeyIndex, InData_IV, InData_Header, Header_Len);
}

rsip_ret_t r_rsip_wrapper_pa4i_aes256ccm_decrypt (const uint32_t * InData_KeyIndex,
                                                  const uint32_t * InData_TextLen,
                                                  const uint32_t * InData_MACLength,
                                                  const uint32_t * InData_IV,
                                                  const uint32_t * InData_Header,
                                                  uint32_t         Header_Len)
{
    uint32_t InData_KeyType[1] = {bswap_32big(RSIP_PRV_KEY_TYPE_AES_CCM_GCM_GENERAL_KEY)};
    (void) InData_TextLen;
    (void) InData_MACLength;

    return r_rsip_pa4i(InData_KeyType, InData_KeyIndex, InData_IV, InData_Header, Header_Len);
}

rsip_ret_t r_rsip_wrapper_p41i_aes128mac (const uint32_t * InData_KeyIndex)
{
    uint32_t key_type = bswap_32big(0);

    return r_rsip_p41i(&key_type, InData_KeyIndex);
}

rsip_ret_t r_rsip_wrapper_p41f_aes128mac_generate (const uint32_t * InData_Text,
                                                   uint32_t       * OutData_DataT,
                                                   const uint32_t   all_msg_len)
{
    uint32_t InData_Cmd[1] =
    {
        ((0 == (all_msg_len % 16)) &&
         (0 !=
          all_msg_len)) ? RSIP_PRV_CMD_AES_CMAC_GENERATE_WITHOUT_REMAINDER :
        RSIP_PRV_CMD_AES_CMAC_GENERATE_WITH_REMAINDER
    };
    uint32_t InData_DataT = 0;

    return r_rsip_p41f(InData_Cmd, InData_Text, &InData_DataT, (uint32_t *) &all_msg_len, OutData_DataT);
}

rsip_ret_t r_rsip_wrapper_p41f_aes128mac_verify (const uint32_t * InData_Text,
                                                 const uint32_t * InData_DataT,
                                                 const uint32_t * InData_DataTLen,
                                                 const uint32_t   all_msg_len)
{
    uint32_t InData_Cmd[1] =
    {
        ((0 == (all_msg_len % 16)) &&
         (0 !=
          all_msg_len)) ? RSIP_PRV_CMD_AES_CMAC_VERIFY_WITHOUT_REMAINDER :
        RSIP_PRV_CMD_AES_CMAC_VERIFY_WITH_REMAINDER
    };
    uint32_t OutData_DataT[4] = {0};
    (void) all_msg_len;

    return r_rsip_p41f(InData_Cmd, InData_Text, InData_DataT, InData_DataTLen, OutData_DataT);
}

rsip_ret_t r_rsip_wrapper_p44i_aes256mac (const uint32_t * InData_KeyIndex)
{
    uint32_t key_type = bswap_32big(0);

    return r_rsip_p44i(&key_type, InData_KeyIndex);
}

rsip_ret_t r_rsip_wrapper_p44f_aes256mac_generate (const uint32_t * InData_Text,
                                                   uint32_t       * OutData_DataT,
                                                   const uint32_t   all_msg_len)
{
    uint32_t InData_Cmd[1] =
    {
        ((0 == (all_msg_len % 16)) &&
         (0 !=
          all_msg_len)) ? RSIP_PRV_CMD_AES_CMAC_GENERATE_WITHOUT_REMAINDER :
        RSIP_PRV_CMD_AES_CMAC_GENERATE_WITH_REMAINDER
    };
    uint32_t InData_DataT = 0;

    return r_rsip_p44f(InData_Cmd, InData_Text, &InData_DataT, (uint32_t *) &all_msg_len, OutData_DataT);
}

rsip_ret_t r_rsip_wrapper_p44f_aes256mac_verify (const uint32_t * InData_Text,
                                                 const uint32_t * InData_DataT,
                                                 const uint32_t * InData_DataTLen,
                                                 const uint32_t   all_msg_len)
{
    uint32_t InData_Cmd[1] =
    {
        ((0 == (all_msg_len % 16)) &&
         (0 !=
          all_msg_len)) ? RSIP_PRV_CMD_AES_CMAC_VERIFY_WITHOUT_REMAINDER :
        RSIP_PRV_CMD_AES_CMAC_VERIFY_WITH_REMAINDER
    };
    uint32_t OutData_DataT[4] = {0};
    (void) all_msg_len;

    return r_rsip_p44f(InData_Cmd, InData_Text, InData_DataT, InData_DataTLen, OutData_DataT);
}

/***********************************************************************************************************************
 * Private Functions
 **********************************************************************************************************************/

static rsip_ret_t select_rfc3394_key_wrap_mode (const rsip_key_type_t key_type,
                                                uint32_t              WrappedKeyType[])
{
	uint8_t  alg     = r_rsip_key_type_to_alg(key_type);
    uint32_t subtype = r_rsip_key_type_to_subtype(key_type);

    rsip_ret_t rsip_ret = RSIP_RET_PASS;
    switch (alg)
    {
        case RSIP_ALG_AES:
        {
            switch (subtype)
            {
                case RSIP_KEY_AES_128:
                {
                    WrappedKeyType[0] = bswap_32big(RSIP_RFC3394_KEY_WRAP_TYPE_AES128);
                    KEY_INDEX_SIZE    = r_rsip_byte_to_word_convert(RSIP_CFG_BYTE_SIZE_WRAPPED_KEY_VALUE_AES_128);
                    WRAPPED_KEY_SIZE  = RSIP_PRV_WORD_SIZE_RFC3394_WRAPPED_KEY_AES_128;
                    break;
                }

                case RSIP_KEY_AES_256:
                {
                    WrappedKeyType[0] = bswap_32big(RSIP_RFC3394_KEY_WRAP_TYPE_AES256);
                    KEY_INDEX_SIZE    = r_rsip_byte_to_word_convert(RSIP_CFG_BYTE_SIZE_WRAPPED_KEY_VALUE_AES_256);
                    WRAPPED_KEY_SIZE  = RSIP_PRV_WORD_SIZE_RFC3394_WRAPPED_KEY_AES_256;
                    break;
                }

                default:
                {
                    rsip_ret = RSIP_RET_KEY_FAIL;
                }
            }
            break;
        }

        default:
        {
            rsip_ret = RSIP_RET_KEY_FAIL;
        }
    }

    return rsip_ret;
}

static rsip_ret_t select_hmac_key_index_size (const rsip_wrapped_key_t * p_wrapped_key)
{
    rsip_ret_t rsip_ret = RSIP_RET_PASS;
    switch (p_wrapped_key->alg)
    {
        case RSIP_ALG_HMAC:
        {
            switch (p_wrapped_key->subtype)
            {
                case RSIP_KEY_HMAC_SHA256:
                {
                    KEY_INDEX_SIZE = r_rsip_byte_to_word_convert(RSIP_CFG_BYTE_SIZE_WRAPPED_KEY_VALUE_HMAC_SHA256);
                    break;
                }

                case RSIP_KEY_HMAC_SHA384:
                {
                    KEY_INDEX_SIZE = r_rsip_byte_to_word_convert(RSIP_CFG_BYTE_SIZE_WRAPPED_KEY_VALUE_HMAC_SHA384);
                    break;
                }

                case RSIP_KEY_HMAC_SHA512:
                {
                    KEY_INDEX_SIZE = r_rsip_byte_to_word_convert(RSIP_CFG_BYTE_SIZE_WRAPPED_KEY_VALUE_HMAC_SHA512);
                    break;
                }

                default:
                {
                    rsip_ret = RSIP_RET_KEY_FAIL;
                }
            }
            break;
        }

        default:
        {
            rsip_ret = RSIP_RET_KEY_FAIL;
        }
    }

    return rsip_ret;
}
