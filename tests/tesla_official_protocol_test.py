"""Execute production C protocol code with a Python cryptographic peer.

Windows: python tests/tesla_official_protocol_test.py --cc PATH/TO/tcc.exe --idf PATH/TO/esp-idf
Uses real protobuf-c and production session/transport/parser functions. Only
RTOS, randomness and crypto backends are substituted; Python cryptography is
the independent P-256/AES-GCM oracle. The control exchange is extracted unchanged.
"""
import argparse
import ctypes as ct
import hashlib
import hmac
import os
import pathlib
import re
import struct
import subprocess
import tempfile
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.ciphers.aead import AESGCM

parser = argparse.ArgumentParser()
parser.add_argument('--cc', required=True)
parser.add_argument('--idf', required=True)
parser.add_argument('--clang', help='Optional clang for C compilation; tcc still links the Windows DLL')
args = parser.parse_args()
root = pathlib.Path(__file__).resolve().parents[1]
component = root / 'components/tesla_ble'
runtime = pathlib.Path(args.idf) / 'components/protobuf-c/protobuf-c/protobuf-c'

def varint(value):
    out = bytearray()
    while value > 127:
        out.append((value & 127) | 128); value >>= 7
    return bytes(out + bytes([value]))

def num(field, value): return varint(field << 3) + varint(value)
def blob(field, value): return varint((field << 3) | 2) + varint(len(value)) + value
def fixed(field, value): return varint((field << 3) | 5) + struct.pack('<I', value)

def fields(wire):
    pos, result = 0, {}
    def readvar():
        nonlocal pos
        value, shift = 0, 0
        while True:
            b = wire[pos]; pos += 1; value |= (b & 127) << shift
            if b < 128: return value
            shift += 7
    while pos < len(wire):
        tag = readvar(); kind = tag & 7
        if kind == 0: value = readvar()
        elif kind == 2:
            size = readvar(); value = wire[pos:pos+size]; pos += size
        elif kind == 5: value = wire[pos:pos+4]; pos += 4
        else: raise AssertionError(kind)
        result[tag >> 3] = value
    return result

