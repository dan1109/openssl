/*
 * Copyright 2016-2017 The OpenSSL Project Authors. All Rights Reserved.
 *
 * Licensed under the OpenSSL license (the "License").  You may not use
 * this file except in compliance with the License.  You can obtain a copy
 * in the file LICENSE in the source distribution or at
 * https://www.openssl.org/source/license.html
 */

#include <stdlib.h>
#include <string.h>

#include <openssl/crypto.h>
#include <openssl/err.h>
#include <openssl/store.h>
#include "internal/thread_once.h"
#include "internal/store_int.h"
#include "store_locl.h"

struct ossl_store_ctx_st {
    const OSSL_STORE_LOADER *loader;
    OSSL_STORE_LOADER_CTX *loader_ctx;
    const UI_METHOD *ui_method;
    void *ui_data;
    OSSL_STORE_post_process_info_fn post_process;
    void *post_process_data;
};

OSSL_STORE_CTX *OSSL_STORE_open(const char *uri, const UI_METHOD *ui_method,
                                void *ui_data,
                                OSSL_STORE_post_process_info_fn post_process,
                                void *post_process_data)
{
    const OSSL_STORE_LOADER *loader;
    OSSL_STORE_LOADER_CTX *loader_ctx = NULL;
    OSSL_STORE_CTX *ctx = NULL;
    char scheme_copy[256], *p;

    OPENSSL_strlcpy(scheme_copy, uri, sizeof(scheme_copy));
    if ((p = strchr(scheme_copy, ':')) != NULL) {
        *p = '\0';
        p = scheme_copy;
    } else {
        p = "file";
    }

    if ((loader = ossl_store_get0_loader_int(p)) == NULL
        || (loader_ctx = loader->open(loader, uri, ui_method, ui_data)) == NULL)
        goto done;
    if ((ctx = OPENSSL_zalloc(sizeof(*ctx))) == NULL) {
        OSSL_STOREerr(OSSL_STORE_F_OSSL_STORE_OPEN, ERR_R_MALLOC_FAILURE);
        goto done;
    }

    ctx->loader = loader;
    ctx->loader_ctx = loader_ctx;
    loader_ctx = NULL;
    ctx->ui_method = ui_method;
    ctx->ui_data = ui_data;
    ctx->post_process = post_process;
    ctx->post_process_data = post_process_data;

 done:
    if (loader_ctx != NULL) {
        /*
         * We ignore a returned error because we will return NULL anyway in
         * this case, so if something goes wrong when closing, that'll simply
         * just add another entry on the error stack.
         */
        (void)loader->close(loader_ctx);
    }
    return ctx;
}

int OSSL_STORE_ctrl(OSSL_STORE_CTX *ctx, int cmd, ...)
{
    va_list args;
    int ret = 0;

    va_start(args, cmd);
    if (ctx->loader->ctrl != NULL)
        ret = ctx->loader->ctrl(ctx->loader_ctx, cmd, args);
    va_end(args);

    return ret;
}

OSSL_STORE_INFO *OSSL_STORE_load(OSSL_STORE_CTX *ctx)
{
    OSSL_STORE_INFO *v = NULL;

 again:
    v = ctx->loader->load(ctx->loader_ctx, ctx->ui_method, ctx->ui_data);

    if (ctx->post_process != NULL && v != NULL) {
        v = ctx->post_process(v, ctx->post_process_data);

        /*
         * By returning NULL, the callback decides that this object should
         * be ignored.
         */
        if (v == NULL)
            goto again;
    }

    return v;
}

int OSSL_STORE_error(OSSL_STORE_CTX *ctx)
{
    return ctx->loader->error(ctx->loader_ctx);
}

int OSSL_STORE_eof(OSSL_STORE_CTX *ctx)
{
    return ctx->loader->eof(ctx->loader_ctx);
}

int OSSL_STORE_close(OSSL_STORE_CTX *ctx)
{
    int loader_ret = ctx->loader->close(ctx->loader_ctx);

    OPENSSL_free(ctx);
    return loader_ret;
}

