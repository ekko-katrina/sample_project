/**
 * @file tesla_protocol.c
 * @brief Tesla 协议消息编码/解码
 *
 * 使用 protobuf-c 实现 Protocol Buffer 消息编解码，与 vehicle-command Go SDK 的 proto 定义一致。
 */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tesla_protocol.h"

/* protobuf-c 生成头（proto-c 由 proto/gen_proto.sh 生成） */
#include "universal_message.pb-c.h"
#include "vcsec.pb-c.h"
#include "signatures.pb-c.h"
#include "car_server_slim.pb-c.h"

static const char *TAG = "TESLA_PROTO";

/* 用于 unpack/free_unpacked 的默认分配器（若 lib 未导出 protobuf_c_default_allocator） */
static void *default_alloc_alloc(void *allocator_data, size_t size)
{
    (void)allocator_data;
    return malloc(size);
}
static void default_alloc_free(void *allocator_data, void *pointer)
{
    (void)allocator_data;
    free(pointer);
}
static ProtobufCAllocator s_default_allocator = {
    .alloc = default_alloc_alloc,
    .free = default_alloc_free,
    .allocator_data = NULL
};

// ========== 消息构建（protobuf-c）==========

/**
 * @brief 构建会话信息请求消息（使用 protobuf-c）
 */
uint32_t tesla_build_session_info_request(
    uint8_t *buf,
    uint32_t buf_size,
    tesla_domain_t domain,
    const uint8_t *public_key,
    const uint8_t *routing_address)
{
    uint8_t challenge[16];
    uint8_t uuid[16];
    esp_fill_random(challenge, sizeof(challenge));
    esp_fill_random(uuid, sizeof(uuid));

    UniversalMessage__Destination to_dest = UNIVERSAL_MESSAGE__DESTINATION__INIT;
    to_dest.sub_destination_case = UNIVERSAL_MESSAGE__DESTINATION__SUB_DESTINATION_DOMAIN;
    to_dest.domain = (UniversalMessage__Domain)domain;

    UniversalMessage__Destination from_dest = UNIVERSAL_MESSAGE__DESTINATION__INIT;
    from_dest.sub_destination_case = UNIVERSAL_MESSAGE__DESTINATION__SUB_DESTINATION_ROUTING_ADDRESS;
    from_dest.routing_address.data = (uint8_t *)routing_address;
    from_dest.routing_address.len = 16;

    UniversalMessage__SessionInfoRequest session_req = UNIVERSAL_MESSAGE__SESSION_INFO_REQUEST__INIT;
    session_req.public_key.data = (uint8_t *)public_key;
    session_req.public_key.len = 65;
    session_req.challenge.data = challenge;
    session_req.challenge.len = sizeof(challenge);

    UniversalMessage__RoutableMessage msg = UNIVERSAL_MESSAGE__ROUTABLE_MESSAGE__INIT;
    msg.to_destination = &to_dest;
    msg.from_destination = &from_dest;
    msg.payload_case = UNIVERSAL_MESSAGE__ROUTABLE_MESSAGE__PAYLOAD_SESSION_INFO_REQUEST;
    msg.session_info_request = &session_req;
    msg.uuid.data = uuid;
    msg.uuid.len = sizeof(uuid);

    size_t packed_size = universal_message__routable_message__get_packed_size(&msg);
    if (packed_size == 0 || packed_size > buf_size) {
        return 0;
    }
    return (uint32_t)universal_message__routable_message__pack(&msg, buf);
}

/**
 * @brief 构建信息请求（VCSEC）消息（UnsignedMessage，protobuf-c）
 */
uint32_t tesla_build_info_request(
    uint8_t *buf,
    uint32_t buf_size,
    tesla_info_request_type_t type)
{
    VCSEC__InformationRequest info_req = VCSEC__INFORMATION_REQUEST__INIT;
    info_req.informationrequesttype = (VCSEC__InformationRequestType)type;

    VCSEC__UnsignedMessage msg = VCSEC__UNSIGNED_MESSAGE__INIT;
    msg.sub_message_case = VCSEC__UNSIGNED_MESSAGE__SUB_MESSAGE_INFORMATION_REQUEST;
    msg.informationrequest = &info_req;

    size_t packed_size = vcsec__unsigned_message__get_packed_size(&msg);
    if (packed_size == 0 || packed_size > buf_size) {
        return 0;
    }
    return (uint32_t)vcsec__unsigned_message__pack(&msg, buf);
}

/**
 * @brief 构建 RKE 动作消息（UnsignedMessage，protobuf-c）
 */
