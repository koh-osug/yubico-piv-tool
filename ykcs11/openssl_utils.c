/*
 * Copyright (c) 2015-2016 Yubico AB
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *
 *   * Redistributions in binary form must reproduce the above
 *     copyright notice, this list of conditions and the following
 *     disclaimer in the documentation and/or other materials provided
 *     with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include "openssl_utils.h"
#include <stdbool.h>
#include <ykpiv.h>
#include "../tool/util.h" // TODO: share this better?
#include "../tool/openssl-compat.h" // TODO: share this better?
#include "debug.h"
#include <string.h>

#if OPENSSL_VERSION_NUMBER >= 0x10100000L
# define X509_set_notBefore X509_set1_notBefore
# define X509_set_notAfter X509_set1_notAfter
#endif

CK_RV do_rand_seed(CK_BYTE_PTR data, CK_ULONG len) {
  RAND_seed(data, len);
  return CKR_OK;
}

CK_RV do_rand_bytes(CK_BYTE_PTR data, CK_ULONG len) {
  RAND_bytes(data, len);
  return CKR_OK;
}

CK_RV do_store_cert(CK_BYTE_PTR data, CK_ULONG len, ykcs11_x509_t **cert) {

  const unsigned char *p = data; // Mandatory temp variable required by OpenSSL
  int                 cert_len;

  if (*p == 0x70) {
    // The certificate is in "PIV" format 0x70 len 0x30 len ...
    p++;
    p += get_length(p, &cert_len);
  }
  else {
    // Raw certificate 0x30 len ...
    cert_len = 0;
    cert_len += get_length(p + 1, &cert_len) + 1;
  }

  if ((CK_ULONG)cert_len > len)
    return CKR_ARGUMENTS_BAD;

  if(*cert)
    X509_free(*cert);

  *cert = d2i_X509(NULL, &p, cert_len);
  if (*cert == NULL)
    return CKR_FUNCTION_FAILED;

  return CKR_OK;

}

CK_RV do_generate_ec_key(int nid, ykcs11_evp_pkey_t **pkey) {
  EC_GROUP *group = EC_GROUP_new_by_curve_name(nid);
  if(group == NULL)
    return CKR_HOST_MEMORY;
  EC_GROUP_set_asn1_flag(group, nid);
  EC_KEY *eckey = EC_KEY_new();
  if(eckey == NULL)
    return CKR_HOST_MEMORY;
  if(EC_KEY_set_group(eckey, group) <= 0)
    return CKR_GENERAL_ERROR;
  if(EC_KEY_generate_key(eckey) <= 0)
    return CKR_GENERAL_ERROR;
  *pkey = EVP_PKEY_new();
  if(*pkey == NULL)
    return CKR_HOST_MEMORY;
  if(EVP_PKEY_set1_EC_KEY(*pkey, eckey) <= 0)
    return CKR_GENERAL_ERROR;
  return CKR_OK;
}

CK_RV do_create_ec_key(CK_BYTE_PTR point, CK_ULONG point_len, int nid, ykcs11_evp_pkey_t **pkey) {
  EC_GROUP *group = EC_GROUP_new_by_curve_name(nid);
  if(group == NULL)
    return CKR_HOST_MEMORY;
  EC_GROUP_set_asn1_flag(group, nid);
  EC_KEY *eckey = EC_KEY_new();
  if(eckey == NULL)
    return CKR_HOST_MEMORY;
  if(EC_KEY_set_group(eckey, group) <= 0)
    return CKR_GENERAL_ERROR;
  EC_POINT *ecpoint = EC_POINT_new(group);
  if(ecpoint == NULL)
    return CKR_HOST_MEMORY;
  if(EC_POINT_oct2point(group, ecpoint, point, point_len, NULL) <= 0)
    return CKR_ARGUMENTS_BAD;
  if(EC_KEY_set_public_key(eckey, ecpoint) <= 0)
    return CKR_GENERAL_ERROR;
  *pkey = EVP_PKEY_new();
  if(*pkey == NULL)
    return CKR_HOST_MEMORY;
  EVP_PKEY_set1_EC_KEY(*pkey, eckey);
  return CKR_OK;
}

CK_RV do_create_rsa_key(CK_BYTE_PTR mod, CK_ULONG mod_len, CK_BYTE_PTR exp, CK_ULONG exp_len, ykcs11_evp_pkey_t **pkey) {
  BIGNUM *n = BN_bin2bn(mod, mod_len, 0);
  if(n == NULL)
    return CKR_HOST_MEMORY;
  BIGNUM *e = BN_bin2bn(exp, exp_len, 0);
  if(e == NULL)
    return CKR_HOST_MEMORY;
  RSA *rsa = RSA_new();
  if(rsa == NULL)
    return CKR_HOST_MEMORY;
  if(RSA_set0_key(rsa, n, e, NULL) <= 0)
      return CKR_GENERAL_ERROR;
  *pkey = EVP_PKEY_new();
  if(*pkey == NULL)
    return CKR_HOST_MEMORY;
  if(EVP_PKEY_set1_RSA(*pkey, rsa) <= 0)
      return CKR_GENERAL_ERROR;
  return CKR_OK;
}

CK_RV do_create_public_key(CK_BYTE_PTR in, CK_ULONG in_len, CK_ULONG algorithm, ykcs11_evp_pkey_t **pkey) {
  int len, nid = get_curve_name(algorithm);

  if (nid == 0) {
    if (*in++ != 0x81)
      return CKR_GENERAL_ERROR;

    in += get_length(in, &len);

    unsigned char *mod = in;
    int mod_len = len;

    in += len;

    if(*in++ != 0x82)
      return CKR_GENERAL_ERROR;

    in += get_length(in, &len);

    return do_create_rsa_key(mod, mod_len, in, len, pkey);
  }
  else {
    if(*in++ != 0x86)
      return CKR_GENERAL_ERROR;

    in += get_length(in, &len);

    return do_create_ec_key(in, len, nid, pkey);
  }
}

CK_RV do_sign_empty_cert(const char *cn, ykcs11_evp_pkey_t *pubkey, ykcs11_evp_pkey_t *pvtkey, ykcs11_x509_t **cert) {
  *cert = X509_new();
  if (*cert == NULL)
    return CKR_HOST_MEMORY;
  X509_set_version(*cert, 2); // Version 3
  X509_NAME_add_entry_by_txt(X509_get_issuer_name(*cert), "CN", MBSTRING_ASC, (unsigned char*)cn, -1, -1, 0);
  X509_NAME_add_entry_by_txt(X509_get_subject_name(*cert), "CN", MBSTRING_ASC, (unsigned char*)cn, -1, -1, 0);
  ASN1_INTEGER_set(X509_get_serialNumber(*cert), 0);
  X509_gmtime_adj(X509_get_notBefore(*cert), 0);
  X509_gmtime_adj(X509_get_notAfter(*cert), 0);
  X509_set_pubkey(*cert, pubkey);
  if (X509_sign(*cert, pvtkey, EVP_sha1()) <= 0)
    return CKR_GENERAL_ERROR;
  return CKR_OK;
}

CK_RV do_create_empty_cert(CK_BYTE_PTR in, CK_ULONG in_len, CK_ULONG algorithm,
                          const char *cn, CK_BYTE_PTR out, CK_ULONG_PTR out_len) {

  EVP_PKEY  *pubkey;
  EVP_PKEY  *pvtkey;
  X509      *cert;
  CK_RV     rv;

  if((rv = do_create_public_key(in, in_len, algorithm, &pubkey)) != CKR_OK)
    return rv;

  if((rv = do_generate_ec_key(NID_X9_62_prime256v1, &pvtkey)) != CKR_OK)
    return rv;
  
  if((rv = do_sign_empty_cert(cn, pubkey, pvtkey, &cert)) != CKR_OK)
    return rv;

  int len = i2d_X509(cert, NULL);
  if (len <= 0)
    return CKR_GENERAL_ERROR;

  if (len > *out_len)
    return CKR_BUFFER_TOO_SMALL;

  len = i2d_X509(cert, &out);
  if (len <= 0)
    return CKR_GENERAL_ERROR;

  *out_len = len;
  return CKR_OK;
}

CK_RV do_check_cert(CK_BYTE_PTR in, CK_ULONG_PTR cert_len) {

  X509                *cert;
  const unsigned char *p = in; // Mandatory temp variable required by OpenSSL
  int                 len;

  len = 0;
  len += get_length(p + 1, &len) + 1;

  *cert_len = (CK_ULONG) len;

  cert = d2i_X509(NULL, &p, (long) *cert_len);
  if (cert == NULL)
    return CKR_FUNCTION_FAILED;

  return CKR_OK;
}

CK_RV do_get_raw_cert(ykcs11_x509_t *cert, CK_BYTE_PTR out, CK_ULONG_PTR out_len) {

  CK_BYTE_PTR p;
  int         len;

  len = i2d_X509(cert, NULL);

  if (len < 0)
    return CKR_FUNCTION_FAILED;

  if ((CK_ULONG)len > *out_len)
    return CKR_BUFFER_TOO_SMALL;

  p = out;
  if ((*out_len = (CK_ULONG) i2d_X509(cert, &p)) == 0)
    return CKR_FUNCTION_FAILED;

  return CKR_OK;
}

CK_RV do_get_raw_name(ykcs11_x509_name_t *name, CK_BYTE_PTR out, CK_ULONG_PTR out_len) {

  CK_BYTE_PTR p;
  int         len;

  len = i2d_X509_NAME(name, NULL);

  if (len < 0)
    return CKR_FUNCTION_FAILED;

  if ((CK_ULONG)len > *out_len)
    return CKR_BUFFER_TOO_SMALL;

  p = out;
  if ((*out_len = (CK_ULONG) i2d_X509_NAME(name, &p)) == 0)
    return CKR_FUNCTION_FAILED;

  return CKR_OK;
}

CK_RV do_get_raw_integer(ykcs11_asn1_integer_t *serial, CK_BYTE_PTR out, CK_ULONG_PTR out_len) {

  CK_BYTE_PTR p;
  int         len;

  len = i2d_ASN1_INTEGER(serial, NULL);

  if (len < 0)
    return CKR_FUNCTION_FAILED;

  if ((CK_ULONG)len > *out_len)
    return CKR_BUFFER_TOO_SMALL;

  p = out;
  if ((*out_len = (CK_ULONG) i2d_ASN1_INTEGER(serial, &p)) == 0)
    return CKR_FUNCTION_FAILED;

  return CKR_OK;
}

CK_RV do_delete_cert(ykcs11_x509_t **cert) {

  X509_free(*cert);
  *cert = NULL;

  return CKR_OK;

}

CK_RV do_store_pubk(ykcs11_x509_t *cert, ykcs11_evp_pkey_t **key) {

  if(*key)
    EVP_PKEY_free(*key);

  *key = X509_get_pubkey(cert);

  if (*key == NULL)
    return CKR_FUNCTION_FAILED;

  return CKR_OK;

}

CK_KEY_TYPE do_get_key_type(ykcs11_evp_pkey_t *key) {

  switch (EVP_PKEY_base_id(key)) {
  case EVP_PKEY_RSA:
    return CKK_RSA;

  case EVP_PKEY_EC:
    return CKK_ECDSA;

  default:
    return CKK_VENDOR_DEFINED; // Actually an error
  }
}

CK_ULONG do_get_key_size(ykcs11_evp_pkey_t *key) {
  return EVP_PKEY_bits(key);
}

CK_BYTE do_get_key_algorithm(ykcs11_evp_pkey_t *key) {

  switch (EVP_PKEY_base_id(key)) {
  case EVP_PKEY_RSA:
    switch(EVP_PKEY_bits(key)) {
    case 1024:
      return YKPIV_ALGO_RSA1024;
    case 2048:
      return YKPIV_ALGO_RSA2048;
    }
  case EVP_PKEY_EC:
    switch(EVP_PKEY_bits(key)) {
    case 256:
      return YKPIV_ALGO_ECCP256;
    case 384:
      return YKPIV_ALGO_ECCP384;
    }
  }
  return 0; // Actually an error
}

CK_RV do_get_modulus(ykcs11_evp_pkey_t *key, CK_BYTE_PTR data, CK_ULONG_PTR len) {
  RSA *rsa;
  const BIGNUM *n;

  rsa = EVP_PKEY_get1_RSA(key);
  if (rsa == NULL)
    return CKR_FUNCTION_FAILED;

  RSA_get0_key(rsa, &n, NULL, NULL);
  if ((CK_ULONG)BN_num_bytes(n) > *len) {
    RSA_free(rsa);
    rsa = NULL;
    return CKR_BUFFER_TOO_SMALL;
  }

  *len = (CK_ULONG)BN_bn2bin(n, data);

  RSA_free(rsa);
  rsa = NULL;

  return CKR_OK;
}

CK_RV do_get_public_exponent(ykcs11_evp_pkey_t *key, CK_BYTE_PTR data, CK_ULONG_PTR len) {

  CK_ULONG e = 0;
  RSA *rsa;
  const BIGNUM *bn_e;

  rsa = EVP_PKEY_get1_RSA(key);
  if (rsa == NULL)
    return CKR_FUNCTION_FAILED;

  RSA_get0_key(rsa, NULL, &bn_e, NULL);
  if ((CK_ULONG)BN_num_bytes(bn_e) > *len) {
    RSA_free(rsa);
    rsa = NULL;
    return CKR_BUFFER_TOO_SMALL;
  }

  *len = (CK_ULONG)BN_bn2bin(bn_e, data);

  RSA_free(rsa);
  rsa = NULL;

  return e;
}

/* #include <stdio.h> */
/* #include <openssl/err.h> */
/*   ERR_load_crypto_strings(); */
/* //SSL_load_error_strings(); */
/*   fprintf(stderr, "ERROR %s\n", ERR_error_string(ERR_get_error(), NULL)); */
CK_RV do_get_public_key(ykcs11_evp_pkey_t *key, CK_BYTE_PTR data, CK_ULONG_PTR len) {

  RSA *rsa;
  unsigned char *p;

  EC_KEY *eck;
  const EC_GROUP *ecg; // Alternative solution is to get i2d_PUBKEY and manually offset
  const EC_POINT *ecp;
  point_conversion_form_t pcf = POINT_CONVERSION_UNCOMPRESSED;

  switch(EVP_PKEY_base_id(key)) {
  case EVP_PKEY_RSA:

    rsa = EVP_PKEY_get1_RSA(key);

    if ((CK_ULONG)RSA_size(rsa) > *len) {
      RSA_free(rsa);
      rsa = NULL;
      return CKR_BUFFER_TOO_SMALL;
    }

    p = data;

    if ((*len = (CK_ULONG) i2d_RSAPublicKey(rsa, &p)) == 0) {
      RSA_free(rsa);
      rsa = NULL;
      return CKR_FUNCTION_FAILED;
    }

    // TODO: this is the correct thing to do so that we strip out the exponent
    // OTOH we also need a function to get the exponent out with CKA_PUBLIC_EXPONENT
    /*BN_bn2bin(rsa->n, data);
     *len = 256;*/

    /* fprintf(stderr, "Public key is: \n"); */
    /* dump_hex(data, *len, stderr, CK_TRUE); */

    break;

  case EVP_PKEY_EC:
    eck = EVP_PKEY_get1_EC_KEY(key);
    ecg = EC_KEY_get0_group(eck);
    ecp = EC_KEY_get0_public_key(eck);

    // Add the DER structure with length after extracting the point
    data[0] = 0x04;

    if ((*len = EC_POINT_point2oct(ecg, ecp, pcf, data + 2, *len - 2, NULL)) == 0) {
      EC_KEY_free(eck);
      eck = NULL;
      return CKR_FUNCTION_FAILED;
    }

    data[1] = *len;

    *len += 2;

    EC_KEY_free(eck);
    eck = NULL;

    break;

  default:
    return CKR_FUNCTION_FAILED;
  }

  return CKR_OK;

}

