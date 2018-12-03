/*****************************************************************************
* Copyright (c) 2016-2017, Cisco Systems, Inc.
* All rights reserved.

* Redistribution and use in source and binary forms, with or without modification,
* are permitted provided that the following conditions are met:
*
* 1. Redistributions of source code must retain the above copyright notice,
*    this list of conditions and the following disclaimer.
*
* 2. Redistributions in binary form must reproduce the above copyright notice,
*    this list of conditions and the following disclaimer in the documentation
*    and/or other materials provided with the distribution.
*
* THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
* AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
* IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
* DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
* FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
* DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
* SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
* CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
* OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE
* USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*****************************************************************************/
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "acvp.h"
#include "acvp_lcl.h"
#include "parson.h"

/*
 * Forward prototypes for local functions
 */
static ACVP_RESULT acvp_des_output_tc(ACVP_CTX *ctx,
                                      ACVP_SYM_CIPHER_TC *stc,
                                      JSON_Object *tc_rsp,
                                      int opt_rv);

static ACVP_RESULT acvp_des_init_tc(ACVP_CTX *ctx,
                                    ACVP_SYM_CIPHER_TC *stc,
                                    unsigned int tc_id,
                                    ACVP_SYM_CIPH_TESTTYPE test_type,
                                    char *j_key,
                                    const char *j_pt,
                                    const char *j_ct,
                                    const char *j_iv,
                                    unsigned int key_len,
                                    unsigned int iv_len,
                                    unsigned int pt_len,
                                    unsigned int ct_len,
                                    ACVP_CIPHER alg_id,
                                    ACVP_SYM_CIPH_DIR dir);

static ACVP_RESULT acvp_des_release_tc(ACVP_SYM_CIPHER_TC *stc);


static unsigned char old_iv[8];
static unsigned char ptext[10001][8];
static unsigned char ctext[10001][8];

static void shiftin(unsigned char *dst, unsigned char *src, int nbits) {
    int n;

    /* move the bytes... */
    memmove(dst, dst + nbits / 8, 3 * 8 - nbits / 8);
    /* append new data */
    memcpy(dst + 3 * 8 - nbits / 8, src, (nbits + 7) / 8);
    /* left shift the bits */
    if (nbits % 8) {
        for (n = 0; n < 3 * 8; ++n) {
            dst[n] = (dst[n] << (nbits % 8)) | (dst[n + 1] >> (8 - nbits % 8));
        }
    }
}

/*
 * After each encrypt/decrypt for a Monte Carlo test the iv
 * and/or pt/ct information may need to be modified.  This function
 * performs the iteration depdedent upon the cipher type and direction.
 */
static ACVP_RESULT acvp_des_mct_iterate_tc(ACVP_CTX *ctx,
                                           ACVP_SYM_CIPHER_TC *stc,
                                           int i,
                                           JSON_Object *r_tobj) {
    int j = stc->mct_index;
    int n;

    memcpy(ctext[j], stc->ct, stc->ct_len);
    memcpy(ptext[j], stc->pt, stc->pt_len);

    switch (stc->cipher) {
    case ACVP_TDES_CBC:
        if (stc->direction == ACVP_SYM_CIPH_DIR_ENCRYPT) {
            if (j == 0) {
                memcpy(stc->pt, old_iv, 8);
            } else {
                for (n = 0; n < 8; ++n) {
                    stc->pt[n] = ctext[j - 1][n];
                }
            }
            for (n = 0; n < 8; ++n) {
                stc->iv[n] = ctext[j][n];
            }
        } else {
            for (n = 0; n < 8; ++n) {
                stc->ct[n] = ptext[j][n];
            }
            if (j != 0) {
                for (n = 0; n < 8; ++n) {
                    stc->iv[n] = ptext[j - 1][n];
                }
            }
        }
        break;
    case ACVP_TDES_CFB64:
        if (stc->direction == ACVP_SYM_CIPH_DIR_ENCRYPT) {
            if (j == 0) {
                memcpy(stc->pt, old_iv, 8);
            } else {
                for (n = 0; n < 8; ++n) {
                    stc->pt[n] = ctext[j - 1][n];
                }
            }
            for (n = 0; n < 8; ++n) {
                stc->iv[n] = ctext[j][n];
            }
        } else {
            for (n = 0; n < 8; ++n) {
                stc->ct[n] ^= stc->pt[n];
            }
            for (n = 0; n < 8; ++n) {
                stc->iv[n] = stc->pt[n] ^ stc->ct[n];
            }
        }
        break;

    case ACVP_TDES_OFB:
        if (stc->direction == ACVP_SYM_CIPH_DIR_ENCRYPT) {
            if (j == 0) {
                memcpy(stc->pt, old_iv, 8);
            } else {
                for (n = 0; n < 8; ++n) {
                    stc->pt[n] = stc->iv_ret[n];
                }
            }
        } else {
            if (j == 0) {
                memcpy(stc->ct, old_iv, 8);
            } else {
                for (n = 0; n < 8; ++n) {
                    stc->ct[n] = stc->iv_ret[n];
                }
            }
        }
        break;
    case ACVP_TDES_CFB1:
    case ACVP_TDES_CFB8:
        if (stc->direction == ACVP_SYM_CIPH_DIR_ENCRYPT) {
            if (j == 0) {
                memcpy(stc->pt, old_iv, 8);
            } else {
                for (n = 0; n < 8; ++n) {
                    stc->pt[n] = stc->iv_ret[n];
                }
            }
        } else {
            for (n = 0; n < 8; ++n) {
                stc->ct[n] ^= stc->pt[n];
            }
            for (n = 0; n < 8; ++n) {
                stc->iv[n] = stc->pt[n] ^ stc->ct[n];
            }
        }
        break;

    case ACVP_TDES_ECB:
        if (stc->direction == ACVP_SYM_CIPH_DIR_ENCRYPT) {
            memcpy(stc->pt, stc->ct, stc->ct_len);
        } else {
            memcpy(stc->ct, stc->pt, stc->pt_len);
        }
        break;
    default:
        break;
    }

    return ACVP_SUCCESS;
}