uint32_t tesla_build_rke_action(
    uint8_t *buf,
    uint32_t buf_size,
    tesla_rke_action_t action)
{
    VCSEC__UnsignedMessage msg = VCSEC__UNSIGNED_MESSAGE__INIT;
    msg.sub_message_case = VCSEC__UNSIGNED_MESSAGE__SUB_MESSAGE_RKEACTION;
    msg.rkeaction = (VCSEC__RKEActionE)action;

    size_t packed_size = vcsec__unsigned_message__get_packed_size(&msg);
    if (packed_size == 0 || packed_size > buf_size) {
        return 0;
    }
    return (uint32_t)vcsec__unsigned_message__pack(&msg, buf);
}

/**
 * @brief 构建添加密钥请求消息（ToVCSECMessage，protobuf-c）
 */
uint32_t tesla_build_add_key_request(
    uint8_t *buf,
    uint32_t buf_size,
    const uint8_t *public_key,
    tesla_key_form_factor_t form_factor)
{
    VCSEC__PublicKey pub_key = VCSEC__PUBLIC_KEY__INIT;
    pub_key.publickeyraw.data = (uint8_t *)public_key;
    pub_key.publickeyraw.len = 65;

    VCSEC__PermissionChange perm_change = VCSEC__PERMISSION_CHANGE__INIT;
    perm_change.key = &pub_key;
    perm_change.keyrole = KEYS__ROLE__ROLE_OWNER;

    VCSEC__KeyMetadata meta = VCSEC__KEY_METADATA__INIT;
    meta.keyformfactor = (VCSEC__KeyFormFactor)form_factor;

    VCSEC__WhitelistOperation whitelist_op = VCSEC__WHITELIST_OPERATION__INIT;
    whitelist_op.sub_message_case = VCSEC__WHITELIST_OPERATION__SUB_MESSAGE_ADD_KEY_TO_WHITELIST_AND_ADD_PERMISSIONS;
    whitelist_op.addkeytowhitelistandaddpermissions = &perm_change;
    whitelist_op.metadataforkey = &meta;

    VCSEC__UnsignedMessage unsigned_msg = VCSEC__UNSIGNED_MESSAGE__INIT;
    unsigned_msg.sub_message_case = VCSEC__UNSIGNED_MESSAGE__SUB_MESSAGE_WHITELIST_OPERATION;
    unsigned_msg.whitelistoperation = &whitelist_op;

    uint8_t unsigned_buf[256];
    size_t unsigned_len = vcsec__unsigned_message__pack(&unsigned_msg, unsigned_buf);
    if (unsigned_len == 0 || unsigned_len > sizeof(unsigned_buf)) {
        return 0;
    }

    VCSEC__SignedMessage signed_msg = VCSEC__SIGNED_MESSAGE__INIT;
    signed_msg.protobufmessageasbytes.data = unsigned_buf;
    signed_msg.protobufmessageasbytes.len = unsigned_len;
    signed_msg.signaturetype = VCSEC__SIGNATURE_TYPE__SIGNATURE_TYPE_PRESENT_KEY;

    VCSEC__ToVCSECMessage to_vcsec = VCSEC__TO_VCSECMESSAGE__INIT;
    to_vcsec.signedmessage = &signed_msg;

    size_t packed_size = vcsec__to_vcsecmessage__get_packed_size(&to_vcsec);
    if (packed_size == 0 || packed_size > buf_size) {
        return 0;
    }
    return (uint32_t)vcsec__to_vcsecmessage__pack(&to_vcsec, buf);
}

/**
 * 构建用于 GetDriveState 的 CarServer Action 消息（使用 protobuf-c 生成的 CarServer__Action）
 */
uint32_t tesla_build_get_drive_state(
    uint8_t *buf,
    uint32_t buf_size)
{
    CarServer__GetDriveState gds = CAR_SERVER__GET_DRIVE_STATE__INIT;
    CarServer__GetVehicleData gvd = CAR_SERVER__GET_VEHICLE_DATA__INIT;
    gvd.getdrivestate = &gds;

    CarServer__VehicleAction va = CAR_SERVER__VEHICLE_ACTION__INIT;
    va.getvehicledata = &gvd;

    CarServer__Action action = CAR_SERVER__ACTION__INIT;
    action.vehicleaction = &va;

    size_t packed_size = car_server__action__get_packed_size(&action);
    if (packed_size == 0 || packed_size > buf_size) {
        return 0;
    }
    car_server__action__pack(&action, buf);
    ESP_LOGD(TAG, "Built GetDriveState Action (%zu bytes)", packed_size);
    return (uint32_t)packed_size;
}

/**
 * @brief 构建元数据 (用于请求认证与响应解密)
 * @param buf 输出缓冲区
 * @param sig_type 签名类型
 * @param domain 域
 * @param vin 车辆 VIN
 * @param epoch 时期
 * @param expires_at 过期时间
 * @param counter 计数器
 * @param flags 标志
 * @return 元数据长度
 */
