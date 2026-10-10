/**
 * @file tesla_protocol.h
 * @brief Tesla Protocol Message Handling
 *
 * 使用 protobuf-c 处理与 vehicle-command Go SDK 一致的 Protocol Buffer 消息编解码。
 */

#ifndef TESLA_PROTOCOL_H
#define TESLA_PROTOCOL_H

#include <stdint.h>
#include <stdbool.h>
#include "tesla_ble.h"
#include "tesla_transport.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    tesla_domain_t domain;
    uint8_t uuid[16];
    uint8_t address[16];
    uint8_t request_id[17];
    bool authenticated;
    tesla_replay_window_t replay;
} tesla_request_t;

bool tesla_request_capture(const uint8_t *wire, size_t len, tesla_request_t *request);
bool tesla_response_matches(const uint8_t *wire, size_t len, const tesla_request_t *request);

// ========== 协议标签（来自 signatures.proto）==========

typedef enum {
    TESLA_TAG_SIGNATURE_TYPE = 0,     // 签名类型
    TESLA_TAG_DOMAIN = 1,              // 域
    TESLA_TAG_PERSONALIZATION = 2,     // 个性化（VIN）
    TESLA_TAG_EPOCH = 3,               // 时期
    TESLA_TAG_EXPIRES_AT = 4,          // 过期时间
    TESLA_TAG_COUNTER = 5,             // 计数器
    TESLA_TAG_CHALLENGE = 6,           // 挑战
    TESLA_TAG_FLAGS = 7,               // 标志
    TESLA_TAG_REQUEST_HASH = 8,        // 请求哈希
    TESLA_TAG_FAULT = 9,               // 故障
    TESLA_TAG_END = 255                // 结束标记
} tesla_tag_t;

// ========== 签名类型 ==========

typedef enum {
    TESLA_SIG_AES_GCM = 0,                  // AES-GCM 加密
    TESLA_SIG_AES_GCM_PERSONALIZED = 5,     // 个性化 AES-GCM 加密
    TESLA_SIG_HMAC = 6,                     // HMAC 签名
    TESLA_SIG_HMAC_PERSONALIZED = 8,        // 个性化 HMAC 签名
    TESLA_SIG_AES_GCM_RESPONSE = 9          // AES-GCM 响应
} tesla_signature_type_t;

// ========== 消息标志 ==========

typedef enum {
    TESLA_FLAG_USER_COMMAND = 0,         // 用户命令
    TESLA_FLAG_ENCRYPT_RESPONSE = 1      // 加密响应
} tesla_message_flag_t;

// ========== 消息故障 ==========

typedef enum {
    TESLA_FAULT_NONE = 0,                    // 无故障
    TESLA_FAULT_BUSY = 1,                    // 忙碌
    TESLA_FAULT_TIMEOUT = 2,                 // 超时
    TESLA_FAULT_UNKNOWN_KEY_ID = 3,          // 未知密钥 ID
    TESLA_FAULT_INACTIVE_KEY = 4,            // 非活动密钥
    TESLA_FAULT_INVALID_SIGNATURE = 5,       // 无效签名
    TESLA_FAULT_INVALID_TOKEN = 6,           // 无效令牌
    TESLA_FAULT_INSUFFICIENT_PRIVILEGES = 7, // 权限不足
    TESLA_FAULT_INVALID_DOMAINS = 8,         // 无效域
    TESLA_FAULT_INVALID_COMMAND = 9,         // 无效命令
    TESLA_FAULT_DECODING = 10,               // 解码错误
    TESLA_FAULT_INTERNAL = 11,               // 内部错误
    TESLA_FAULT_WRONG_VIN = 12,              // 错误的 VIN
    TESLA_FAULT_BAD_PARAMETER = 13,          // 错误的参数
    TESLA_FAULT_KEYCHAIN_FULL = 14,          // 密钥链已满
    TESLA_FAULT_INCORRECT_EPOCH = 15,
    TESLA_FAULT_IV_LENGTH = 16,
    TESLA_FAULT_TIME_EXPIRED = 17,
    TESLA_FAULT_NOT_PROVISIONED = 18,
    TESLA_FAULT_METADATA = 19,
    TESLA_FAULT_TTL_TOO_LONG = 20,
    TESLA_FAULT_REMOTE_ACCESS_DISABLED = 21,
    TESLA_FAULT_SERVICE_ACCESS_DISABLED = 22,
    TESLA_FAULT_ACCOUNT_REQUIRED = 23,
    TESLA_FAULT_REQUEST_MTU = 24,
    TESLA_FAULT_RESPONSE_MTU = 25,
    TESLA_FAULT_REPEATED_COUNTER = 26,
    TESLA_FAULT_INVALID_KEY_HANDLE = 27,
    TESLA_FAULT_RESPONSE_ENCRYPTION_REQUIRED = 28
} tesla_fault_t;
bool tesla_fault_retryable(tesla_fault_t fault);
typedef enum {
    TESLA_REPLY_INVALID, TESLA_REPLY_OK, TESLA_REPLY_FAULT,
    TESLA_REPLY_SESSION, TESLA_REPLY_DUPLICATE
} tesla_reply_result_t;
tesla_reply_result_t tesla_decode_response(const uint8_t *wire, size_t len,
    const tesla_session_t *session, const char *vin, tesla_request_t *request,
    uint8_t *payload, size_t capacity, uint32_t *payload_len, tesla_fault_t *fault);
