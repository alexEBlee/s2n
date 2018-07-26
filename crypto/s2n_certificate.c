/*
 * Copyright 2014 Amazon.com, Inc. or its affiliates. All Rights Reserved.
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

#include <s2n.h>

#include "crypto/s2n_certificate.h"
#include "utils/s2n_safety.h"
#include "utils/s2n_mem.h"

int s2n_cert_public_key_set_rsa_from_openssl(s2n_cert_public_key *public_key, RSA *openssl_rsa)
{
    notnull_check(openssl_rsa);
    notnull_check(public_key);
    public_key->key.rsa_key.rsa = openssl_rsa;

    return 0;
}

int s2n_cert_set_cert_type(struct s2n_cert *cert, s2n_cert_type cert_type)
{
    notnull_check(cert);
    cert->cert_type = cert_type;
    s2n_pkey_setup_for_type(&cert->public_key, cert_type);
    return 0;
}

int s2n_create_cert_chain_from_stuffer(struct s2n_cert_chain *cert_chain_out, struct s2n_stuffer *chain_in_stuffer)
{
    struct s2n_stuffer cert_out_stuffer;
    GUARD(s2n_stuffer_growable_alloc(&cert_out_stuffer, 2048));

    struct s2n_cert **insert = &cert_chain_out->head;
    uint32_t chain_size = 0;
    do {
        struct s2n_cert *new_node;

        if (s2n_stuffer_certificate_from_pem(chain_in_stuffer, &cert_out_stuffer) < 0) {
            if (chain_size == 0) {
                GUARD(s2n_stuffer_free(&cert_out_stuffer));
                S2N_ERROR(S2N_ERR_NO_CERTIFICATE_IN_PEM);
            }
            break;
        }
        struct s2n_blob mem;
        GUARD(s2n_alloc(&mem, sizeof(struct s2n_cert)));
        new_node = (struct s2n_cert *)(void *)mem.data;

        GUARD(s2n_alloc(&new_node->raw, s2n_stuffer_data_available(&cert_out_stuffer)));
        GUARD(s2n_stuffer_read(&cert_out_stuffer, &new_node->raw));

        /* Additional 3 bytes for the length field in the protocol */
        chain_size += new_node->raw.size + 3;
        new_node->next = NULL;
        *insert = new_node;
        insert = &new_node->next;
    } while (s2n_stuffer_data_available(chain_in_stuffer));

    GUARD(s2n_stuffer_free(&cert_out_stuffer));
    
    /* Leftover data at this point means one of two things:
     * A bug in s2n's PEM parsing OR a malformed PEM in the user's chain.
     * Be conservative and fail instead of using a partial chain.
     */
    S2N_ERROR_IF(s2n_stuffer_data_available(chain_in_stuffer) > 0, S2N_ERR_INVALID_PEM);
    
    cert_chain_out->chain_size = chain_size;
    
    return 0;
}

int s2n_cert_chain_and_key_set_cert_chain_from_stuffer(struct s2n_cert_chain_and_key *cert_and_key, struct s2n_stuffer *chain_in_stuffer)
{
    return s2n_create_cert_chain_from_stuffer(cert_and_key->cert_chain, chain_in_stuffer);
}

int s2n_cert_chain_and_key_set_cert_chain(struct s2n_cert_chain_and_key *cert_and_key, const char *cert_chain_pem)
{
    struct s2n_stuffer chain_in_stuffer;

    /* Turn the chain into a stuffer */
    GUARD(s2n_stuffer_alloc_ro_from_string(&chain_in_stuffer, cert_chain_pem));
    int rc = s2n_cert_chain_and_key_set_cert_chain_from_stuffer(cert_and_key, &chain_in_stuffer);

    GUARD(s2n_stuffer_free(&chain_in_stuffer));

    return rc;
}

int s2n_cert_chain_and_key_set_private_key(struct s2n_cert_chain_and_key *cert_and_key, const char *private_key_pem)
{
    struct s2n_stuffer key_in_stuffer, key_out_stuffer;
    struct s2n_blob key_blob;

    GUARD(s2n_pkey_zero_init(cert_and_key->private_key));

    /* Put the private key pem in a stuffer */
    GUARD(s2n_stuffer_alloc_ro_from_string(&key_in_stuffer, private_key_pem));
    GUARD(s2n_stuffer_growable_alloc(&key_out_stuffer, strlen(private_key_pem)));

    /* Convert pem to asn1 and asn1 to the private key. Handles both PKCS#1 and PKCS#8 formats */
    GUARD(s2n_stuffer_private_key_from_pem(&key_in_stuffer, &key_out_stuffer));
    GUARD(s2n_stuffer_free(&key_in_stuffer));
    key_blob.size = s2n_stuffer_data_available(&key_out_stuffer);
    key_blob.data = s2n_stuffer_raw_read(&key_out_stuffer, key_blob.size);
    notnull_check(key_blob.data);

    /* Get key type and create appropriate key context */
    GUARD(s2n_asn1der_to_private_key(cert_and_key->private_key, &key_blob));
    GUARD(s2n_stuffer_free(&key_out_stuffer));

    return 0;
}