uint32_t tesla_build_metadata(
    uint8_t *buf,
    tesla_signature_type_t sig_type,
    tesla_domain_t domain,
    const char *vin,
    const uint8_t *epoch,
    uint32_t expires_at,
    uint32_t counter,
    uint32_t flags)
{
    uint8_t *p = buf;

    // TAG_SIGNATURE_TYPE
    *p++ = TESLA_TAG_SIGNATURE_TYPE;
    *p++ = 1;
    *p++ = sig_type;

    // TAG_DOMAIN
    *p++ = TESLA_TAG_DOMAIN;
    *p++ = 1;
    *p++ = domain;

    // TAG_PERSONALIZATION (VIN)
    if (vin) {
        *p++ = TESLA_TAG_PERSONALIZATION;
        *p++ = 17;
        memcpy(p, vin, 17);
        p += 17;
    }

    // TAG_EPOCH
    if (epoch) {
        *p++ = TESLA_TAG_EPOCH;
        *p++ = 16;
        memcpy(p, epoch, 16);
        p += 16;
    }

    // TAG_EXPIRES_AT (Big-endian, as per Go SDK)
    *p++ = TESLA_TAG_EXPIRES_AT;
    *p++ = 4;
    p[0] = (expires_at >> 24) & 0xFF;  // MSB 优先
    p[1] = (expires_at >> 16) & 0xFF;
    p[2] = (expires_at >> 8) & 0xFF;
    p[3] = expires_at & 0xFF;          // LSB 最后
    p += 4;

    // TAG_COUNTER（计数器，大端序，与 Go SDK 一致）
    *p++ = TESLA_TAG_COUNTER;
    *p++ = 4;
    p[0] = (counter >> 24) & 0xFF;  // MSB 优先
    p[1] = (counter >> 16) & 0xFF;
    p[2] = (counter >> 8) & 0xFF;
    p[3] = counter & 0xFF;          // LSB 最后
    p += 4;

    // TAG_FLAGS（标志，大端序，与 Go SDK 一致）
    if (flags) {
        *p++ = TESLA_TAG_FLAGS;
        *p++ = 4;
        p[0] = (flags >> 24) & 0xFF;  // MSB 优先
        p[1] = (flags >> 16) & 0xFF;
        p[2] = (flags >> 8) & 0xFF;
        p[3] = flags & 0xFF;          // LSB 最后
        p += 4;
    }

    // TAG_END（结束标记）
    *p++ = TESLA_TAG_END;

    return p - buf;
}

// 构建用于响应解密的元数据（与请求元数据不同）
// 响应元数据格式：sig_type, domain, VIN, counter, flags, request_hash, fault
// request_id 是 17 字节：signature_type(1) + GCM_tag(16) 来自原始请求
static uint32_t tesla_build_response_metadata(
    uint8_t *buf,
    tesla_domain_t domain,
    const char *vin,
    uint32_t counter,
    uint32_t flags,
    const uint8_t *request_id,
    uint32_t request_id_len,
    uint32_t fault)
{
    uint8_t *p = buf;

    // TAG_SIGNATURE_TYPE = AES_GCM_RESPONSE (9)
    *p++ = TESLA_TAG_SIGNATURE_TYPE;
    *p++ = 1;
    *p++ = TESLA_SIG_AES_GCM_RESPONSE;

    // TAG_DOMAIN（域）
    *p++ = TESLA_TAG_DOMAIN;
    *p++ = 1;
    *p++ = domain;

    // TAG_PERSONALIZATION (VIN)
    if (vin) {
        *p++ = TESLA_TAG_PERSONALIZATION;
        *p++ = 17;
        memcpy(p, vin, 17);
        p += 17;
    }

    // TAG_COUNTER (Big-endian)
    *p++ = TESLA_TAG_COUNTER;
    *p++ = 4;
    p[0] = (counter >> 24) & 0xFF;
    p[1] = (counter >> 16) & 0xFF;
    p[2] = (counter >> 8) & 0xFF;
    p[3] = counter & 0xFF;
    p += 4;

    // TAG_FLAGS（标志，大端序） - 总是包含，即使为 0
    *p++ = TESLA_TAG_FLAGS;
    *p++ = 4;
    p[0] = (flags >> 24) & 0xFF;
    p[1] = (flags >> 16) & 0xFF;
    p[2] = (flags >> 8) & 0xFF;
    p[3] = flags & 0xFF;
    p += 4;

    // TAG_REQUEST_HASH（request_id = 请求的 signature_type + GCM tag）
    if (request_id && request_id_len > 0) {
        *p++ = TESLA_TAG_REQUEST_HASH;
        *p++ = (uint8_t)request_id_len;
        memcpy(p, request_id, request_id_len);
        p += request_id_len;
    }

    // TAG_FAULT (Big-endian)
    *p++ = TESLA_TAG_FAULT;
    *p++ = 4;
    p[0] = (fault >> 24) & 0xFF;
    p[1] = (fault >> 16) & 0xFF;
    p[2] = (fault >> 8) & 0xFF;
    p[3] = fault & 0xFF;
    p += 4;

    // TAG_END（结束标记）
    *p++ = TESLA_TAG_END;

    return p - buf;
}