/*
 * After the test case has been processed by the DUT, the results
 * need to be JSON formated to be included in the vector set results
 * file that will be uploaded to the server.  This routine handles
 * the JSON processing for a single test case for MCT.
 */
static ACVP_RESULT acvp_des_output_mct_tc(ACVP_CTX *ctx,
                                          ACVP_SYM_CIPHER_TC *stc,
                                          JSON_Object *r_tobj) {
    ACVP_RESULT rv = ACVP_SUCCESS;
    int single_key_str_len = 0;
    int single_key_byte_len = 0;
    char *tmp_k1 = NULL;
    char *tmp_k2 = NULL;
    char *tmp_k3 = NULL;
    char *tmp_pt = NULL;
    char *tmp_ct = NULL;
    char *tmp_iv = NULL;

    single_key_str_len = (ACVP_TDES_KEY_STR_LEN / 3);
    single_key_byte_len = (ACVP_TDES_KEY_BYTE_LEN / 3);

    tmp_k1 = calloc(single_key_str_len + 1, sizeof(char));
    if (!tmp_k1) {
        ACVP_LOG_ERR("Unable to malloc");
        rv = ACVP_MALLOC_FAIL;
        goto err;
    }
    tmp_k2 = calloc(single_key_str_len + 1, sizeof(char));
    if (!tmp_k2) {
        ACVP_LOG_ERR("Unable to malloc");
        rv = ACVP_MALLOC_FAIL;
        goto err;
    }
    tmp_k3 = calloc(single_key_str_len + 1, sizeof(char));
    if (!tmp_k3) {
        ACVP_LOG_ERR("Unable to malloc");
        rv = ACVP_MALLOC_FAIL;
        goto err;
    }

    /*
     * Split the 48 byte key into 3 parts, and convert to hex.
     */
    rv = acvp_bin_to_hexstr(stc->key, single_key_byte_len,
                            tmp_k1, single_key_str_len);
    if (rv != ACVP_SUCCESS) {
        ACVP_LOG_ERR("hex conversion failure (key)");
        goto err;
    }

    rv = acvp_bin_to_hexstr(stc->key + 8, single_key_byte_len,
                            tmp_k2, single_key_str_len);
    if (rv != ACVP_SUCCESS) {
        ACVP_LOG_ERR("hex conversion failure (key)");
        goto err;
    }

    rv = acvp_bin_to_hexstr(stc->key + 16, single_key_byte_len,
                            tmp_k3, single_key_str_len);
    if (rv != ACVP_SUCCESS) {
        ACVP_LOG_ERR("hex conversion failure (key)");
        goto err;
    }

    json_object_set_string(r_tobj, "key1", tmp_k1);
    json_object_set_string(r_tobj, "key2", tmp_k2);
    json_object_set_string(r_tobj, "key3", tmp_k3);

    if (stc->cipher != ACVP_TDES_ECB) {
        tmp_iv = calloc(ACVP_SYM_IV_MAX + 1, sizeof(char));
        if (!tmp_iv) {
            ACVP_LOG_ERR("Unable to malloc");
            rv = ACVP_MALLOC_FAIL;
            goto err;
        }

        rv = acvp_bin_to_hexstr(stc->iv, stc->iv_len, tmp_iv, ACVP_SYM_IV_MAX);
        if (rv != ACVP_SUCCESS) {
            ACVP_LOG_ERR("hex conversion failure (iv)");
            goto err;
        }
        json_object_set_string(r_tobj, "iv", tmp_iv);
    }

    if (stc->direction == ACVP_SYM_CIPH_DIR_ENCRYPT) {
        tmp_pt = calloc(ACVP_SYM_PT_MAX + 1, sizeof(char));
        if (!tmp_pt) {
            ACVP_LOG_ERR("Unable to malloc");
            rv = ACVP_MALLOC_FAIL;
            goto err;
        }

        if (stc->cipher == ACVP_TDES_CFB1) {
            stc->pt[0] &= ACVP_CFB1_BIT_MASK;
            rv = acvp_bin_to_hexstr(stc->pt, 1, tmp_pt, ACVP_SYM_PT_MAX);
            if (rv != ACVP_SUCCESS) {
                ACVP_LOG_ERR("hex conversion failure (pt)");
                goto err;
            }
            json_object_set_string(r_tobj, "pt", tmp_pt);
        } else {
            rv = acvp_bin_to_hexstr(stc->pt, stc->pt_len, tmp_pt, ACVP_SYM_PT_MAX);
            if (rv != ACVP_SUCCESS) {
                ACVP_LOG_ERR("hex conversion failure (pt)");
                goto err;
            }
            json_object_set_string(r_tobj, "pt", tmp_pt);
        }
    } else {
        /*
         * Decrypt
         */
        tmp_ct = calloc(ACVP_SYM_CT_MAX + 1, sizeof(char));
        if (!tmp_ct) {
            ACVP_LOG_ERR("Unable to malloc");
            rv = ACVP_MALLOC_FAIL;
            goto err;
        }

        if (stc->cipher == ACVP_TDES_CFB1) {
            rv = acvp_bin_to_hexstr(stc->ct, 1, tmp_ct, ACVP_SYM_CT_MAX);
            if (rv != ACVP_SUCCESS) {
                ACVP_LOG_ERR("hex conversion failure (ct)");
                goto err;
            }
            json_object_set_string(r_tobj, "ct", tmp_ct);
        } else {
            rv = acvp_bin_to_hexstr(stc->ct, stc->ct_len, tmp_ct, ACVP_SYM_CT_MAX);
            if (rv != ACVP_SUCCESS) {
                ACVP_LOG_ERR("hex conversion failure (ct)");
                goto err;
            }
            json_object_set_string(r_tobj, "ct", tmp_ct);
        }
    }

err:
    if (tmp_k1) free(tmp_k1);
    if (tmp_k2) free(tmp_k2);
    if (tmp_k3) free(tmp_k3);
    if (tmp_pt) free(tmp_pt);
    if (tmp_ct) free(tmp_ct);
    if (tmp_iv) free(tmp_iv);

    return rv;
}

