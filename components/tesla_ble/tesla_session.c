/* Protocol behavior ported from vehicle-command a4b43c1 (Apache-2.0).
 * Source: internal/dispatcher, internal/authentication, pkg/protocol/error.go.
 */
#include "tesla_protocol.h"
#include "universal_message.pb-c.h"
#include "signatures.pb-c.h"
#include <string.h>

static bool bytes_equal(const uint8_t *a, const uint8_t *b, size_t len)
{
    uint8_t diff = 0;
    for (size_t i = 0; i < len; ++i) diff |= a[i] ^ b[i];
    return diff == 0;
}

bool tesla_request_capture(const uint8_t *wire, size_t len, tesla_request_t *r)
{
    UniversalMessage__RoutableMessage *m = universal_message__routable_message__unpack(NULL, len, wire);
    bool ok = m && m->to_destination && m->from_destination &&
        m->to_destination->sub_destination_case == UNIVERSAL_MESSAGE__DESTINATION__SUB_DESTINATION_DOMAIN &&
        m->from_destination->sub_destination_case == UNIVERSAL_MESSAGE__DESTINATION__SUB_DESTINATION_ROUTING_ADDRESS &&
        m->from_destination->routing_address.len == 16 && m->uuid.len == 16;
    if (ok) {
        memset(r, 0, sizeof(*r));
        r->domain = (tesla_domain_t)m->to_destination->domain;
        memcpy(r->uuid, m->uuid.data, 16);
        memcpy(r->address, m->from_destination->routing_address.data, 16);
        if (m->signature_data && m->signature_data->sig_type_case ==
            SIGNATURES__SIGNATURE_DATA__SIG_TYPE_AES__GCM__PERSONALIZED_DATA &&
            m->signature_data->aes_gcm_personalized_data &&
            m->signature_data->aes_gcm_personalized_data->tag.len == 16) {
            r->authenticated = true;
            r->request_id[0] = TESLA_SIG_AES_GCM_PERSONALIZED;
            memcpy(r->request_id + 1, m->signature_data->aes_gcm_personalized_data->tag.data, 16);
        }
    }
    if (m) universal_message__routable_message__free_unpacked(m, NULL);
    return ok;
}

static bool matches(const UniversalMessage__RoutableMessage *m, const tesla_request_t *r)
{
    if (!m || !m->from_destination || !m->to_destination ||
        m->from_destination->sub_destination_case != UNIVERSAL_MESSAGE__DESTINATION__SUB_DESTINATION_DOMAIN ||
        (int)m->from_destination->domain != (int)r->domain ||
        m->to_destination->sub_destination_case != UNIVERSAL_MESSAGE__DESTINATION__SUB_DESTINATION_ROUTING_ADDRESS ||
        m->to_destination->routing_address.len != 16 ||
        !bytes_equal(m->to_destination->routing_address.data, r->address, 16)) return false;
    /* VCSEC uses a fresh routing address per request and may omit request_uuid. */
    if (m->request_uuid.len != 0 && m->request_uuid.len != 16) return false;
    return r->domain == TESLA_DOMAIN_VEHICLE_SECURITY ||
        (m->request_uuid.len == 16 && bytes_equal(m->request_uuid.data, r->uuid, 16));
}

bool tesla_response_matches(const uint8_t *wire, size_t len, const tesla_request_t *r)
{
    UniversalMessage__RoutableMessage *m = universal_message__routable_message__unpack(NULL, len, wire);
    bool ok = matches(m, r);
    if (m) universal_message__routable_message__free_unpacked(m, NULL);
    return ok;
}

tesla_session_info_result_t tesla_verify_session_info(const uint8_t *wire, size_t len,
    const uint8_t *private_key, const char *vin, const tesla_request_t *r,
    uint32_t local_sec, tesla_session_t *session)
{
    tesla_session_info_result_t result = TESLA_SESSION_INFO_INVALID;
    UniversalMessage__RoutableMessage *m = universal_message__routable_message__unpack(NULL, len, wire);
    if (!matches(m, r) || m->payload_case != UNIVERSAL_MESSAGE__ROUTABLE_MESSAGE__PAYLOAD_SESSION_INFO) goto done;
    tesla_session_t candidate = {0};
    result = tesla_parse_session_info(wire, len, &candidate);
    /* Official GetError also recognizes a request-correlated, explicit rejection.
     * Rejection never creates an authenticated session. */
    if (result != TESLA_SESSION_INFO_OK) goto done;
    result = TESLA_SESSION_INFO_INVALID;
    if (m->request_uuid.len != 16 || !bytes_equal(m->request_uuid.data, r->uuid, 16)) goto done;
    if (!m->signature_data || m->signature_data->sig_type_case !=
        SIGNATURES__SIGNATURE_DATA__SIG_TYPE_SESSION_INFO_TAG ||
        !m->signature_data->session_info_tag || m->signature_data->session_info_tag->tag.len != 32) goto done;
    if (session->established) {
        if (!bytes_equal(candidate.vehicle_public_key, session->vehicle_public_key, 65)) goto done;
        memcpy(candidate.session_key, session->session_key, 16);
        memcpy(candidate.shared_secret, session->shared_secret, 32);
    } else {
        if (!tesla_crypto_ecdh(private_key, candidate.vehicle_public_key, candidate.shared_secret)) goto done;
        tesla_crypto_derive_key(candidate.shared_secret, candidate.session_key);
    }
    /* K' = HMAC(K, "session info"); tag = HMAC(K', TLV || 0xff || encodedInfo). */
    uint8_t subkey[32], tag[32], metadata[TESLA_WIRE_MAX + 64];
    static const uint8_t label[] = "session info";
    if (strlen(vin) != 17 || m->session_info.len > TESLA_WIRE_MAX) goto done;
    if (!tesla_crypto_hmac_sha256(candidate.session_key, 16, label, sizeof(label) - 1, subkey)) goto done;
    uint8_t *p = metadata;
    *p++ = TESLA_TAG_SIGNATURE_TYPE; *p++ = 1; *p++ = TESLA_SIG_HMAC;
    *p++ = TESLA_TAG_PERSONALIZATION; *p++ = 17; memcpy(p, vin, 17); p += 17;
    *p++ = TESLA_TAG_CHALLENGE; *p++ = 16; memcpy(p, r->uuid, 16); p += 16;
    *p++ = TESLA_TAG_END;
    memcpy(p, m->session_info.data, m->session_info.len); p += m->session_info.len;
    if (!tesla_crypto_hmac_sha256(subkey, sizeof(subkey), metadata, p - metadata, tag) ||
        !bytes_equal(tag, m->signature_data->session_info_tag->tag.data, 32)) goto done;
    if (!session->established || !bytes_equal(candidate.epoch, session->epoch, 16) ||
        candidate.clock_time >= session->clock_time) {
        if (session->established && candidate.counter < session->counter) candidate.counter = session->counter;
        candidate.local_time_sec = local_sec;
        candidate.established = true;
        *session = candidate;
    }
    result = TESLA_SESSION_INFO_OK;
done:
    if (m) universal_message__routable_message__free_unpacked(m, NULL);
    return result;
}

