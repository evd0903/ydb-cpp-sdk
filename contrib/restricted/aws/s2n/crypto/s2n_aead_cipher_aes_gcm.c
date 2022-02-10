/*
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License").
 * You may not use this file except in compliance with the License.
 * A copy of the License is located at
 *
 *  http://aws.amazon.com/apache2.0
 *
 * or in the "license" file accompanying this file. This file is distributed
 * on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either
 * express or implied. See the License for the specific language governing
 * permissions and limitations under the License.
 */

#include <openssl/aes.h>
#include <openssl/evp.h>

#include "crypto/s2n_cipher.h"

#include "tls/s2n_crypto.h"

#include "utils/s2n_safety.h"
#include "utils/s2n_blob.h"

#if defined(OPENSSL_IS_BORINGSSL) || defined(OPENSSL_IS_AWSLC)
#define S2N_AEAD_AES_GCM_AVAILABLE
#endif

static uint8_t s2n_aead_cipher_aes128_gcm_available()
{
#if defined(S2N_AEAD_AES_GCM_AVAILABLE)
    return (EVP_aead_aes_128_gcm() ? 1 : 0);
#else
    return (EVP_aes_128_gcm() ? 1 : 0);
#endif
}

static uint8_t s2n_aead_cipher_aes256_gcm_available()
{
#if defined(S2N_AEAD_AES_GCM_AVAILABLE)
    return (EVP_aead_aes_256_gcm() ? 1 : 0);
#else
    return (EVP_aes_256_gcm() ? 1 : 0);
#endif
}

#if defined(S2N_AEAD_AES_GCM_AVAILABLE) /* BoringSSL and AWS-LC AEAD API implementation */

static int s2n_aead_cipher_aes_gcm_encrypt(struct s2n_session_key *key, struct s2n_blob *iv, struct s2n_blob *aad, struct s2n_blob *in, struct s2n_blob *out)
{
    notnull_check(in);
    notnull_check(out);
    notnull_check(iv);
    notnull_check(key);
    notnull_check(aad);

    /* The size of the |in| blob includes the size of the data and the size of the AES-GCM tag */
    gte_check(in->size, S2N_TLS_GCM_TAG_LEN);
    gte_check(out->size, in->size);
    eq_check(iv->size, S2N_TLS_GCM_IV_LEN);

    /* Adjust input length to account for the Tag length */
    size_t in_len = in->size - S2N_TLS_GCM_TAG_LEN;
    size_t out_len = 0;

    GUARD_OSSL(EVP_AEAD_CTX_seal(key->evp_aead_ctx, out->data, &out_len, out->size, iv->data, iv->size, in->data, in_len, aad->data, aad->size), S2N_ERR_ENCRYPT);

    S2N_ERROR_IF((in_len + S2N_TLS_GCM_TAG_LEN) != out_len, S2N_ERR_ENCRYPT);

    return S2N_SUCCESS;
}

static int s2n_aead_cipher_aes_gcm_decrypt(struct s2n_session_key *key, struct s2n_blob *iv, struct s2n_blob *aad, struct s2n_blob *in, struct s2n_blob *out)
{
    notnull_check(in);
    notnull_check(out);
    notnull_check(iv);
    notnull_check(key);
    notnull_check(aad);

    gte_check(in->size, S2N_TLS_GCM_TAG_LEN);
    gte_check(out->size, in->size - S2N_TLS_GCM_TAG_LEN);
    eq_check(iv->size, S2N_TLS_GCM_IV_LEN);

    size_t out_len = 0;

    GUARD_OSSL(EVP_AEAD_CTX_open(key->evp_aead_ctx, out->data, &out_len, out->size, iv->data, iv->size, in->data, in->size, aad->data, aad->size), S2N_ERR_DECRYPT);

    S2N_ERROR_IF((in->size - S2N_TLS_GCM_TAG_LEN) != out_len, S2N_ERR_ENCRYPT);

    return S2N_SUCCESS;
}

