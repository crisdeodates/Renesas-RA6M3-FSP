/**
 * This file is a copy of the ecp.h file from mbedCrypto
 * \file ecp_alt.h
 *
 * \brief This file contains ECP Alternate definitions and functions.
 */

/*
 *  Copyright (C) 2006-2018, Arm Limited (or its affiliates), All Rights Reserved
 *  SPDX-License-Identifier: Apache-2.0
 *
 *  Licensed under the Apache License, Version 2.0 (the "License"); you may
 *  not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 *  WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 *  This file is part of Mbed TLS (https://tls.mbed.org)
 */
#ifndef MBEDTLS_ECP_ALT_H
 #define MBEDTLS_ECP_ALT_H

 #include "mbedtls/bignum.h"
 #include "mbedtls/md.h"
 #include "hw_sce_ra_private.h"
 #include "hw_sce_ecc_private.h"

 #ifdef __cplusplus
extern "C" {
 #endif

 #if defined(MBEDTLS_ECP_ALT)

// Alternate implementation
//
  #define RM_PSA_CRYPTO_ECP_LOOKUP_INDEX(bits)    (((((bits) >> 7) & 1) | ((bits) & 8) >> 2))
  #define RM_PSA_CRYPTO_ECC_KEY_PLAINTEXT                        (0U)
  #define RM_PSA_CRYPTO_ECC_KEY_WRAPPED                          (1U)
  #define RM_PSA_CRYPTO_LARGEST_WRAPPED_ECC_PRIVATE_KEY_WORDS    (25U) /* Corresponding to ECC P-521 curves on RSIP */

  #define PSA_ECC_BYTES_VENDOR_RAW(bit_length)                                                                     \
    ((bit_length) ==                                                                                               \
     PSA_BITS_TO_BYTES(ECC_256_PRIVATE_KEY_HRK_LENGTH_BITS) ? PSA_BITS_TO_BYTES(ECC_256_PRIVATE_KEY_LENGTH_BITS) : \
     (bit_length) ==                                                                                               \
     PSA_BITS_TO_BYTES(ECC_384_PRIVATE_KEY_HRK_LENGTH_BITS) ? PSA_BITS_TO_BYTES(ECC_384_PRIVATE_KEY_LENGTH_BITS) : \
     (bit_length) ==                                                                                               \
     PSA_BITS_TO_BYTES(ECC_521_PRIVATE_KEY_HRK_LENGTH_BITS) ? PSA_BITS_TO_BYTES(ECC_521_PRIVATE_KEY_LENGTH_BITS) : \
     0)

  #define PSA_ECC_BITS_VENDOR_RAW(byte_length)                                              \
    ((byte_length) ==                                                                       \
     PSA_BITS_TO_BYTES(ECC_256_PRIVATE_KEY_LENGTH_BITS) ? ECC_256_PRIVATE_KEY_LENGTH_BITS : \
     (byte_length) ==                                                                       \
     PSA_BITS_TO_BYTES(ECC_384_PRIVATE_KEY_LENGTH_BITS) ? ECC_384_PRIVATE_KEY_LENGTH_BITS : \
     (byte_length) ==                                                                       \
     PSA_BITS_TO_BYTES(ECC_521_PRIVATE_KEY_LENGTH_BITS) ? ECC_521_PRIVATE_KEY_LENGTH_BITS : \
     0)

  #define RM_PSA_CRYPTO_ECC_KEY_WRAPPED_SIZE_BYTES(bit_length)                                    \
    ((bit_length) ==                                                                              \
     ECC_256_PRIVATE_KEY_LENGTH_BITS ? PSA_BITS_TO_BYTES(ECC_256_PRIVATE_KEY_HRK_LENGTH_BITS) :   \
     (bit_length) ==                                                                              \
     ECC_384_PRIVATE_KEY_LENGTH_BITS ? PSA_BITS_TO_BYTES(ECC_384_PRIVATE_KEY_HRK_LENGTH_BITS) :   \
     (bit_length) ==                                                                              \
     ECC_521_PRIVATE_KEY_LENGTH_BITS ? PSA_BITS_TO_BYTES(ECC_521_PRIVATE_KEY_HRK_LENGTH_BITS) :   \
     (bit_length) ==                                                                              \
     ECC_25519_PRIVATE_KEY_LENGTH_BITS ? PSA_BITS_TO_BYTES(ECC_256_PRIVATE_KEY_HRK_LENGTH_BITS) : \
     0)

/*
 * default mbed TLS elliptic curve arithmetic implementation
 *
 * (in case MBEDTLS_ECP_ALT is defined then the developer has to provide an
 * alternative implementation for the whole module and it will replace this
 * one.)
 */

/**
 * \brief           The ECP group structure.
 *
 * We consider two types of curve equations:
 * <ul><li>Short Weierstrass: <code>y^2 = x^3 + A x + B mod P</code>
 * (SEC1 + RFC-4492)</li>
 * <li>Montgomery: <code>y^2 = x^3 + A x^2 + x mod P</code> (Curve25519,
 * Curve448)</li></ul>
 * In both cases, the generator (\p G) for a prime-order subgroup is fixed.
 *
 * For Short Weierstrass, this subgroup is the whole curve, and its
 * cardinality is denoted by \p N. Our code requires that \p N is an
 * odd prime as mbedtls_ecp_mul() requires an odd number, and
 * mbedtls_ecdsa_sign() requires that it is prime for blinding purposes.
 *
 * For Montgomery curves, we do not store \p A, but <code>(A + 2) / 4</code>,
 * which is the quantity used in the formulas. Additionally, \p nbits is
 * not the size of \p N but the required size for private keys.
 *
 * If \p modp is NULL, reduction modulo \p P is done using a generic algorithm.
 * Otherwise, \p modp must point to a function that takes an \p mbedtls_mpi in the
 * range of <code>0..2^(2*pbits)-1</code>, and transforms it in-place to an integer
 * which is congruent mod \p P to the given MPI, and is close enough to \p pbits
 * in size, so that it may be efficiently brought in the 0..P-1 range by a few
 * additions or subtractions. Therefore, it is only an approximative modular
 * reduction. It must return 0 on success and non-zero on failure.
 *
 * \note        Alternative implementations must keep the group IDs distinct. If
 *              two group structures have the same ID, then they must be
 *              identical.
 *
 */
typedef struct mbedtls_ecp_group
{
    mbedtls_ecp_group_id id;                     /*!< An internal group identifier. */
    mbedtls_mpi          P;                      /*!< The prime modulus of the base field. */
    mbedtls_mpi          A;                      /*!< For Short Weierstrass: \p A in the equation. For
                                                  * Montgomery curves: <code>(A + 2) / 4</code>. */
    mbedtls_mpi B;                               /*!< For Short Weierstrass: \p B in the equation.
                                                  * For Montgomery curves: unused. */
    mbedtls_ecp_point G;                         /*!< The generator of the subgroup used. */
    mbedtls_mpi       N;                         /*!< The order of \p G. */
    size_t            pbits;                     /*!< The number of bits in \p P.*/
    size_t            nbits;                     /*!< For Short Weierstrass: The number of bits in \p P.
                                                  * For Montgomery curves: the number of bits in the
                                                  * private keys. */
    unsigned int h;                              /*!< \internal 1 if the constants are static. */
    int (* modp)(mbedtls_mpi *);                 /*!< The function for fast pseudo-reduction
                                                  * mod \p P (see above).*/
    int (* t_pre)(mbedtls_ecp_point *, void *);  /*!< Unused. */
    int (* t_post)(mbedtls_ecp_point *, void *); /*!< Unused. */
    void              * t_data;                  /*!< Unused. */
    mbedtls_ecp_point * T;                       /*!< Pre-computed points for ecp_mul_comb(). */
    size_t              T_size;                  /*!< The number of pre-computed points. */
    void              * vendor_ctx;              /*!< Vendor defined context. */
} mbedtls_ecp_group;

/// **
// * \name SECTION: Module settings
// *
// * The configuration options you can set for this module are in this section.
// * Either change them in config.h, or define them using the compiler command line.
// * \{
// */
//
// #define MBEDTLS_ECP_MAX_BYTES       ((MBEDTLS_ECP_MAX_BITS + 7) / 8)
// #define MBEDTLS_ECP_MAX_PT_LEN      (2 * MBEDTLS_ECP_MAX_BYTES + 1)

  #if !defined(MBEDTLS_ECP_WINDOW_SIZE)

/*
 * Maximum "window" size used for point multiplication.
 * Default: 6.
 * Minimum value: 2. Maximum value: 7.
 *
 * Result is an array of at most ( 1 << ( MBEDTLS_ECP_WINDOW_SIZE - 1 ) )
 * points used for point multiplication. This value is directly tied to EC
 * peak memory usage, so decreasing it by one should roughly cut memory usage
 * by two (if large curves are in use).
 *
 * Reduction in size may reduce speed, but larger curves are impacted first.
 * Sample performances (in ECDHE handshakes/s, with FIXED_POINT_OPTIM = 1):
 *      w-size:     6       5       4       3       2
 *      521       145     141     135     120      97
 *      384       214     209     198     177     146
 *      256       320     320     303     262     226
 *      224       475     475     453     398     342
 *      192       640     640     633     587     476
 */
   #define MBEDTLS_ECP_WINDOW_SIZE    6 /**< The maximum window size used. */
  #endif                                /* MBEDTLS_ECP_WINDOW_SIZE */

  #if !defined(MBEDTLS_ECP_FIXED_POINT_OPTIM)

/*
 * Trade memory for speed on fixed-point multiplication.
 *
 * This speeds up repeated multiplication of the generator (that is, the
 * multiplication in ECDSA signatures, and half of the multiplications in
 * ECDSA verification and ECDHE) by a factor roughly 3 to 4.
 *
 * The cost is increasing EC peak memory usage by a factor roughly 2.
 *
 * Change this value to 0 to reduce peak memory usage.
 */
   #define MBEDTLS_ECP_FIXED_POINT_OPTIM    1 /**< Enable fixed-point speed-up. */
  #endif                                      /* MBEDTLS_ECP_FIXED_POINT_OPTIM */

 #endif /* MBEDTLS_ECP_ALT */

 #ifdef __cplusplus
}
 #endif

#endif                                 /* MBEDTLS_ECP_ALT_H */
