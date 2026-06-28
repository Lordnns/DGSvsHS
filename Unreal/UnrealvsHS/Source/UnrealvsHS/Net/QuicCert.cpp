
#include "QuicCert.h"

#include <cstring>

#define UI OpenSSL_UI_RenamedToAvoidUObjectClash_
#include <openssl/evp.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>
#include <openssl/pem.h>
#include <openssl/pkcs12.h>
#include <openssl/bio.h>
#include <openssl/err.h>
#undef UI

namespace UnrealvsHS::Net
{
	uint8_t* GenerateSelfSignedPkcs12(int32_t* OutLength)
	{
		if (OutLength) *OutLength = 0;

		// 1. Generate a 2048-bit RSA key.
		EVP_PKEY* PKey = EVP_PKEY_new();
		if (!PKey) return nullptr;
		EVP_PKEY_CTX* KeyCtx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);
		if (!KeyCtx) { EVP_PKEY_free(PKey); return nullptr; }
		if (EVP_PKEY_keygen_init(KeyCtx) <= 0
			|| EVP_PKEY_CTX_set_rsa_keygen_bits(KeyCtx, 2048) <= 0
			|| EVP_PKEY_keygen(KeyCtx, &PKey) <= 0)
		{
			EVP_PKEY_CTX_free(KeyCtx);
			EVP_PKEY_free(PKey);
			return nullptr;
		}
		EVP_PKEY_CTX_free(KeyCtx);
		
		X509* Cert = X509_new();
		if (!Cert) { EVP_PKEY_free(PKey); return nullptr; }
		ASN1_INTEGER_set(X509_get_serialNumber(Cert), 1);
		X509_gmtime_adj(X509_get_notBefore(Cert), -3600);
		X509_gmtime_adj(X509_get_notAfter(Cert), 60 * 60 * 24 * 365);
		X509_set_pubkey(Cert, PKey);
		X509_NAME* Name = X509_get_subject_name(Cert);
		X509_NAME_add_entry_by_txt(Name, "CN", MBSTRING_ASC,
			reinterpret_cast<const unsigned char*>("UnrealvsHS-server"), -1, -1, 0);
		X509_set_issuer_name(Cert, Name);
		if (!X509_sign(Cert, PKey, EVP_sha256()))
		{
			X509_free(Cert); EVP_PKEY_free(PKey);
			return nullptr;
		}
		
		PKCS12* P12 = PKCS12_create(
			/*pass = NULL  → no encryption, no MAC*/  nullptr,
			/*friendly*/                              "UnrealvsHS-server",
			PKey, Cert, /*ca chain*/ nullptr,
			0, 0, 0, 0, 0);
		if (!P12) { X509_free(Cert); EVP_PKEY_free(PKey); return nullptr; }

		BIO* MemBio = BIO_new(BIO_s_mem());
		const int Wrote = i2d_PKCS12_bio(MemBio, P12);
		PKCS12_free(P12);
		X509_free(Cert);
		EVP_PKEY_free(PKey);
		if (Wrote <= 0) { BIO_free(MemBio); return nullptr; }
		
		BUF_MEM* Bm = nullptr;
		BIO_get_mem_ptr(MemBio, &Bm);
		uint8_t* OutBuf = new uint8_t[Bm->length];
		std::memcpy(OutBuf, Bm->data, Bm->length);
		const int32_t OutLen = static_cast<int32_t>(Bm->length);
		BIO_free(MemBio);

		if (OutLength) *OutLength = OutLen;
		return OutBuf;
	}

	void FreePkcs12Buffer(uint8_t* Buffer)
	{
		delete[] Buffer;
	}

	// ---------- PEM file pair (Linux fallback) ----------

	bool GenerateSelfSignedPemFiles(const char* CertPath, const char* KeyPath)
	{
		if (!CertPath || !KeyPath) return false;
		
		EVP_PKEY* PKey = EVP_PKEY_new();
		if (!PKey) return false;
		EVP_PKEY_CTX* KeyCtx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);
		if (!KeyCtx) { EVP_PKEY_free(PKey); return false; }
		if (EVP_PKEY_keygen_init(KeyCtx) <= 0
			|| EVP_PKEY_CTX_set_rsa_keygen_bits(KeyCtx, 2048) <= 0
			|| EVP_PKEY_keygen(KeyCtx, &PKey) <= 0)
		{
			EVP_PKEY_CTX_free(KeyCtx);
			EVP_PKEY_free(PKey);
			return false;
		}
		EVP_PKEY_CTX_free(KeyCtx);
		
		X509* Cert = X509_new();
		if (!Cert) { EVP_PKEY_free(PKey); return false; }
		ASN1_INTEGER_set(X509_get_serialNumber(Cert), 1);
		X509_gmtime_adj(X509_get_notBefore(Cert), -3600);
		X509_gmtime_adj(X509_get_notAfter(Cert), 60 * 60 * 24 * 365);
		X509_set_pubkey(Cert, PKey);
		X509_NAME* Name = X509_get_subject_name(Cert);
		X509_NAME_add_entry_by_txt(Name, "CN", MBSTRING_ASC,
			reinterpret_cast<const unsigned char*>("UnrealvsHS-server"), -1, -1, 0);
		X509_set_issuer_name(Cert, Name);
		if (!X509_sign(Cert, PKey, EVP_sha256()))
		{
			X509_free(Cert); EVP_PKEY_free(PKey);
			return false;
		}
		
		BIO* CertBio = BIO_new_file(CertPath, "wb");
		if (!CertBio) { X509_free(Cert); EVP_PKEY_free(PKey); return false; }
		const int CertOk = PEM_write_bio_X509(CertBio, Cert);
		BIO_free(CertBio);
		
		BIO* KeyBio = BIO_new_file(KeyPath, "wb");
		if (!KeyBio) { X509_free(Cert); EVP_PKEY_free(PKey); return false; }
		const int KeyOk = PEM_write_bio_PrivateKey(KeyBio, PKey,
			/*cipher*/ nullptr,
			/*kstr*/   nullptr, /*klen*/ 0,
			/*cb*/     nullptr, /*u*/    nullptr);
		BIO_free(KeyBio);

		X509_free(Cert);
		EVP_PKEY_free(PKey);

		return CertOk == 1 && KeyOk == 1;
	}
}
