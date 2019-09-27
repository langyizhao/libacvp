/** @file */
/*
 * Copyright (c) 2019, Cisco Systems, Inc.
 *
 * Licensed under the Apache License 2.0 (the "License").  You may not use
 * this file except in compliance with the License.  You can obtain a copy
 * in the file LICENSE in the source distribution or at
 * https://github.com/cisco/libacvp/LICENSE
 */

#ifdef USE_MURL
# include <murl/murl.h>
#else
# include <curl/curl.h>
#endif

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "acvp.h"
#include "acvp_lcl.h"
#include "safe_lib.h"

#ifdef WIN32
#include <Windows.h>
#include <intrin.h>
#elif defined __linux__
#include <cpuid.h>
#include <sys/utsname.h>
#elif defined __APPLE__
#if TARGET_OS_MAC == 1
#include <cpuid.h>
#endif
#include <sys/utsname.h>
#include <TargetConditionals.h>
#endif


/*
 * Macros
 */
#define HTTP_OK    200
#define HTTP_UNAUTH    401

//Used for knowing which environment variable is being looked for in case of HTTP user-agent.
typedef enum acvp_user_agent_env_type {
    ACVP_USER_AGENT_OSNAME = 1,
    ACVP_USER_AGENT_OSVER,
    ACVP_USER_AGENT_ARCH,
    ACVP_USER_AGENT_PROC,
    ACVP_USER_AGENT_COMP,
    ACVP_USER_AGENT_NONE,
} ACVP_OE_ENV_VAR;

#define ACVP_AUTH_BEARER_TITLE_LEN 23

typedef enum acvp_net_action {
    ACVP_NET_GET = 1, /**< Generic (get) */
    ACVP_NET_GET_VS, /**< Vector Set (get) */
    ACVP_NET_GET_VS_RESULT, /**< Vector Set result (get) */
    ACVP_NET_GET_VS_SAMPLE, /**< Sample (get) */
    ACVP_NET_POST, /**< Generic (post) */
    ACVP_NET_POST_LOGIN, /**< Login (post) */
    ACVP_NET_POST_REG, /**< Registration (post) */
    ACVP_NET_POST_VS_RESP, /**< Vector set response (post) */
    ACVP_NET_PUT, /**< Generic (put) */
    ACVP_NET_PUT_VALIDATION /**< Submit testSession for validation (put) */
} ACVP_NET_ACTION;

#ifndef ACVP_OFFLINE
/*
 * Prototypes
 */
static ACVP_RESULT acvp_network_action(ACVP_CTX *ctx, ACVP_NET_ACTION action,
                                       const char *url, const char *data, int data_len);

static struct curl_slist *acvp_add_auth_hdr(ACVP_CTX *ctx, struct curl_slist *slist) {
    char *bearer = NULL;
    char bearer_title[] = "Authorization: Bearer ";
    int bearer_title_size = (int)sizeof(bearer_title) - 1;
    int bearer_size = 0;

    if (!ctx->jwt_token && !(ctx->tmp_jwt && ctx->use_tmp_jwt)) {
        /*
         * We don't have a token to embed
         */
        return slist;
    }

    if (ctx->use_tmp_jwt && !ctx->tmp_jwt) {
        ACVP_LOG_ERR("Trying to use tmp_jwt, but it is NULL");
        return slist;
    }

    if (ctx->use_tmp_jwt) {
        bearer_size = strnlen_s(ctx->tmp_jwt, ACVP_JWT_TOKEN_MAX) + bearer_title_size;
    } else {
        bearer_size = strnlen_s(ctx->jwt_token, ACVP_JWT_TOKEN_MAX) + bearer_title_size;
    }

    bearer = calloc(bearer_size + 1, sizeof(char));
    if (!bearer) {
        ACVP_LOG_ERR("unable to allocate memory.");
        goto end;
    }

    if (ctx->use_tmp_jwt) {
        snprintf(bearer, bearer_size + 1, "%s%s", bearer_title, ctx->tmp_jwt);
    } else {
        snprintf(bearer, bearer_size + 1, "%s%s", bearer_title, ctx->jwt_token);
    }

    slist = curl_slist_append(slist, bearer);

    free(bearer);

end:
    if (ctx->use_tmp_jwt) {
        /* 
         * This was a single-use token.
         * Turn it off now... the library might turn it back on later.
         */
        ctx->use_tmp_jwt = 0;
    }

    return slist;
}

/*
 * This is a callback used by curl to send the HTTP body
 * to the application (us).  We will store the HTTP body
 * in the ACVP_CTX curl_buf field.
 */
static size_t acvp_curl_write_callback(void *ptr, size_t size, size_t nmemb, void *userdata) {
    ACVP_CTX *ctx = (ACVP_CTX *)userdata;

    if (size != 1) {
        fprintf(stderr, "\ncurl size not 1\n");
        return 0;
    }

    if (!ctx->curl_buf) {
        ctx->curl_buf = calloc(ACVP_CURL_BUF_MAX, sizeof(char));
        if (!ctx->curl_buf) {
            fprintf(stderr, "\nmalloc failed in curl write reg func\n");
            return 0;
        }
    }

    if ((ctx->curl_read_ctr + nmemb) > ACVP_CURL_BUF_MAX) {
        fprintf(stderr, "\nServer response is too large\n");
        return 0;
    }

    memcpy_s(&ctx->curl_buf[ctx->curl_read_ctr], (ACVP_CURL_BUF_MAX - ctx->curl_read_ctr), ptr, nmemb);
    ctx->curl_buf[ctx->curl_read_ctr + nmemb] = 0;
    ctx->curl_read_ctr += nmemb;

    return nmemb;
}

/**
 * This function is called to look for operating enivronment info in the environment
 * for the HTTP user-agent string when the library cannot automatically find it
 */
static void acvp_http_user_agent_check_env_for_var(ACVP_CTX *ctx, char *agent_string, ACVP_OE_ENV_VAR var_to_check) {
    int maxLength = 0;
    int includeSemicolon = 1;
    char *var;

    switch(var_to_check) {
    case ACVP_USER_AGENT_OSNAME:
        var = ACVP_USER_AGENT_OSNAME_ENV;
        maxLength = ACVP_USER_AGENT_OSNAME_STR_MAX;
        break;
    case ACVP_USER_AGENT_OSVER:
        var = ACVP_USER_AGENT_OSVER_ENV;
        maxLength = ACVP_USER_AGENT_OSVER_STR_MAX;
        break;
    case ACVP_USER_AGENT_ARCH:
        var = ACVP_USER_AGENT_ARCH_ENV;
        maxLength = ACVP_USER_AGENT_ARCH_STR_MAX;
        break;
    case ACVP_USER_AGENT_PROC:
        var = ACVP_USER_AGENT_PROC_ENV;
        maxLength = ACVP_USER_AGENT_PROC_STR_MAX;
        break;
    case ACVP_USER_AGENT_COMP:
        var = ACVP_USER_AGENT_COMP_ENV;
        maxLength = ACVP_USER_AGENT_COMP_STR_MAX;
        includeSemicolon = 0;
        break;
    default:
        return;
    }

    //Check presence and length of variable's value, concatenate if valid, warn and ignore if not
    char *envVal = getenv(var);
    if (envVal) {
        if (strnlen_s(envVal, maxLength + 1) > maxLength) {
            ACVP_LOG_WARN("Environment-provided %s string too long! (%d char max.) Omitting...\n", var, maxLength);
        } else {
            strncat_s(agent_string, ACVP_USER_AGENT_STR_MAX, envVal, maxLength);
        }
    } else {
        ACVP_LOG_WARN("Unable to collect info for HTTP user-agent - please define %s (%d char max.)", var, maxLength);
    }

    if (includeSemicolon) {
        strncat_s(agent_string, ACVP_USER_AGENT_STR_MAX, ";", 1);
    }

}