static const unsigned char odd_parity[256] = {
    1,   1,   2,   2,   4,   4,   7,   7,   8,   8,   11,  11,  13,  13,  14,  14,
    16,  16,  19,  19,  21,  21,  22,  22,  25,  25,  26,  26,  28,  28,  31,  31,
    32,  32,  35,  35,  37,  37,  38,  38,  41,  41,  42,  42,  44,  44,  47,  47,
    49,  49,  50,  50,  52,  52,  55,  55,  56,  56,  59,  59,  61,  61,  62,  62,
    64,  64,  67,  67,  69,  69,  70,  70,  73,  73,  74,  74,  76,  76,  79,  79,
    81,  81,  82,  82,  84,  84,  87,  87,  88,  88,  91,  91,  93,  93,  94,  94,
    97,  97,  98,  98,  100, 100, 103, 103, 104, 104, 107, 107, 109, 109, 110, 110,
    112, 112, 115, 115, 117, 117, 118, 118, 121, 121, 122, 122, 124, 124, 127, 127,
    128, 128, 131, 131, 133, 133, 134, 134, 137, 137, 138, 138, 140, 140, 143, 143,
    145, 145, 146, 146, 148, 148, 151, 151, 152, 152, 155, 155, 157, 157, 158, 158,
    161, 161, 162, 162, 164, 164, 167, 167, 168, 168, 171, 171, 173, 173, 174, 174,
    176, 176, 179, 179, 181, 181, 182, 182, 185, 185, 186, 186, 188, 188, 191, 191,
    193, 193, 194, 194, 196, 196, 199, 199, 200, 200, 203, 203, 205, 205, 206, 206,
    208, 208, 211, 211, 213, 213, 214, 214, 217, 217, 218, 218, 220, 220, 223, 223,
    224, 224, 227, 227, 229, 229, 230, 230, 233, 233, 234, 234, 236, 236, 239, 239,
    241, 241, 242, 242, 244, 244, 247, 247, 248, 248, 251, 251, 253, 253, 254, 254
};

void acvp_des_set_odd_parity(unsigned char *key) {
    unsigned int i;

    for (i = 0; i < 24; i++) {
        (key)[i] = odd_parity[(key)[i]];
    }
}

/*
 * This is the handler for DES MCT values.  This will parse
 * a JSON encoded vector set for DES.  Each test case is
 * parsed, processed, and a response is generated to be sent
 * back to the ACV server by the transport layer.
 */