static int s2n_aead_cipher_aes128_gcm_set_encryption_key(struct s2n_session_key *key, struct s2n_blob *in)
{
    notnull_check(key);
    notnull_check(in);

    eq_check(in->size, S2N_TLS_AES_128_GCM_KEY_LEN);

    GUARD_OSSL(EVP_AEAD_CTX_init(key->evp_aead_ctx, EVP_aead_aes_128_gcm(), in->data, in->size, S2N_TLS_GCM_TAG_LEN, NULL), S2N_ERR_KEY_INIT);

    return S2N_SUCCESS;
}

static int s2n_aead_cipher_aes256_gcm_set_encryption_key(struct s2n_session_key *key, struct s2n_blob *in)
{
    notnull_check(key);
    notnull_check(in);

    eq_check(in->size, S2N_TLS_AES_256_GCM_KEY_LEN);

    GUARD_OSSL(EVP_AEAD_CTX_init(key->evp_aead_ctx, EVP_aead_aes_256_gcm(), in->data, in->size, S2N_TLS_GCM_TAG_LEN, NULL), S2N_ERR_KEY_INIT);

    return S2N_SUCCESS;
}

static int s2n_aead_cipher_aes128_gcm_set_decryption_key(struct s2n_session_key *key, struct s2n_blob *in)
{
    notnull_check(key);
    notnull_check(in);

    eq_check(in->size, S2N_TLS_AES_128_GCM_KEY_LEN);

    GUARD_OSSL(EVP_AEAD_CTX_init(key->evp_aead_ctx, EVP_aead_aes_128_gcm(), in->data, in->size, S2N_TLS_GCM_TAG_LEN, NULL), S2N_ERR_KEY_INIT);

    return S2N_SUCCESS;
}

static int s2n_aead_cipher_aes256_gcm_set_decryption_key(struct s2n_session_key *key, struct s2n_blob *in)
{
    notnull_check(key);
    notnull_check(in);

    eq_check(in->size, S2N_TLS_AES_256_GCM_KEY_LEN);

    GUARD_OSSL(EVP_AEAD_CTX_init(key->evp_aead_ctx, EVP_aead_aes_256_gcm(), in->data, in->size, S2N_TLS_GCM_TAG_LEN, NULL), S2N_ERR_KEY_INIT);

    return S2N_SUCCESS;
}

static int s2n_aead_cipher_aes_gcm_init(struct s2n_session_key *key)
{
    notnull_check(key);

    EVP_AEAD_CTX_zero(key->evp_aead_ctx);

    return S2N_SUCCESS;
}

static int s2n_aead_cipher_aes_gcm_destroy_key(struct s2n_session_key *key)
{
    notnull_check(key);

    EVP_AEAD_CTX_cleanup(key->evp_aead_ctx);

    return S2N_SUCCESS;
}

#else /* Standard AES-GCM implementation */