uint32_t tesla_build_response_metadata(uint8_t *buf, tesla_domain_t domain,
    const char *vin, uint32_t counter, uint32_t flags, const uint8_t *request_id,
    uint32_t request_id_len, uint32_t fault);

// ========== VCSEC 消息类型 ==========

typedef enum {
    TESLA_INFO_GET_STATUS = 0,            // 获取状态
    TESLA_INFO_GET_WHITELIST = 5,         // 获取白名单
    TESLA_INFO_GET_WHITELIST_ENTRY = 6    // 获取白名单条目
} tesla_info_request_type_t;

typedef enum {
    TESLA_RKE_UNLOCK = 0,          // 解锁
    TESLA_RKE_LOCK = 1,            // 锁定
    TESLA_RKE_REMOTE_DRIVE = 20,   // 远程驾驶
    TESLA_RKE_AUTO_SECURE = 29,    // 自动安全
    TESLA_RKE_WAKE = 30            // 唤醒
} tesla_rke_action_t;

typedef enum {
    TESLA_CLOSURE_MOVE_NONE = 0,   // 无动作
    TESLA_CLOSURE_MOVE_MOVE = 1,   // 移动
    TESLA_CLOSURE_MOVE_STOP = 2,   // 停止
    TESLA_CLOSURE_MOVE_OPEN = 3,   // 打开
    TESLA_CLOSURE_MOVE_CLOSE = 4   // 关闭
} tesla_closure_move_t;

// ========== 车载服务器分类 ==========

typedef enum {
    TESLA_STATE_DRIVE = 0,         // 驾驶状态
    TESLA_STATE_CHARGE = 1,        // 充电状态
    TESLA_STATE_CLIMATE = 2,       // 气候状态
    TESLA_STATE_CLOSURES = 3,      // 关闭状态
    TESLA_STATE_LOCATION = 4,      // 位置
    TESLA_STATE_TIRE_PRESSURE = 5, // 胎压
    TESLA_STATE_SOFTWARE = 6,      // 软件
    TESLA_STATE_MEDIA = 7          // 媒体
} tesla_state_category_t;

// ========== 密钥形态 ==========

typedef enum {
    TESLA_KEY_UNKNOWN = 0,           // 未知
    TESLA_KEY_NFC_CARD = 1,          // NFC 卡
    TESLA_KEY_IOS_DEVICE = 6,        // iOS 设备
    TESLA_KEY_ANDROID_DEVICE = 7,    // Android 设备
    TESLA_KEY_CLOUD_KEY = 9          // 云密钥
} tesla_key_form_factor_t;

// ========== 消息构建器（均使用 protobuf-c 生成代码）==========

/**
 * @brief 构建会话信息请求消息
 * @param buf 输出缓冲区
 * @param buf_size 缓冲区大小
 * @param domain 目标域
 * @param public_key 公钥（65 字节）
 * @param routing_address 路由地址（16 字节）
 * @return 消息长度，0 表示失败
 */
uint32_t tesla_build_session_info_request(
    uint8_t *buf,
    uint32_t buf_size,
    tesla_domain_t domain,
    const uint8_t *public_key,
    const uint8_t *routing_address);