static ACVP_RESULT acvp_des_mct_tc(ACVP_CTX *ctx,
                                   ACVP_CAPS_LIST *cap,
                                   ACVP_TEST_CASE *tc,
                                   ACVP_SYM_CIPHER_TC *stc,
                                   JSON_Array *res_array) {
    int i, j, n, bit_len;
    ACVP_RESULT rv;
    JSON_Value *r_tval = NULL;  /* Response testval */
    JSON_Object *r_tobj = NULL; /* Response testobj */
    char *tmp = NULL;
    unsigned char nk[4 * 8]; /* longest key+8 */

    tmp = calloc(1, ACVP_SYM_CT_MAX + 1);
    if (!tmp) {
        ACVP_LOG_ERR("Unable to malloc in acvp_des_mct_tc");
        return ACVP_MALLOC_FAIL;
    }

    switch (stc->cipher) {
    case ACVP_TDES_CBC:
    case ACVP_TDES_OFB:
    case ACVP_TDES_CFB64:
    case ACVP_TDES_ECB:
        bit_len = 64;
        break;
    case ACVP_TDES_CFB8:
        bit_len = 8;
        break;
    case ACVP_TDES_CFB1:
        bit_len = 1;
        break;
    default:
        ACVP_LOG_ERR("unsupported algorithm (%d)", stc->cipher);
        free(tmp);
        return ACVP_UNSUPPORTED_OP;
    }


    for (i = 0; i < ACVP_DES_MCT_OUTER; ++i) {
        /*
         * Create a new test case in the response
         */
        r_tval = json_value_init_object();
        r_tobj = json_value_get_object(r_tval);

        /*
         * Output the test case request values using JSON
         */
        rv = acvp_des_output_mct_tc(ctx, stc, r_tobj);
        if (rv != ACVP_SUCCESS) {
            ACVP_LOG_ERR("JSON output failure in DES module");
            free(tmp);
            return rv;
        }

        for (j = 0; j < ACVP_DES_MCT_INNER; ++j) {
            if (j == 0) {
                memcpy(old_iv, stc->iv, stc->iv_len);
            }
            stc->mct_index = j;    /* indicates init vs. update */
            /* Process the current DES encrypt test vector... */
            if ((cap->crypto_handler)(tc)) {
                ACVP_LOG_ERR("crypto module failed the operation");
                free(tmp);
                json_value_free(r_tval);
                return ACVP_CRYPTO_MODULE_FAIL;
            }
            /*
             * Adjust the parameters for next iteration if needed.
             */
            if (stc->direction == ACVP_SYM_CIPH_DIR_ENCRYPT) {
                shiftin(nk, stc->ct, bit_len);
            } else {
                shiftin(nk, stc->pt, bit_len);
            }
            rv = acvp_des_mct_iterate_tc(ctx, stc, i, r_tobj);
            if (rv != ACVP_SUCCESS) {
                ACVP_LOG_ERR("Failed the MCT iteration changes");
                free(tmp);
                json_value_free(r_tval);
                return rv;
            }
        }

        for (n = 0; n < 8; ++n) {
            stc->key[n] ^= nk[16 + n];
        }
        for (n = 0; n < 8; ++n) {
            stc->key[8 + n] ^= nk[8 + n];
        }
        for (n = 0; n < 8; ++n) {
            stc->key[16 + n] ^= nk[n];
        }

#if 0   /* TODO: Do we really need to special case 2-key ? */
        if (numkeys == 2)
            for (n = 0; n < 8; ++n) {
                stc->key[n + 16] = stc->key[n];
            }
#endif

        acvp_des_set_odd_parity(stc->key);
        memcpy(stc->iv, stc->iv_ret_after, 8); /* only on encrypt */

        if (stc->cipher == ACVP_TDES_OFB) {
            if (stc->direction == ACVP_SYM_CIPH_DIR_ENCRYPT) {
                for (n = 0; n < 8; ++n) {
                    stc->pt[n] = ptext[0][n] ^ stc->iv_ret[n];
                }
            } else {
                for (n = 0; n < 8; ++n) {
                    stc->ct[n] = ctext[0][n] ^ stc->iv_ret[n];
                }
            }
        }

        if (stc->direction == ACVP_SYM_CIPH_DIR_ENCRYPT) {
            memset(tmp, 0x0, ACVP_SYM_CT_MAX);
            if (stc->cipher == ACVP_TDES_CFB1) {
                stc->ct[0] &= ACVP_CFB1_BIT_MASK;
                rv = acvp_bin_to_hexstr(stc->ct, 1, tmp, ACVP_SYM_CT_MAX);
                if (rv != ACVP_SUCCESS) {
                    ACVP_LOG_ERR("hex conversion failure (ct)");
                    free(tmp);
                    json_value_free(r_tval);
                    return rv;
                }
            } else {
                rv = acvp_bin_to_hexstr(stc->ct, stc->ct_len, tmp, ACVP_SYM_CT_MAX);
                if (rv != ACVP_SUCCESS) {
                    ACVP_LOG_ERR("hex conversion failure (ct)");
                    free(tmp);
                    json_value_free(r_tval);
                    return rv;
                }
            }
            json_object_set_string(r_tobj, "ct", tmp);
        } else {
            memset(tmp, 0x0, ACVP_SYM_CT_MAX);
            if (stc->cipher == ACVP_TDES_CFB1) {
                rv = acvp_bin_to_hexstr(stc->pt, 1, tmp, ACVP_SYM_CT_MAX);
                if (rv != ACVP_SUCCESS) {
                    ACVP_LOG_ERR("hex conversion failure (pt)");
                    free(tmp);
                    json_value_free(r_tval);
                    return rv;
                }
            } else {
                rv = acvp_bin_to_hexstr(stc->pt, stc->pt_len, tmp, ACVP_SYM_CT_MAX);
                if (rv != ACVP_SUCCESS) {
                    ACVP_LOG_ERR("hex conversion failure (pt)");
                    free(tmp);
                    json_value_free(r_tval);
                    return rv;
                }
            }
            json_object_set_string(r_tobj, "pt", tmp);
        }
        /* Append the test response value to array */
        json_array_append_value(res_array, r_tval);
    }


    free(tmp);

    return ACVP_SUCCESS;
}

/*
 * This is the handler for 3DES values.  This will parse
 * a JSON encoded vector set for 3DES.  Each test case is
 * parsed, processed, and a response is generated to be sent
 * back to the ACV server by the transport layer.
 */