bridge = r'''
#include "tesla_protocol.h"
#include "universal_message.pb-c.h"
#include "vcsec.pb-c.h"
#include <string.h>
#include <stdio.h>
typedef int (*crypto_fn)(int,const uint8_t*,uint32_t,const uint8_t*,uint32_t,
    const uint8_t*,uint32_t,const uint8_t*,uint32_t,uint8_t*,uint8_t*);
static crypto_fn oracle;
void host_register(crypto_fn cb) { oracle = cb; }
int host_layout(void) {
    UniversalMessage__RoutableMessage m=UNIVERSAL_MESSAGE__ROUTABLE_MESSAGE__INIT;
    return m.base.descriptor==&universal_message__routable_message__descriptor;
}
void esp_fill_random(void *p, size_t n) { oracle(0,NULL,0,NULL,0,NULL,0,NULL,n,p,NULL); }
bool tesla_crypto_hmac_sha256(const uint8_t *k,size_t kl,const uint8_t *d,size_t n,uint8_t *o)
{ return oracle(1,k,kl,d,n,NULL,0,NULL,0,o,NULL); }
void tesla_crypto_sha256(const uint8_t *d,uint32_t n,uint8_t *o)
{ oracle(2,NULL,0,d,n,NULL,0,NULL,0,o,NULL); }
void tesla_crypto_sha1(const uint8_t *d,uint32_t n,uint8_t *o)
{ oracle(3,NULL,0,d,n,NULL,0,NULL,0,o,NULL); }
bool tesla_crypto_ecdh(const uint8_t *k,const uint8_t *pub,uint8_t *o)
{ return oracle(4,k,32,pub,65,NULL,0,NULL,0,o,NULL); }
void tesla_crypto_derive_key(const uint8_t *d,uint8_t *o)
{ uint8_t h[20]; tesla_crypto_sha1(d,32,h); memcpy(o,h,16); }
bool tesla_crypto_encrypt_gcm(const uint8_t *k,const uint8_t *n,const uint8_t *aad,uint32_t al,
    const uint8_t *p,uint32_t pl,uint8_t *o,uint8_t *tag)
{ return oracle(5,k,16,n,12,aad,al,p,pl,o,tag); }
bool tesla_crypto_decrypt_gcm(const uint8_t *k,const uint8_t *n,const uint8_t *aad,uint32_t al,
    const uint8_t *p,uint32_t pl,const uint8_t *tag,uint8_t *o)
{ return oracle(6,k,16,n,12,aad,al,p,pl,o,(uint8_t*)tag); }
static uint32_t tick;
uint32_t xTaskGetTickCount(void) { return tick; }
static tesla_connection_t g_conn;
static tesla_request_t g_request;
static uint8_t g_payload[1024];
static bool g_operations_cancelled;
static tesla_stream_t stream;
void host_init(const uint8_t *pub) {
    memset(&g_conn,0,sizeof(g_conn)); memset(&g_request,0,sizeof(g_request));
    memcpy(g_conn.public_key,pub,65); g_conn.private_key[31]=1;
    strcpy(g_conn.vin,"5YJ30123456789ABC"); memset(g_conn.routing_address,0x23,16);
    tesla_stream_reset(&stream); tick=0; g_operations_cancelled=false;
}
int host_handshake(int domain,uint8_t *out) {
    int n=tesla_build_session_info_request(out,1024,domain,g_conn.public_key,g_conn.routing_address);
    tesla_request_capture(out,n,&g_request); return n;
}
int host_verify(const uint8_t *wire,int n) {
    return tesla_verify_session_info(wire,n,g_conn.private_key,g_conn.vin,&g_request,
        tick/1000,&g_conn.session[g_request.domain==3]);
}
int host_counter(void) { return g_conn.session[g_request.domain==3].counter; }
void host_set_counter(int domain,uint32_t value) {g_conn.session[domain==3].counter=value;}
int host_established(void) { return g_conn.session[g_request.domain==3].established; }
int host_clock(void) { return g_conn.session[g_request.domain==3].clock_time; }
int host_command(int domain,uint8_t *out) {
    uint8_t payload[64]; uint32_t size=tesla_build_rke_action(payload,64,TESLA_RKE_LOCK);
    tesla_session_t *s=&g_conn.session[domain==3]; ++s->counter;
    int n=tesla_build_routable_message(out,1024,domain,payload,size,s,g_conn.routing_address,
        g_conn.public_key,g_conn.vin,2,NULL); tesla_request_capture(out,n,&g_request); return n;
}
int host_decode(const uint8_t *wire,int n,uint8_t *out) {
    uint32_t len; tesla_fault_t fault;
    return tesla_decode_response(wire,n,&g_conn.session[g_request.domain==3],g_conn.vin,
        &g_request,out,1024,&len,&fault);
}
int host_matches(const uint8_t *wire,int n) { return tesla_response_matches(wire,n,&g_request); }
int host_metadata(uint8_t *out) {
    static const uint8_t e[16]={0x4c,0x46,0x3f,0x9c,0xc0,0xd3,0xd2,0x69,0x06,0xe9,0x82,0xed,0x22,0x4a,0xdd,0xe6};
    return tesla_build_metadata(out,5,3,g_conn.vin,e,2655,7,2);
}
int host_drive(const uint8_t *wire,int n,float *speed) {
    tesla_drive_state_t state; bool ok=tesla_parse_drive_state(wire,n,&state);
    if(ok) *speed=state.speed_kmh; return ok;
}
int host_feed(const uint8_t *wire,int n,uint32_t now) {return tesla_stream_feed(&stream,wire,n,now);}
int host_pop(uint8_t *out) {uint16_t n;return tesla_stream_pop(&stream,out,&n)?n:0;}
static unsigned sends, scenario, responses;
static void clear_pending_notifications(uint32_t unused) {(void)unused;}
static int start_session(tesla_domain_t d) {(void)d;return 0;}
static int send_message(const uint8_t *wire,uint16_t n) {++sends;tesla_request_capture(wire,n,&g_request);return 0;}
static void vTaskDelay(uint32_t ms) {tick+=ms;}
static int update_session(tesla_session_t *s) {(void)s;return TESLA_SESSION_INFO_INVALID;}
static bool wait_for_response(uint32_t ms) {
    tick+=ms;
    if (scenario==1) g_operations_cancelled=true;
    if (scenario<2 || responses++) return false;
    /* Wire fixtures independently encoded with official field numbers. */
    uint8_t *p=g_conn.rx_buffer;
    *p++=0x32;*p++=18;*p++=0x12;*p++=16;memcpy(p,g_request.address,16);p+=16;
    *p++=0x3a;*p++=2;*p++=8;*p++=g_request.domain;
    *p++=0x92;*p++=3;*p++=16;memcpy(p,g_request.uuid,16);p+=16;
    if(scenario==2 || scenario==3) {
        *p++=0x62;*p++=2;*p++=0x10;*p++=scenario==2?1:3;
    } else {
        *p++=0x52;*p++=2;*p++=scenario==4?0x22:0x0a;*p++=0;
    }
    g_conn.rx_len=p-g_conn.rx_buffer;return true;
}
'''
ble_source = (component/'tesla_ble.c').read_text(encoding='utf-8')
exchange = ble_source[ble_source.index('static esp_err_t exchange('):ble_source.index('static esp_err_t get_body_state(')]
bridge += exchange + r'''
int host_engine(int control,int plan) {
    tick=0;sends=responses=0;scenario=plan;g_operations_cancelled=false;
    for(int i=0;i<2;++i)g_conn.session[i].established=true;
    uint8_t payload[64];uint32_t n=tesla_build_rke_action(payload,64,TESLA_RKE_LOCK),reply_len;
    return exchange(control?2:3,payload,n,true,control,&reply_len);
}
int host_sends(void) {return sends;}
'''