/**
 * @brief 构建已认证的可路由消息（protobuf-c）
 * @param buf 输出缓冲区
 * @param buf_size 缓冲区大小
 * @param domain 域
 * @param payload 负载
 * @param payload_len 负载长度
 * @param session 会话
 * @param routing_address 路由地址
 * @param public_key 公钥
 * @param vin 车辆 VIN
 * @param flags 标志
 * @param request_id_out 输出请求 ID
 * @return 消息长度
 */
uint32_t tesla_build_routable_message(
    uint8_t *buf,
    uint32_t buf_size,
    tesla_domain_t domain,
    const uint8_t *payload,
    uint32_t payload_len,
    const tesla_session_t *session,
    const uint8_t *routing_address,
    const uint8_t *public_key,
    const char *vin,
    uint32_t flags,
    uint8_t *request_id_out)
{
    uint8_t metadata[128];
    uint32_t current_local_sec = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS / 1000);
    uint32_t elapsed_sec = current_local_sec - session->local_time_sec;
    uint32_t current_vehicle_clock = session->clock_time + elapsed_sec;
    uint32_t expires_at = current_vehicle_clock + 30;

    uint32_t metadata_len = tesla_build_metadata(metadata,
        TESLA_SIG_AES_GCM_PERSONALIZED,
        domain, vin, session->epoch, expires_at,
        session->counter, flags);

    ESP_LOGD(TAG, "Metadata for AAD: sig=%d domain=%d expires=%lu counter=%lu",
             TESLA_SIG_AES_GCM_PERSONALIZED, domain,
             (unsigned long)expires_at, (unsigned long)session->counter);
    ESP_LOG_BUFFER_HEX_LEVEL(TAG, metadata, metadata_len, ESP_LOG_DEBUG);

    uint8_t aad_hash[32];
    tesla_crypto_sha256(metadata, metadata_len, aad_hash);

    uint8_t nonce[12];
    esp_fill_random(nonce, sizeof(nonce));

    uint8_t ciphertext[512];
    uint8_t tag[16];

    if (!tesla_crypto_encrypt_gcm(session->session_key, nonce, aad_hash, 32,
                                   payload, payload_len, ciphertext, tag)) {
        ESP_LOGE(TAG, "Encryption failed");
        return 0;
    }

    if (request_id_out) {
        request_id_out[0] = TESLA_SIG_AES_GCM_PERSONALIZED;
        memcpy(request_id_out + 1, tag, 16);
    }

    Signatures__KeyIdentity key_identity = SIGNATURES__KEY_IDENTITY__INIT;
    key_identity.identity_type_case = SIGNATURES__KEY_IDENTITY__IDENTITY_TYPE_PUBLIC_KEY;
    key_identity.public_key.data = (uint8_t *)public_key;
    key_identity.public_key.len = 65;

    Signatures__AESGCMPersonalizedSignatureData gcm_data = SIGNATURES__AES__GCM__PERSONALIZED__SIGNATURE__DATA__INIT;
    gcm_data.epoch.data = (uint8_t *)session->epoch;
    gcm_data.epoch.len = 16;
    gcm_data.nonce.data = nonce;
    gcm_data.nonce.len = 12;
    gcm_data.counter = session->counter;
    gcm_data.expires_at = expires_at;
    gcm_data.tag.data = tag;
    gcm_data.tag.len = 16;

    Signatures__SignatureData sig_data = SIGNATURES__SIGNATURE_DATA__INIT;
    sig_data.signer_identity = &key_identity;
    sig_data.sig_type_case = SIGNATURES__SIGNATURE_DATA__SIG_TYPE_AES__GCM__PERSONALIZED_DATA;
    sig_data.aes_gcm_personalized_data = &gcm_data;

    uint8_t uuid[16];
    esp_fill_random(uuid, sizeof(uuid));

    UniversalMessage__Destination to_dest = UNIVERSAL_MESSAGE__DESTINATION__INIT;
    to_dest.sub_destination_case = UNIVERSAL_MESSAGE__DESTINATION__SUB_DESTINATION_DOMAIN;
    to_dest.domain = (UniversalMessage__Domain)domain;

    UniversalMessage__Destination from_dest = UNIVERSAL_MESSAGE__DESTINATION__INIT;
    from_dest.sub_destination_case = UNIVERSAL_MESSAGE__DESTINATION__SUB_DESTINATION_ROUTING_ADDRESS;
    from_dest.routing_address.data = (uint8_t *)routing_address;
    from_dest.routing_address.len = 16;

    UniversalMessage__RoutableMessage msg = UNIVERSAL_MESSAGE__ROUTABLE_MESSAGE__INIT;
    msg.to_destination = &to_dest;
    msg.from_destination = &from_dest;
    msg.payload_case = UNIVERSAL_MESSAGE__ROUTABLE_MESSAGE__PAYLOAD_PROTOBUF_MESSAGE_AS_BYTES;
    msg.protobuf_message_as_bytes.data = ciphertext;
    msg.protobuf_message_as_bytes.len = payload_len;
    msg.sub_sig_data_case = UNIVERSAL_MESSAGE__ROUTABLE_MESSAGE__SUB_SIG_DATA_SIGNATURE_DATA;
    msg.signature_data = &sig_data;
    msg.uuid.data = uuid;
    msg.uuid.len = sizeof(uuid);
    msg.flags = flags;

    size_t packed_size = universal_message__routable_message__get_packed_size(&msg);
    if (packed_size == 0 || packed_size > buf_size) {
        return 0;
    }
    return (uint32_t)universal_message__routable_message__pack(&msg, buf);
}

