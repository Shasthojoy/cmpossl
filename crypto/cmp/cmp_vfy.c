/* crypto/cmp/cmp_vfy.c
 * Functions to verify CMP (RFC 4210) messages for OpenSSL
 */
/* ====================================================================
 * Originally written by Martin Peylo for the OpenSSL project.
 * <martin dot peylo at nsn dot com>
 * 2010-2012 Miikka Viljanen <mviljane@users.sourceforge.net>
 */
/* ====================================================================
 * Copyright (c) 2007-2010 The OpenSSL Project.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *        notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *        notice, this list of conditions and the following disclaimer in
 *        the documentation and/or other materials provided with the
 *        distribution.
 *
 * 3. All advertising materials mentioning features or use of this
 *        software must display the following acknowledgment:
 *        "This product includes software developed by the OpenSSL Project
 *        for use in the OpenSSL Toolkit. (http://www.openssl.org/)"
 *
 * 4. The names "OpenSSL Toolkit" and "OpenSSL Project" must not be used to
 *        endorse or promote products derived from this software without
 *        prior written permission. For written permission, please contact
 *        openssl-core@openssl.org.
 *
 * 5. Products derived from this software may not be called "OpenSSL"
 *        nor may "OpenSSL" appear in their names without prior written
 *        permission of the OpenSSL Project.
 *
 * 6. Redistributions of any form whatsoever must retain the following
 *        acknowledgment:
 *        "This product includes software developed by the OpenSSL Project
 *        for use in the OpenSSL Toolkit (http://www.openssl.org/)"
 *
 * THIS SOFTWARE IS PROVIDED BY THE OpenSSL PROJECT ``AS IS'' AND ANY
 * EXPRESSED OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.      IN NO EVENT SHALL THE OpenSSL PROJECT OR
 * ITS CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
 * OF THE POSSIBILITY OF SUCH DAMAGE.
 * ====================================================================
 *
 * This product includes cryptographic software written by Eric Young
 * (eay@cryptsoft.com).  This product includes software written by Tim
 * Hudson (tjh@cryptsoft.com).
 *
 */
/* ====================================================================
 * Copyright 2007-2014 Nokia Oy. ALL RIGHTS RESERVED.
 * CMP support in OpenSSL originally developed by
 * Nokia for contribution to the OpenSSL project.
 */

#include <openssl/asn1.h>
#include <openssl/asn1t.h>
#include <openssl/crmf.h>
#include <openssl/cmp.h>
#include <openssl/err.h>
#if OPENSSL_VERSION_NUMBER < 0x10100000L
#define X509_STORE_CTX_get_by_subject X509_STORE_get_by_subject
#endif

#include "cmp_int.h"

/* ########################################################################## *
 * internal function
 *
 * validate a message protected by signature according to section 5.1.3.3
 * (sha1+RSA/DSA or any other algorithm supported by OpenSSL)
 * returns 0 on error
 * ########################################################################## */
static int CMP_verify_signature(const CMP_PKIMESSAGE *msg, const X509 *cert)
{
    EVP_MD_CTX *ctx = NULL;
    CMP_PROTECTEDPART protPart;
    int ret = 0;
    int digest_NID;
    EVP_MD *digest = NULL;
    EVP_PKEY *pubkey = NULL;

    size_t protPartDerLen = 0;
    unsigned char *protPartDer = NULL;

    if (!msg || !cert)
        return 0;

    /* verify that keyUsage, if present, contains digitalSignature */
    if (!(X509_get_key_usage((X509 *)cert) & X509v3_KU_DIGITAL_SIGNATURE)) {
        if (X509_get_key_usage((X509 *)cert) & X509v3_KU_KEY_CERT_SIGN) {
            /* workaround for Insta CA having Certificate Sign and CRL Sign */
            puts("warning: server certificate does not allow key usage 'digitalSignature' but 'keyCertSign'");
        /* TODO: add proper warning function */
        }
        else {
            CMPerr(CMP_F_CMP_VERIFY_SIGNATURE, CMP_R_WRONG_KEY_USAGE);
            return 0;
        }
    }

    pubkey = X509_get_pubkey((X509 *)cert);
    if (!pubkey)
        return 0;

    /* create the DER representation of protected part */
    protPart.header = msg->header;
    protPart.body = msg->body;
    protPartDerLen = i2d_CMP_PROTECTEDPART(&protPart, &protPartDer);

    /* verify protection of protected part */
    ctx = EVP_MD_CTX_create();
    if (!OBJ_find_sigid_algs(OBJ_obj2nid(msg->header->protectionAlg->algorithm),
                                         &digest_NID, NULL) ||
        !(digest = (EVP_MD *)EVP_get_digestbynid(digest_NID)))
        goto notsup;
    ret = EVP_VerifyInit_ex(ctx, digest, NULL) &&
          EVP_VerifyUpdate(ctx, protPartDer, protPartDerLen) &&
          EVP_VerifyFinal(ctx, msg->protection->data,
                          msg->protection->length, pubkey);

    /* cleanup */
    EVP_MD_CTX_destroy(ctx);
    OPENSSL_free(protPartDer);
    EVP_PKEY_free(pubkey);
    return ret;
 notsup:
    CMPerr(CMP_F_CMP_VERIFY_SIGNATURE, CMP_R_ALGORITHM_NOT_SUPPORTED);
    return 0;
}