bridge = re.sub(r'(?m)^(void|int) (host_\w+)', r'__declspec(dllexport) \1 \2', bridge)
def tcc_initializers(source):
    # tcc 0.9.27 miscompiles automatic nested protobuf aggregate initializers
    # in Windows DLLs. Use the equivalent runtime descriptor assignment only
    # in the host copy; production source and parser logic stay unchanged.
    def replace(match):
        typename, name, macro = match.groups()
        descriptor = macro.removesuffix('__INIT').lower() + '__descriptor'
        return f'{typename} {name}; memset(&{name},0,sizeof({name})); {name}.base.descriptor=&{descriptor};'
    return re.sub(r'(\w+__\w+)\s+(\w+)\s*=\s*(\w+__INIT)\s*;', replace, source)
with tempfile.TemporaryDirectory(prefix='tesla-protocol-test-', ignore_cleanup_errors=True) as directory:
    directory = pathlib.Path(directory); (directory/'freertos').mkdir()
    (directory/'esp_err.h').write_text('''typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_TIMEOUT 0x107
''')
    (directory/'esp_log.h').write_text('''#define ESP_LOGI(...) ((void)0)
#define ESP_LOGD(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define ESP_LOG_BUFFER_HEX_LEVEL(...) ((void)0)
''')
    (directory/'esp_random.h').write_text('#include <stddef.h>\nvoid esp_fill_random(void*,size_t);\n')
    (directory/'freertos/FreeRTOS.h').write_text('#include <stdint.h>\ntypedef uint32_t TickType_t;\n#define portTICK_PERIOD_MS 1\n#define pdMS_TO_TICKS(ms) (ms)\n')
    (directory/'freertos/task.h').write_text('#include "FreeRTOS.h"\nuint32_t xTaskGetTickCount(void);\n')
    (directory/'bridge.c').write_text('#include "freertos/FreeRTOS.h"\n#include "esp_log.h"\n'+tcc_initializers(bridge))
    (directory/'protocol.c').write_text(tcc_initializers((component/'tesla_protocol.c').read_text(encoding='utf-8')))
    library_path = directory/'tesla.dll'
    includes = ['-I'+str(directory),'-I'+str(component/'include'),
        '-I'+str(component/'proto-c'),'-I'+str(runtime.parent)]
    sources = [str(directory/'bridge.c'),
        str(directory/'protocol.c'),str(component/'tesla_session.c'),str(component/'tesla_transport.c'),
        str(runtime/'protobuf-c.c'),*map(str,(component/'proto-c').glob('*.pb-c.c'))]
    if args.clang:
        objects = []
        for index, source in enumerate(sources):
            obj = directory/f'{index}.o'; objects.append(str(obj))
            subprocess.run([args.clang,'--target=x86_64-pc-windows-gnu','-nostdinc',
                '-I'+str(pathlib.Path(args.cc).parent/'include'),*includes,'-c',source,'-o',str(obj)],check=True)
        sources = objects
    subprocess.run([args.cc,'-shared','-Wl,-image-base=0x10000000',*includes,*sources,
        '-o',str(library_path)],check=True)
    lib = ct.CDLL(str(library_path))
    crypto_type = ct.CFUNCTYPE(ct.c_int,ct.c_int,ct.c_void_p,ct.c_uint32,ct.c_void_p,ct.c_uint32,
        ct.c_void_p,ct.c_uint32,ct.c_void_p,ct.c_uint32,ct.c_void_p,ct.c_void_p)
    def crypto(op,k,kl,a,al,b,bl,c,cl,out,tag):
        try:
            key=ct.string_at(k,kl) if k else b''
            aa=ct.string_at(a,al) if a else b''
            bb=ct.string_at(b,bl) if b else b''
            cc=ct.string_at(c,cl) if c else b''
            if op==0: value=os.urandom(cl)
            elif op==1: value=hmac.digest(key,aa,'sha256')
            elif op==2: value=hashlib.sha256(aa).digest()
            elif op==3: value=hashlib.sha1(aa).digest()
            elif op==4:
                private=ec.derive_private_key(int.from_bytes(key,'big'),ec.SECP256R1())
                value=private.exchange(ec.ECDH(),ec.EllipticCurvePublicKey.from_encoded_point(ec.SECP256R1(),aa))
            elif op==5:
                encrypted=AESGCM(key).encrypt(aa,cc,bb); value=encrypted[:-16];ct.memmove(tag,encrypted[-16:],16)
            elif op==6: value=AESGCM(key).decrypt(aa,cc+ct.string_at(tag,16),bb)
            else: raise AssertionError(op)
            ct.memmove(out,value,len(value));return 1
        except Exception: return 0
    callback = crypto_type(crypto);lib.host_register(callback)
    assert lib.host_layout()==1, 'Host compiler cannot initialize the protobuf descriptor correctly'
    private=ec.derive_private_key(1,ec.SECP256R1()); vehicle=ec.derive_private_key(2,ec.SECP256R1())
    public=lambda k:k.public_key().public_bytes(serialization.Encoding.X962,serialization.PublicFormat.UncompressedPoint)
    key=hashlib.sha1(private.exchange(ec.ECDH(),vehicle.public_key())).digest()[:16]
    vin=b'5YJ30123456789ABC';epoch=bytes(range(16));out=ct.create_string_buffer(1024)
    checks=0
    def check(value):
        global checks
        assert value;checks+=1
    def init(domain=3):
        lib.host_init(public(private));n=lib.host_handshake(domain,out);return fields(out.raw[:n])
    request=init()
    check(2 not in fields(request[14]))  # SDK handshake: UUID is the challenge, no extra challenge field.
    def envelope(payload_field,payload,domain=3,uuid=None,address=None,status=b'',signature=b''):
        return blob(6,blob(2,address if address is not None else fields(request[7])[2]))+blob(7,num(1,domain))+\
            blob(payload_field,payload)+blob(50,request[51] if uuid is None else uuid)+\
            (blob(12,status) if status else b'')+(blob(13,signature) if signature else b'')
    def session_info(counter=7,clock=100,ep=epoch,pub=None,wrong_tag=False):
        info=num(1,counter)+blob(2,public(vehicle) if pub is None else pub)+blob(3,ep)+fixed(4,clock)
        meta=b'\x00\x01\x06\x02\x11'+vin+b'\x06\x10'+request[51]+b'\xff'+info
        tag=hmac.digest(hmac.digest(key,b'session info','sha256'),meta,'sha256')
        if wrong_tag:tag=bytes([tag[0]^1])+tag[1:]
        return envelope(15,info,signature=blob(6,blob(1,tag)))
    valid=session_info()
    for invalid_epoch in [b'',bytes(17)]:
        bad=session_info(ep=invalid_epoch);check(lib.host_verify(bad,len(bad))==0)
    bad=session_info(pub=b'\x04'+bytes(64));check(lib.host_verify(bad,len(bad))==0)
    check(lib.host_verify(session_info(wrong_tag=True),len(valid))==0)
    check(lib.host_established()==0)
    check(lib.host_verify(valid,len(valid))==1)
    check(lib.host_counter()==7 and lib.host_established()==1)
    newer=session_info(counter=20,clock=110);check(lib.host_verify(newer,len(newer))==1)
    check(lib.host_counter()==20 and lib.host_clock()==110)
    older=session_info(counter=3,clock=90);check(lib.host_verify(older,len(older))==1)
    check(lib.host_counter()==20 and lib.host_clock()==110)
    otherkey=session_info(pub=public(ec.derive_private_key(3,ec.SECP256R1())))
    check(lib.host_verify(otherkey,len(otherkey))==0 and lib.host_counter()==20)
    badroute=valid.replace(fields(request[7])[2],b'Z'*16)
    check(lib.host_verify(badroute,len(badroute))==0)
    baduuid=valid.replace(request[51],b'Y'*16);check(lib.host_verify(baduuid,len(baduuid))==0)
    unsigned=envelope(15,num(1,5)+blob(2,public(vehicle))+blob(3,epoch)+fixed(4,100))
    check(lib.host_verify(unsigned,len(unsigned))==0)
    rejected=envelope(15,num(5,1));check(lib.host_verify(rejected,len(rejected))==2)
    n=lib.host_metadata(out)
    check(out.raw[:n].hex()=='000105010103021135594a333031323334353637383941424303104c463f9cc0d3d26906e982ed224adde6040400000a5f050400000007070400000002ff')
    n=lib.host_command(3,out);request=fields(out.raw[:n]);signature=fields(fields(request[13])[5]);check(request[52]==2)
    plaintext=b'\x12\x04\x52\x02\x08\x01'
    request_id=b'\x05'+signature[5]
    def encrypted_response(counter=1,tamper=False):
        nonce=bytes(range(12));meta=b'\x00\x01\x09\x01\x01\x03\x02\x11'+vin+\
            b'\x05\x04'+struct.pack('>I',counter)+b'\x07\x04'+bytes(4)+b'\x08\x11'+request_id+b'\x09\x04'+bytes(4)+b'\xff'
        ciphertext=AESGCM(key).encrypt(nonce,plaintext,hashlib.sha256(meta).digest())
        if tamper:ciphertext=ciphertext[:-1]+bytes([ciphertext[-1]^1])
        return envelope(10,ciphertext[:-16],signature=blob(9,blob(1,nonce)+num(2,counter)+blob(3,ciphertext[-16:])))
    wire=encrypted_response(tamper=True);check(lib.host_decode(wire,len(wire),out)==0)
    wire=encrypted_response(9)+blob(12,num(2,3))
    check(lib.host_decode(wire,len(wire),out)==0)  # Modified encrypted fault metadata is not trusted.
    wire=encrypted_response(8);check(lib.host_decode(wire,len(wire),out)==1 and out.raw[:len(plaintext)]==plaintext)
    check(lib.host_decode(wire,len(wire),out)==4)
    wire=encrypted_response(7);check(lib.host_decode(wire,len(wire),out)==1)
    plain=envelope(10,plaintext);check(lib.host_decode(plain,len(plain),out)==1)
    check(lib.host_matches(envelope(10,plaintext,uuid=b'X'*16),len(plain))==0)
    check(lib.host_matches(envelope(10,plaintext,domain=2),len(plain))==0)
    # CarServer response: present zero float wins over integer speed; missing fields fail.
    drive=blob(1,blob(2,b''))+num(102,12)+num(103,0)+fixed(106,0)
    data=blob(2,blob(5,drive));speed=ct.c_float()
    check(lib.host_drive(data,len(data),ct.byref(speed))==1 and speed.value==0)
    missing=blob(2,blob(5,blob(1,blob(2,b''))))
    check(lib.host_drive(missing,len(missing),ct.byref(speed))==0)
    error=blob(1,num(1,1))+data;check(lib.host_drive(error,len(error),ct.byref(speed))==0)
    # Stream coalescing, fragments, malformed length, timeout, depth bound.
    for fragment in [b'\x00',b'\x03a',b'bc\x00\x02de']:
        check(lib.host_feed(fragment,len(fragment),1)==1)
    check(lib.host_pop(out)==3 and out.raw[:3]==b'abc')
    check(lib.host_pop(out)==2 and out.raw[:2]==b'de')
    check(lib.host_pop(out)==0)
    check(lib.host_feed(b'\x04\x01',2,2)==0)
    lib.host_feed(b'\x00\x03x',3,3);lib.host_feed(b'\x00\x01z',3,1005)
    check(lib.host_pop(out)==1 and out.raw[:1]==b'z')
    check(lib.host_feed(b'\x00\x01a'*6,18,1006)==0)
    for _ in range(5):check(lib.host_pop(out)==1)
    check(lib.host_pop(out)==0)
    # Actual exchange: uncertain controls don't repeat, queries retry, cancellation wins.
    check(lib.host_engine(1,0)==0x7502 and lib.host_sends()==1)
    check(lib.host_engine(0,0)==0x107 and lib.host_sends()==3)
    check(lib.host_engine(1,1)==0x103 and lib.host_sends()==1)
    check(lib.host_engine(1,2)==0x7502 and lib.host_sends()==2)
    check(lib.host_engine(1,3)==0x7501 and lib.host_sends()==1)
    check(lib.host_engine(1,4)==0x7502 and lib.host_sends()==1)  # ACK is not a terminal result.
    check(lib.host_engine(1,5)==0 and lib.host_sends()==1)
    before=lib.host_counter();check(lib.host_engine(1,0)==0x7502 and lib.host_counter()==before+1)
    check(lib.host_engine(0,1)==0x103 and lib.host_sends()==1)
    lib.host_set_counter(2,ct.c_uint32(0xffffffff))
    check(lib.host_engine(1,0)==0x103 and lib.host_sends()==0)
    print(f'tesla_official_protocol_test: PASS ({checks} assertions; real C + independent crypto oracle)')
    ct.windll.kernel32.FreeLibrary(ct.c_void_p(lib._handle))