/**
 * @brief 构建未认证的 VCSEC 可路由消息（protobuf-c）
 * @param buf 输出缓冲区
 * @param buf_size 缓冲区大小
 * @param domain 域
 * @param payload 负载
 * @param payload_len 负载长度
 * @param routing_address 路由地址
 * @return 消息长度
 */
uint32_t tesla_build_vcsec_message_unauthenticated(
    uint8_t *buf,
    uint32_t buf_size,
    tesla_domain_t domain,
    const uint8_t *payload,
    uint32_t payload_len,
    const uint8_t *routing_address)
{
    uint8_t uuid[16];
    esp_fill_random(uuid, sizeof(uuid));

    UniversalMessage__Destination to_dest = UNIVERSAL_MESSAGE__DESTINATION__INIT;
    to_dest.sub_destination_case = UNIVERSAL_MESSAGE__DESTINATION__SUB_DESTINATION_DOMAIN;
    to_dest.domain = (UniversalMessage__Domain)domain;

    UniversalMessage__Destination from_dest = UNIVERSAL_MESSAGE__DESTINATION__INIT;
    from_dest.sub_destination_case = UNIVERSAL_MESSAGE__DESTINATION__SUB_DESTINATION_ROUTING_ADDRESS;
    from_dest.routing_address.data = (uint8_t *)routing_address;
    from_dest.routing_address.len = 16;

    UniversalMessage__RoutableMessage msg = UNIVERSAL_MESSAGE__ROUTABLE_MESSAGE__INIT;
    msg.to_destination = &to_dest;
    msg.from_destination = &from_dest;
    msg.payload_case = UNIVERSAL_MESSAGE__ROUTABLE_MESSAGE__PAYLOAD_PROTOBUF_MESSAGE_AS_BYTES;
    msg.protobuf_message_as_bytes.data = (uint8_t *)payload;
    msg.protobuf_message_as_bytes.len = payload_len;
    msg.uuid.data = uuid;
    msg.uuid.len = sizeof(uuid);

    size_t packed_size = universal_message__routable_message__get_packed_size(&msg);
    if (packed_size == 0 || packed_size > buf_size) {
        return 0;
    }
    return (uint32_t)universal_message__routable_message__pack(&msg, buf);
}

// ========== 消息解析（protobuf-c）==========

/**
 * @brief 解析会话信息响应
 * @param data 响应数据
 * @param len 数据长度
 * @param session 输出会话信息
 * @return true 成功，false 失败
 */