/*
 * Functions to generate OSSL_STORE_INFOs, one function for each type we
 * support having in them.  Along with each of them, one macro that
 * can be used to determine what types are supported.
 *
 * In all cases, ownership of the object is transfered to the OSSL_STORE_INFO
 * and will therefore be freed when the OSSL_STORE_INFO is freed.
 */
static OSSL_STORE_INFO *store_info_new(int type, void *data)
{
    OSSL_STORE_INFO *info = OPENSSL_zalloc(sizeof(*info));

    if (info == NULL)
        return NULL;

    info->type = type;
    info->_.data = data;
    return info;
}

OSSL_STORE_INFO *OSSL_STORE_INFO_new_NAME(char *name)
{
    OSSL_STORE_INFO *info = store_info_new(OSSL_STORE_INFO_NAME, NULL);

    if (info == NULL) {
        OSSL_STOREerr(OSSL_STORE_F_OSSL_STORE_INFO_NEW_NAME,
                      ERR_R_MALLOC_FAILURE);
        return NULL;
    }

    info->_.name.name = name;
    info->_.name.desc = NULL;

    return info;
}

int OSSL_STORE_INFO_set0_NAME_description(OSSL_STORE_INFO *info, char *desc)
{
    if (info->type != OSSL_STORE_INFO_NAME) {
        OSSL_STOREerr(OSSL_STORE_F_OSSL_STORE_INFO_SET0_NAME_DESCRIPTION,
                      ERR_R_PASSED_INVALID_ARGUMENT);
        return 0;
    }

    info->_.name.desc = desc;

    return 1;
}
OSSL_STORE_INFO *OSSL_STORE_INFO_new_PARAMS(EVP_PKEY *params)
{
    OSSL_STORE_INFO *info = store_info_new(OSSL_STORE_INFO_PARAMS, params);

    if (info == NULL)
        OSSL_STOREerr(OSSL_STORE_F_OSSL_STORE_INFO_NEW_PARAMS,
                      ERR_R_MALLOC_FAILURE);
    return info;
}

OSSL_STORE_INFO *OSSL_STORE_INFO_new_PKEY(EVP_PKEY *pkey)
{
    OSSL_STORE_INFO *info = store_info_new(OSSL_STORE_INFO_PKEY, pkey);

    if (info == NULL)
        OSSL_STOREerr(OSSL_STORE_F_OSSL_STORE_INFO_NEW_PKEY,
                      ERR_R_MALLOC_FAILURE);
    return info;
}

OSSL_STORE_INFO *OSSL_STORE_INFO_new_CERT(X509 *x509)
{
    OSSL_STORE_INFO *info = store_info_new(OSSL_STORE_INFO_CERT, x509);

    if (info == NULL)
        OSSL_STOREerr(OSSL_STORE_F_OSSL_STORE_INFO_NEW_CERT,
                      ERR_R_MALLOC_FAILURE);
    return info;
}

OSSL_STORE_INFO *OSSL_STORE_INFO_new_CRL(X509_CRL *crl)
{
    OSSL_STORE_INFO *info = store_info_new(OSSL_STORE_INFO_CRL, crl);

    if (info == NULL)
        OSSL_STOREerr(OSSL_STORE_F_OSSL_STORE_INFO_NEW_CRL,
                      ERR_R_MALLOC_FAILURE);
    return info;
}

/*
 * Functions to try to extract data from a OSSL_STORE_INFO.
 */
int OSSL_STORE_INFO_get_type(const OSSL_STORE_INFO *info)
{
    return info->type;
}

const char *OSSL_STORE_INFO_get0_NAME(const OSSL_STORE_INFO *info)
{
    if (info->type == OSSL_STORE_INFO_NAME)
        return info->_.name.name;
    return NULL;
}

char *OSSL_STORE_INFO_get1_NAME(const OSSL_STORE_INFO *info)
{
    if (info->type == OSSL_STORE_INFO_NAME) {
        char *ret = OPENSSL_strdup(info->_.name.name);

        if (ret == NULL)
            OSSL_STOREerr(OSSL_STORE_F_OSSL_STORE_INFO_GET1_NAME,
                          ERR_R_MALLOC_FAILURE);
        return ret;
    }
    OSSL_STOREerr(OSSL_STORE_F_OSSL_STORE_INFO_GET1_NAME,
                  OSSL_STORE_R_NOT_A_NAME);
    return NULL;
}