/* ########################################################################## *
 * internal function
 *
 * Validates a message protected with PBMAC
 * ########################################################################## */
static int CMP_verify_MAC(const CMP_PKIMESSAGE *msg,
                          const ASN1_OCTET_STRING *secret)
{
    ASN1_BIT_STRING *protection = NULL;
    int valid = 0;

    /* generate expected protection for the message */
    if (!(protection = CMP_calc_protection_pbmac(msg, secret)))
        goto err;               /* failed to generate protection string! */

    valid = ASN1_STRING_cmp((const ASN1_STRING *)protection,
                            (const ASN1_STRING *)msg->protection) == 0;
    ASN1_BIT_STRING_free(protection);
    return valid;
 err:
    return 0;
}

/* ########################################################################## *
 * Attempt to validate certificate path. returns 1 if the path was
 * validated successfully and 0 if not.
 * ########################################################################## */
int CMP_validate_cert_path(CMP_CTX *ctx, X509_STORE *trusted_store,
                       const STACK_OF (X509) *untrusted_certs, const X509 *cert)
{
    int valid = 0;
    X509_STORE_CTX *csc = NULL;

    if (!cert)
        goto end;

    if (!trusted_store) {
        CMPerr(CMP_F_CMP_VALIDATE_CERT_PATH,
               CMP_R_NO_TRUSTED_CERTIFICATES_SET);
        goto end;
    }

    /* A cert callback could be used to do additional checking, policies for
     * example. */
    /* X509_STORE_set_verify_cb(trusted_store, CMP_cert_callback); */

    if (!(csc = X509_STORE_CTX_new()))
        goto end;

    if (!X509_STORE_CTX_init(csc, trusted_store, (X509 *)cert,
                             (STACK_OF (X509) *)untrusted_certs))
        goto end;

    if (ctx->crls)
        X509_STORE_CTX_set0_crls(csc, ctx->crls);
    valid = X509_verify_cert(csc);
    if (ctx->cert_verify_cb)
        valid = (ctx->cert_verify_cb)(valid, csc);

    X509_STORE_CTX_free(csc);

 end:
    if (valid > 0)
        return 1;

    return 0;
}

#if 0
/* ########################################################################## *
 * NOTE: This is only needed if/when we want to do additional checking on the
 *       certificates!
 *
 *               It is not currently used.
 *
 * This is called for every valid certificate. Here we could add additional
 * checks, for policies for example.
 * ########################################################################## */
int CMP_cert_callback(int ok, X509_STORE_CTX *ctx)
{
    int cert_error = X509_STORE_CTX_get_error(ctx);
    X509 *current_cert = X509_STORE_CTX_get_current_cert(ctx);

    if (!ok) {
        switch (cert_error) {
        case X509_V_ERR_NO_EXPLICIT_POLICY:
            /* policies_print(NULL, ctx); */
        case X509_V_ERR_CERT_HAS_EXPIRED:

            /* since we are just checking the certificates, it is
             * ok if they are self signed. But we should still warn
             * the user.
             */

        case X509_V_ERR_DEPTH_ZERO_SELF_SIGNED_CERT:
            /* Continue after extension errors too */
        case X509_V_ERR_INVALID_CA:
        case X509_V_ERR_INVALID_NON_CA:
        case X509_V_ERR_PATH_LENGTH_EXCEEDED:
        case X509_V_ERR_INVALID_PURPOSE:
        case X509_V_ERR_CRL_HAS_EXPIRED:
        case X509_V_ERR_CRL_NOT_YET_VALID:
        case X509_V_ERR_UNHANDLED_CRITICAL_EXTENSION:
            ok = 1;

        }

        return ok;
    }
# if 0
    /* TODO: we could check policies here too */
    if (cert_error == X509_V_OK && ok == 2)
        policies_print(NULL, ctx);
# endif

    return (ok);
}
#endif

/* ########################################################################## *
 * internal function
 *
 * Find in the given list of certificates one or more certs
 * that have the given subject name and are not yet expired.
 * - if only one match found, return that
 * - else if only one match has the subject key ID given, return that
 * - else return the list of all matches, which may be empty
 * returns NULL on (out of memory) error
 * ########################################################################## */