ACVP_RESULT acvp_des_kat_handler(ACVP_CTX *ctx, JSON_Object *obj) {
    JSON_Value *groupval;
    JSON_Object *groupobj = NULL;
    JSON_Value *testval;
    JSON_Object *testobj = NULL;
    JSON_Array *groups;
    JSON_Array *tests;
    JSON_Array *res_tarr = NULL; /* Response resultsArray */

    JSON_Value *reg_arry_val = NULL;
    JSON_Object *reg_obj = NULL;
    JSON_Array *reg_arry = NULL;

    int i, g_cnt;
    int j, t_cnt;
    JSON_Value *r_vs_val = NULL;
    JSON_Object *r_vs = NULL;
    JSON_Array *r_tarr = NULL, *r_garr = NULL;  /* Response testarray, grouparray */
    JSON_Value *r_tval = NULL, *r_gval = NULL;  /* Response testval, groupval */
    JSON_Object *r_tobj = NULL, *r_gobj = NULL; /* Response testobj, groupobj */
    ACVP_CAPS_LIST *cap;
    ACVP_SYM_CIPHER_TC stc;
    ACVP_TEST_CASE tc;
    ACVP_RESULT rv;

    const char *alg_str = NULL;
    ACVP_SYM_CIPH_TESTTYPE test_type = 0;
    ACVP_SYM_CIPH_DIR dir = 0;
    ACVP_CIPHER alg_id = 0;
    char *json_result = NULL;
    const char *test_type_str = NULL, *dir_str = NULL;
    unsigned int tc_id = 0, keylen = 0;

    if (!ctx) {
        ACVP_LOG_ERR("No ctx for handler operation");
        return ACVP_NO_CTX;
    }

    alg_str = json_object_get_string(obj, "algorithm");
    if (!alg_str) {
        ACVP_LOG_ERR("unable to parse 'algorithm' from JSON");
        return ACVP_MALFORMED_JSON;
    }

    /*
     * Get a reference to the abstracted test case
     */
    tc.tc.symmetric = &stc;

    /*
     * Get the crypto module handler for DES mode
     */
    alg_id = acvp_lookup_cipher_index(alg_str);
    if (alg_id < ACVP_CIPHER_START) {
        ACVP_LOG_ERR("unsupported algorithm (%s)", alg_str);
        return ACVP_UNSUPPORTED_OP;
    }
    cap = acvp_locate_cap_entry(ctx, alg_id);
    if (!cap) {
        ACVP_LOG_ERR("ACVP server requesting unsupported capability");
        return ACVP_UNSUPPORTED_OP;
    }

    /*
     * Create ACVP array for response
     */
    rv = acvp_create_array(&reg_obj, &reg_arry_val, &reg_arry);
    if (rv != ACVP_SUCCESS) {
        ACVP_LOG_ERR("Failed to create JSON response struct. ");
        return rv;
    }

    /*
     * Start to build the JSON response
     */
    rv = acvp_setup_json_rsp_group(&ctx, &reg_arry_val, &r_vs_val, &r_vs, alg_str, &r_garr);
    if (rv != ACVP_SUCCESS) {
        ACVP_LOG_ERR("Failed to setup json response");
        return rv;
    }

    groups = json_object_get_array(obj, "testGroups");
    g_cnt = json_array_get_count(groups);
    for (i = 0; i < g_cnt; i++) {
        int tgId = 0;
        groupval = json_array_get_value(groups, i);
        groupobj = json_value_get_object(groupval);

        /*
         * Create a new group in the response with the tgid
         * and an array of tests
         */
        r_gval = json_value_init_object();
        r_gobj = json_value_get_object(r_gval);
        tgId = json_object_get_number(groupobj, "tgId");
        if (!tgId) {
            ACVP_LOG_ERR("Missing tgid from server JSON groub obj");
            return ACVP_MALFORMED_JSON;
        }
        json_object_set_number(r_gobj, "tgId", tgId);
        json_object_set_value(r_gobj, "tests", json_value_init_array());
        r_tarr = json_object_get_array(r_gobj, "tests");

        dir_str = json_object_get_string(groupobj, "direction");
        if (!dir_str) {
            ACVP_LOG_ERR("Server JSON missing 'direction'");
            return ACVP_MISSING_ARG;
        }
        /*
         * verify the direction is valid
         */
        if (!strncmp(dir_str, "encrypt", strlen("encrypt"))) {
            dir = ACVP_SYM_CIPH_DIR_ENCRYPT;
        } else if (!strncmp(dir_str, "decrypt", strlen("decrypt"))) {
            dir = ACVP_SYM_CIPH_DIR_DECRYPT;
        } else {
            ACVP_LOG_ERR("Server JSON invalid 'direction'");
            return ACVP_INVALID_ARG;
        }

        test_type_str = json_object_get_string(groupobj, "testType");
        if (!test_type_str) {
            ACVP_LOG_ERR("Server JSON missing 'testType'");
            return ACVP_MISSING_ARG;
        }

        if (!strncmp(test_type_str, "MCT", strlen("MCT"))) {
            test_type = ACVP_SYM_TEST_TYPE_MCT;
        } else if (!strncmp(test_type_str, "AFT", strlen("AFT"))) {
            test_type = ACVP_SYM_TEST_TYPE_AFT;
        } else if (!strncmp(test_type_str, "CTR", strlen("CTR"))) {
            test_type = ACVP_SYM_TEST_TYPE_CTR;
        } else {
            return ACVP_INVALID_ARG;
        }

        // keyLen will always be the same for TDES
        keylen = ACVP_TDES_KEY_BIT_LEN;

        ACVP_LOG_INFO("    Test group: %d", i);
        ACVP_LOG_INFO("        keylen: %d", keylen);
        ACVP_LOG_INFO("         dir:   %s", dir_str);
        ACVP_LOG_INFO("      testtype: %s", test_type_str);

        tests = json_object_get_array(groupobj, "tests");
        t_cnt = json_array_get_count(tests);
        for (j = 0; j < t_cnt; j++) {
            const char *pt = NULL, *ct = NULL, *iv = NULL;
            const char *key1 = NULL, *key2 = NULL, *key3 = NULL;
            unsigned int ivlen = 0, ptlen = 0, ctlen = 0, tmp_key_len = 0;
            char *key = NULL;

            ACVP_LOG_INFO("Found new 3DES test vector...");
            testval = json_array_get_value(tests, j);
            testobj = json_value_get_object(testval);

            tc_id = (unsigned int)json_object_get_number(testobj, "tcId");

            key1 = json_object_get_string(testobj, "key1");
            if (!key1) {
                ACVP_LOG_ERR("Server JSON missing 'key1'");
                return ACVP_MISSING_ARG;
            }
            tmp_key_len = strnlen(key1, ACVP_SYM_KEY_MAX_BYTES + 1);
            if (tmp_key_len != (ACVP_TDES_KEY_STR_LEN / 3)) {
                ACVP_LOG_ERR("'key1' wrong length (%u). Expected (%d)",
                             tmp_key_len, (ACVP_TDES_KEY_STR_LEN / 3));
                return ACVP_INVALID_ARG;
            }

            key2 = json_object_get_string(testobj, "key2");
            if (!key2) {
                ACVP_LOG_ERR("Server JSON missing 'key2'");
                return ACVP_MISSING_ARG;
            }
            tmp_key_len = strnlen(key2, ACVP_SYM_KEY_MAX_BYTES + 1);
            if (tmp_key_len != (ACVP_TDES_KEY_STR_LEN / 3)) {
                ACVP_LOG_ERR("'key2' wrong length (%u). Expected (%d)",
                             tmp_key_len, (ACVP_TDES_KEY_STR_LEN / 3));
                return ACVP_INVALID_ARG;
            }

            key3 = json_object_get_string(testobj, "key3");
            if (!key3) {
                ACVP_LOG_ERR("Server JSON missing 'key3'");
                return ACVP_MISSING_ARG;
            }
            tmp_key_len = strnlen(key3, ACVP_SYM_KEY_MAX_BYTES + 1);
            if (tmp_key_len != (ACVP_TDES_KEY_STR_LEN / 3)) {
                ACVP_LOG_ERR("'key3' wrong length (%u). Expected (%d)",
                             tmp_key_len, (ACVP_TDES_KEY_STR_LEN / 3));
                return ACVP_INVALID_ARG;
            }

            if (key == NULL) {
                key = calloc(ACVP_SYM_KEY_MAX_BYTES + 1, sizeof(char));
                if (!key) {
                    ACVP_LOG_ERR("Unable to malloc");
                    return ACVP_MALLOC_FAIL;
                }

                strncpy(key, key1, (ACVP_TDES_KEY_STR_LEN / 3));
                strncpy(key + 16, key2, (ACVP_TDES_KEY_STR_LEN / 3));
                strncpy(key + 32, key3, (ACVP_TDES_KEY_STR_LEN / 3));
            }

            if (dir == ACVP_SYM_CIPH_DIR_ENCRYPT) {
                pt = json_object_get_string(testobj, "pt");
                if (!pt) {
                    ACVP_LOG_ERR("Server JSON missing 'pt'");
                    free(key);
                    return ACVP_MISSING_ARG;
                }

                ptlen = strnlen(pt, ACVP_SYM_PT_MAX + 1);
                if (ptlen > ACVP_SYM_PT_MAX) {
                    ACVP_LOG_ERR("'pt' too long, max allowed=(%d)",
                                 ACVP_SYM_PT_MAX);
                    free(key);
                    return ACVP_INVALID_ARG;
                }
                // Convert to bits
                ptlen = ptlen * 4;

                if (alg_id == ACVP_TDES_CFB1) {
                    unsigned int tmp_pt_len = 0;
                    tmp_pt_len = (unsigned int)json_object_get_number(testobj, "payloadLen");
                    if (tmp_pt_len) {
                        // Replace with the provided ptLen
                        ptlen = tmp_pt_len;
                    }
                }
            } else {
                ct = json_object_get_string(testobj, "ct");
                if (!ct) {
                    ACVP_LOG_ERR("Server JSON missing 'ct'");
                    free(key);
                    return ACVP_MISSING_ARG;
                }

                ctlen = strnlen(ct, ACVP_SYM_CT_MAX + 1);
                if (ctlen > ACVP_SYM_CT_MAX) {
                    ACVP_LOG_ERR("'ct' too long, max allowed=(%d)",
                                 ACVP_SYM_CT_MAX);
                    free(key);
                    return ACVP_INVALID_ARG;
                }
                // Convert to bits
                ctlen = ctlen * 4;

                if (alg_id == ACVP_TDES_CFB1) {
                    unsigned int tmp_ct_len = 0;
                    tmp_ct_len = (unsigned int)json_object_get_number(testobj, "payloadLen");
                    if (tmp_ct_len) {
                        // Replace with the provided ctLen
                        ctlen = tmp_ct_len;
                    }
                }
            }

            if (alg_id != ACVP_TDES_ECB) {
                iv = json_object_get_string(testobj, "iv");
                if (!iv) {
                    ACVP_LOG_ERR("Server JSON missing 'iv'");
                    free(key);
                    return ACVP_MISSING_ARG;
                }

                ivlen = strnlen(iv, ACVP_SYM_IV_MAX + 1);
                if (ivlen != 16) {
                    ACVP_LOG_ERR("Invalid 'iv' length (%u). Expected (%u)", ivlen, 16);
                    free(key);
                    return ACVP_INVALID_ARG;
                }
                // Convert to bits
                ivlen = ivlen * 4;
            }

            ACVP_LOG_INFO("        Test case: %d", j);
            ACVP_LOG_INFO("            tcId: %d", tc_id);
            ACVP_LOG_INFO("              key: %s", key);
            ACVP_LOG_INFO("               pt: %s", pt);
            ACVP_LOG_INFO("            ptlen: %d", ptlen);
            ACVP_LOG_INFO("               ct: %s", ct);
            ACVP_LOG_INFO("            ctlen: %d", ctlen);
            ACVP_LOG_INFO("               iv: %s", iv);
            ACVP_LOG_INFO("            ivlen: %d", ivlen);
            ACVP_LOG_INFO("              dir: %s", dir_str);

            /*
             * Create a new test case in the response
             */
            r_tval = json_value_init_object();
            r_tobj = json_value_get_object(r_tval);

            json_object_set_number(r_tobj, "tcId", tc_id);

            /*
             * Setup the test case data that will be passed down to
             * the crypto module.
             */
            rv = acvp_des_init_tc(ctx, &stc, tc_id, test_type, key, pt, ct, iv,
                                  keylen, ivlen, ptlen, ctlen, alg_id, dir);
            if (rv != ACVP_SUCCESS) {
                acvp_des_release_tc(&stc);
                free(key);
                return rv;
            }

            // Key has been copied, we can free here
            free(key);

            /* If Monte Carlo start that here */
            if (stc.test_type == ACVP_SYM_TEST_TYPE_MCT) {
                json_object_set_value(r_tobj, "resultsArray", json_value_init_array());
                res_tarr = json_object_get_array(r_tobj, "resultsArray");
                rv = acvp_des_mct_tc(ctx, cap, &tc, &stc, res_tarr);
                if (rv != ACVP_SUCCESS) {
                    ACVP_LOG_ERR("crypto module failed the DES MCT operation");
                    acvp_des_release_tc(&stc);
                    return ACVP_CRYPTO_MODULE_FAIL;
                }
            } else {
                /* Process the current DES encrypt test vector... */
                int t_rv = (cap->crypto_handler)(&tc);
                if (t_rv) {
                    if (rv != ACVP_CRYPTO_WRAP_FAIL) {
                        ACVP_LOG_ERR("ERROR: crypto module failed the operation");
                        acvp_des_release_tc(&stc);
                        return ACVP_CRYPTO_MODULE_FAIL;
                    }
                }

                /*
                 * Output the test case results using JSON
                 */
                rv = acvp_des_output_tc(ctx, &stc, r_tobj, t_rv);
                if (rv != ACVP_SUCCESS) {
                    ACVP_LOG_ERR("JSON output failure in 3DES module");
                    acvp_des_release_tc(&stc);
                    return rv;
                }
            }

            /*
             * Release all the memory associated with the test case
             */
            acvp_des_release_tc(&stc);

            /* Append the test response value to array */
            json_array_append_value(r_tarr, r_tval);
        }
        json_array_append_value(r_garr, r_gval);
    }

    json_array_append_value(reg_arry, r_vs_val);

    json_result = json_serialize_to_string_pretty(ctx->kat_resp);
    if (ctx->debug == ACVP_LOG_LVL_VERBOSE) {
        printf("\n\n%s\n\n", json_result);
    } else {
        ACVP_LOG_INFO("\n\n%s\n\n", json_result);
    }
    json_free_serialized_string(json_result);

    return ACVP_SUCCESS;
}