/**
 * @brief 构建信息请求（VCSEC）消息
 * @param buf 输出缓冲区
 * @param buf_size 缓冲区大小
 * @param type 请求类型
 * @return 消息长度，0 表示失败
 */
uint32_t tesla_build_info_request(
    uint8_t *buf,
    uint32_t buf_size,
    tesla_info_request_type_t type);

/**
 * @brief 构建 RKE 动作消息
 * @param buf 输出缓冲区
 * @param buf_size 缓冲区大小
 * @param action 动作类型
 * @return 消息长度，0 表示失败
 */
uint32_t tesla_build_rke_action(
    uint8_t *buf,
    uint32_t buf_size,
    tesla_rke_action_t action);

/**
 * @brief 构建添加密钥请求消息
 * @param buf 输出缓冲区
 * @param buf_size 缓冲区大小
 * @param public_key 公钥（65 字节）
 * @param form_factor 密钥形态
 * @return 消息长度，0 表示失败
 */
uint32_t tesla_build_add_key_request(
    uint8_t *buf,
    uint32_t buf_size,
    const uint8_t *public_key,
    tesla_key_form_factor_t form_factor);

/**
 * @brief 构建获取驾驶状态请求（CarServer Action）
 * @param buf 输出缓冲区
 * @param buf_size 缓冲区大小
 * @return 消息长度，0 表示失败
 * 
 * 构建 CarServer Action 消息以从信息娱乐系统获取驾驶状态
 */
uint32_t tesla_build_get_drive_state(
    uint8_t *buf,
    uint32_t buf_size);

/**
 * @brief 构建已认证的可路由消息
 * @param buf 输出缓冲区
 * @param buf_size 缓冲区大小
 * @param domain 目标域
 * @param payload 负载数据
 * @param payload_len 负载长度
 * @param session 会话信息
 * @param routing_address 路由地址
 * @param public_key 公钥
 * @param vin 车辆识别码
 * @param flags 标志
 * @param request_id_out 输出请求 ID（17 字节：签名类型 + GCM 标签），可为 NULL
 * @return 消息长度，0 表示失败
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
    uint8_t *request_id_out);

/**
 * @brief 构建未认证的 VCSEC 消息（用于 InformationRequest）
 * @param buf 输出缓冲区
 * @param buf_size 缓冲区大小
 * @param domain 域
 * @param payload 负载数据
 * @param payload_len 负载长度
 * @param routing_address 路由地址
 * @return 消息长度，0 表示失败
 */
uint32_t tesla_build_vcsec_message_unauthenticated(
    uint8_t *buf,
    uint32_t buf_size,
    tesla_domain_t domain,
    const uint8_t *payload,
    uint32_t payload_len,
    const uint8_t *routing_address);

// ========== 消息解析器 ==========

typedef enum {
    TESLA_SESSION_INFO_INVALID = 0,
    TESLA_SESSION_INFO_OK,
    TESLA_SESSION_INFO_KEY_NOT_ON_WHITELIST,
} tesla_session_info_result_t;

/* Verify HMAC before installing or refreshing any session state. */
tesla_session_info_result_t tesla_verify_session_info(
    const uint8_t *wire, size_t len, const uint8_t *private_key, const char *vin,
    const tesla_request_t *request, uint32_t local_sec, tesla_session_t *session);
bool tesla_crypto_hmac_sha256(const uint8_t *key, size_t key_len,
    const uint8_t *data, size_t len, uint8_t output[32]);

/**
 * @brief 结构解码会话信息；本函数不认证，业务必须调用 tesla_verify_session_info
 * @param data 响应数据
 * @param len 数据长度
 * @param session 输出会话信息
 * @return 解析结果，区分有效会话、无效响应与车辆拒绝钥匙
 */
tesla_session_info_result_t tesla_parse_session_info(
    const uint8_t *data,
    uint32_t len,
    tesla_session_t *session);

/**
 * @brief 解析车辆状态（VCSEC）响应
 * @param data 响应数据
 * @param len 数据长度
 * @param state 输出车身状态
 * @return true 成功，false 失败
 */
bool tesla_parse_vehicle_status(
    const uint8_t *data,
    uint32_t len,
    tesla_body_state_t *state);

bool tesla_parse_body_payload(const uint8_t *data, uint32_t len, tesla_body_state_t *state);