bool tesla_parse_session_info(
    const uint8_t *data,
    uint32_t len,
    tesla_session_t *session)
{
    if (len < 2) return false;

    UniversalMessage__RoutableMessage *rmsg = universal_message__routable_message__unpack(NULL, len, data);
    if (!rmsg) return false;

    bool ok = false;
    if (rmsg->payload_case == UNIVERSAL_MESSAGE__ROUTABLE_MESSAGE__PAYLOAD_SESSION_INFO &&
        rmsg->session_info.data && rmsg->session_info.len > 0) {
        Signatures__SessionInfo *info = signatures__session_info__unpack(NULL,
            rmsg->session_info.len, rmsg->session_info.data);
        if (info) {
            session->counter = info->counter;
            session->clock_time = info->clock_time;
            if (info->publickey.len <= 65) {
                memcpy(session->vehicle_public_key, info->publickey.data, info->publickey.len);
            }
            if (info->epoch.len <= 16) {
                memcpy(session->epoch, info->epoch.data, info->epoch.len);
            }
            signatures__session_info__free_unpacked(info, NULL);
            ok = true;
        }
    }
    universal_message__routable_message__free_unpacked(rmsg, NULL);
    return ok;
}

bool tesla_parse_vehicle_status(
    const uint8_t *data,
    uint32_t len,
    tesla_body_state_t *state)
{
    if (len < 2) return false;

    UniversalMessage__RoutableMessage *rmsg = universal_message__routable_message__unpack(NULL, len, data);
    if (!rmsg || rmsg->payload_case != UNIVERSAL_MESSAGE__ROUTABLE_MESSAGE__PAYLOAD_PROTOBUF_MESSAGE_AS_BYTES ||
        !rmsg->protobuf_message_as_bytes.data) {
        if (rmsg) universal_message__routable_message__free_unpacked(rmsg, NULL);
        return false;
    }

    VCSEC__FromVCSECMessage *from_vcsec = vcsec__from_vcsecmessage__unpack(NULL,
        rmsg->protobuf_message_as_bytes.len, rmsg->protobuf_message_as_bytes.data);
    universal_message__routable_message__free_unpacked(rmsg, NULL);
    if (!from_vcsec || from_vcsec->sub_message_case != VCSEC__FROM_VCSECMESSAGE__SUB_MESSAGE_VEHICLE_STATUS ||
        !from_vcsec->vehiclestatus) {
        if (from_vcsec) vcsec__from_vcsecmessage__free_unpacked(from_vcsec, NULL);
        return false;
    }

    VCSEC__VehicleStatus *vs = from_vcsec->vehiclestatus;
    state->lock_state = (tesla_lock_state_t)vs->vehiclelockstate;
    state->sleep_status = (tesla_sleep_state_t)vs->vehiclesleepstatus;
    state->user_present = (vs->userpresence == VCSEC__USER_PRESENCE__E__VEHICLE_USER_PRESENCE_PRESENT);
    if (vs->closurestatuses) {
        state->closures.front_driver_door = (tesla_closure_state_t)vs->closurestatuses->frontdriverdoor;
        state->closures.front_passenger_door = (tesla_closure_state_t)vs->closurestatuses->frontpassengerdoor;
        state->closures.rear_driver_door = (tesla_closure_state_t)vs->closurestatuses->reardriverdoor;
        state->closures.rear_passenger_door = (tesla_closure_state_t)vs->closurestatuses->rearpassengerdoor;
        state->closures.rear_trunk = (tesla_closure_state_t)vs->closurestatuses->reartrunk;
        state->closures.front_trunk = (tesla_closure_state_t)vs->closurestatuses->fronttrunk;
        state->closures.charge_port = (tesla_closure_state_t)vs->closurestatuses->chargeport;
        state->closures.tonneau = (tesla_closure_state_t)vs->closurestatuses->tonneau;
    }
    vcsec__from_vcsecmessage__free_unpacked(from_vcsec, NULL);
    return true;
}

/**
 * 解析并解密加密的 RoutableMessage 响应（protobuf-c）
 * @param data 响应数据
 * @param len 响应数据长度
 * @param session 会话
 * @param vin 车辆 VIN
 * @param request_id 请求 ID
 * @param plaintext 解密后的负载
 * @param plaintext_len 解密后的负载长度
 * @param fault 故障码
 * @return true 成功，false 失败
 */