static void acvp_http_user_agent_check_compiler_ver(ACVP_CTX *ctx, char *agent_string) {

#ifdef __GNUC__
    strncat_s(agent_string, ACVP_USER_AGENT_STR_MAX, "GCC/", 4);

    char versionBuffer[4];
    snprintf(versionBuffer, sizeof(versionBuffer), "%d", __GNUC__);
    strncat_s(agent_string, ACVP_USER_AGENT_STR_MAX, versionBuffer, sizeof(versionBuffer));

#ifdef __GNUC_MINOR__
    strncat_s(agent_string, ACVP_USER_AGENT_STR_MAX, ".", 1);
    snprintf(versionBuffer, sizeof(versionBuffer), "%d", __GNUC_MINOR__);
    strncat_s(agent_string, ACVP_USER_AGENT_STR_MAX, versionBuffer, sizeof(versionBuffer));
#endif

#ifdef __GNUC_PATCHLEVEL__
    strncat_s(agent_string, ACVP_USER_AGENT_STR_MAX, ".", 1);
    snprintf(versionBuffer, sizeof(versionBuffer), "%d", __GNUC_PATCHLEVEL__);
    strncat_s(agent_string, ACVP_USER_AGENT_STR_MAX, versionBuffer, sizeof(versionBuffer));
#endif

#elif defined _MSC_FULL_VER
    strncat_s(agent_string, ACVP_USER_AGENT_STR_MAX, "MSVC/", 5);
    char versionBuffer[16];
    snprintf(versionBuffer, sizeof(versionBuffer), "%d", _MSC_FULL_VER);
    strncat_s(agent_string, ACVP_USER_AGENT_STR_MAX, versionBuffer, sizeof(versionBuffer));
#else
    acvp_http_user_agent_check_env_for_var(ctx, agent_string, ACVP_USER_AGENT_COMP);
#endif
}