static int s2n_aead_cipher_aes_gcm_encrypt(struct s2n_session_key *key, struct s2n_blob *iv, struct s2n_blob *aad, struct s2n_blob *in, struct s2n_blob *out)
{
    /* The size of the |in| blob includes the size of the data and the size of the ChaCha20-Poly1305 tag */
    gte_check(in->size, S2N_TLS_GCM_TAG_LEN);
    gte_check(out->size, in->size);
    eq_check(iv->size, S2N_TLS_GCM_IV_LEN);

    /* Initialize the IV */
    GUARD_OSSL(EVP_EncryptInit_ex(key->evp_cipher_ctx, NULL, NULL, NULL, iv->data), S2N_ERR_KEY_INIT);

    /* Adjust input length and buffer pointer to account for the Tag length */
    int in_len = in->size - S2N_TLS_GCM_TAG_LEN;
    uint8_t *tag_data = out->data + out->size - S2N_TLS_GCM_TAG_LEN;

    int out_len;
    /* Specify the AAD */
    GUARD_OSSL(EVP_EncryptUpdate(key->evp_cipher_ctx, NULL, &out_len, aad->data, aad->size), S2N_ERR_ENCRYPT);

    /* Encrypt the data */
    GUARD_OSSL(EVP_EncryptUpdate(key->evp_cipher_ctx, out->data, &out_len, in->data, in_len), S2N_ERR_ENCRYPT);

    /* When using AES-GCM, *out_len is the number of bytes written by EVP_EncryptUpdate. Since the tag is not written during this call, we do not take S2N_TLS_GCM_TAG_LEN into account */
    S2N_ERROR_IF(in_len != out_len, S2N_ERR_ENCRYPT);

    /* Finalize */
    GUARD_OSSL(EVP_EncryptFinal_ex(key->evp_cipher_ctx, out->data, &out_len), S2N_ERR_ENCRYPT);

    /* write the tag */
    GUARD_OSSL(EVP_CIPHER_CTX_ctrl(key->evp_cipher_ctx, EVP_CTRL_GCM_GET_TAG, S2N_TLS_GCM_TAG_LEN, tag_data), S2N_ERR_ENCRYPT);

    /* When using AES-GCM, EVP_EncryptFinal_ex does not write any bytes. So, we should expect *out_len = 0. */
    S2N_ERROR_IF(0 != out_len, S2N_ERR_ENCRYPT);

    return S2N_SUCCESS;
}

static int s2n_aead_cipher_aes_gcm_decrypt(struct s2n_session_key *key, struct s2n_blob *iv, struct s2n_blob *aad, struct s2n_blob *in, struct s2n_blob *out)
{
    gte_check(in->size, S2N_TLS_GCM_TAG_LEN);
    gte_check(out->size, in->size);
    eq_check(iv->size, S2N_TLS_GCM_IV_LEN);

    /* Initialize the IV */
    GUARD_OSSL(EVP_DecryptInit_ex(key->evp_cipher_ctx, NULL, NULL, NULL, iv->data), S2N_ERR_KEY_INIT);

    /* Adjust input length and buffer pointer to account for the Tag length */
    int in_len = in->size - S2N_TLS_GCM_TAG_LEN;
    uint8_t *tag_data = in->data + in->size - S2N_TLS_GCM_TAG_LEN;

    /* Set the TAG */
    GUARD_OSSL(EVP_CIPHER_CTX_ctrl(key->evp_cipher_ctx, EVP_CTRL_GCM_SET_TAG, S2N_TLS_GCM_TAG_LEN, tag_data), S2N_ERR_DECRYPT);

    int out_len;
    /* Specify the AAD */
    GUARD_OSSL(EVP_DecryptUpdate(key->evp_cipher_ctx, NULL, &out_len, aad->data, aad->size), S2N_ERR_DECRYPT);

    int evp_decrypt_rc = 1;
    /* Decrypt the data, but don't short circuit tag verification. EVP_Decrypt* return 0 on failure, 1 for success. */
    evp_decrypt_rc &= EVP_DecryptUpdate(key->evp_cipher_ctx, out->data, &out_len, in->data, in_len);

    /* Verify the tag */
    evp_decrypt_rc &= EVP_DecryptFinal_ex(key->evp_cipher_ctx, out->data, &out_len);

    S2N_ERROR_IF(evp_decrypt_rc != 1, S2N_ERR_DECRYPT);

    /* While we verify the content of out_len in s2n_aead_cipher_aes_gcm_encrypt, we refrain from this here. This is to avoid doing any branching before the ciphertext is verified. */

    return S2N_SUCCESS;
}