bool tesla_parse_and_decrypt_response(
    const uint8_t *data,
    uint32_t len,
    const tesla_session_t *session,
    const char *vin,
    const uint8_t *request_id,
    uint8_t *plaintext,
    uint32_t *plaintext_len,
    tesla_fault_t *fault)
{
    *fault = TESLA_FAULT_NONE;
    *plaintext_len = 0;
    if (len < 2) return false;

    UniversalMessage__RoutableMessage *rmsg = universal_message__routable_message__unpack(NULL, len, data);
    if (!rmsg) return false;

    uint32_t from_domain = TESLA_DOMAIN_INFOTAINMENT;
    if (rmsg->from_destination && rmsg->from_destination->sub_destination_case ==
        UNIVERSAL_MESSAGE__DESTINATION__SUB_DESTINATION_DOMAIN) {
        from_domain = rmsg->from_destination->domain;
    }

    if (rmsg->payload_case != UNIVERSAL_MESSAGE__ROUTABLE_MESSAGE__PAYLOAD_PROTOBUF_MESSAGE_AS_BYTES ||
        !rmsg->protobuf_message_as_bytes.data || rmsg->protobuf_message_as_bytes.len == 0) {
        if (rmsg->signedmessagestatus) {
            *fault = (tesla_fault_t)rmsg->signedmessagestatus->signed_message_fault;
        }
        universal_message__routable_message__free_unpacked(rmsg, NULL);
        return *fault != TESLA_FAULT_NONE;
    }

    if (rmsg->signedmessagestatus) {
        *fault = (tesla_fault_t)rmsg->signedmessagestatus->signed_message_fault;
    }

    uint32_t counter = 0;
    uint8_t nonce[12] = {0};
    uint8_t tag[16] = {0};
    bool have_gcm = false;
    if (rmsg->sub_sig_data_case == UNIVERSAL_MESSAGE__ROUTABLE_MESSAGE__SUB_SIG_DATA_SIGNATURE_DATA &&
        rmsg->signature_data &&
        rmsg->signature_data->sig_type_case == SIGNATURES__SIGNATURE_DATA__SIG_TYPE_AES__GCM__RESPONSE_DATA &&
        rmsg->signature_data->aes_gcm_response_data) {
        Signatures__AESGCMResponseSignatureData *gcm = rmsg->signature_data->aes_gcm_response_data;
        counter = gcm->counter;
        if (gcm->nonce.len == 12) {
            memcpy(nonce, gcm->nonce.data, 12);
        }
        if (gcm->tag.len == 16) {
            memcpy(tag, gcm->tag.data, 16);
        }
        have_gcm = (gcm->nonce.len == 12 && gcm->tag.len == 16);
    }

    uint32_t ciphertext_len = (uint32_t)rmsg->protobuf_message_as_bytes.len;
    uint32_t response_flags = rmsg->flags;
    uint8_t ciphertext_buf[512];
    if (ciphertext_len > sizeof(ciphertext_buf)) {
        universal_message__routable_message__free_unpacked(rmsg, NULL);
        return false;
    }
    memcpy(ciphertext_buf, rmsg->protobuf_message_as_bytes.data, ciphertext_len);
    universal_message__routable_message__free_unpacked(rmsg, NULL);

    if (!have_gcm) {
        ESP_LOGW(TAG, "Missing nonce or tag for decryption");
        return *fault != TESLA_FAULT_NONE;
    }

    uint8_t metadata[128];
    uint32_t metadata_len = tesla_build_response_metadata(metadata,
        (tesla_domain_t)from_domain, vin, counter, response_flags,
        request_id, request_id ? 17 : 0, (uint32_t)*fault);

    uint8_t aad_hash[32];
    tesla_crypto_sha256(metadata, metadata_len, aad_hash);

    if (!tesla_crypto_decrypt_gcm(session->session_key, nonce, aad_hash, 32,
                                   ciphertext_buf, ciphertext_len, tag, plaintext)) {
        ESP_LOGE(TAG, "Decryption failed");
        return false;
    }
    *plaintext_len = ciphertext_len;
    return true;
}

/**
 * @brief 从 CarServer 响应中解析 DriveState（使用 protobuf-c 生成的 CarServer__Response）
 * @param data 响应数据
 * @param len 响应数据长度
 * @param state 输出驾驶状态
 * @return true 成功，false 失败
 */