struct s2n_cert_chain_and_key *s2n_cert_chain_and_key_new(const char *cert_chain_pem, const char *private_key_pem)
{
    struct s2n_blob mem, cert_chain_mem, pkey_mem;
    struct s2n_cert_chain_and_key *cert_and_key;

    /* Allocate the memory for the chain and key struct */
    GUARD_PTR(s2n_alloc(&mem, sizeof(struct s2n_cert_chain_and_key)));
    cert_and_key = (struct s2n_cert_chain_and_key *)(void *)mem.data;
    GUARD_GOTO(s2n_alloc(&cert_chain_mem, sizeof(struct s2n_cert_chain)), failed);
    cert_and_key->cert_chain = (struct s2n_cert_chain *)(void *)cert_chain_mem.data;
    GUARD_GOTO(s2n_alloc(&pkey_mem, sizeof(s2n_cert_private_key)), failed);
    cert_and_key->private_key = (s2n_cert_private_key *)(void *)pkey_mem.data;

    cert_and_key->cert_chain->head = NULL;
    memset(&cert_and_key->ocsp_status, 0, sizeof(cert_and_key->ocsp_status));
    memset(&cert_and_key->sct_list, 0, sizeof(cert_and_key->sct_list));
    GUARD_GOTO(s2n_pkey_zero_init(cert_and_key->private_key), failed);
    
    GUARD_GOTO(s2n_cert_chain_and_key_set_cert_chain(cert_and_key, cert_chain_pem), failed);
    GUARD_GOTO(s2n_cert_chain_and_key_set_private_key(cert_and_key, private_key_pem), failed);

    /* Parse the leaf cert for the public key and certificate type */
    struct s2n_pkey public_key;
    s2n_cert_type cert_type;
    GUARD_GOTO(s2n_asn1der_to_public_key_and_type(&public_key, &cert_type, &cert_and_key->cert_chain->head->raw), failed);
    GUARD_GOTO(s2n_cert_set_cert_type(cert_and_key->cert_chain->head, cert_type), failed);

    /* Validate the leaf cert's public key matches the provided private key */
    int key_match_ret = s2n_pkey_match(&public_key, cert_and_key->private_key);
    GUARD_GOTO(s2n_pkey_free(&public_key), failed);
    if (key_match_ret < 0) {
        /* s2n_errno already set */
        goto failed;
    }

    return cert_and_key;

failed:
    GUARD_PTR(s2n_cert_chain_and_key_free(cert_and_key));    
    return NULL;
}

int s2n_cert_chain_and_key_free(struct s2n_cert_chain_and_key *cert_and_key)
{
    if (cert_and_key == NULL) {
        return 0;
    }

    struct s2n_blob b = {
        .data = (uint8_t *) cert_and_key,
        .size = sizeof(struct s2n_cert_chain_and_key)
    };

    /* Walk the chain and free the certs */
    if (cert_and_key->cert_chain) {
        struct s2n_cert *node = cert_and_key->cert_chain->head;
        while (node) {
            struct s2n_blob n = {
                .data = (uint8_t *) node,
                .size = sizeof(struct s2n_cert)
            };
            /* Free the cert */
            GUARD(s2n_free(&node->raw));
            /* Advance to next */
            node = node->next;
            /* Free the node */
            GUARD(s2n_free(&n));
        }
    }
    
    GUARD(s2n_pkey_free(cert_and_key->private_key));
    GUARD(s2n_free(&cert_and_key->ocsp_status));
    GUARD(s2n_free(&cert_and_key->sct_list));

    GUARD(s2n_free(&b));
    return 0;
}

int s2n_send_cert_chain(struct s2n_stuffer *out, struct s2n_cert_chain *chain)
{
    notnull_check(out);
    notnull_check(chain);
    GUARD(s2n_stuffer_write_uint24(out, chain->chain_size));

    struct s2n_cert *cur_cert = chain->head;
    while (cur_cert) {
        notnull_check(cur_cert); 
        GUARD(s2n_stuffer_write_uint24(out, cur_cert->raw.size));
        GUARD(s2n_stuffer_write_bytes(out, cur_cert->raw.data, cur_cert->raw.size));
        cur_cert = cur_cert->next;
    }

    return 0;
}

int s2n_send_empty_cert_chain(struct s2n_stuffer *out) {
    notnull_check(out);
    GUARD(s2n_stuffer_write_uint24(out, 0));
    return 0;
}