bool tesla_fault_retryable(tesla_fault_t fault)
{
    switch (fault) {
    case TESLA_FAULT_BUSY: case TESLA_FAULT_TIMEOUT: case TESLA_FAULT_INVALID_SIGNATURE:
    case TESLA_FAULT_INVALID_TOKEN: case TESLA_FAULT_INTERNAL: case TESLA_FAULT_INCORRECT_EPOCH:
    case TESLA_FAULT_TIME_EXPIRED: case TESLA_FAULT_TTL_TOO_LONG: return true;
    default: return false;
    }
}

tesla_reply_result_t tesla_decode_response(const uint8_t *wire, size_t len,
    const tesla_session_t *session, const char *vin, tesla_request_t *r,
    uint8_t *payload, size_t capacity, uint32_t *payload_len, tesla_fault_t *fault)
{
    *payload_len = 0; *fault = TESLA_FAULT_NONE;
    tesla_reply_result_t result = TESLA_REPLY_INVALID;
    UniversalMessage__RoutableMessage *m = universal_message__routable_message__unpack(NULL, len, wire);
    if (!matches(m, r)) goto done;
    if (m->signedmessagestatus) *fault = (tesla_fault_t)m->signedmessagestatus->signed_message_fault;
    if (m->payload_case == UNIVERSAL_MESSAGE__ROUTABLE_MESSAGE__PAYLOAD_SESSION_INFO) {
        result = *fault == TESLA_FAULT_NONE ? TESLA_REPLY_SESSION : TESLA_REPLY_FAULT; goto done;
    }
    if (m->payload_case != UNIVERSAL_MESSAGE__ROUTABLE_MESSAGE__PAYLOAD_PROTOBUF_MESSAGE_AS_BYTES &&
        m->payload_case != UNIVERSAL_MESSAGE__ROUTABLE_MESSAGE__PAYLOAD__NOT_SET) goto done;
    size_t size = m->protobuf_message_as_bytes.len;
    if (size > capacity) goto done;
    Signatures__AESGCMResponseSignatureData *gcm = NULL;
    if (m->signature_data && m->signature_data->sig_type_case ==
        SIGNATURES__SIGNATURE_DATA__SIG_TYPE_AES__GCM__RESPONSE_DATA) gcm = m->signature_data->aes_gcm_response_data;
    if (gcm) {
        if (!r->authenticated || !session->established || gcm->nonce.len != 12 || gcm->tag.len != 16) goto done;
        uint8_t metadata[128], aad[32];
        size_t size_meta = tesla_build_response_metadata(metadata, r->domain, vin, gcm->counter,
            m->flags, r->request_id, sizeof(r->request_id), (uint32_t)*fault);
        tesla_crypto_sha256(metadata, size_meta, aad);
        if (!tesla_crypto_decrypt_gcm(session->session_key, gcm->nonce.data, aad, sizeof(aad),
            m->protobuf_message_as_bytes.data, size, gcm->tag.data, payload)) goto done;
        if (!tesla_replay_accept(&r->replay, gcm->counter)) { result = TESLA_REPLY_DUPLICATE; goto done; }
    } else {
        /* Same fallback as the SDK: pre-2024.38 firmware replies in plaintext. */
        if (size) memcpy(payload, m->protobuf_message_as_bytes.data, size);
    }
    /* Authenticate encrypted fault metadata before acting on rejection/retry. */
    if (*fault != TESLA_FAULT_NONE) { result = TESLA_REPLY_FAULT; goto done; }
    if (m->signedmessagestatus && m->signedmessagestatus->operation_status ==
        UNIVERSAL_MESSAGE__OPERATION_STATUS__E__OPERATIONSTATUS_WAIT) {
        *fault = TESLA_FAULT_BUSY; result = TESLA_REPLY_FAULT; goto done;
    }
    if (m->signedmessagestatus && m->signedmessagestatus->operation_status ==
        UNIVERSAL_MESSAGE__OPERATION_STATUS__E__OPERATIONSTATUS_ERROR) goto done;
    *payload_len = size;
    result = TESLA_REPLY_OK;
done:
    if (m) universal_message__routable_message__free_unpacked(m, NULL);
    return result;
}