const char *OSSL_STORE_INFO_get0_NAME_description(const OSSL_STORE_INFO *info)
{
    if (info->type == OSSL_STORE_INFO_NAME)
        return info->_.name.desc;
    return NULL;
}

char *OSSL_STORE_INFO_get1_NAME_description(const OSSL_STORE_INFO *info)
{
    if (info->type == OSSL_STORE_INFO_NAME) {
        char *ret = OPENSSL_strdup(info->_.name.desc
                                   ? info->_.name.desc : "");

        if (ret == NULL)
            OSSL_STOREerr(OSSL_STORE_F_OSSL_STORE_INFO_GET1_NAME_DESCRIPTION,
                     ERR_R_MALLOC_FAILURE);
        return ret;
    }
    OSSL_STOREerr(OSSL_STORE_F_OSSL_STORE_INFO_GET1_NAME_DESCRIPTION,
                  OSSL_STORE_R_NOT_A_NAME);
    return NULL;
}

EVP_PKEY *OSSL_STORE_INFO_get0_PARAMS(const OSSL_STORE_INFO *info)
{
    if (info->type == OSSL_STORE_INFO_PARAMS)
        return info->_.params;
    return NULL;
}

EVP_PKEY *OSSL_STORE_INFO_get1_PARAMS(const OSSL_STORE_INFO *info)
{
    if (info->type == OSSL_STORE_INFO_PARAMS) {
        EVP_PKEY_up_ref(info->_.params);
        return info->_.params;
    }
    OSSL_STOREerr(OSSL_STORE_F_OSSL_STORE_INFO_GET1_PARAMS,
                  OSSL_STORE_R_NOT_PARAMETERS);
    return NULL;
}

EVP_PKEY *OSSL_STORE_INFO_get0_PKEY(const OSSL_STORE_INFO *info)
{
    if (info->type == OSSL_STORE_INFO_PKEY)
        return info->_.pkey;
    return NULL;
}

EVP_PKEY *OSSL_STORE_INFO_get1_PKEY(const OSSL_STORE_INFO *info)
{
    if (info->type == OSSL_STORE_INFO_PKEY) {
        EVP_PKEY_up_ref(info->_.pkey);
        return info->_.pkey;
    }
    OSSL_STOREerr(OSSL_STORE_F_OSSL_STORE_INFO_GET1_PKEY,
                  OSSL_STORE_R_NOT_A_KEY);
    return NULL;
}

X509 *OSSL_STORE_INFO_get0_CERT(const OSSL_STORE_INFO *info)
{
    if (info->type == OSSL_STORE_INFO_CERT)
        return info->_.x509;
    return NULL;
}

X509 *OSSL_STORE_INFO_get1_CERT(const OSSL_STORE_INFO *info)
{
    if (info->type == OSSL_STORE_INFO_CERT) {
        X509_up_ref(info->_.x509);
        return info->_.x509;
    }
    OSSL_STOREerr(OSSL_STORE_F_OSSL_STORE_INFO_GET1_CERT,
                  OSSL_STORE_R_NOT_A_CERTIFICATE);
    return NULL;
}

X509_CRL *OSSL_STORE_INFO_get0_CRL(const OSSL_STORE_INFO *info)
{
    if (info->type == OSSL_STORE_INFO_CRL)
        return info->_.crl;
    return NULL;
}

X509_CRL *OSSL_STORE_INFO_get1_CRL(const OSSL_STORE_INFO *info)
{
    if (info->type == OSSL_STORE_INFO_CRL) {
        X509_CRL_up_ref(info->_.crl);
        return info->_.crl;
    }
    OSSL_STOREerr(OSSL_STORE_F_OSSL_STORE_INFO_GET1_CRL,
                  OSSL_STORE_R_NOT_A_CRL);
    return NULL;
}

/*
 * Free the OSSL_STORE_INFO
 */