static int s2n_aead_cipher_aes128_gcm_set_encryption_key(struct s2n_session_key *key, struct s2n_blob *in)
{
    eq_check(in->size, S2N_TLS_AES_128_GCM_KEY_LEN);

    GUARD_OSSL(EVP_EncryptInit_ex(key->evp_cipher_ctx, EVP_aes_128_gcm(), NULL, NULL, NULL), S2N_ERR_KEY_INIT);

    EVP_CIPHER_CTX_ctrl(key->evp_cipher_ctx, EVP_CTRL_GCM_SET_IVLEN, S2N_TLS_GCM_IV_LEN, NULL);

    GUARD_OSSL(EVP_EncryptInit_ex(key->evp_cipher_ctx, NULL, NULL, in->data, NULL), S2N_ERR_KEY_INIT);

    return S2N_SUCCESS;
}

static int s2n_aead_cipher_aes256_gcm_set_encryption_key(struct s2n_session_key *key, struct s2n_blob *in)
{
    eq_check(in->size, S2N_TLS_AES_256_GCM_KEY_LEN);

    GUARD_OSSL(EVP_EncryptInit_ex(key->evp_cipher_ctx, EVP_aes_256_gcm(), NULL, NULL, NULL), S2N_ERR_KEY_INIT);

    EVP_CIPHER_CTX_ctrl(key->evp_cipher_ctx, EVP_CTRL_GCM_SET_IVLEN, S2N_TLS_GCM_IV_LEN, NULL);

    GUARD_OSSL(EVP_EncryptInit_ex(key->evp_cipher_ctx, NULL, NULL, in->data, NULL), S2N_ERR_KEY_INIT);

    return S2N_SUCCESS;
}

static int s2n_aead_cipher_aes128_gcm_set_decryption_key(struct s2n_session_key *key, struct s2n_blob *in)
{
    eq_check(in->size, S2N_TLS_AES_128_GCM_KEY_LEN);

    GUARD_OSSL(EVP_DecryptInit_ex(key->evp_cipher_ctx, EVP_aes_128_gcm(), NULL, NULL, NULL), S2N_ERR_KEY_INIT);

    EVP_CIPHER_CTX_ctrl(key->evp_cipher_ctx, EVP_CTRL_GCM_SET_IVLEN, S2N_TLS_GCM_IV_LEN, NULL);

    GUARD_OSSL(EVP_DecryptInit_ex(key->evp_cipher_ctx, NULL, NULL, in->data, NULL), S2N_ERR_KEY_INIT);

    return S2N_SUCCESS;
}

static int s2n_aead_cipher_aes256_gcm_set_decryption_key(struct s2n_session_key *key, struct s2n_blob *in)
{
    eq_check(in->size, S2N_TLS_AES_256_GCM_KEY_LEN);

    GUARD_OSSL(EVP_DecryptInit_ex(key->evp_cipher_ctx, EVP_aes_256_gcm(), NULL, NULL, NULL), S2N_ERR_KEY_INIT);

    EVP_CIPHER_CTX_ctrl(key->evp_cipher_ctx, EVP_CTRL_GCM_SET_IVLEN, S2N_TLS_GCM_IV_LEN, NULL);

    GUARD_OSSL(EVP_DecryptInit_ex(key->evp_cipher_ctx, NULL, NULL, in->data, NULL), S2N_ERR_KEY_INIT);

    return S2N_SUCCESS;
}

static int s2n_aead_cipher_aes_gcm_init(struct s2n_session_key *key)
{
    s2n_evp_ctx_init(key->evp_cipher_ctx);

    return S2N_SUCCESS;
}

static int s2n_aead_cipher_aes_gcm_destroy_key(struct s2n_session_key *key)
{
    EVP_CIPHER_CTX_cleanup(key->evp_cipher_ctx);

    return S2N_SUCCESS;
}

#endif