bool tesla_parse_drive_state(
    const uint8_t *data,
    uint32_t len,
    tesla_drive_state_t *state)
{
    if (!data || len < 2 || !state) {
        return false;
    }

    ESP_LOGD(TAG, "Parsing DriveState response (%lu bytes)", (unsigned long)len);
    memset(state, 0, sizeof(tesla_drive_state_t));

    CarServer__Response *resp = car_server__response__unpack(
        &s_default_allocator, (size_t)len, data);
    if (!resp || !resp->vehicledata || !resp->vehicledata->drive_state) {
        if (resp) {
            car_server__response__free_unpacked(resp, &s_default_allocator);
        }
        ESP_LOGW(TAG, "DriveState not found in response");
        return false;
    }

    CarServer__DriveState *ds = resp->vehicledata->drive_state;

    /* 档位：ShiftState oneof 映射到 tesla_gear_state_t */
    switch (ds->shift_state ? ds->shift_state->type_case : CAR_SERVER__SHIFT_STATE__TYPE__NOT_SET) {
    case CAR_SERVER__SHIFT_STATE__TYPE_INVALID:
    case CAR_SERVER__SHIFT_STATE__TYPE__NOT_SET:
        state->gear = TESLA_GEAR_INVALID;
        break;
    case CAR_SERVER__SHIFT_STATE__TYPE_P:
        state->gear = TESLA_GEAR_P;
        break;
    case CAR_SERVER__SHIFT_STATE__TYPE_R:
        state->gear = TESLA_GEAR_R;
        break;
    case CAR_SERVER__SHIFT_STATE__TYPE_N:
        state->gear = TESLA_GEAR_N;
        break;
    case CAR_SERVER__SHIFT_STATE__TYPE_D:
        state->gear = TESLA_GEAR_D;
        break;
    case CAR_SERVER__SHIFT_STATE__TYPE_SNA:
        state->gear = TESLA_GEAR_SNA;
        break;
    default:
        state->gear = TESLA_GEAR_INVALID;
        break;
    }

    state->speed_kmh = (ds->speed_float != 0.0f) ? ds->speed_float : (float)ds->speed;
    state->power_kw = ds->power;
    state->odometer_km = (float)ds->odometer_in_hundredths_of_a_mile / 100.0f * 1.60934f;

    ESP_LOGD(TAG, "DriveState: gear=%d speed=%.1f power=%ld odometer=%.1f",
             (int)state->gear, (double)state->speed_kmh, (long)state->power_kw, (double)state->odometer_km);

    car_server__response__free_unpacked(resp, &s_default_allocator);
    return true;
}

/**
 * @brief 解析 FromVCSECMessage 响应用于调试（使用 protobuf-c 生成的 VCSEC__FromVCSECMessage）
 * @param data 响应数据
 * @param len 响应数据长度
 */
void tesla_parse_vcsec_response(const uint8_t *data, uint32_t len)
{
    if (len < 2) {
        ESP_LOGW(TAG, "VCSEC response too short: %lu bytes", (unsigned long)len);
        return;
    }

    ESP_LOGI(TAG, "=== Parsing VCSEC Response (%lu bytes) ===", (unsigned long)len);
    ESP_LOG_BUFFER_HEX_LEVEL(TAG, data, len > 64 ? 64 : len, ESP_LOG_DEBUG);

    VCSEC__FromVCSECMessage *msg = vcsec__from_vcsecmessage__unpack(
        &s_default_allocator, (size_t)len, data);
    if (!msg) {
        ESP_LOGW(TAG, "VCSEC response unpack failed");
        return;
    }

    switch (msg->sub_message_case) {
    case VCSEC__FROM_VCSECMESSAGE__SUB_MESSAGE_VEHICLE_STATUS:
        ESP_LOGI(TAG, "vehicleStatus found");
        break;

    case VCSEC__FROM_VCSECMESSAGE__SUB_MESSAGE_COMMAND_STATUS: {
        VCSEC__CommandStatus *cs = msg->commandstatus;
        const char *op_names[] = {"OK", "WAIT", "ERROR"};
        unsigned op = (unsigned)cs->operationstatus;
        ESP_LOGI(TAG, "CommandStatus: operationStatus = %u (%s)",
                 op, op <= 2 ? op_names[op] : "UNKNOWN");

        if (cs->sub_message_case == VCSEC__COMMAND_STATUS__SUB_MESSAGE_WHITELIST_OPERATION_STATUS) {
            VCSEC__WhitelistOperationStatus *ws = cs->whitelistoperationstatus;
            ESP_LOGI(TAG, "  whitelistOperationStatus: info=%lu signer=%lu op=%lu",
                     (unsigned long)ws->whitelistoperationinformation,
                     (unsigned long)ws->signerofoperation,
                     (unsigned long)ws->operationforkeyinwhitelist);
        } else if (cs->sub_message_case == VCSEC__COMMAND_STATUS__SUB_MESSAGE_SIGNED_MESSAGE_STATUS) {
            VCSEC__SignedMessageStatus *sms = cs->signedmessagestatus;
            ESP_LOGI(TAG, "  signedMessageStatus: counter=%lu fault=%lu",
                     (unsigned long)sms->counter, (unsigned long)sms->signedmessagefault);
        }
        break;
    }

    case VCSEC__FROM_VCSECMESSAGE__SUB_MESSAGE_NOMINAL_ERROR:
        ESP_LOGI(TAG, "nominalError: genericError = %d", (int)msg->nominalerror->genericerror);
        break;

    case VCSEC__FROM_VCSECMESSAGE__SUB_MESSAGE__NOT_SET:
    default:
        ESP_LOGI(TAG, "FromVCSECMessage: no sub_message set");
        break;
    }

    vcsec__from_vcsecmessage__free_unpacked(msg, &s_default_allocator);
    ESP_LOGI(TAG, "=== End VCSEC Response ===");
}