static void acvp_http_user_agent_handler(ACVP_CTX *ctx, char *agent_string) {
    if (!agent_string || !ctx) {
        ACVP_LOG_WARN("Error generating HTTP user-agent\n");
        return;
    }
    snprintf(agent_string, ACVP_USER_AGENT_STR_MAX, "libacvp/%s;", ACVP_VERSION);

#if defined __linux__ || defined __APPLE__

    //collects basic OS/hardware info
    struct utsname info;
    if (uname(&info) != 0) {
        acvp_http_user_agent_check_env_for_var(ctx, agent_string, ACVP_USER_AGENT_OSNAME);
        acvp_http_user_agent_check_env_for_var(ctx, agent_string, ACVP_USER_AGENT_OSVER);
        acvp_http_user_agent_check_env_for_var(ctx, agent_string, ACVP_USER_AGENT_ARCH);
    } else {
        //usually Linux/Darwin
        strncat_s(agent_string, ACVP_USER_AGENT_STR_MAX, info.sysname, ACVP_USER_AGENT_OSNAME_STR_MAX);
        strncat_s(agent_string, ACVP_USER_AGENT_STR_MAX, "/", 1 );

        //usually linux kernel version/darwin version
        strncat_s(agent_string, ACVP_USER_AGENT_STR_MAX, info.release, ACVP_USER_AGENT_OSVER_STR_MAX);
        strncat_s(agent_string, ACVP_USER_AGENT_STR_MAX, ";", 1);

        //hardware architecture
        strncat_s(agent_string, ACVP_USER_AGENT_STR_MAX, info.machine, ACVP_USER_AGENT_ARCH_STR_MAX);
        strncat_s(agent_string, ACVP_USER_AGENT_STR_MAX, ";", 1);
    }

    //checks if the 'machine' string indicates x86 or x86/64
    int isX86orX64 = 0;
    int diff = 0;
    strncmp_s(info.machine, sizeof(info.machine), "i386", 4, &diff);
    if(!diff) {
        isX86orX64 = 1;
    } else {
        strncmp_s(info.machine, sizeof(info.machine), "i686", 4, &diff);
        if(!diff) {
            isX86orX64 = 1;
        } else {
            strncmp_s(info.machine, sizeof(info.machine), "x86_64", 6, &diff);
            if(!diff) {
                isX86orX64 = 1;
            }
        }
    }

    //get CPU model
    if (isX86orX64) {
        /* 48 byte CPU brand string, obtained via CPUID opcode in x86/amd64 processors.
        The 0x8000000X values are specifically for that opcode.
        Each __get_cpuid call gets 16 bytes, or 1/3 of the brand string */
        unsigned int registers[4];
        char brandString[48];

        if (!__get_cpuid(0x80000002, &registers[0], &registers[1], &registers[2], &registers[3])) {
            acvp_http_user_agent_check_env_for_var(ctx, agent_string, ACVP_USER_AGENT_PROC);
        } else {
            memcpy_s(brandString, 16, &registers, 16);
        }
        if (!__get_cpuid(0x80000003, &registers[0], &registers[1], &registers[2], &registers[3])) {
            acvp_http_user_agent_check_env_for_var(ctx, agent_string, ACVP_USER_AGENT_PROC);
        } else {
            memcpy_s(brandString + 16, 16, &registers, 16);
        }
        if (!__get_cpuid(0x80000004, &registers[0], &registers[1], &registers[2], &registers[3])) {
            acvp_http_user_agent_check_env_for_var(ctx, agent_string, ACVP_USER_AGENT_PROC);
        } else {
            memcpy_s(brandString + 32, 16, &registers, 16);
            strncat_s(agent_string, ACVP_USER_AGENT_STR_MAX, brandString, ACVP_USER_AGENT_PROC_STR_MAX);
            strncat_s(agent_string, ACVP_USER_AGENT_STR_MAX, ";", 1);
        }
    } else {
        acvp_http_user_agent_check_env_for_var(ctx, agent_string, ACVP_USER_AGENT_PROC);
    }

    //gets compiler version, or checks environment for it
    acvp_http_user_agent_check_compiler_ver(ctx, agent_string);

#elif defined WIN32

    HKEY key;
    long status = RegOpenKeyExA(HKEY_LOCAL_MACHINE, "Software\\Microsoft\\Windows NT\\CurrentVersion", 0,
                  KEY_QUERY_VALUE | KEY_WOW64_64KEY, &key);

    if (status != ERROR_SUCCESS) {
        acvp_http_user_agent_check_env_for_var(ctx, agent_string, ACVP_USER_AGENT_OSNAME);
        acvp_http_user_agent_check_env_for_var(ctx, agent_string, ACVP_USER_AGENT_OSVER);
    } else {
        //product name string, containing general version of windows
        DWORD bufferLength;
        if (RegQueryValueRxW(key, "ProductName", NULL, NULL, NULL, &bufferLength != ERROR_SUCCESS)) {
            acvp_http_user_agent_check_env_for_var(ctx, agent_string, ACVP_USER_AGENT_OSNAME);
        } else {
            //get string - registry strings not garuanteed to be null terminated
            char *productNameBuffer = calloc(bufferLength + 1, sizeof(char));
            memset_s(productNameBuffer, bufferLength + 1, '\0', bufferLength + 1);
            if (RegQueryValueRxA(key, "ProductName", NULL, NULL, productNameBuffer, &bufferLength) != ERROR_SUCCESS) {
                ACVP_LOG_INFO("Unable to access Windows version name, omitting from HTTP user-agent\n");
            } else {
                strncat_s(agent_string, ACVP_USER_AGENT_STR_MAX, productNameBuffer, ACVP_USER_AGENT_OSNAME_STR_MAX);
                strncat_s(agent_string, ACVP_USER_AGENT_STR_MAX, ";", 1);
            }
            free(productNameBuffer);
        }

        //get the "BuildLab" string, which contains more specific windows build information
        if (RegQueryValueRxA(key, "BuildLab", NULL, NULL, NULL, &bufferLength != ERROR_SUCCESS)) {
            acvp_http_user_agent_check_env_for_var(ctx, agent_string, ACVP_USER_AGENT_OSVER);
        } else {
            //get string
            char *buildLabBuffer = calloc(bufferLength + 1, sizeof(char));
            memset_s(buildLabBuffer, bufferLength + 1, '\0', bufferLength + 1);
            if (RegQueryValueRxW(key, "BuildLab", NULL, NULL, buildLabBuffer, &bufferLength) != ERROR_SUCCESS) {
                ACVP_LOG_WARN("Unable to access Windows version build, omitting from HTTP user-agent\n");
            } else {
                strncat_s(agent_string, ACVP_USER_AGENT_STR_MAX, buildLabBuffer, ACVP_USER_AGENT_OSNAME_STR_MAX);
                strncat_s(agent_string, ACVP_USER_AGENT_STR_MAX, ";", 1);
            }
            free(buildLabBuffer);
        }
    }

    SYSTEM_INFO *sysInfo = NULL;
    GetNativeSystemInfo(sysInfo);
    if(!sysInfo) {
        acvp_http_user_agent_check_env_for_var(ctx, agent_string, ACVP_USER_AGENT_ARCH);
        acvp_http_user_agent_check_env_for_var(ctx, agent_string, ACVP_USER_AGENT_PROC);
    } else {
        char brandString[48];
        int brandString_resp[4];
        switch(sysInfo->wProcessorArchitecture) {
        case PROCESSOR_ARCHITECTURE_AMD64:
            strncat_s(agent_string, ACVP_USER_AGENT_STR_MAX, "x86_64;", 7);
             //get CPU model string 
            __cpuid(brandString_resp, 0x80000002);
            memcpy_s(brandString, 16, &brandString_resp, 16);
            __cpuid(brandString_resp, 0x80000003);
            memcpy_s(brandString + 16, 16, &brandString_resp, 16);
            __cpuid(brandString_resp, 0x80000004);
            memcpy_s(brandString + 32, 16, &brandString_resp, 16);
            strncat_s(agent_string, ACVP_USER_AGENT_STR_MAX, brandString, ACVP_USER_AGENT_PROC_STR_MAX);
            break;
        case PROCESSOR_ARCHITECTURE_INTEL:
            strncat_s(agent_string, ACVP_USER_AGENT_STR_MAX, "x86;", 4);
            //get CPU model string 
            __cpuid(brandString_resp, 0x80000002);
            memcpy_s(brandString, 16, &brandString_resp, 16);
            __cpuid(brandString_resp, 0x80000003);
            memcpy_s(brandString + 16, 16, &brandString_resp, 16);
            __cpuid(brandString_resp, 0x80000004);
            memcpy_s(brandString + 32, 16, &brandString_resp, 16);
            strncat_s(agent_string, ACVP_USER_AGENT_STR_MAX, brandString, ACVP_USER_AGENT_PROC_STR_MAX);
            break;
        case PROCESSOR_ARCHITECTURE_ARM64:
            strncat_s(agent_string, ACVP_USER_AGENT_STR_MAX, "arm64;", 6);
            acvp_http_user_agent_check_env_for_var(ctx, agent_string, ACVP_USER_AGENT_PROC);
            break;
        case PROCESSOR_ARCHITECTURE_ARM:
            strncat_s(agent_string, ACVP_USER_AGENT_STR_MAX, "arm;", 4);
            acvp_http_user_agent_check_env_for_var(ctx, agent_string, ACVP_USER_AGENT_PROC);
            break;
        case PROCESSOR_ARCHITECTURE_PPC:
            strncat_s(agent_string, ACVP_USER_AGENT_STR_MAX, "ppc;", 4);
            acvp_http_user_agent_check_env_for_var(ctx, agent_string, ACVP_USER_AGENT_PROC);
            break;
        case PROCESSOR_ARCHITECTURE_MIPS:
            strncat_s(agent_string, ACVP_USER_AGENT_STR_MAX, "mips;", 5);
            acvp_http_user_agent_check_env_for_var(ctx, agent_string, ACVP_USER_AGENT_PROC);
            break;
        default:
            acvp_http_user_agent_check_env_for_var(ctx, agent_string, ACVP_USER_AGENT_PROC);
            acvp_http_user_agent_check_env_for_var(ctx, agent_string, ACVP_USER_AGENT_PROC);
            break;
        }     
    }

    //gets compiler version
    acvp_http_user_agent_check_compiler_ver(ctx, agent_string);

#else
    acvp_http_user_agent_check_env_for_var(ctx, agent_string, ACVP_USER_AGENT_OSNAME);
    acvp_http_user_agent_check_env_for_var(ctx, agent_string, ACVP_USER_AGENT_OSVER);
    acvp_http_user_agent_check_env_for_var(ctx, agent_string, ACVP_USER_AGENT_ARCH);
    acvp_http_user_agent_check_env_for_var(ctx, agent_string, ACVP_USER_AGENT_PROC);
    acvp_http_user_agent_check_compiler_ver(ctx, agent_string);
#endif

ACVP_LOG_INFO("HTTP User-Agent: %s\n", agent_string);
}

/*
 * This function uses libcurl to send a simple HTTP GET
 * request with no Content-Type header.
 * TLS peer verification is enabled, but not HTTP authentication.
 * The parameters are:
 *
 * ctx: Ptr to ACVP_CTX, which contains the server name
 * url: URL to use for the GET request
 *
 * Return value is the HTTP status value from the server
 *	    (e.g. 200 for HTTP OK)
 */