void OSSL_STORE_INFO_free(OSSL_STORE_INFO *info)
{
    if (info != NULL) {
        switch (info->type) {
        case OSSL_STORE_INFO_EMBEDDED:
            BUF_MEM_free(info->_.embedded.blob);
            OPENSSL_free(info->_.embedded.pem_name);
            break;
        case OSSL_STORE_INFO_NAME:
            OPENSSL_free(info->_.name.name);
            OPENSSL_free(info->_.name.desc);
            break;
        case OSSL_STORE_INFO_PARAMS:
            EVP_PKEY_free(info->_.params);
            break;
        case OSSL_STORE_INFO_PKEY:
            EVP_PKEY_free(info->_.pkey);
            break;
        case OSSL_STORE_INFO_CERT:
            X509_free(info->_.x509);
            break;
        case OSSL_STORE_INFO_CRL:
            X509_CRL_free(info->_.crl);
            break;
        }
        OPENSSL_free(info);
    }
}

/* Internal functions */
OSSL_STORE_INFO *ossl_store_info_new_EMBEDDED(const char *new_pem_name,
                                              BUF_MEM *embedded)
{
    OSSL_STORE_INFO *info = store_info_new(OSSL_STORE_INFO_EMBEDDED, NULL);

    if (info == NULL) {
        OSSL_STOREerr(OSSL_STORE_F_OSSL_STORE_INFO_NEW_EMBEDDED,
                      ERR_R_MALLOC_FAILURE);
        return NULL;
    }

    info->_.embedded.blob = embedded;
    info->_.embedded.pem_name =
        new_pem_name == NULL ? NULL : OPENSSL_strdup(new_pem_name);

    if (new_pem_name != NULL && info->_.embedded.pem_name == NULL) {
        OSSL_STOREerr(OSSL_STORE_F_OSSL_STORE_INFO_NEW_EMBEDDED,
                      ERR_R_MALLOC_FAILURE);
        OSSL_STORE_INFO_free(info);
        info = NULL;
    }

    return info;
}

BUF_MEM *ossl_store_info_get0_EMBEDDED_buffer(OSSL_STORE_INFO *info)
{
    if (info->type == OSSL_STORE_INFO_EMBEDDED)
        return info->_.embedded.blob;
    return NULL;
}

char *ossl_store_info_get0_EMBEDDED_pem_name(OSSL_STORE_INFO *info)
{
    if (info->type == OSSL_STORE_INFO_EMBEDDED)
        return info->_.embedded.pem_name;
    return NULL;
}

OSSL_STORE_CTX *ossl_store_attach_pem_bio(BIO *bp, const UI_METHOD *ui_method,
                                          void *ui_data)
{
    OSSL_STORE_CTX *ctx = NULL;
    const OSSL_STORE_LOADER *loader = NULL;
    OSSL_STORE_LOADER_CTX *loader_ctx = NULL;

    if ((loader = ossl_store_get0_loader_int("file")) == NULL
        || ((loader_ctx = ossl_store_file_attach_pem_bio_int(bp)) == NULL))
        goto done;
    if ((ctx = OPENSSL_zalloc(sizeof(*ctx))) == NULL) {
        OSSL_STOREerr(OSSL_STORE_F_OSSL_STORE_ATTACH_PEM_BIO,
                     ERR_R_MALLOC_FAILURE);
        goto done;
    }

    ctx->loader = loader;
    ctx->loader_ctx = loader_ctx;
    loader_ctx = NULL;
    ctx->ui_method = ui_method;
    ctx->ui_data = ui_data;
    ctx->post_process = NULL;
    ctx->post_process_data = NULL;

 done:
    if (loader_ctx != NULL)
        /*
         * We ignore a returned error because we will return NULL anyway in
         * this case, so if something goes wrong when closing, that'll simply
         * just add another entry on the error stack.
         */
        (void)loader->close(loader_ctx);
    return ctx;
}

int ossl_store_detach_pem_bio(OSSL_STORE_CTX *ctx)
{
    int loader_ret = ossl_store_file_detach_pem_bio_int(ctx->loader_ctx);

    OPENSSL_free(ctx);
    return loader_ret;
}