/*
 * After the test case has been processed by the DUT, the results
 * need to be JSON formated to be included in the vector set results
 * file that will be uploaded to the server.  This routine handles
 * the JSON processing for a single test case.
 */
static ACVP_RESULT acvp_des_output_tc(ACVP_CTX *ctx,
                                      ACVP_SYM_CIPHER_TC *stc,
                                      JSON_Object *tc_rsp,
                                      int opt_rv) {
    ACVP_RESULT rv;
    char *tmp = NULL;

    tmp = calloc(ACVP_SYM_CT_MAX + 1, sizeof(char));
    if (!tmp) {
        ACVP_LOG_ERR("Unable to malloc in acvp_des_output_tc");
        return ACVP_MALLOC_FAIL;
    }

    if (stc->direction == ACVP_SYM_CIPH_DIR_ENCRYPT) {
        memset(tmp, 0x0, ACVP_SYM_CT_MAX);
        if (stc->cipher == ACVP_TDES_CFB1) {
            rv = acvp_bin_to_hexstr(stc->ct, (stc->ct_len+7)/8, tmp, ACVP_SYM_CT_MAX);
            if (rv != ACVP_SUCCESS) {
                ACVP_LOG_ERR("hex conversion failure (ct)");
                free(tmp);
                return rv;
            }
            json_object_set_string(tc_rsp, "ct", tmp);
        } else {
            rv = acvp_bin_to_hexstr(stc->ct, stc->ct_len, tmp, ACVP_SYM_CT_MAX);
            if (rv != ACVP_SUCCESS) {
                ACVP_LOG_ERR("hex conversion failure (ct)");
                free(tmp);
                return rv;
            }
            json_object_set_string(tc_rsp, "ct", tmp);
        }
    } else {
        if ((stc->cipher == ACVP_TDES_KW) && (opt_rv != 0)) {
            json_object_set_boolean(tc_rsp, "testPassed", 1);
            free(tmp);
            return ACVP_SUCCESS;
        }

        memset(tmp, 0x0, ACVP_SYM_CT_MAX);
        if (stc->cipher == ACVP_TDES_CFB1) {
            rv = acvp_bin_to_hexstr(stc->pt, (stc->pt_len+7)/8, tmp, ACVP_SYM_CT_MAX);
            if (rv != ACVP_SUCCESS) {
                ACVP_LOG_ERR("hex conversion failure (pt)");
                free(tmp);
                return rv;
            }
            json_object_set_string(tc_rsp, "pt", tmp);
        } else {
            rv = acvp_bin_to_hexstr(stc->pt, stc->pt_len, tmp, ACVP_SYM_CT_MAX);
            if (rv != ACVP_SUCCESS) {
                ACVP_LOG_ERR("hex conversion failure (pt)");
                free(tmp);
                return rv;
            }
            json_object_set_string(tc_rsp, "pt", tmp);
        }
    }

    free(tmp);
    return ACVP_SUCCESS;
}