CK_RV do_decode_rsa_public_key(ykcs11_rsa_key_t **key, CK_BYTE_PTR modulus,
          CK_ULONG mlen, CK_BYTE_PTR exponent, CK_ULONG elen) {
  ykcs11_rsa_key_t *k;
  BIGNUM *k_n = NULL, *k_e = NULL;
  if (modulus == NULL || exponent == NULL)
    return CKR_ARGUMENTS_BAD;

  if ((k = RSA_new()) == NULL)
    return CKR_HOST_MEMORY;

  if ((k_n = BN_bin2bn(modulus, mlen, NULL)) == NULL)
    return CKR_FUNCTION_FAILED;

  if ((k_e = BN_bin2bn(exponent, elen, NULL)) == NULL)
    return CKR_FUNCTION_FAILED;

  if (RSA_set0_key(k, k_n, k_e, NULL) == 0)
    return CKR_FUNCTION_FAILED;

  *key = k;
  return CKR_OK;
}

CK_RV do_free_rsa_public_key(ykcs11_rsa_key_t *key) {
  RSA_free(key);
  return CKR_OK;
}

CK_RV do_get_curve_parameters(ykcs11_evp_pkey_t *key, CK_BYTE_PTR data, CK_ULONG_PTR len) {

  EC_KEY *eck;
  const EC_GROUP *ecg;
  unsigned char *p;

  eck = EVP_PKEY_get1_EC_KEY(key);
  ecg = EC_KEY_get0_group(eck);

  p = data;

  if ((*len = (CK_ULONG) i2d_ECPKParameters(ecg, &p)) == 0) {
    EC_KEY_free(eck);
    eck = NULL;
    return CKR_FUNCTION_FAILED;
  }

  EC_KEY_free(eck);
  eck = NULL;

  return CKR_OK;
}