static STACK_OF (X509) *find_cert(STACK_OF (X509) *certs, X509_NAME *subject,
                               ASN1_OCTET_STRING *keyid, X509_VERIFY_PARAM *vpm)
{
    int i, keyid_matches = 0;
    STACK_OF (X509) *found_certs = sk_X509_new_null();

    if (!found_certs)
        return NULL;

    for (i = 0; i < sk_X509_num(certs); i++) {
        X509 *cert = sk_X509_value(certs, i);
        ASN1_OCTET_STRING *cert_keyid = CMP_get_cert_subject_key_id(cert);
        X509_NAME *name = X509_get_subject_name(cert);
        time_t check_time, *ptime = NULL;
        int expired = 0;
        if (X509_VERIFY_PARAM_get_flags(vpm) & X509_V_FLAG_USE_CHECK_TIME) {
            check_time = X509_VERIFY_PARAM_get_time(vpm);
            ptime = &check_time;
        }
        if (!(X509_VERIFY_PARAM_get_flags(vpm) & X509_V_FLAG_NO_CHECK_TIME))
            expired = X509_cmp_time(X509_get0_notAfter(cert), ptime) < 0;
        if (!expired && name && X509_NAME_cmp(name, subject) == 0) {
            if (!sk_X509_push(found_certs, cert)) {
                sk_X509_pop_free(found_certs, X509_free);
                return NULL;
            }
            X509_up_ref(cert);
            if (keyid && cert_keyid &&
                ASN1_OCTET_STRING_cmp(keyid, cert_keyid) == 0)
                keyid_matches++;
        }
    }

    /* At this point found_certs contains non-expired certs with matching
       subject name, with (key_matches) of them having a matching keyID */
    if (keyid_matches >= 1) { /* at least one of them has a matching keyID;
                                 remove all other entries. keyid is not NULL. */
        for (i = 0; i < sk_X509_num(found_certs); i++) {
            X509 *cert = sk_X509_value(found_certs, i);
            ASN1_OCTET_STRING *cert_keyid = CMP_get_cert_subject_key_id(cert);
            if (!cert_keyid || ASN1_OCTET_STRING_cmp(cert_keyid, keyid) != 0) {
                sk_X509_delete(found_certs, i--);
                X509_free(cert);
            }
        }
    }
    return found_certs;
}

/* ########################################################################## *
 * internal function
 *
 * Find one or more server certificates by using the find_cert() function
 * looking for a non-expired cert with subject matching the msg sender name
 * and (as far as available) a matching sender keyID.
 * Return exactly one if there is a single clear hit, else several candidates.
 * - first try with the trusted store in the context
 * - if no match, then try the untrusted certs in the context
 * - if no match either, then try the extra certs in the message
 * returns NULL on (out of memory) error, else the list of matches
 * ########################################################################## */
static STACK_OF(X509) *find_server_cert(CMP_CTX *ctx, const CMP_PKIMESSAGE *msg)
{
    X509_VERIFY_PARAM *vpm;
    X509_NAME *name;
    ASN1_OCTET_STRING *keyid;
    STACK_OF (X509) *candidates, *found_certs;

    if (!ctx || !msg)
        return NULL;
    vpm = X509_STORE_get0_param(ctx->trusted_store);
    name = msg->header->sender->d.directoryName;
    keyid = msg->header->senderKID;
    if (!ctx || !name) /* keyid is allowed to be NULL */
        return NULL;

    candidates = X509_STORE_get1_certs(ctx->trusted_store);
    found_certs = find_cert(candidates, name, keyid, vpm);
    sk_X509_pop_free(candidates, X509_free);
    if (!found_certs || sk_X509_num(found_certs) > 0)
        return found_certs;

    found_certs = find_cert(ctx->untrusted_certs, name, keyid, vpm);
    if (!found_certs || sk_X509_num(found_certs) > 0)
        return found_certs;

    return find_cert(msg->extraCerts, name, keyid, vpm);
}

/* ##########################################################################
 * Validates the protection of the given PKIMessage using either password-
 * based mac or a signature algorithm. In the case of signature algorithm,
 * the certificate can be provided in ctx->srvCert,
 * else it is taken from extraCerts and validated against ctx->trusted_store
 * utilizing ctx->untrusted_certs and extraCerts.
 *
 * If ctx->permitTAInExtraCertsForIR is true, the trust anchor may be taken from
 * the extraCerts field when a self-signed certificate is found there which can
 * be used to validate the issued certificate returned in IP.  This is according
 * to the need given in 3GPP TS 33.310.
 *
 * returns 1 on success, 0 on error or validation failed
 * ########################################################################## */