struct s2n_cipher s2n_aes128_gcm = {
    .key_material_size = S2N_TLS_AES_128_GCM_KEY_LEN,
    .type = S2N_AEAD,
    .io.aead = {
                .record_iv_size = S2N_TLS_GCM_EXPLICIT_IV_LEN,
                .fixed_iv_size = S2N_TLS_GCM_FIXED_IV_LEN,
                .tag_size = S2N_TLS_GCM_TAG_LEN,
                .decrypt = s2n_aead_cipher_aes_gcm_decrypt,
                .encrypt = s2n_aead_cipher_aes_gcm_encrypt},
    .is_available = s2n_aead_cipher_aes128_gcm_available,
    .init = s2n_aead_cipher_aes_gcm_init,
    .set_encryption_key = s2n_aead_cipher_aes128_gcm_set_encryption_key,
    .set_decryption_key = s2n_aead_cipher_aes128_gcm_set_decryption_key,
    .destroy_key = s2n_aead_cipher_aes_gcm_destroy_key,
};

struct s2n_cipher s2n_aes256_gcm = {
    .key_material_size = S2N_TLS_AES_256_GCM_KEY_LEN,
    .type = S2N_AEAD,
    .io.aead = {
                .record_iv_size = S2N_TLS_GCM_EXPLICIT_IV_LEN,
                .fixed_iv_size = S2N_TLS_GCM_FIXED_IV_LEN,
                .tag_size = S2N_TLS_GCM_TAG_LEN,
                .decrypt = s2n_aead_cipher_aes_gcm_decrypt,
                .encrypt = s2n_aead_cipher_aes_gcm_encrypt},
    .is_available = s2n_aead_cipher_aes256_gcm_available,
    .init = s2n_aead_cipher_aes_gcm_init,
    .set_encryption_key = s2n_aead_cipher_aes256_gcm_set_encryption_key,
    .set_decryption_key = s2n_aead_cipher_aes256_gcm_set_decryption_key,
    .destroy_key = s2n_aead_cipher_aes_gcm_destroy_key,
};

/* TLS 1.3 GCM ciphers */
struct s2n_cipher s2n_tls13_aes128_gcm = {
    .key_material_size = S2N_TLS_AES_128_GCM_KEY_LEN,
    .type = S2N_AEAD,
    .io.aead = {
                .record_iv_size = S2N_TLS13_RECORD_IV_LEN,
                .fixed_iv_size = S2N_TLS13_FIXED_IV_LEN,
                .tag_size = S2N_TLS_GCM_TAG_LEN,
                .decrypt = s2n_aead_cipher_aes_gcm_decrypt,
                .encrypt = s2n_aead_cipher_aes_gcm_encrypt},
    .is_available = s2n_aead_cipher_aes128_gcm_available,
    .init = s2n_aead_cipher_aes_gcm_init,
    .set_encryption_key = s2n_aead_cipher_aes128_gcm_set_encryption_key,
    .set_decryption_key = s2n_aead_cipher_aes128_gcm_set_decryption_key,
    .destroy_key = s2n_aead_cipher_aes_gcm_destroy_key,
};

struct s2n_cipher s2n_tls13_aes256_gcm = {
    .key_material_size = S2N_TLS_AES_256_GCM_KEY_LEN,
    .type = S2N_AEAD,
    .io.aead = {
                .record_iv_size = S2N_TLS13_RECORD_IV_LEN,
                .fixed_iv_size = S2N_TLS13_FIXED_IV_LEN,
                .tag_size = S2N_TLS_GCM_TAG_LEN,
                .decrypt = s2n_aead_cipher_aes_gcm_decrypt,
                .encrypt = s2n_aead_cipher_aes_gcm_encrypt},
    .is_available = s2n_aead_cipher_aes256_gcm_available,
    .init = s2n_aead_cipher_aes_gcm_init,
    .set_encryption_key = s2n_aead_cipher_aes256_gcm_set_encryption_key,
    .set_decryption_key = s2n_aead_cipher_aes256_gcm_set_decryption_key,
    .destroy_key = s2n_aead_cipher_aes_gcm_destroy_key,
};