static long acvp_curl_http_get(ACVP_CTX *ctx, const char *url) {
    long http_code = 0;
    CURL *hnd;
    struct curl_slist *slist;

    slist = NULL;
    /*
     * Create the Authorzation header if needed
     */
    slist = acvp_add_auth_hdr(ctx, slist);

    ctx->curl_read_ctr = 0;

    /*
     * Create the HTTP User Agent value, if it has not been done already
     */
    if(!ctx->http_user_agent) {
        ctx->http_user_agent = calloc(ACVP_USER_AGENT_STR_MAX + 1, sizeof(char));
        acvp_http_user_agent_handler(ctx, ctx->http_user_agent);
    }

    /*
     * Setup Curl
     */
    hnd = curl_easy_init();
    curl_easy_setopt(hnd, CURLOPT_URL, url);
    curl_easy_setopt(hnd, CURLOPT_NOPROGRESS, 1L);
    curl_easy_setopt(hnd, CURLOPT_USERAGENT, ctx->http_user_agent);
    curl_easy_setopt(hnd, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(hnd, CURLOPT_SSLVERSION, CURL_SSLVERSION_TLSv1_2);
    if (slist) {
        curl_easy_setopt(hnd, CURLOPT_HTTPHEADER, slist);
    }

    /*
     * Always verify the server
     */
    curl_easy_setopt(hnd, CURLOPT_SSL_VERIFYPEER, 1L);
    if (ctx->cacerts_file) {
        curl_easy_setopt(hnd, CURLOPT_CAINFO, ctx->cacerts_file);
        curl_easy_setopt(hnd, CURLOPT_CERTINFO, 1L);
    }

    /*
     * Mutual-auth
     */
    if (ctx->tls_cert && ctx->tls_key) {
        curl_easy_setopt(hnd, CURLOPT_SSLCERTTYPE, "PEM");
        curl_easy_setopt(hnd, CURLOPT_SSLCERT, ctx->tls_cert);
        curl_easy_setopt(hnd, CURLOPT_SSLKEYTYPE, "PEM");
        curl_easy_setopt(hnd, CURLOPT_SSLKEY, ctx->tls_key);
    }

    /*
     * To record the HTTP data recieved from the server,
     * set the callback function.
     */
    curl_easy_setopt(hnd, CURLOPT_WRITEDATA, ctx);
    curl_easy_setopt(hnd, CURLOPT_WRITEFUNCTION, acvp_curl_write_callback);

    if (ctx->curl_buf) {
        /* Clear the HTTP buffer for next server response */
        memzero_s(ctx->curl_buf, ACVP_CURL_BUF_MAX);
    }

    /*
     * Send the HTTP GET request
     */
    curl_easy_perform(hnd);

    /*
     * Get the HTTP reponse status code from the server
     */
    curl_easy_getinfo(hnd, CURLINFO_RESPONSE_CODE, &http_code);

    curl_easy_cleanup(hnd);
    hnd = NULL;
    if (slist) {
        curl_slist_free_all(slist);
        slist = NULL;
    }
    return http_code;
}

/*
 * This function uses libcurl to send a simple HTTP POST
 * request with no Content-Type header.
 * TLS peer verification is enabled, but not HTTP authentication.
 * The parameters are:
 *
 * ctx: Ptr to ACVP_CTX, which contains the server name
 * url: URL to use for the GET request
 * data: data to POST to the server
 * writefunc: Function pointer to handle writing the data
 *            from the HTTP body received from the server.
 *
 * Return value is the HTTP status value from the server
 *	    (e.g. 200 for HTTP OK)
 */
static long acvp_curl_http_post(ACVP_CTX *ctx, const char *url, const char *data, int data_len) {
    long http_code = 0;
    CURL *hnd;
    CURLcode crv;
    struct curl_slist *slist;

    /*
     * Set the Content-Type header in the HTTP request
     */
    slist = NULL;
    slist = curl_slist_append(slist, "Content-Type:application/json");

    /*
     * Create the Authorzation header if needed
     */
    slist = acvp_add_auth_hdr(ctx, slist);

    ctx->curl_read_ctr = 0;

    /*
     * Create the HTTP User Agent value, if it has not been done already
     */
    if(!ctx->http_user_agent) {
        ctx->http_user_agent = calloc(ACVP_USER_AGENT_STR_MAX + 1, sizeof(char));
        acvp_http_user_agent_handler(ctx, ctx->http_user_agent);
    }


    /*
     * Setup Curl
     */
    hnd = curl_easy_init();
    curl_easy_setopt(hnd, CURLOPT_URL, url);
    curl_easy_setopt(hnd, CURLOPT_NOPROGRESS, 1L);
    curl_easy_setopt(hnd, CURLOPT_USERAGENT, ctx->http_user_agent);
    curl_easy_setopt(hnd, CURLOPT_HTTPHEADER, slist);
    curl_easy_setopt(hnd, CURLOPT_CUSTOMREQUEST, "POST");
    curl_easy_setopt(hnd, CURLOPT_POST, 1L);
    curl_easy_setopt(hnd, CURLOPT_POSTFIELDS, data);
    curl_easy_setopt(hnd, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)data_len);
    curl_easy_setopt(hnd, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(hnd, CURLOPT_SSLVERSION, CURL_SSLVERSION_TLSv1_2);

    /*
     * Always verify the server
     */
    curl_easy_setopt(hnd, CURLOPT_SSL_VERIFYPEER, 1L);
    if (ctx->cacerts_file) {
        curl_easy_setopt(hnd, CURLOPT_CAINFO, ctx->cacerts_file);
        curl_easy_setopt(hnd, CURLOPT_CERTINFO, 1L);
    }

    /*
     * Mutual-auth
     */
    if (ctx->tls_cert && ctx->tls_key) {
        curl_easy_setopt(hnd, CURLOPT_SSLCERTTYPE, "PEM");
        curl_easy_setopt(hnd, CURLOPT_SSLCERT, ctx->tls_cert);
        curl_easy_setopt(hnd, CURLOPT_SSLKEYTYPE, "PEM");
        curl_easy_setopt(hnd, CURLOPT_SSLKEY, ctx->tls_key);
    }

    /*
     * To record the HTTP data recieved from the server,
     * set the callback function.
     */
    curl_easy_setopt(hnd, CURLOPT_WRITEDATA, ctx);
    curl_easy_setopt(hnd, CURLOPT_WRITEFUNCTION, acvp_curl_write_callback);

    if (ctx->curl_buf) {
        /* Clear the HTTP buffer for next server response */
        memzero_s(ctx->curl_buf, ACVP_CURL_BUF_MAX);
    }

    /*
     * Send the HTTP POST request
     */
    crv = curl_easy_perform(hnd);
    if (crv != CURLE_OK) {
        ACVP_LOG_ERR("Curl failed with code %d (%s)\n", crv, curl_easy_strerror(crv));
    }

    /*
     * Get the HTTP reponse status code from the server
     */
    curl_easy_getinfo(hnd, CURLINFO_RESPONSE_CODE, &http_code);

    curl_easy_cleanup(hnd);
    hnd = NULL;
    curl_slist_free_all(slist);
    slist = NULL;

    return http_code;
}

/**
 * @brief Uses libcurl to send a simple HTTP PUT.
 *
 * TLS peer verification is enabled, but not mutual authentication.
 *
 * @param ctx Ptr to ACVP_CTX, which contains the server name
 * @param url URL to use for the PUT operation
 * @param data: data to PUT to the server
 * @param data_len: Length of \p data (in bytes)
 *
 * @return HTTP status value from the server
 *	       (e.g. 200 for HTTP OK)
 */
static long acvp_curl_http_put(ACVP_CTX *ctx, const char *url, const char *data, int data_len) {
    long http_code = 0;
    CURL *hnd;
    CURLcode crv;
    struct curl_slist *slist;


    ctx->curl_read_ctr = 0;

    /*
     * Create the HTTP User Agent value, if it has not been done already
     */
    if(!ctx->http_user_agent) {
        ctx->http_user_agent = calloc(ACVP_USER_AGENT_STR_MAX + 1, sizeof(char));
        acvp_http_user_agent_handler(ctx, ctx->http_user_agent);
    }



    /*
     * Set the Content-Type header in the HTTP request
     */
    slist = NULL;
    slist = curl_slist_append(slist, "Content-Type:application/json");

    /*
     * Create the Authorzation header if needed
     */
    slist = acvp_add_auth_hdr(ctx, slist);

    /*
     * Setup Curl
     */
    hnd = curl_easy_init();
    curl_easy_setopt(hnd, CURLOPT_URL, url);
    curl_easy_setopt(hnd, CURLOPT_NOPROGRESS, 1L);
    curl_easy_setopt(hnd, CURLOPT_USERAGENT, ctx->http_user_agent);
    curl_easy_setopt(hnd, CURLOPT_HTTPHEADER, slist);
    curl_easy_setopt(hnd, CURLOPT_CUSTOMREQUEST, "PUT");
    curl_easy_setopt(hnd, CURLOPT_POSTFIELDS, data);
    curl_easy_setopt(hnd, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)data_len);
    curl_easy_setopt(hnd, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(hnd, CURLOPT_SSLVERSION, CURL_SSLVERSION_TLSv1_2);

    /*
     * Always verify the server
     */
    curl_easy_setopt(hnd, CURLOPT_SSL_VERIFYPEER, 1L);
    if (ctx->cacerts_file) {
        curl_easy_setopt(hnd, CURLOPT_CAINFO, ctx->cacerts_file);
        curl_easy_setopt(hnd, CURLOPT_CERTINFO, 1L);
    }

    /*
     * Mutual-auth
     */
    if (ctx->tls_cert && ctx->tls_key) {
        curl_easy_setopt(hnd, CURLOPT_SSLCERTTYPE, "PEM");
        curl_easy_setopt(hnd, CURLOPT_SSLCERT, ctx->tls_cert);
        curl_easy_setopt(hnd, CURLOPT_SSLKEYTYPE, "PEM");
        curl_easy_setopt(hnd, CURLOPT_SSLKEY, ctx->tls_key);
    }

    /*
     * To record the HTTP data recieved from the server,
     * set the callback function.
     */
    curl_easy_setopt(hnd, CURLOPT_WRITEDATA, ctx);
    curl_easy_setopt(hnd, CURLOPT_WRITEFUNCTION, acvp_curl_write_callback);

    if (ctx->curl_buf) {
        /* Clear the HTTP buffer for next server response */
        memzero_s(ctx->curl_buf, ACVP_CURL_BUF_MAX);
    }

    /*
     * Send the HTTP PUT request
     */
    crv = curl_easy_perform(hnd);
    if (crv != CURLE_OK) {
        ACVP_LOG_ERR("Curl failed with code %d (%s)\n", crv, curl_easy_strerror(crv));
    }

    /*
     * Get the HTTP reponse status code from the server
     */
    curl_easy_getinfo(hnd, CURLINFO_RESPONSE_CODE, &http_code);

    curl_easy_cleanup(hnd);
    curl_slist_free_all(slist);

    return http_code;
}

static ACVP_RESULT sanity_check_ctx(ACVP_CTX *ctx) {
    if (!ctx) {
        ACVP_LOG_ERR("Missing ctx");
        return ACVP_NO_CTX;
    }

    if (!ctx->server_port || !ctx->server_name) {
        ACVP_LOG_ERR("Call acvp_set_server to fill in server name and port");
        return ACVP_MISSING_ARG;
    }

    return ACVP_SUCCESS;
}

static ACVP_RESULT acvp_send_with_path_seg(ACVP_CTX *ctx,
                                           ACVP_NET_ACTION action,
                                           const char *uri,
                                           char *data,
                                           int data_len) {
    ACVP_RESULT rv = 0;
    char url[ACVP_ATTR_URL_MAX] = {0};

    rv = sanity_check_ctx(ctx);
    if (ACVP_SUCCESS != rv) return rv;

    if (!ctx->path_segment) {
        ACVP_LOG_ERR("No path segment, need to call acvp_set_path_segment first");
        return ACVP_MISSING_ARG;
    }

    snprintf(url, ACVP_ATTR_URL_MAX - 1, "https://%s:%d%s%s", ctx->server_name,
             ctx->server_port, ctx->path_segment, uri);

    return acvp_network_action(ctx, action, url, data, data_len);
}
#endif

/*
 * This is the transport function used within libacvp to register
 * the DUT attributes with the ACVP server.
 *
 * The reg parameter is the JSON encoded registration message that
 * will be sent to the server.
 */
#define ACVP_TEST_SESSIONS_URI "testSessions"
ACVP_RESULT acvp_send_test_session_registration(ACVP_CTX *ctx,
                                                char *reg,
                                                int len) {
#ifdef ACVP_OFFLINE 
    ACVP_LOG_ERR("Curl not linked, exiting function"); 
    return ACVP_TRANSPORT_FAIL;
#else
    return acvp_send_with_path_seg(ctx, ACVP_NET_POST_REG,
                                   ACVP_TEST_SESSIONS_URI, reg, len);
#endif
}

/*
 * This is the transport function used within libacvp to login before
 * it is able to register parameters with the server
 *
 * The reg parameter is the JSON encoded registration message that
 * will be sent to the server.
 */
#define ACVP_LOGIN_URI "login"
ACVP_RESULT acvp_send_login(ACVP_CTX *ctx,
                            char *login,
                            int len) {
#ifdef ACVP_OFFLINE 
    ACVP_LOG_ERR("Curl not linked, exiting function"); 
    return ACVP_TRANSPORT_FAIL;
#else
    return acvp_send_with_path_seg(ctx, ACVP_NET_POST_LOGIN,
                                   ACVP_LOGIN_URI, login, len);
#endif
}

/*
 * This function is used to submit a vector set response
 * to the ACV server.
 */
ACVP_RESULT acvp_submit_vector_responses(ACVP_CTX *ctx, char *vsid_url) {
#ifdef ACVP_OFFLINE 
    ACVP_LOG_ERR("Curl not linked, exiting function"); 
    return ACVP_TRANSPORT_FAIL;
#else
    ACVP_RESULT rv = 0;
    char url[ACVP_ATTR_URL_MAX] = {0};

    rv = sanity_check_ctx(ctx);
    if (ACVP_SUCCESS != rv) return rv;

    if (!vsid_url) {
        ACVP_LOG_ERR("Missing vsid_url");
        return ACVP_MISSING_ARG;
    }

    snprintf(url, ACVP_ATTR_URL_MAX - 1,
            "https://%s:%d%s/results",
            ctx->server_name, ctx->server_port, vsid_url);

    return acvp_network_action(ctx, ACVP_NET_POST_VS_RESP, url, NULL, 0);
#endif
}

ACVP_RESULT acvp_transport_post(ACVP_CTX *ctx,
                                const char *uri,
                                char *data,
                                int data_len) {
#ifdef ACVP_OFFLINE 
    ACVP_LOG_ERR("Curl not linked, exiting function"); 
    return ACVP_TRANSPORT_FAIL;
#else
    ACVP_RESULT rv = 0;
    char url[ACVP_ATTR_URL_MAX] = {0};

    rv = sanity_check_ctx(ctx);
    if (ACVP_SUCCESS != rv) return rv;

    if (!uri) {
        ACVP_LOG_ERR("Missing endpoint");
        return ACVP_MISSING_ARG;
    }

    snprintf(url, ACVP_ATTR_URL_MAX - 1,
            "https://%s:%d%s",
            ctx->server_name, ctx->server_port, uri);

    return acvp_network_action(ctx, ACVP_NET_POST, url, data, data_len);
#endif
}


/*
 * This is the top level function used within libacvp to retrieve
 * a KAT vector set from the ACVP server.
 */
ACVP_RESULT acvp_retrieve_vector_set(ACVP_CTX *ctx, char *vsid_url) {
#ifdef ACVP_OFFLINE 
    ACVP_LOG_ERR("Curl not linked, exiting function"); 
    return ACVP_TRANSPORT_FAIL;
#else
    ACVP_RESULT rv = 0;
    char url[ACVP_ATTR_URL_MAX] = {0};

    rv = sanity_check_ctx(ctx);
    if (ACVP_SUCCESS != rv) return rv;

    if (!vsid_url) {
        ACVP_LOG_ERR("Missing vsid_url");
        return ACVP_MISSING_ARG;
    }

    snprintf(url, ACVP_ATTR_URL_MAX - 1,
            "https://%s:%d%s",
            ctx->server_name, ctx->server_port, vsid_url);

    return acvp_network_action(ctx, ACVP_NET_GET_VS, url, NULL, 0);
#endif
}

/*
 * This is the top level function used within libacvp to retrieve
 * It can be used to get the results for an entire session, or
 * more specifically for a vectorSet
 */
ACVP_RESULT acvp_retrieve_vector_set_result(ACVP_CTX *ctx, char *api_url) {
#ifdef ACVP_OFFLINE 
    ACVP_LOG_ERR("Curl not linked, exiting function"); 
    return ACVP_TRANSPORT_FAIL;
#else
    ACVP_RESULT rv = 0;
    char url[ACVP_ATTR_URL_MAX] = {0};

    rv = sanity_check_ctx(ctx);
    if (ACVP_SUCCESS != rv) return rv;

    if (!api_url) {
        ACVP_LOG_ERR("Missing api_url");
        return ACVP_MISSING_ARG;
    }

    snprintf(url, ACVP_ATTR_URL_MAX - 1,
            "https://%s:%d%s/results",
            ctx->server_name, ctx->server_port, api_url);

    return acvp_network_action(ctx, ACVP_NET_GET_VS_RESULT, url, NULL, 0);
#endif
}

ACVP_RESULT acvp_retrieve_expected_result(ACVP_CTX *ctx, char *api_url) {
#ifdef ACVP_OFFLINE 
    ACVP_LOG_ERR("Curl not linked, exiting function"); 
    return ACVP_TRANSPORT_FAIL;
#else
    ACVP_RESULT rv = 0;
    char url[ACVP_ATTR_URL_MAX] = {0};

    rv = sanity_check_ctx(ctx);
    if (ACVP_SUCCESS != rv) return rv;

    if (!api_url) {
        ACVP_LOG_ERR("Missing api_url");
        return ACVP_MISSING_ARG;
    }

    snprintf(url, ACVP_ATTR_URL_MAX - 1,
            "https://%s:%d%s/expected",
            ctx->server_name, ctx->server_port, api_url);

    return acvp_network_action(ctx, ACVP_NET_GET_VS_SAMPLE, url, NULL, 0);
#endif
}

ACVP_RESULT acvp_transport_put(ACVP_CTX *ctx,
                               const char *endpoint,
                               const char *data,
                               int data_len) {
#ifdef ACVP_OFFLINE 
    ACVP_LOG_ERR("Curl not linked, exiting function"); 
    return ACVP_TRANSPORT_FAIL;
#else
    ACVP_RESULT rv = 0;
    char url[ACVP_ATTR_URL_MAX] = {0};

    rv = sanity_check_ctx(ctx);
    if (ACVP_SUCCESS != rv) return rv;

    if (!endpoint) {
        ACVP_LOG_ERR("Missing endpoint");
        return ACVP_MISSING_ARG;
    }

    snprintf(url, ACVP_ATTR_URL_MAX - 1,
            "https://%s:%d%s",
            ctx->server_name, ctx->server_port, endpoint);

    return acvp_network_action(ctx, ACVP_NET_PUT_VALIDATION, url, data, data_len);
#endif
}

ACVP_RESULT acvp_transport_put_validation(ACVP_CTX *ctx,
                                          const char *validation,
                                          int validation_len) {
    if (!ctx) return ACVP_NO_CTX;
    if (validation == NULL) return ACVP_INVALID_ARG;

    return acvp_transport_put(ctx, ctx->session_url, validation, validation_len);
}

ACVP_RESULT acvp_transport_get(ACVP_CTX *ctx,
                               const char *url,
                               const ACVP_KV_LIST *parameters) {
#ifdef ACVP_OFFLINE 
    ACVP_LOG_ERR("Curl not linked, exiting function"); 
    return ACVP_TRANSPORT_FAIL;
#else
    ACVP_RESULT rv = 0;
    CURL *curl_hnd = NULL;
    char *full_url = NULL, *escaped_value = NULL;
    int max_url = ACVP_ATTR_URL_MAX + 1;
    int rem_space = max_url - 1;
    int join = 0, len = 0;

    rv = sanity_check_ctx(ctx);
    if (ACVP_SUCCESS != rv) return rv;

    if (!url) {
        ACVP_LOG_ERR("Missing url");
        return ACVP_MISSING_ARG;
    }

    full_url = calloc(ACVP_ATTR_URL_MAX, sizeof(char));
    if (full_url == NULL) {
        ACVP_LOG_ERR("Failed to malloc");
        rv = ACVP_TRANSPORT_FAIL;
        goto end;
    }

    len = snprintf(full_url, max_url - 1, "https://%s:%d%s",
                   ctx->server_name, ctx->server_port, url);
    rem_space = rem_space - strnlen_s(full_url, max_url);

    if (parameters) {
        const ACVP_KV_LIST *param = parameters;

        curl_hnd = curl_easy_init();
        if (curl_hnd == NULL) {
            ACVP_LOG_ERR("Failed to intialize curl handle");
            rv = ACVP_TRANSPORT_FAIL;
            goto end;
        }

        while (1) {
            if (join) {
                len += snprintf(full_url+len, rem_space, "&%s", param->key);
                rem_space = rem_space - strnlen_s(full_url, max_url);
            } else {
                len += snprintf(full_url+len, rem_space, "%s", param->key);
                rem_space = rem_space - strnlen_s(full_url, max_url);
                join = 1;
            }

            escaped_value = curl_easy_escape(curl_hnd, param->value, 0);
            if (escaped_value == NULL) {
                ACVP_LOG_ERR("Failed curl_easy_escape()");
                rv = ACVP_TRANSPORT_FAIL;
                goto end;
            }

            len += snprintf(full_url+len, rem_space, "%s", escaped_value);
            rem_space = rem_space - strnlen_s(full_url, max_url);

            if (param->next == NULL) break;
            param = param->next;
            curl_free(escaped_value); escaped_value = NULL;
        }

        /* Don't need these anymore */
        curl_easy_cleanup(curl_hnd); curl_hnd = NULL;
        curl_free(escaped_value); escaped_value = NULL;
    }

    rv = acvp_network_action(ctx, ACVP_NET_GET, full_url, NULL, 0);

end:
    if (curl_hnd) curl_easy_cleanup(curl_hnd);
    if (full_url) free(full_url);
    return rv;
#endif
}

#ifndef ACVP_OFFLINE
#define JWT_EXPIRED_STR "JWT expired"
#define JWT_EXPIRED_STR_LEN 11
#define JWT_INVALID_STR "JWT signature does not match"
#define JWT_INVALID_STR_LEN 28
static ACVP_RESULT inspect_http_code(ACVP_CTX *ctx, int code) {
    ACVP_RESULT result = ACVP_TRANSPORT_FAIL; /* Generic failure */
    JSON_Value *root_value = NULL;
    const JSON_Object *obj = NULL;
    const char *err_str = NULL;

    if (code == HTTP_OK) {
        /* 200 */
        return ACVP_SUCCESS;
    }

    if (code == HTTP_UNAUTH) {
        int diff = 1;

        root_value = json_parse_string(ctx->curl_buf);

        obj = json_value_get_object(root_value);
        if (!obj) {
            ACVP_LOG_ERR("HTTP body doesn't contain top-level JSON object");
            goto end;
        }

        err_str = json_object_get_string(obj, "error");
        if (!err_str) {
            ACVP_LOG_ERR("JSON object doesn't contain 'error'");
            goto end;
        }

        strncmp_s(JWT_EXPIRED_STR, JWT_EXPIRED_STR_LEN, err_str, JWT_EXPIRED_STR_LEN, &diff);
        if (!diff) {
            result = ACVP_JWT_EXPIRED;
            goto end;
        }

        strncmp_s(JWT_INVALID_STR, JWT_INVALID_STR_LEN, err_str, JWT_INVALID_STR_LEN, &diff);
        if (!diff) {
            result = ACVP_JWT_INVALID;
            goto end;
        }
    }

end:
    if (root_value) json_value_free(root_value);

    return result;
}

static ACVP_RESULT execute_network_action(ACVP_CTX *ctx,
                                          ACVP_NET_ACTION action,
                                          const char *url,
                                          const char *data,
                                          int data_len,
                                          int *curl_code) {
    ACVP_RESULT result = 0;
    char *resp = NULL;
#ifdef ACVP_DEPRECATED
    char large_url[ACVP_ATTR_URL_MAX + 1] = {0};
    int large_submission = 0;
#endif
    int resp_len = 0;
    int rc = 0;

    switch(action) {
    case ACVP_NET_GET:
    case ACVP_NET_GET_VS:
    case ACVP_NET_GET_VS_RESULT:
    case ACVP_NET_GET_VS_SAMPLE:
        rc = acvp_curl_http_get(ctx, url);
        break;

    case ACVP_NET_POST:
    case ACVP_NET_POST_LOGIN:
    case ACVP_NET_POST_REG:
        rc = acvp_curl_http_post(ctx, url, data, data_len);
        break;

    case ACVP_NET_PUT:
    case ACVP_NET_PUT_VALIDATION:
        rc = acvp_curl_http_put(ctx, url, data, data_len);
        break;

    case ACVP_NET_POST_VS_RESP:
        resp = json_serialize_to_string(ctx->kat_resp, &resp_len);
        if (!resp) {
            ACVP_LOG_ERR("Failed to serialize JSON to string");
            return ACVP_JSON_ERR;
        }
        json_value_free(ctx->kat_resp);
        ctx->kat_resp = NULL;

#ifdef ACVP_DEPRECATED
        if (ctx->post_size_constraint && resp_len > ctx->post_size_constraint) {
            /* Determine if this POST body goes over the "constraint" */
            large_submission = 1;
        }

        if (large_submission) {
            /*
             * Need to tell the server about this large submission.
             * The server will supply us with a one-time "large" URL;
             */
            result = acvp_notify_large(ctx, url, large_url, resp_len);
            if (result != ACVP_SUCCESS) goto end;

            rc = acvp_curl_http_post(ctx, large_url, resp, resp_len);
        } else {
#endif
            rc = acvp_curl_http_post(ctx, url, resp, resp_len);
#ifdef ACVP_DEPRECATED
        }
#endif
        break;

    default:
        ACVP_LOG_ERR("Unknown ACVP_NET_ACTION");
        return ACVP_INVALID_ARG;
    }

    /* Peek at the HTTP code */
    result = inspect_http_code(ctx, rc);

    if (result != ACVP_SUCCESS) {
        if (result == ACVP_JWT_EXPIRED &&
            action != ACVP_NET_POST_LOGIN) {
            /*
             * Expired JWT
             * We are going to refresh the session
             * and try to obtain a new JWT!
             * This should not ever happen during "login"...
             * and we need to avoid an infinite loop (via acvp_refesh).
             */
            ACVP_LOG_ERR("JWT authorization has timed out, curl rc=%d.\n"
                         "Refreshing session...", rc);

            result = acvp_refresh(ctx);
            if (result != ACVP_SUCCESS) {
                ACVP_LOG_ERR("JWT refresh failed.");
                goto end;
            }

            /* Try action again after the refresh */
            switch(action) {
            case ACVP_NET_GET:
            case ACVP_NET_GET_VS:
            case ACVP_NET_GET_VS_RESULT:
            case ACVP_NET_GET_VS_SAMPLE:
                rc = acvp_curl_http_get(ctx, url);
                break;

            case ACVP_NET_POST:
            case ACVP_NET_POST_REG:
                rc = acvp_curl_http_post(ctx, url, data, data_len);
                break;

            case ACVP_NET_PUT:
            case ACVP_NET_PUT_VALIDATION:
                rc = acvp_curl_http_put(ctx, url, data, data_len);
                break;

            case ACVP_NET_POST_VS_RESP:
#ifdef ACVP_DEPRECATED
                if (large_submission) {
                    rc = acvp_curl_http_post(ctx, large_url, resp, resp_len);
                } else {
#endif
                    rc = acvp_curl_http_post(ctx, url, resp, resp_len);
#ifdef ACVP_DEPRECATED
                }
#endif
                break;

            case ACVP_NET_POST_LOGIN:
                ACVP_LOG_ERR("We should never be here!");
                break;
            }

            result = inspect_http_code(ctx, rc);
            if (result != ACVP_SUCCESS) {
                ACVP_LOG_ERR("Refreshed + retried, HTTP transport fails. curl rc=%d\n", rc);
                goto end;
            }
        } else if (result == ACVP_JWT_INVALID) {
            /*
             * Invalid JWT
             */
            ACVP_LOG_ERR("JWT invalid. curl rc=%d.\n", rc);
            goto end;
        }

        /* Generic error */
        goto end;
    }

    result = ACVP_SUCCESS;

end:
    if (resp) json_free_serialized_string(resp);

    *curl_code = rc;

    return result;
}

static void log_network_status(ACVP_CTX *ctx,
                               ACVP_NET_ACTION action,
                               int curl_code,
                               const char *url) {
    switch(action) {
    case ACVP_NET_GET:
        ACVP_LOG_STATUS("GET...\n\tStatus: %d\n\tUrl: %s\n\tResp:\n%s\n",
                        curl_code, url, ctx->curl_buf);
        break;
    case ACVP_NET_GET_VS:
        if (ctx->debug == ACVP_LOG_LVL_VERBOSE) {
            printf("GET Vector Set...\n\tStatus: %d\n\tUrl: %s\n\tResp:\n%s\n",
                   curl_code, url, ctx->curl_buf);
        } else {
            ACVP_LOG_STATUS("GET Vector Set...\n\tStatus: %d\n\tUrl: %s\n\tResp:\n%s\n",
                            curl_code, url, ctx->curl_buf);
        }
        break;
    case ACVP_NET_GET_VS_RESULT:
        if (ctx->debug == ACVP_LOG_LVL_VERBOSE) {
            printf("GET Vector Set Result...\n\tStatus: %d\n\tUrl: %s\n\tResp:\n%s\n",
                   curl_code, url, ctx->curl_buf);
        } else {
            ACVP_LOG_STATUS("GET Vector Set Result...\n\tStatus: %d\n\tUrl: %s\n\tResp:\n%s\n",
                            curl_code, url, ctx->curl_buf);
        }
        break;
    case ACVP_NET_GET_VS_SAMPLE:
        if (ctx->debug == ACVP_LOG_LVL_VERBOSE) {
            printf("GET Vector Set Sample...\n\tStatus: %d\n\tUrl: %s\n\tResp:\n%s\n",
                   curl_code, url, ctx->curl_buf);
        } else {
            ACVP_LOG_STATUS("GET Vector Set Sample...\n\tStatus: %d\n\tUrl: %s\n\tResp:\n%s\n",
                            curl_code, url, ctx->curl_buf);
        }
        break;
    case ACVP_NET_POST:
        ACVP_LOG_STATUS("POST...\n\tStatus: %d\n\tUrl: %s\n\tResp: %s\n",
                        curl_code, url, ctx->curl_buf);
        break;
    case ACVP_NET_POST_LOGIN:
        ACVP_LOG_STATUS("POST Login...\n\tStatus: %d\n\tUrl: %s\n\tResp:\n%s\n",
                        curl_code, url, ctx->curl_buf);
        break;
    case ACVP_NET_POST_REG:
        ACVP_LOG_STATUS("POST Registration...\n\tStatus: %d\n\tUrl: %s\n\tResp:\n%s\n",
                        curl_code, url, ctx->curl_buf);
        break;
    case ACVP_NET_POST_VS_RESP:
        ACVP_LOG_STATUS("POST Response Submission...\n\tStatus: %d\n\tUrl: %s\n\tResp:\n%s\n",
                        curl_code, url, ctx->curl_buf);
        break;
    case ACVP_NET_PUT:
        ACVP_LOG_STATUS("PUT...\n\tStatus: %d\n\tUrl: %s\n\tResp: %s\n",
                        curl_code, url, ctx->curl_buf);
        break;
    case ACVP_NET_PUT_VALIDATION:
        ACVP_LOG_STATUS("PUT testSession Validation...\n\tStatus: %d\n\tUrl: %s\n\tResp: %s\n",
                        curl_code, url, ctx->curl_buf);
        break;
    }
}

/*
 * This is the internal send function that takes the URI as an extra
 * parameter. This removes repeated code without having to change the
 * API that the library uses to send registrations
 */
static ACVP_RESULT acvp_network_action(ACVP_CTX *ctx,
                                       ACVP_NET_ACTION action,
                                       const char *url,
                                       const char *data,
                                       int data_len) {
    ACVP_RESULT rv = ACVP_SUCCESS;
    ACVP_NET_ACTION generic_action = 0;
    int check_data = 0;
    int curl_code = 0;

    if (!ctx) {
        ACVP_LOG_ERR("Missing ctx");
        return ACVP_NO_CTX;
    }

    if (!url) {
        ACVP_LOG_ERR("URL required for transmission");
        return ACVP_MISSING_ARG;
    }

    switch (action) {
    case ACVP_NET_GET:
    case ACVP_NET_GET_VS:
    case ACVP_NET_GET_VS_RESULT:
    case ACVP_NET_GET_VS_SAMPLE:
        generic_action = ACVP_NET_GET;
        break;

    case ACVP_NET_POST:
    case ACVP_NET_POST_REG:
        check_data = 1;
        generic_action = ACVP_NET_POST;
        break;

    case ACVP_NET_POST_LOGIN:
        /* Clear jwt if logging in */
        if (ctx->jwt_token) free(ctx->jwt_token);
        ctx->jwt_token = NULL;
        check_data = 1;
        generic_action = ACVP_NET_POST_LOGIN;
        break;

    case ACVP_NET_POST_VS_RESP:
        generic_action = ACVP_NET_POST_VS_RESP;
        break;

    case ACVP_NET_PUT:
    case ACVP_NET_PUT_VALIDATION:
        check_data = 1;
        generic_action = ACVP_NET_PUT;
        break;
    }

    if (check_data && (!data || !data_len)) {
        ACVP_LOG_ERR("POST action requires non-zero data/data_len");
        return ACVP_NO_DATA;
    }

    rv = execute_network_action(ctx, generic_action, url,
                                data, data_len, &curl_code);

    /* Log to the console */
    log_network_status(ctx, action, curl_code, url);

    return rv;
}

#endif