int CMP_validate_msg(CMP_CTX *ctx, const CMP_PKIMESSAGE *msg)
{
    X509 *srvCert = ctx->srvCert;
    int srvCert_valid = 0;
    int nid = 0;
#if OPENSSL_VERSION_NUMBER >= 0x1010001fL
    const
#endif
    ASN1_OBJECT *algorOID = NULL;

    if (!msg->header->protectionAlg)
        /* unprotected message */
        return 0;

    /* determine the nid for the used protection algorithm */
    X509_ALGOR_get0(&algorOID, NULL, NULL, msg->header->protectionAlg);
    nid = OBJ_obj2nid(algorOID);

    switch (nid) {
        /* 5.1.3.1.  Shared Secret Information */
    case NID_id_PasswordBasedMAC:
        return CMP_verify_MAC(msg, ctx->secretValue);

        /* TODO: 5.1.3.2.  DH Key Pairs --> feature request #33 */
    case NID_id_DHBasedMac:
        CMPerr(CMP_F_CMP_VALIDATE_MSG,
               CMP_R_UNSUPPORTED_PROTECTION_ALG_DHBASEDMAC);
        break;

        /* 5.1.3.3.  Signature */
        /* TODO: should that better whitelist DSA/RSA etc.?
         * -> check all possible options from OpenSSL, should there be macro? */
    default:
        if (!srvCert) {
            /* if we've already found and validated a server cert, and it
             * matches the sender name, we will use that, this is used for
             * PKIconf where the server certificate and others could be missing
             * from the extraCerts */
            if (ctx->validatedSrvCert &&
                !X509_NAME_cmp(X509_get_subject_name(ctx->validatedSrvCert),
                               msg->header->sender->d.directoryName)) {
                srvCert = ctx->validatedSrvCert;
                srvCert_valid = 1;
            } else {
                STACK_OF (X509) *untrusted = sk_X509_new_null();
                if (untrusted &&
                    sk_X509_add_certs(untrusted, ctx->untrusted_certs, 0) &&
                /* load provided extraCerts to help with cert path validation */
                    sk_X509_add_certs(untrusted, msg->extraCerts, 0)) {
                    int i;

                    /* try to find server certificate(s) from
                     * 1) trusted_store 2) untrusted_certs 3) extaCerts */
                    STACK_OF (X509) *found_certs = find_server_cert(ctx, msg);

                    /* select first server certificate that can be validated */
                    for (i = 0;
                         !srvCert_valid && i < sk_X509_num(found_certs); i++) {
                        srvCert = sk_X509_value(found_certs, i);
                        srvCert_valid = CMP_validate_cert_path(ctx,
                                        ctx->trusted_store, untrusted, srvCert);
                    }
                }

                if (!srvCert_valid) {
                    /* do an exceptional handling for 3GPP for IP:
                     * when the ctx option is explicitly set, extract the Trust
                     * Anchor from ExtraCerts, provided that there is a
                     * self-signed certificate which can be used to validate
                     * the issued certificate - refer to 3GPP TS 33.310 */
                    if (ctx->permitTAInExtraCertsForIR &&
                            CMP_PKIMESSAGE_get_bodytype(msg) == V_CMP_PKIBODY_IP) {
                        X509_STORE *tempStore = X509_STORE_new();
                        if (tempStore &&
                            X509_STORE_add_certs(tempStore, msg->extraCerts,
                                                 1 /* only self_signed */)) {
                            /* TODO: check that issued certificates can validate
                             * against trust anchor - and then exclusively use this
                             * CA */
                            srvCert_valid = CMP_validate_cert_path(ctx, tempStore,
                                                                   ctx->untrusted_certs,
                                                                   srvCert);
                        }
                        if (srvCert_valid) {
                            /* verify that our received certificate can also be
                             * validated with the same trusted store as srvCert */
                            /* TODO handle multiple CertResponses in CertRepMsg (in case multiple requests have been sent) */
                            CMP_CERTRESPONSE *crep = CMP_CERTREPMESSAGE_certResponse_get0(msg->body->value.ip, 0);
                            X509 *newClCert = CMP_CERTRESPONSE_get_certificate(ctx, crep);
                            if (newClCert) {
                                srvCert_valid =
                                    CMP_validate_cert_path(ctx, tempStore, ctx->untrusted_certs, newClCert);
                                X509_free(newClCert);
                            }
                        }

                        X509_STORE_free(tempStore);
                    }
                }
            }

            /* verification failed if no valid server cert was found */
            if (!srvCert_valid) {
                X509_free(srvCert);
                CMPerr(CMP_F_CMP_VALIDATE_MSG, CMP_R_NO_VALID_SRVCERT_FOUND);
                return 0;
            }

            /* store trusted srv cert for future messages in this transaction */
            ctx->validatedSrvCert = srvCert;
        }
        return CMP_verify_signature(msg, srvCert);
    }
    return 0;
}