CK_RV do_delete_pubk(EVP_PKEY **key) {

  EVP_PKEY_free(*key);
  *key = NULL;

  return CKR_OK;

}

CK_RV do_pkcs_1_t1(CK_BYTE_PTR in, CK_ULONG in_len, CK_BYTE_PTR out, CK_ULONG_PTR out_len, CK_ULONG key_len) {
  unsigned char buffer[512];

  key_len /= 8;
  DBG("Apply padding to %lu bytes and get %lu", in_len, key_len);

  // TODO: rand must be seeded first (should be automatic)
  if (*out_len < key_len)
    return CKR_BUFFER_TOO_SMALL;

  if (RSA_padding_add_PKCS1_type_1(buffer, key_len, in, in_len) == 0)
    return CKR_FUNCTION_FAILED;

  memcpy(out, buffer, key_len);
  *out_len = key_len;

  return CKR_OK;
}

CK_RV do_pkcs_1_digest_info(CK_BYTE_PTR in, CK_ULONG in_len, int nid, CK_BYTE_PTR out, CK_ULONG_PTR out_len) {

  unsigned int len;
  CK_RV rv;

  rv = prepare_rsa_signature(in, in_len, out, &len, nid);
  if (!rv)
    return CKR_FUNCTION_FAILED;

  *out_len = len;

  return CKR_OK;

}

