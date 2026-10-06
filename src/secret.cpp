// ************************************************************
//           SDカードに保存するパスワードの暗号化
// ************************************************************
//
// 設定ファイルと実行パラメータのファイルには、パスワードを暗号化して書く。
// SDカードを抜いて読まれても、パスワードが分からないようにするため。
//
//   書式   "enc:v1:" + Base64( nonce(12) + 暗号文 + 認証タグ(16) )
//   暗号   AES-256-GCM（mbedTLS）
//   鍵     初回の起動時に乱数で作り、内蔵フラッシュ(NVS)に保存する
//
// ・鍵は本体ごとに違う。SDカードを別の本体に差しても復号できない。
// ・内蔵フラッシュを全て消すと鍵も消える（通常の書き込みでは消えない）。
// ・内蔵フラッシュをUSBから読み出されると鍵が分かる。そこまでは守らない。
//
// nonce は、乱数と平文から鍵付きハッシュで作る。無線を始める前は乱数の質が
// 落ちるので、乱数が重なっても、平文が違えば nonce が重ならないようにしている。

#include <Arduino.h>
#include <Preferences.h>
#include <mbedtls/gcm.h>
#include <mbedtls/md.h>
#include <mbedtls/base64.h>
#include <esp_system.h>

extern "C" {
#include <bootloader_random.h>
}

#include "app.h"

#define SECRET_PREFIX "enc:v1:"
#define KEY_BYTES 32
#define NONCE_BYTES 12
#define TAG_BYTES 16
#define PLAIN_MAX 128		// 暗号化する文字列の最大バイト数

static uint8_t mKey[ KEY_BYTES ];
static bool mKeyReady = false;

// 鍵を用意する。無ければ作って保存する
//
// ・無線(Wifi、BLE)を始める前に、setup()から1回呼び出す事。
//   鍵を作る時に使う乱数源(bootloader_random)は、無線と同時には使えない。
//
// 戻り値＝ 0:正常終了
//         -1:鍵を保存できない
//
int secretInit()
{
	Preferences prefs;
	if ( ! prefs.begin( "m5f9p", false ) ) return -1;

	if ( prefs.getBytesLength( "cfgkey" ) == KEY_BYTES ){
		prefs.getBytes( "cfgkey", mKey, KEY_BYTES );
		mKeyReady = true;
	}
	else {
		bootloader_random_enable();
		esp_fill_random( mKey, KEY_BYTES );
		bootloader_random_disable();
		mKeyReady = ( prefs.putBytes( "cfgkey", mKey, KEY_BYTES ) == KEY_BYTES );
		dbgPrintf( "Secret key created (%d)\r\n", (int) mKeyReady );
	}
	prefs.end();
	return mKeyReady ? 0 : -1;
}

// 暗号化された文字列かどうか
//
bool secretIsEncrypted( const char *text )
{
	return text && strncmp( text, SECRET_PREFIX, strlen( SECRET_PREFIX ) ) == 0;
}

// nonce を作る。乱数と平文の鍵付きハッシュ(HMAC-SHA256)の先頭を使う
//
static void makeNonce( const uint8_t *plain, int length, uint8_t *nonce )
{
	uint8_t seed[16], hash[32];
	esp_fill_random( seed, sizeof(seed) );

	mbedtls_md_context_t md;
	mbedtls_md_init( &md );
	mbedtls_md_setup( &md, mbedtls_md_info_from_type( MBEDTLS_MD_SHA256 ), 1 );
	mbedtls_md_hmac_starts( &md, mKey, KEY_BYTES );
	mbedtls_md_hmac_update( &md, (const uint8_t*) "nonce", 5 );
	mbedtls_md_hmac_update( &md, seed, sizeof(seed) );
	mbedtls_md_hmac_update( &md, plain, length );
	mbedtls_md_hmac_finish( &md, hash );
	mbedtls_md_free( &md );
	memcpy( nonce, hash, NONCE_BYTES );
}

// 文字列を暗号化する
//
// ・空の文字列と、すでに暗号化されている文字列は、そのまま返す。
// ・暗号化できない時（鍵が無い、長すぎる）も、そのまま返す。
//
String secretEncrypt( const char *plain )
{
	int length = plain ? strlen( plain ) : 0;
	if ( length == 0 || secretIsEncrypted( plain ) ) return plain ? plain : "";
	if ( ! mKeyReady || length > PLAIN_MAX ) return plain;

	uint8_t packed[ NONCE_BYTES + PLAIN_MAX + TAG_BYTES ];
	uint8_t *nonce = packed, *cipher = packed + NONCE_BYTES, *tag = cipher + length;
	makeNonce( (const uint8_t*) plain, length, nonce );

	mbedtls_gcm_context gcm;
	mbedtls_gcm_init( &gcm );
	int nret = mbedtls_gcm_setkey( &gcm, MBEDTLS_CIPHER_ID_AES, mKey, KEY_BYTES * 8 );
	if ( nret == 0 ) nret = mbedtls_gcm_crypt_and_tag( &gcm, MBEDTLS_GCM_ENCRYPT, length, nonce, NONCE_BYTES,
										NULL, 0, (const uint8_t*) plain, cipher, TAG_BYTES, tag );
	mbedtls_gcm_free( &gcm );
	if ( nret != 0 ) return plain;

	char text[ ( sizeof(packed) + 2 ) / 3 * 4 + 1 ];
	size_t textLength = 0;
	if ( mbedtls_base64_encode( (uint8_t*) text, sizeof(text), &textLength, packed, NONCE_BYTES + length + TAG_BYTES ) != 0 ) return plain;
	text[ textLength ] = '\0';
	return String( SECRET_PREFIX ) + text;
}

// 文字列を復号する
//
// 戻り値＝ 1:復号した（plainに結果）
//          0:暗号化されていない文字列だった（plainにそのまま入る）
//         -1:復号できない（別の本体で暗号化された、内容が壊れている。plainは空）
//
int secretDecrypt( const char *text, String &plain )
{
	if ( ! secretIsEncrypted( text ) ){
		plain = text ? text : "";
		return 0;
	}
	plain = "";
	if ( ! mKeyReady ) return -1;

	uint8_t packed[ NONCE_BYTES + PLAIN_MAX + TAG_BYTES ];
	size_t packedLength = 0;
	const char *body = text + strlen( SECRET_PREFIX );
	if ( mbedtls_base64_decode( packed, sizeof(packed), &packedLength, (const uint8_t*) body, strlen( body ) ) != 0 ) return -1;
	if ( packedLength <= NONCE_BYTES + TAG_BYTES ) return -1;
	int length = packedLength - NONCE_BYTES - TAG_BYTES;

	char out[ PLAIN_MAX + 1 ];
	mbedtls_gcm_context gcm;
	mbedtls_gcm_init( &gcm );
	int nret = mbedtls_gcm_setkey( &gcm, MBEDTLS_CIPHER_ID_AES, mKey, KEY_BYTES * 8 );
	if ( nret == 0 ) nret = mbedtls_gcm_auth_decrypt( &gcm, length, packed, NONCE_BYTES, NULL, 0,
										packed + NONCE_BYTES + length, TAG_BYTES, packed + NONCE_BYTES, (uint8_t*) out );
	mbedtls_gcm_free( &gcm );
	if ( nret != 0 ) return -1;

	out[ length ] = '\0';
	plain = out;
	return 1;
}