/*
 * This function is used to fill-in the data for a 3DES
 * test case.  The JSON parsing logic invokes this after the
 * plaintext, key, etc. have been parsed from the vector set.
 * The ACVP_SYM_CIPHER_TC struct will hold all the data for
 * a given test case, which is then passed to the crypto
 * module to perform the actual encryption/decryption for
 * the test case.
 */
static ACVP_RESULT acvp_des_init_tc(ACVP_CTX *ctx,
                                    ACVP_SYM_CIPHER_TC *stc,
                                    unsigned int tc_id,
                                    ACVP_SYM_CIPH_TESTTYPE test_type,
                                    char *j_key,
                                    const char *j_pt,
                                    const char *j_ct,
                                    const char *j_iv,
                                    unsigned int key_len,
                                    unsigned int iv_len,
                                    unsigned int pt_len,
                                    unsigned int ct_len,
                                    ACVP_CIPHER alg_id,
                                    ACVP_SYM_CIPH_DIR dir) {
    ACVP_RESULT rv;
    memset(stc, 0x0, sizeof(ACVP_SYM_CIPHER_TC));

    stc->key = calloc(1, ACVP_SYM_KEY_MAX_BYTES);
    if (!stc->key) { return ACVP_MALLOC_FAIL; }
    stc->pt = calloc(1, ACVP_SYM_PT_MAX);
    if (!stc->pt) { return ACVP_MALLOC_FAIL; }
    stc->ct = calloc(1, ACVP_SYM_CT_MAX);
    if (!stc->ct) { return ACVP_MALLOC_FAIL; }
    stc->iv = calloc(1, ACVP_SYM_IV_MAX);
    if (!stc->iv) { return ACVP_MALLOC_FAIL; }
    stc->iv_ret = calloc(1, ACVP_SYM_IV_MAX);
    if (!stc->iv_ret) { return ACVP_MALLOC_FAIL; }
    stc->iv_ret_after = calloc(1, ACVP_SYM_IV_MAX);
    if (!stc->iv_ret_after) { return ACVP_MALLOC_FAIL; }

    rv = acvp_hexstr_to_bin(j_key, stc->key, ACVP_SYM_KEY_MAX_BYTES, NULL);
    if (rv != ACVP_SUCCESS) {
        ACVP_LOG_ERR("Hex converstion failure (key)");
        return rv;
    }

    if (j_pt) {
        if (alg_id == ACVP_TDES_CFB1) {
            rv = acvp_hexstr_to_bin(j_pt, stc->pt, ACVP_SYM_PT_BYTE_MAX, NULL);
            if (rv != ACVP_SUCCESS) {
                ACVP_LOG_ERR("Hex conversion failure (pt)");
                return rv;
            }
        } else {
            rv = acvp_hexstr_to_bin(j_pt, stc->pt, ACVP_SYM_PT_BYTE_MAX, NULL);
            if (rv != ACVP_SUCCESS) {
                ACVP_LOG_ERR("Hex converstion failure (pt)");
                return rv;
            }
        }
    }

    if (j_ct) {
        if (alg_id == ACVP_TDES_CFB1) {
            rv = acvp_hexstr_to_bin(j_ct, stc->ct, ACVP_SYM_PT_BYTE_MAX, NULL);
            if (rv != ACVP_SUCCESS) {
                ACVP_LOG_ERR("Hex conversion failure (ct)");
                return rv;
            }
        } else {
            rv = acvp_hexstr_to_bin(j_ct, stc->ct, ACVP_SYM_CT_BYTE_MAX, NULL);
            if (rv != ACVP_SUCCESS) {
                ACVP_LOG_ERR("Hex converstion failure (ct)");
                return rv;
            }
        }
    }

    if (j_iv) {
        rv = acvp_hexstr_to_bin(j_iv, stc->iv, ACVP_SYM_IV_BYTE_MAX, NULL);
        if (rv != ACVP_SUCCESS) {
            ACVP_LOG_ERR("Hex converstion failure (iv)");
            return rv;
        }
    }

    /*
     * These lengths come in as bit lengths from the ACVP server.
     * We convert to bytes.
     */
    stc->tc_id = tc_id;
    stc->key_len = key_len;
    stc->iv_len = (iv_len + 7) / 8;
    if (alg_id == ACVP_TDES_CFB1) {
        // Use the bit lengths
        stc->pt_len = pt_len;
        stc->ct_len = ct_len;
    } else {
        stc->pt_len = (pt_len + 7) / 8;
        stc->ct_len = (ct_len + 7) / 8;
    }
    stc->cipher = alg_id;
    stc->direction = dir;
    stc->test_type = test_type;

    return ACVP_SUCCESS;
}

/*
 * This function simply releases the data associated with
 * a test case.
 */
static ACVP_RESULT acvp_des_release_tc(ACVP_SYM_CIPHER_TC *stc) {
    if (stc->key) free(stc->key);
    if (stc->pt) free(stc->pt);
    if (stc->ct) free(stc->ct);
    if (stc->iv) free(stc->iv);
    if (stc->iv_ret) free(stc->iv_ret);
    if (stc->iv_ret_after) free(stc->iv_ret_after);
    memset(stc, 0x0, sizeof(ACVP_SYM_CIPHER_TC));

    return ACVP_SUCCESS;
}