CK_RV do_pkcs_pss(ykcs11_rsa_key_t *key, CK_BYTE_PTR in, CK_ULONG in_len,
          int nid, CK_BYTE_PTR out, CK_ULONG_PTR out_len) {
  unsigned char em[RSA_size(key)];

#if OPENSSL_VERSION_NUMBER < 0x10100000L
  OpenSSL_add_all_digests();
#endif

  DBG("Apply PSS padding to %lu bytes and get %d", in_len, RSA_size(key));

  // TODO: rand must be seeded first (should be automatic)
  if (out != in)
    memcpy(out, in, in_len);

  // In case of raw PSS (no hash) this function will fail because OpenSSL requires an MD
  if (RSA_padding_add_PKCS1_PSS(key, em, out, EVP_get_digestbynid(nid), -2) == 0) {
#if OPENSSL_VERSION_NUMBER < 0x10100000L
    EVP_cleanup();
#endif
    return CKR_FUNCTION_FAILED;
  }

  memcpy(out, em, sizeof(em));
  *out_len = (CK_ULONG) sizeof(em);

#if OPENSSL_VERSION_NUMBER < 0x10100000L
  EVP_cleanup();
#endif

  return CKR_OK;
}

CK_RV do_md_init(ykcs11_hash_t hash, ykcs11_md_ctx_t **ctx) {

  const EVP_MD *md;

  switch (hash) {
  case YKCS11_NO_HASH:
    return CKR_FUNCTION_FAILED;

  case YKCS11_SHA1:
    md = EVP_sha1();
    break;

    //case YKCS11_SHA224:

  case YKCS11_SHA256:
    md = EVP_sha256();
    break;

  case YKCS11_SHA384:
    md = EVP_sha384();
    break;

  case YKCS11_SHA512:
    md = EVP_sha512();
    break;

  //case YKCS11_RIPEMD128_RSA_PKCS_HASH:
  //case YKCS11_RIPEMD160_HASH:

  default:
    return CKR_FUNCTION_FAILED;
  }

  *ctx = EVP_MD_CTX_create();

  // The OpenSSL function above never fail
  if (EVP_DigestInit_ex(*ctx, md, NULL) == 0) {
    EVP_MD_CTX_destroy(*ctx);
    return CKR_FUNCTION_FAILED;
  }

  return CKR_OK;
}

CK_RV do_md_update(ykcs11_md_ctx_t *ctx, CK_BYTE_PTR in, CK_ULONG in_len) {

  if (EVP_DigestUpdate(ctx, in, in_len) != 1) {
    EVP_MD_CTX_destroy(ctx);
    return CKR_FUNCTION_FAILED;
  }

  return CKR_OK;

}

CK_RV do_md_finalize(ykcs11_md_ctx_t *ctx, CK_BYTE_PTR out, CK_ULONG_PTR out_len, int *nid) {

  int rv;
  unsigned int len;

  // Keep track of the md type if requested
  if (nid != NULL)
    *nid = EVP_MD_CTX_type(ctx);

  // Finalize digest and store result
  rv = EVP_DigestFinal_ex(ctx, out, &len);

  // Destroy the md context
  EVP_MD_CTX_destroy(ctx);

  // Error if the previous call failed
  if (rv != 1)
    return CKR_FUNCTION_FAILED;

  *out_len = len;

  return CKR_OK;
}

CK_RV do_md_cleanup(ykcs11_md_ctx_t *ctx) {

  EVP_MD_CTX_destroy(ctx);

  return CKR_OK;
}
