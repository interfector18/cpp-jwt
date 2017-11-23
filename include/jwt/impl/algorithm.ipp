#ifndef CPP_JWT_ALGORITHM_IPP
#define CPP_JWT_ALGORITHM_IPP

#include <iostream>

namespace jwt {

template <typename Hasher>
verify_result_t HMACSign<Hasher>::verify(
    const string_view key,
    const string_view head,
    const string_view jwt_sign)
{
  std::error_code ec{};

  std::cout << "Key: "  << key      << std::endl;
  std::cout << "Head: " << head     << std::endl;
  std::cout << "JWT: "  << jwt_sign << std::endl;

  BIO_uptr b64{BIO_new(BIO_f_base64()), bio_deletor};
  if (!b64) {
    throw MemoryAllocationException("BIO_new failed");
  }

  BIO* bmem = BIO_new(BIO_s_mem());
  if (!bmem) {
    throw MemoryAllocationException("BIO_new failed");
  }

  BIO_push(b64.get(), bmem);
  BIO_set_flags(b64.get(), BIO_FLAGS_BASE64_NO_NL);

  unsigned char enc_buf[EVP_MAX_MD_SIZE];
  uint32_t enc_buf_len = 0;

  unsigned char* res = HMAC(Hasher{}(),
                            key.data(),
                            key.length(),
                            reinterpret_cast<const unsigned char*>(head.data()),
                            head.length(),
                            enc_buf,
                            &enc_buf_len);
  if (!res) {
    ec = AlgorithmErrc::VerificationErr;
    return {false, ec};
  }

  BIO_write(b64.get(), enc_buf, enc_buf_len);
  (void)BIO_flush(b64.get());

  int len = BIO_pending(bmem);
  if (len < 0) {
    ec = AlgorithmErrc::VerificationErr;
    return {false, ec};
  }

  std::string cbuf;
  cbuf.resize(len + 1);

  len = BIO_read(bmem, &cbuf[0], len);
  cbuf.resize(len);

  //Make the base64 string url safe
  auto new_len = jwt::base64_uri_encode(&cbuf[0], cbuf.length());
  cbuf.resize(new_len);
  std::cout << "cbuf: " << cbuf << std::endl;

  bool ret = (string_view{cbuf} == jwt_sign);

  return { ret, ec };
}


template <typename Hasher>
verify_result_t PEMSign<Hasher>::verify(
    const string_view key,
    const string_view head,
    const string_view jwt_sign)
{
  /*
   * unsigned char *sig = NULL;
   * EVP_MD_CTX *mdctx = NULL;
   * ECDSA_SIG *ec_sig = NULL;
   * BIGNUM *ec_sig_r = NULL;
   * BIGNUM *ec_sig_s = NULL;
   * EVP_PKEY *pkey = NULL;
   * const EVP_MD *alg;
   * int type;
   * int pkey_type;
   * BIO *bufkey = NULL;
   * int ret = 0;
   * int slen;
   */
  std::error_code ec{};
  return { true, ec };
}

template <typename Hasher>
EVP_PKEY* PEMSign<Hasher>::load_key(
    const string_view key,
    std::error_code& ec)
{
  ec.clear();

  BIO_uptr bio_ptr{
      BIO_new_mem_buf((void*)key.data(), key.length()), 
      bio_deletor};

  if (!bio_ptr) {
    throw MemoryAllocationException("BIO_new_mem_buf failed");
  }

  EVP_PKEY* pkey = PEM_read_bio_PrivateKey(bio_ptr.get(), nullptr, nullptr, nullptr);

  if (!pkey) {
    ec = AlgorithmErrc::SigningErr;
    return nullptr;
  }

  return pkey;
}

template <typename Hasher>
std::string PEMSign<Hasher>::evp_digest(
    EVP_PKEY* pkey, 
    const string_view data, 
    std::error_code& ec)
{
  ec.clear();

  EVP_MDCTX_uptr mdctx_ptr{EVP_MD_CTX_create(), evp_md_ctx_deletor};

  if (!mdctx_ptr) {
    throw MemoryAllocationException("EVP_MD_CTX_create failed");
  }

  //Initialiaze the digest algorithm
  if (EVP_DigestSignInit(
        mdctx_ptr.get(), nullptr, Hasher{}(), nullptr, pkey) != 1) {
    ec = AlgorithmErrc::SigningErr;
    return {};
  }

  //Update the digest with the input data
  if (EVP_DigestSignUpdate(mdctx_ptr.get(), data.data(), data.length()) != 1) {
    ec = AlgorithmErrc::SigningErr;
    return std::string{};
  }

  unsigned long len = 0;

  if (EVP_DigestSignFinal(mdctx_ptr.get(), nullptr, &len) != 1) {
    ec = AlgorithmErrc::SigningErr;
    return {};
  }

  std::string sign;
  sign.resize(len);

  //Get the signature
  if (EVP_DigestSignFinal(mdctx_ptr.get(), (unsigned char*)&sign[0], &len) != 1) {
    ec = AlgorithmErrc::SigningErr;
    return {};
  }

  return sign;
}

template <typename Hasher>
std::string PEMSign<Hasher>::public_key_ser(
    EVP_PKEY* pkey, 
    string_view sign, 
    std::error_code& ec)
{
  // Get the EC_KEY representing a public key and
  // (optionaly) an associated private key
  std::string new_sign;
  ec.clear();

  EC_KEY_uptr ec_key{EVP_PKEY_get1_EC_KEY(pkey), ec_key_deletor};

  if (!ec_key) {
    ec = AlgorithmErrc::SigningErr;
    return {};
  }

  uint32_t degree = EC_GROUP_get_degree(EC_KEY_get0_group(ec_key.get()));

  EC_SIG_uptr ec_sig{d2i_ECDSA_SIG(nullptr,
                                   (const unsigned char**)&sign[0],
                                   sign.length()),
                     ec_sig_deletor};

  if (!ec_sig) {
    ec = AlgorithmErrc::SigningErr;
    return {};
  }

  const BIGNUM* ec_sig_r = nullptr;
  const BIGNUM* ec_sig_s = nullptr;

#if 1
  //Taken from https://github.com/nginnever/zogminer/issues/39
  static auto ECDSA_SIG_get0 = [](const ECDSA_SIG *sig, const BIGNUM **pr, const BIGNUM **ps)
  {
    if (pr != nullptr) *pr = sig->r;
    if (ps != nullptr) *ps = sig->s;
  };

#endif

  ECDSA_SIG_get0(ec_sig.get(), &ec_sig_r, &ec_sig_s);

  auto r_len = BN_num_bytes(ec_sig_r);
  auto s_len = BN_num_bytes(ec_sig_s);
  auto bn_len = (degree + 7) / 8;

  if ((r_len > bn_len) || (s_len > bn_len)) {
    ec = AlgorithmErrc::SigningErr;
    return {};
  }

  auto buf_len = 2 * bn_len;
  new_sign.resize(buf_len);

  BN_bn2bin(ec_sig_r, (unsigned char*)&new_sign[0] + bn_len - r_len);
  BN_bn2bin(ec_sig_s, (unsigned char*)&new_sign[0] + buf_len - s_len);

  return new_sign;
}

} // END namespace jwt

#endif