/**
 * @brief 从 CarServer 响应解析驾驶状态
 * @param data 响应数据
 * @param len 数据长度
 * @param state 输出驾驶状态
 * @return true 成功，false 失败
 */
bool tesla_parse_drive_state(
    const uint8_t *data,
    uint32_t len,
    tesla_drive_state_t *state);

/**
 * @brief 解析 VCSEC 响应（用于调试）
 * @param data 响应数据
 * @param len 数据长度
 */
void tesla_parse_vcsec_response(const uint8_t *data, uint32_t len);

// ========== 加密函数 ==========

/**
 * @brief 生成 ECDSA P-256 密钥对
 * @param private_key 输出私钥（32 字节）
 * @param public_key 输出公钥（65 字节）
 * @return true 成功，false 失败
 */
bool tesla_crypto_generate_keypair(uint8_t *private_key, uint8_t *public_key);

/**
 * @brief 从私钥派生公钥
 * @param private_key 私钥（32 字节）
 * @param public_key 输出公钥（65 字节）
 * @return true 成功，false 失败
 */
bool tesla_crypto_derive_public_key(const uint8_t *private_key, uint8_t *public_key);

/**
 * @brief 计算 ECDH 共享密钥
 * @param private_key 我们的私钥（32 字节）
 * @param peer_public_key 对等方公钥（65 字节）
 * @param shared_secret 输出共享密钥（32 字节）
 * @return true 成功，false 失败
 */
bool tesla_crypto_ecdh(
    const uint8_t *private_key,
    const uint8_t *peer_public_key,
    uint8_t *shared_secret);

/**
 * @brief 从共享密钥派生会话密钥
 * @param shared_secret 共享密钥（32 字节）
 * @param session_key 输出会话密钥（16 字节）
 */
void tesla_crypto_derive_key(
    const uint8_t *shared_secret,
    uint8_t *session_key);

/**
 * @brief 使用 AES-GCM 加密消息
 * @param key 密钥（16 字节）
 * @param nonce 随机数（12 字节）
 * @param aad 附加认证数据
 * @param aad_len 附加认证数据长度
 * @param plaintext 明文
 * @param plaintext_len 明文长度
 * @param ciphertext 输出密文
 * @param tag 输出认证标签（16 字节）
 * @return true 成功，false 失败
 */
bool tesla_crypto_encrypt_gcm(
    const uint8_t *key,
    const uint8_t *nonce,
    const uint8_t *aad,
    uint32_t aad_len,
    const uint8_t *plaintext,
    uint32_t plaintext_len,
    uint8_t *ciphertext,
    uint8_t *tag);

/**
 * @brief 使用 AES-GCM 解密消息
 * @param key 密钥（16 字节）
 * @param nonce 随机数（12 字节）
 * @param aad 附加认证数据
 * @param aad_len 附加认证数据长度
 * @param ciphertext 密文
 * @param ciphertext_len 密文长度
 * @param tag 认证标签（16 字节）
 * @param plaintext 输出明文
 * @return true 成功，false 失败
 */
bool tesla_crypto_decrypt_gcm(
    const uint8_t *key,
    const uint8_t *nonce,
    const uint8_t *aad,
    uint32_t aad_len,
    const uint8_t *ciphertext,
    uint32_t ciphertext_len,
    const uint8_t *tag,
    uint8_t *plaintext);

/**
 * @brief 计算 SHA1 哈希
 * @param data 数据
 * @param data_len 数据长度
 * @param hash 输出哈希（20 字节）
 */
void tesla_crypto_sha1(
    const uint8_t *data,
    uint32_t data_len,
    uint8_t *hash);

/**
 * @brief 计算 SHA256 哈希
 * @param data 数据
 * @param data_len 数据长度
 * @param hash 输出哈希（32 字节）
 */
void tesla_crypto_sha256(
    const uint8_t *data,
    uint32_t data_len,
    uint8_t *hash);

/**
 * @brief 构建签名元数据
 * @param buf 输出缓冲区
 * @param sig_type 签名类型
 * @param domain 域
 * @param vin 车辆 VIN
 * @param epoch 时期（16 字节）
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
    uint32_t flags);

#ifdef __cplusplus
}
#endif

#endif // TESLA_PROTOCOL_H
