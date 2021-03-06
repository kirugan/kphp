// Compiler for PHP (aka KPHP)
// Copyright (c) 2020 LLC «V Kontakte»
// Distributed under the GPL v3 License, see LICENSE.notice.txt

#include "runtime/openssl.h"

#include <cerrno>
#include <memory>
#include <netdb.h>
#include <openssl/asn1.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/md5.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/sha.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>
#include <poll.h>
#include <sys/time.h>
#include <unistd.h>

#include "common/crc32.h"
#include "common/resolver.h"
#include "common/smart_ptrs/unique_ptr_with_delete_function.h"
#include "common/wrappers/openssl.h"
#include "common/wrappers/string_view.h"

#include "runtime/critical_section.h"
#include "runtime/datetime.h"
#include "runtime/files.h"
#include "runtime/net_events.h"
#include "runtime/streams.h"
#include "runtime/string_functions.h"
#include "runtime/url.h"

array<string> f$hash_algos() {
  return array<string>::create(
    string("sha1", 4),
    string("sha256", 6),
    string("md5", 3));
}

bool f$hash_equals(const string &known_string, const string &user_string) noexcept {
  if (known_string.size() != user_string.size()) {
    return false;
  }
  int result = 0;
  // This is security sensitive code. Do not optimize this for speed
  for (int i = 0; i != known_string.size(); ++i) {
    result |= known_string[i] ^ user_string[i];
  }
  return !result;
}

string f$hash(const string &algo, const string &s, bool raw_output) {
  if (!strcmp(algo.c_str(), "sha256")) {
    string res;
    if (raw_output) {
      res.assign(32, false);
    } else {
      res.assign(64, false);
    }

    SHA256(reinterpret_cast <const unsigned char *> (s.c_str()), (unsigned long)s.size(), reinterpret_cast <unsigned char *> (res.buffer()));

    if (!raw_output) {
      for (int i = 31; i >= 0; i--) {
        res[2 * i + 1] = lhex_digits[res[i] & 15];
        res[2 * i] = lhex_digits[(res[i] >> 4) & 15];
      }
    }
    return res;
  } else if (!strcmp(algo.c_str(), "md5")) {
    return f$md5(s, raw_output);
  } else if (!strcmp(algo.c_str(), "sha1")) {
    return f$sha1(s, raw_output);
  }

  php_critical_error ("algo %s not supported in function hash", algo.c_str());
}

string f$hash_hmac(const string &algo, const string &data, const string &key, bool raw_output) {
  const EVP_MD *evp_md = nullptr;
  string::size_type hash_len = 0;
  if (!strcmp(algo.c_str(), "sha1")) {
    evp_md = EVP_sha1();
    hash_len = 20;
  } else if (!strcmp(algo.c_str(), "sha256")) {
    evp_md = EVP_sha256();
    hash_len = 32;
  }

  if (evp_md != nullptr) {
    string res;
    if (raw_output) {
      res.assign(hash_len, false);
    } else {
      res.assign(hash_len * 2, false);
    }

    unsigned int md_len;
    HMAC(evp_md, static_cast <const void *> (key.c_str()), (int)key.size(),
         reinterpret_cast <const unsigned char *> (data.c_str()), (int)data.size(),
         reinterpret_cast <unsigned char *> (res.buffer()), &md_len);
    php_assert (md_len == hash_len);

    if (!raw_output) {
      for (int i = hash_len - 1; i >= 0; i--) {
        res[2 * i + 1] = lhex_digits[res[i] & 15];
        res[2 * i] = lhex_digits[(res[i] >> 4) & 15];
      }
    }
    return res;
  }

  php_critical_error ("unsupported algo = \"%s\" in function hash_hmac", algo.c_str());
  return string();
}

string f$sha1(const string &s, bool raw_output) {
  string res;
  if (raw_output) {
    res.assign(20, false);
  } else {
    res.assign(40, false);
  }

  SHA1(reinterpret_cast <const unsigned char *> (s.c_str()), (unsigned long)s.size(), reinterpret_cast <unsigned char *> (res.buffer()));

  if (!raw_output) {
    for (int i = 19; i >= 0; i--) {
      res[2 * i + 1] = lhex_digits[res[i] & 15];
      res[2 * i] = lhex_digits[(res[i] >> 4) & 15];
    }
  }
  return res;
}


string f$md5(const string &s, bool raw_output) {
  string res;
  if (raw_output) {
    res.assign(16, false);
  } else {
    res.assign(32, false);
  }

  MD5(reinterpret_cast <const unsigned char *> (s.c_str()), (unsigned long)s.size(), reinterpret_cast <unsigned char *> (res.buffer()));

  if (!raw_output) {
    for (int i = 15; i >= 0; i--) {
      res[2 * i + 1] = lhex_digits[res[i] & 15];
      res[2 * i] = lhex_digits[(res[i] >> 4) & 15];
    }
  }
  return res;
}

Optional<string> f$md5_file(const string &file_name, bool raw_output) {
  dl::enter_critical_section();//OK
  struct stat stat_buf;
  int read_fd = open(file_name.c_str(), O_RDONLY);
  if (read_fd < 0) {
    dl::leave_critical_section();
    return false;
  }
  if (fstat(read_fd, &stat_buf) < 0) {
    close(read_fd);
    dl::leave_critical_section();
    return false;
  }

  if (!S_ISREG (stat_buf.st_mode)) {
    close(read_fd);
    dl::leave_critical_section();
    php_warning("Regular file expected in function md5_file, \"%s\" is given", file_name.c_str());
    return false;
  }

  MD5_CTX c;
  php_assert (MD5_Init(&c) == 1);

  size_t size = stat_buf.st_size;
  while (size > 0) {
    size_t len = min(size, (size_t)PHP_BUF_LEN);
    if (read_safe(read_fd, php_buf, len) < (ssize_t)len) {
      break;
    }
    php_assert (MD5_Update(&c, static_cast <const void *> (php_buf), (unsigned long)len) == 1);
    size -= len;
  }
  close(read_fd);
  php_assert (MD5_Final(reinterpret_cast <unsigned char *> (php_buf), &c) == 1);
  dl::leave_critical_section();

  if (size > 0) {
    php_warning("Error while reading file \"%s\"", file_name.c_str());
    return false;
  }

  if (!raw_output) {
    string res(32, false);
    for (int i = 15; i >= 0; i--) {
      res[2 * i + 1] = lhex_digits[php_buf[i] & 15];
      res[2 * i] = lhex_digits[(php_buf[i] >> 4) & 15];
    }
    return res;
  } else {
    return string(php_buf, 16);
  }
}

int64_t f$crc32(const string &s) {
  return compute_crc32(static_cast <const void *> (s.c_str()), s.size());
}

int64_t f$crc32_file(const string &file_name) {
  dl::enter_critical_section();//OK
  struct stat stat_buf;
  int read_fd = open(file_name.c_str(), O_RDONLY);
  if (read_fd < 0) {
    dl::leave_critical_section();
    return -1;
  }
  if (fstat(read_fd, &stat_buf) < 0) {
    close(read_fd);
    dl::leave_critical_section();
    return -1;
  }

  if (!S_ISREG (stat_buf.st_mode)) {
    close(read_fd);
    dl::leave_critical_section();
    php_warning("Regular file expected in function crc32_file, \"%s\" is given", file_name.c_str());
    return -1;
  }

  uint32_t res = std::numeric_limits<uint32_t>::max();
  size_t size = stat_buf.st_size;
  while (size > 0) {
    size_t len = min(size, (size_t)PHP_BUF_LEN);
    if (read_safe(read_fd, php_buf, len) < (ssize_t)len) {
      break;
    }
    res = crc32_partial(php_buf, (int)len, res);
    size -= len;
  }
  close(read_fd);
  dl::leave_critical_section();

  if (size > 0) {
    return -1;
  }

  return res ^ std::numeric_limits<uint32_t>::max();
}

struct EVPKeyResourceStorage {
public:
  EVPKeyResourceStorage (char prefixChar)
    : registered_keys_(reinterpret_cast<KeysArray *>(&storage_)),
      last_query_num_(-1),
      prefixChar_(prefixChar)
    {}

  string register_resource(EVP_PKEY *pkey) {
    if (dl::query_num != last_query_num_) {
      new(&storage_) KeysArray();
      last_query_num_ = dl::query_num;
    }

    string result(2, prefixChar_);
    result.append(int64_t{registered_keys_->count()});
    registered_keys_->push_back(pkey);
    return result;
  }

  EVP_PKEY *find_resource(const string &key) {
    int num = 0;
    if (last_query_num_ == dl::query_num &&
        key.size() > 2 && key[0] == prefixChar_ && key[1] == prefixChar_ &&
        sscanf(key.c_str() + 2, "%d", &num) == 1 &&
        static_cast <unsigned int> (num) < static_cast <unsigned int> (registered_keys_->count())) {
      return registered_keys_->get_value(num);
    }
    return nullptr;
  }

  void free_resources() {
    if (dl::query_num == last_query_num_) {
      for (auto keyIt = registered_keys_->begin(); keyIt != registered_keys_->end(); ++keyIt) {
        EVP_PKEY_free(keyIt.get_value());
      }
      last_query_num_--;
    }
  }

private:
  using KeysArray = array<EVP_PKEY *>;
  using KeysArrayStorage = std::aligned_storage<sizeof(KeysArray), alignof(KeysArray)>::type;

  KeysArrayStorage storage_;
  KeysArray *registered_keys_;
  long long last_query_num_;
  const char prefixChar_;
};

// uses different prefixes for avoid of resource identifier collisions
static EVPKeyResourceStorage public_keys{';'};
static EVPKeyResourceStorage private_keys{':'};

static EVP_PKEY *openssl_get_private_evp(const string &key, const string &passphrase, bool &from_cache) {
  if (EVP_PKEY *evp_pkey = private_keys.find_resource(key)) {
    from_cache = true;
    return evp_pkey;
  }

  from_cache = false;
  EVP_PKEY *evp_pkey = nullptr;
  dl::enter_critical_section(); // OK
  if (BIO *in = BIO_new_mem_buf(static_cast <void *> (const_cast <char *> (key.c_str())), key.size())) {
    if (passphrase.empty()) {
      evp_pkey = PEM_read_bio_PrivateKey(in, nullptr, nullptr, nullptr);
    } else {
      evp_pkey = PEM_read_bio_PrivateKey(in, nullptr, nullptr, static_cast <void *> (const_cast <char *> (passphrase.c_str())));
    }
    BIO_free(in);
  }
  dl::leave_critical_section();
  return evp_pkey;
}

static EVP_PKEY *openssl_get_public_evp(const string &key, bool &from_cache) {
  if (EVP_PKEY *evp_pkey = public_keys.find_resource(key)) {
    from_cache = true;
    return evp_pkey;
  }

  from_cache = false;
  EVP_PKEY *evp_pkey = nullptr;
  dl::enter_critical_section(); // OK
  BIO *cert_in = BIO_new_mem_buf(static_cast <void *> (const_cast <char *> (key.c_str())), key.size());
  if (cert_in == nullptr) {
    dl::leave_critical_section();
    return nullptr;
  }
  X509 *cert = reinterpret_cast<X509 *>(PEM_ASN1_read_bio(
    reinterpret_cast<d2i_of_void *>(d2i_X509), PEM_STRING_X509, cert_in, nullptr, nullptr, nullptr));
  BIO_free(cert_in);
  if (cert) {
    evp_pkey = X509_get_pubkey(cert);
    X509_free(cert);
  } else if (BIO *key_in = BIO_new_mem_buf(static_cast <void *> (const_cast <char *> (key.c_str())), key.size())) {
    evp_pkey = PEM_read_bio_PUBKEY(key_in, nullptr, nullptr, nullptr);
    BIO_free(key_in);
  }

  dl::leave_critical_section();
  return evp_pkey;
}

bool f$openssl_public_encrypt(const string &data, string &result, const string &key) {
  dl::enter_critical_section();//OK
  bool from_cache = false;
  EVP_PKEY *pkey = openssl_get_public_evp(key, from_cache);
  if (pkey == nullptr) {
    dl::leave_critical_section();

    php_warning("Parameter key is not a valid public key");
    result = string();
    return false;
  }
  if (EVP_PKEY_id(pkey) != EVP_PKEY_RSA && EVP_PKEY_id(pkey) != EVP_PKEY_RSA2) {
    if (!from_cache) {
      EVP_PKEY_free(pkey);
    }
    dl::leave_critical_section();

    php_warning("Key type is neither RSA nor RSA2");
    result = string();
    return false;
  }

  int key_size = EVP_PKEY_size(pkey);
  php_assert (PHP_BUF_LEN >= key_size);

  if (RSA_public_encrypt((int)data.size(), reinterpret_cast <const unsigned char *> (data.c_str()),
                         reinterpret_cast <unsigned char *> (php_buf), EVP_PKEY_get0_RSA(pkey), RSA_PKCS1_PADDING) != key_size) {
    if (!from_cache) {
      EVP_PKEY_free(pkey);
    }
    dl::leave_critical_section();

    php_warning("RSA public encrypt failed");
    result = string();
    return false;
  }

  if (!from_cache) {
    EVP_PKEY_free(pkey);
  }
  dl::leave_critical_section();

  result = string(php_buf, key_size);
  return true;
}

bool f$openssl_public_encrypt(const string &data, mixed &result, const string &key) {
  string result_string;
  if (f$openssl_public_encrypt(data, result_string, key)) {
    result = result_string;
    return true;
  }
  result = mixed();
  return false;
}

bool f$openssl_private_decrypt(const string &data, string &result, const string &key) {
  dl::enter_critical_section();//OK
  bool from_cache = false;
  EVP_PKEY *pkey = openssl_get_private_evp(key, string(), from_cache);
  if (pkey == nullptr) {
    dl::leave_critical_section();
    php_warning("Parameter key is not a valid private key");
    return false;
  }
  if (EVP_PKEY_id(pkey) != EVP_PKEY_RSA && EVP_PKEY_id(pkey) != EVP_PKEY_RSA2) {
    if (!from_cache) {
      EVP_PKEY_free(pkey);
    }
    dl::leave_critical_section();
    php_warning("Key type is not an RSA nor RSA2");
    return false;
  }

  int key_size = EVP_PKEY_size(pkey);
  php_assert (PHP_BUF_LEN >= key_size);

  int len = RSA_private_decrypt((int)data.size(), reinterpret_cast <const unsigned char *> (data.c_str()),
                                reinterpret_cast <unsigned char *> (php_buf), EVP_PKEY_get0_RSA(pkey), RSA_PKCS1_PADDING);
  if (!from_cache) {
    EVP_PKEY_free(pkey);
  }
  dl::leave_critical_section();
  if (len == -1) {
    //php_warning ("RSA private decrypt failed"); 
    result = string();
    return false;
  }

  result.assign(php_buf, len);
  return true;
}

bool f$openssl_private_decrypt(const string &data, mixed &result, const string &key) {
  string result_string;
  if (f$openssl_private_decrypt(data, result_string, key)) {
    result = result_string;
    return true;
  }
  result = mixed();
  return false;
}

Optional<string> f$openssl_pkey_get_private(const string &key, const string &passphrase) {
  Optional<string> result = false;
  dl::enter_critical_section(); // NOT OK: openssl_pkey
  bool from_cache = false;
  if (EVP_PKEY *pkey = openssl_get_private_evp(key, passphrase, from_cache)) {
    result = from_cache ? key : private_keys.register_resource(pkey);
  } else {
    php_warning("Parameter key is not a valid key or passphrase is not a valid password");
  }
  dl::leave_critical_section();
  return result;
}

Optional<string> f$openssl_pkey_get_public(const string &key) {
  Optional<string> result = false;
  dl::enter_critical_section(); // NOT OK: openssl_pkey
  bool from_cache = false;
  if (EVP_PKEY *pkey = openssl_get_public_evp(key, from_cache)) {
    result = from_cache ? key : public_keys.register_resource(pkey);
  } else {
    php_warning("Parameter key is not a valid key");
  }
  dl::leave_critical_section();
  return result;
}

static const EVP_MD *openssl_algo_to_evp_md(openssl_algo algo) {
  switch (algo) {
    case OPENSSL_ALGO_SHA1:
      return EVP_sha1();

# ifndef OPENSSL_NO_MD5
    case OPENSSL_ALGO_MD5:
      return EVP_md5();
# endif

# ifndef OPENSSL_NO_MD4
    case OPENSSL_ALGO_MD4:
      return EVP_md4();
#endif

#ifndef OPENSSL_NO_MD2
      case OPENSSL_ALGO_MD2:  return EVP_md2();
#endif

    case OPENSSL_ALGO_SHA224:
      return EVP_sha224();
    case OPENSSL_ALGO_SHA256:
      return EVP_sha256();
    case OPENSSL_ALGO_SHA384:
      return EVP_sha384();
    case OPENSSL_ALGO_SHA512:
      return EVP_sha512();

#ifndef OPENSSL_NO_RMD160
    case OPENSSL_ALGO_RMD160:
      return EVP_ripemd160();
#endif

    default:
      return nullptr;
  }
}

static const char *ssl_get_error_string() {
  static_SB.clean();
  while (unsigned long error_code = ERR_get_error()) {
    static_SB << "Error " << (int)error_code << ": [" << ERR_error_string(error_code, nullptr) << "]\n";
  }
  return static_SB.c_str();
}

bool f$openssl_sign(const string &data, string &signature, const string &priv_key_id, int64_t algo) {
  dl::enter_critical_section();
  const EVP_MD *mdtype = openssl_algo_to_evp_md(static_cast<openssl_algo>(algo));
  if (!mdtype) {
    php_warning("Unknown signature algorithm");
    dl::leave_critical_section();
    return false;
  }

  bool from_cache = false;
  EVP_PKEY *pkey = openssl_get_private_evp(priv_key_id, string{}, from_cache);
  if (pkey == nullptr) {
    php_warning("Parameter key cannot be converted into a private key");
    dl::leave_critical_section();
    return false;
  }

  const int pkey_size = EVP_PKEY_size(pkey);
  php_assert(pkey_size >= 0);
  signature.assign(static_cast<unsigned int>(pkey_size), '\0');

  unsigned int siglen = 0;
  bool result = false;
  EVP_MD_CTX *md_ctx = EVP_MD_CTX_create();
  if (md_ctx &&
      EVP_SignInit(md_ctx, mdtype) &&
      EVP_SignUpdate(md_ctx, data.c_str(), data.size()) &&
      EVP_SignFinal(md_ctx, reinterpret_cast<unsigned char *>(signature.buffer()), &siglen, pkey)) {
    php_assert(siglen <= signature.size());
    signature.shrink(siglen);
    result = true;
  } else {
    signature = string{};
    php_warning("Can't create signature for data\n%s", ssl_get_error_string());
  }
  EVP_MD_CTX_destroy(md_ctx);

  if (!from_cache) {
    EVP_PKEY_free(pkey);
  }

  dl::leave_critical_section();
  return result;
}

int64_t f$openssl_verify(const string &data, const string &signature, const string &pub_key_id, int64_t algo) {
  dl::enter_critical_section();
  const EVP_MD *mdtype = openssl_algo_to_evp_md(static_cast<openssl_algo>(algo));

  if (!mdtype) {
    php_warning("Unknown signature algorithm");
    dl::leave_critical_section();
    return 0;
  }

  if (signature.size() > UINT_MAX) {
    php_warning("Signature is too long");
    dl::leave_critical_section();
    return 0;
  }

  bool from_cache = false;
  EVP_PKEY *pkey = openssl_get_public_evp(pub_key_id, from_cache);
  if (pkey == nullptr) {
    php_warning("Parameter key cannot be converted into a public key");
    dl::leave_critical_section();
    return 0;
  }

  int err = 0;
  EVP_MD_CTX *md_ctx = EVP_MD_CTX_create();
  if (md_ctx == nullptr ||
      !EVP_VerifyInit (md_ctx, mdtype) ||
      !EVP_VerifyUpdate (md_ctx, data.c_str(), data.size()) ||
      (err = EVP_VerifyFinal(md_ctx, (unsigned char *)signature.c_str(), (unsigned int)signature.size(), pkey)) < 0) {
  }
  EVP_MD_CTX_destroy(md_ctx);
  if (!from_cache) {
    EVP_PKEY_free(pkey);
  }
  dl::leave_critical_section();
  return err;
}

Optional<string> f$openssl_random_pseudo_bytes(int64_t length) {
  if (length <= 0 || length > string::max_size()) {
    return false;
  }
  string buffer(static_cast<string::size_type>(length), ' ');
  struct timeval tv;

  gettimeofday(&tv, nullptr);
  RAND_add(&tv, sizeof(tv), 0.0);

  if (RAND_bytes(reinterpret_cast<unsigned char *>(buffer.buffer()), static_cast<int32_t>(length)) <= 0) {
    return false;
  }
  return buffer;
}


struct ssl_connection {
  int sock;
  bool is_blocking;

  SSL *ssl_handle;
  SSL_CTX *ssl_ctx;
};

static char ssl_connections_storage[sizeof(array<ssl_connection>)];
static array<ssl_connection> *ssl_connections = reinterpret_cast <array<ssl_connection> *> (ssl_connections_storage);
static long long ssl_connections_last_query_num = -1;


const int DEFAULT_SOCKET_TIMEOUT = 60;

static Stream ssl_stream_socket_client(const string &url, int64_t &error_number, string &error_description, double timeout, int64_t flags __attribute__((unused)), const mixed &options) {
#define RETURN(dump_error_stack)                        \
  if (dump_error_stack) {                               \
    php_warning ("%s: %s", error_description.c_str(),   \
                           ssl_get_error_string());     \
  } else {                                              \
    php_warning ("%s", error_description.c_str());      \
  }                                                     \
  if (ssl_handle != nullptr) {                             \
    SSL_free (ssl_handle);                              \
  }                                                     \
  if (ssl_ctx != nullptr) {                                \
    SSL_CTX_free (ssl_ctx);                             \
  }                                                     \
  if (sock != -1) {                                     \
    close (sock);                                       \
  }                                                     \
  dl::leave_critical_section();                         \
  return false

#define RETURN_ERROR(dump_error_stack, error_no, error) \
  error_number = error_no;                              \
  error_description = CONST_STRING(error);              \
  RETURN(dump_error_stack)

#define RETURN_ERROR_FORMAT(dump_error_stack, error_no, format, ...) \
  error_number = error_no;                                           \
  error_description = f$sprintf (                                    \
    CONST_STRING(format), array<mixed>::create(__VA_ARGS__));          \
  RETURN(dump_error_stack)

  if (timeout < 0) {
    timeout = DEFAULT_SOCKET_TIMEOUT;
  }
  double end_time = microtime_monotonic() + timeout;

  int sock = -1;

  SSL *ssl_handle = nullptr;
  SSL_CTX *ssl_ctx = nullptr;

  mixed parsed_url = f$parse_url(url);
  string host = f$strval(parsed_url.get_value(string("host", 4)));
  int64_t port = f$intval(parsed_url.get_value(string("port", 4)));

  //getting connection options
#define GET_OPTION(option_name) options.get_value (string (option_name, sizeof (option_name) - 1))
  bool verify_peer = GET_OPTION("verify_peer").to_bool();
  mixed verify_depth_var = GET_OPTION("verify_depth");
  int64_t verify_depth = verify_depth_var.to_int();
  if (verify_depth_var.is_null()) {
    verify_depth = -1;
  }
  string cafile = GET_OPTION("cafile").to_string();
  string capath = GET_OPTION("capath").to_string();
  string cipher_list = GET_OPTION("ciphers").to_string();
  string local_cert = GET_OPTION("local_cert").to_string();
  Optional<string> local_cert_file_path = local_cert.empty() ? false : f$realpath(local_cert);
#undef GET_OPTION

  if (host.empty()) {
    RETURN_ERROR_FORMAT(false, -1, "Wrong host specified in url \"%s\"", url);
  }

  if (port <= 0 || port >= 65536) {
    RETURN_ERROR_FORMAT(false, -2, "Wrong port specified in url \"%s\"", url);
  }

  struct hostent *h;
  if (!(h = kdb_gethostbyname(host.c_str())) || !h->h_addr_list || !h->h_addr_list[0]) {
    RETURN_ERROR_FORMAT(false, -3, "Can't resolve host \"%s\"", host);
  }

  struct sockaddr_storage addr;
  addr.ss_family = static_cast<sa_family_t>(h->h_addrtype);
  int addrlen;
  switch (h->h_addrtype) {
    case AF_INET:
      php_assert (h->h_length == 4);
      (reinterpret_cast <struct sockaddr_in *> (&addr))->sin_port = htons(static_cast<uint16_t>(port));
      (reinterpret_cast <struct sockaddr_in *> (&addr))->sin_addr = *(struct in_addr *)h->h_addr;
      addrlen = sizeof(struct sockaddr_in);
      break;
    case AF_INET6:
      php_assert (h->h_length == 16);
      (reinterpret_cast <struct sockaddr_in6 *> (&addr))->sin6_port = htons(static_cast<uint16_t>(port));
      memcpy(&(reinterpret_cast <struct sockaddr_in6 *> (&addr))->sin6_addr, h->h_addr, h->h_length);
      addrlen = sizeof(struct sockaddr_in6);
      break;
    default:
      //there is no other known address types
      php_assert (0);
  }


  dl::enter_critical_section();//OK
  sock = socket(h->h_addrtype, SOCK_STREAM, 0);
  if (sock == -1) {
    RETURN_ERROR(false, -4, "Can't create tcp socket");
  }

  php_assert (fcntl(sock, F_SETFL, O_NONBLOCK) == 0);

  if (connect(sock, reinterpret_cast <const sockaddr *> (&addr), addrlen) == -1) {
    if (errno != EINPROGRESS) {
      RETURN_ERROR(false, -5, "Can't connect to tcp socket");
    }

    pollfd poll_fds;
    poll_fds.fd = sock;
    poll_fds.events = POLLIN | POLLERR | POLLHUP | POLLOUT | POLLPRI;

    double left_time = end_time - microtime_monotonic();
    if (left_time <= 0 || poll(&poll_fds, 1, timeout_convert_to_ms(left_time)) <= 0) {
      RETURN_ERROR(false, -6, "Can't connect to tcp socket");
    }
  }

  ERR_clear_error();

  ssl_ctx = SSL_CTX_new(TLSv1_client_method());
  if (ssl_ctx == nullptr) {
    RETURN_ERROR(true, -7, "Failed to create an SSL context");
  }
//  SSL_CTX_set_options (ssl_ctx, SSL_OP_ALL); don't want others bugs workarounds
  SSL_CTX_set_mode (ssl_ctx, SSL_MODE_AUTO_RETRY | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER | SSL_MODE_ENABLE_PARTIAL_WRITE);

  if (verify_peer) {
    SSL_CTX_set_verify(ssl_ctx, SSL_VERIFY_PEER, nullptr);

    if (verify_depth != -1) {
      SSL_CTX_set_verify_depth(ssl_ctx, static_cast<int32_t>(verify_depth));
    }

    if (cafile.empty() && capath.empty()) {
      SSL_CTX_set_default_verify_paths(ssl_ctx);
    } else {
      if (SSL_CTX_load_verify_locations(ssl_ctx, cafile.empty() ? nullptr : cafile.c_str(), capath.empty() ? nullptr : capath.c_str()) == 0) {
        RETURN_ERROR_FORMAT(true, -8, "Failed to load verify locations \"%s\" and \"%s\"", cafile, capath);
      }
    }
  } else {
    SSL_CTX_set_verify(ssl_ctx, SSL_VERIFY_NONE, nullptr);
  }

  if (SSL_CTX_set_cipher_list(ssl_ctx, cipher_list.empty() ? "DEFAULT" : cipher_list.c_str()) == 0) {
    RETURN_ERROR_FORMAT(true, -9, "Failed to set cipher list \"%s\"", cipher_list);
  }

  if (f$boolval(local_cert_file_path)) {
    if (SSL_CTX_use_certificate_chain_file(ssl_ctx, local_cert_file_path.val().c_str()) != 1) {
      RETURN_ERROR_FORMAT(true, -10, "Failed to use certificate from local_cert \"%s\"", local_cert);
    }

    if (SSL_CTX_use_PrivateKey_file(ssl_ctx, local_cert_file_path.val().c_str(), SSL_FILETYPE_PEM) != 1) {
      RETURN_ERROR_FORMAT(true, -11, "Failed to use private key from local_cert \"%s\"", local_cert);
    }

    if (SSL_CTX_check_private_key(ssl_ctx) != 1) {
      RETURN_ERROR(true, -12, "Private key doesn't match certificate");
    }
  }

  ssl_handle = SSL_new(ssl_ctx);
  if (ssl_handle == nullptr) {
    RETURN_ERROR(true, -13, "Failed to create an SSL handle");
  }

  if (!SSL_set_fd(ssl_handle, sock)) {
    RETURN_ERROR(true, -14, "Failed to set fd");
  }

#if OPENSSL_VERSION_NUMBER >= 0x0090806fL && !defined(OPENSSL_NO_TLSEXT)
  SSL_set_tlsext_host_name (ssl_handle, host.c_str());
#endif

  SSL_set_connect_state(ssl_handle); //TODO remove

  while (true) {
    int connect_result = SSL_connect(ssl_handle);
    if (connect_result == 1) {
      if (verify_peer) {
        X509 *peer_cert = SSL_get_peer_certificate(ssl_handle);
        if (peer_cert == nullptr) {
          SSL_shutdown(ssl_handle);
          RETURN_ERROR(false, -15, "Failed to get peer certificate");
        }

        X509_free(peer_cert);
        php_assert (SSL_get_verify_result(ssl_handle) == X509_V_OK);
      }

      break;
    }

    int error = SSL_get_error(ssl_handle, connect_result);
    if (error != SSL_ERROR_WANT_READ && error != SSL_ERROR_WANT_WRITE) {
      RETURN_ERROR(true, -16, "Failed to do SSL_connect");
    }

    pollfd poll_fds;
    poll_fds.fd = sock;
    poll_fds.events = POLLIN | POLLERR | POLLHUP | POLLOUT | POLLPRI;

    double left_time = end_time - microtime_monotonic();
    if (left_time <= 0 || poll(&poll_fds, 1, timeout_convert_to_ms(left_time)) <= 0) {
      RETURN_ERROR(false, -17, "Can't establish SSL connect due to timeout");
    }
  }

  php_assert (fcntl(sock, F_SETFL, 0) == 0);
  dl::leave_critical_section();

  if (dl::query_num != ssl_connections_last_query_num) {
    new(ssl_connections_storage) array<ssl_connection>();

    ssl_connections_last_query_num = dl::query_num;
  }

  ssl_connection result;
  result.sock = sock;
  result.is_blocking = true;

  result.ssl_handle = ssl_handle;
  result.ssl_ctx = ssl_ctx;

  string stream_key = url;
  if (ssl_connections->has_key(stream_key)) {
    int try_count;
    for (try_count = 0; try_count < 3; try_count++) {
      stream_key = url;
      stream_key.append("#", 1);
      stream_key.append(string(rand()));

      if (!ssl_connections->has_key(stream_key)) {
        break;
      }
    }

    if (try_count == 3) {
      SSL_shutdown(ssl_handle);
      RETURN_ERROR(false, -18, "Can't generate stream_name in 3 tries. Something is definitely wrong.");
    }
  }

  dl::enter_critical_section();//NOT OK: ssl_connections
  ssl_connections->set_value(stream_key, result);
  dl::leave_critical_section();

  return stream_key;
#undef RETURN
#undef RETURN_ERROR
#undef RETURN_ERROR_FORMAT
}

static bool ssl_context_set_option(mixed &context_ssl, const string &option, const mixed &value) {
  if (STRING_EQUALS(option, "verify_peer") ||
      STRING_EQUALS(option, "verify_depth") ||
      STRING_EQUALS(option, "cafile") ||
      STRING_EQUALS(option, "capath") ||
      STRING_EQUALS(option, "local_cert") ||
      STRING_EQUALS(option, "ciphers")) {
    context_ssl[option] = value;
    return true;
  }

  php_warning("Option \"%s\" for wrapper ssl is not supported", option.c_str());
  return false;
}

ssl_connection *get_connection(const Stream &stream) {
  if (dl::query_num != ssl_connections_last_query_num || !ssl_connections->has_key(stream.to_string())) {
    php_warning("Connection to \"%s\" not found", stream.to_string().c_str());
    return nullptr;
  }

  return &(*ssl_connections)[stream.to_string()];
}

static void ssl_do_shutdown(ssl_connection *c) {
  php_assert (dl::in_critical_section > 0);

  if (c->sock == -1) {
    return;
  }

  if (!c->is_blocking) {
    php_assert (fcntl(c->sock, F_SETFL, 0) == 0);
  }

  php_assert (c->ssl_ctx != nullptr);
  php_assert (c->ssl_handle != nullptr);

  SSL_shutdown(c->ssl_handle);
  SSL_free(c->ssl_handle);
  SSL_CTX_free(c->ssl_ctx);
  close(c->sock);

  c->sock = -1;
  c->ssl_ctx = nullptr;
  c->ssl_handle = nullptr;
}

static bool process_ssl_error(ssl_connection *c, int result) {
  php_assert (dl::in_critical_section > 0);

  int error = SSL_get_error(c->ssl_handle, result);
  switch (error) {
    case SSL_ERROR_NONE:
      php_assert (0);
      return false;
    case SSL_ERROR_ZERO_RETURN:
      if (c->sock != -1) {
        ssl_do_shutdown(c);
      } else {
        php_warning("SSL connection is already closed");
      }
      return false;
    case SSL_ERROR_WANT_READ:
    case SSL_ERROR_WANT_WRITE:
      return true;
    case SSL_ERROR_WANT_CONNECT:
    case SSL_ERROR_WANT_ACCEPT:
    case SSL_ERROR_WANT_X509_LOOKUP:
      php_assert (0);
      return false;

    case SSL_ERROR_SYSCALL:
      if (ERR_peek_error() == 0) {
        if (result != 0) {
          if (errno == EAGAIN) {
            return true;
          }

          php_warning("SSL gets error %d: %s", errno, strerror(errno));
        } else {
          //socket was closed from other side, not an error
          ssl_do_shutdown(c);
        }
        return false;
      }
      /* fall through */
    default:
      php_warning("SSL gets error %d: %s", error, ssl_get_error_string());
      ssl_do_shutdown(c);
      return false;
  }
}

static Optional<int64_t> ssl_fwrite(const Stream &stream, const string &data) {
  ssl_connection *c = get_connection(stream);
  if (c == nullptr || c->sock == -1) {
    return false;
  }

  const void *data_ptr = static_cast <const void *> (data.c_str());
  int data_len = static_cast <int> (data.size());
  if (data_len == 0) {
    return 0;
  }

  dl::enter_critical_section();//OK
  ERR_clear_error();
  int written = SSL_write(c->ssl_handle, data_ptr, data_len);

  if (written <= 0) {
    bool ok = process_ssl_error(c, written);
    if (ok) {
      dl::leave_critical_section();
      php_assert (!c->is_blocking);//because of SSL_MODE_AUTO_RETRY
      return 0;
    }
    dl::leave_critical_section();
    return false;
  } else {
    dl::leave_critical_section();
    if (c->is_blocking) {
      php_assert (written == data_len);
    } else {
      php_assert (written <= data_len);
    }
    return written;
  }
}

static Optional<string> ssl_fread(const Stream &stream, int64_t length) {
  if (length <= 0) {
    php_warning("Parameter length in function fread must be positive");
    return false;
  }
  if (length > string::max_size()) {
    php_warning("Parameter length in function fread is too large");
    return false;
  }

  ssl_connection *c = get_connection(stream);
  if (c == nullptr || c->sock == -1) {
    return false;
  }

  string res(static_cast<string::size_type>(length), false);
  dl::enter_critical_section();//OK
  ERR_clear_error();
  int res_size = SSL_read(c->ssl_handle, &res[0], static_cast<int32_t>(length));
  if (res_size <= 0) {
    bool ok = process_ssl_error(c, res_size);
    if (ok) {
      dl::leave_critical_section();
      php_assert (!c->is_blocking);//because of SSL_MODE_AUTO_RETRY

      return string();
    }
    dl::leave_critical_section();
    return false;
  } else {
    dl::leave_critical_section();
    php_assert (res_size <= length);
    res.shrink(static_cast<string::size_type>(res_size));
    return res;
  }
}

static bool ssl_feof(const Stream &stream) {
  ssl_connection *c = get_connection(stream);
  if (c == nullptr || c->sock == -1) {
    return true;
  }

  pollfd poll_fds;
  poll_fds.fd = c->sock;
  poll_fds.events = POLLIN | POLLERR | POLLHUP | POLLPRI;

  if (poll(&poll_fds, 1, 0) <= 0) {
    return false;//nothing to read for now
  }

  dl::enter_critical_section();//OK
  char b;
  ERR_clear_error();
  int res_size = SSL_peek(c->ssl_handle, &b, 1);
  if (res_size <= 0) {
    bool ok = process_ssl_error(c, res_size);
    if (ok) {
      dl::leave_critical_section();
      return false;
    }
    dl::leave_critical_section();

    return true;
  } else {
    dl::leave_critical_section();

    return false;
  }
}

static bool ssl_fclose(const Stream &stream) {
  const string &stream_key = stream.to_string();
  if (dl::query_num != ssl_connections_last_query_num || !ssl_connections->has_key(stream_key)) {
    return false;
  }

  dl::enter_critical_section();//NOT OK: ssl_connections
  ssl_do_shutdown(&(*ssl_connections)[stream_key]);
  ssl_connections->unset(stream_key);
  dl::leave_critical_section();
  return true;
}

bool ssl_stream_set_option(const Stream &stream, int64_t option, int64_t value) {
  ssl_connection *c = get_connection(stream);
  if (c == nullptr) {
    return false;
  }

  switch (option) {
    case STREAM_SET_BLOCKING_OPTION: {
      bool is_blocking = value;
      if (c->is_blocking != is_blocking) {
        dl::enter_critical_section();//OK
        if (is_blocking) {
          php_assert (fcntl(c->sock, F_SETFL, 0) == 0);
        } else {
          php_assert (fcntl(c->sock, F_SETFL, O_NONBLOCK) == 0);
        }
        dl::leave_critical_section();

        c->is_blocking = is_blocking;
      }
      return true;
    }
    case STREAM_SET_WRITE_BUFFER_OPTION:
    case STREAM_SET_READ_BUFFER_OPTION:
      if (value != 0) {
//TODO
//        BIO_set_read_buffer_size (SSL_get_rbio (c->ssl_handle), value);
//        BIO_set_write_buffer_size (SSL_get_wbio (c->ssl_handle), value);

        php_warning("SSL wrapper doesn't support buffered input/output");
        return false;
      }
      return true;
    default:
      php_assert (0);
  }
  return false;
}

static int ssl_get_fd(const Stream &stream) {
  ssl_connection *c = get_connection(stream);
  if (c == nullptr) {
    return -1;
  }

  if (c->sock == -1) {
    php_warning("Connection to \"%s\" already closed", stream.to_string().c_str());
  }
  return c->sock;
}

void global_init_openssl_lib() {
  static stream_functions ssl_stream_functions;

  ssl_stream_functions.name = string("ssl", 3);
  ssl_stream_functions.fopen = nullptr;
  ssl_stream_functions.fwrite = ssl_fwrite;
  ssl_stream_functions.fseek = nullptr;
  ssl_stream_functions.ftell = nullptr;
  ssl_stream_functions.fread = ssl_fread;
  ssl_stream_functions.fgetc = nullptr;
  ssl_stream_functions.fgets = nullptr;
  ssl_stream_functions.fpassthru = nullptr;
  ssl_stream_functions.fflush = nullptr;
  ssl_stream_functions.feof = ssl_feof;
  ssl_stream_functions.fclose = ssl_fclose;

  ssl_stream_functions.file_get_contents = nullptr;
  ssl_stream_functions.file_put_contents = nullptr;

  ssl_stream_functions.stream_socket_client = ssl_stream_socket_client;
  ssl_stream_functions.context_set_option = ssl_context_set_option;
  ssl_stream_functions.stream_set_option = ssl_stream_set_option;
  ssl_stream_functions.get_fd = ssl_get_fd;

  register_stream_functions(&ssl_stream_functions, false);

  reinit_openssl_lib_hack();
}

// TODO: When we'll fix curl, inline this function inside the global_init_openssl_lib
void reinit_openssl_lib_hack() {
  OPENSSL_config(nullptr);
  SSL_library_init();
  OpenSSL_add_all_ciphers();
  OpenSSL_add_all_digests();
  OpenSSL_add_all_algorithms();

  SSL_load_error_strings();
}

void free_openssl_lib() {
  dl::enter_critical_section(); //OK
  public_keys.free_resources();
  private_keys.free_resources();

  if (dl::query_num == ssl_connections_last_query_num) {
    const array<ssl_connection> *const_ssl_connections = ssl_connections;
    for (array<ssl_connection>::const_iterator p = const_ssl_connections->begin(); p != const_ssl_connections->end(); ++p) {
      ssl_connection c = p.get_value();
      ssl_do_shutdown(&c);
    }
    ssl_connections_last_query_num--;
  }
  dl::leave_critical_section();
}

namespace {

void x509_stack_pop_free(STACK_OF(X509) *stack) {
  if (stack) {
    sk_X509_pop_free(stack, X509_free);
  }
}

// wrap this function, because in openssl 1.0 it is a macros
void x509_stack_empty_free(STACK_OF(X509) *stack) {
  if (stack) {
    sk_X509_free(stack);
  }
}

// wrap this function, because in openssl 1.0 it is a macros
void x509_stack_info_free(STACK_OF(X509_INFO) *stack) {
  if (stack) {
    sk_X509_INFO_free(stack);
  }
}

}

using BIO_ptr = vk::unique_ptr_with_delete_function<BIO, BIO_vfree>;
using PKCS7_ptr = vk::unique_ptr_with_delete_function<PKCS7, PKCS7_free>;
using X509_ptr = vk::unique_ptr_with_delete_function<X509, X509_free>;
using X509_info_stack_ptr = vk::unique_ptr_with_delete_function<STACK_OF(X509_INFO), x509_stack_info_free>;
using X509_empty_stack_ptr = vk::unique_ptr_with_delete_function<STACK_OF(X509), x509_stack_empty_free>;
using X509_stack_ptr = vk::unique_ptr_with_delete_function<STACK_OF(X509), x509_stack_pop_free>;

class X509_parser {
private:
  using ASN1_TIME_ptr = vk::unique_ptr_with_delete_function<ASN1_TIME, ASN1_TIME_free>;
  using X509_STORE_ptr = vk::unique_ptr_with_delete_function<X509_STORE, X509_STORE_free>;
  using X509_STORE_CTX_ptr = vk::unique_ptr_with_delete_function<X509_STORE_CTX, X509_STORE_CTX_free>;

public:
  explicit X509_parser(const string &data) :
    x509_(get_x509_from_data(data)) {}

  array<mixed> get_subject(bool shortnames = true) const {
    php_assert(x509_.get());

    X509_NAME *subj = X509_get_subject_name(x509_.get());

    return get_entries_of(subj, shortnames);
  }

  string get_subject_name() const {
    php_assert(x509_.get());

    auto issuer_name = X509_get_subject_name(x509_.get());
    auto issuer = X509_NAME_oneline(issuer_name, nullptr, 0);

    string res(issuer);
    OPENSSL_free(issuer);

    return res;
  }

  string get_hash() const {
    php_assert(x509_.get());

    static char buf[9];
    snprintf(buf, sizeof(buf), "%08lx", X509_subject_name_hash(x509_.get()));

    return string(buf, 8);
  }

  array<mixed> get_issuer(bool shortnames = true) const {
    php_assert(x509_.get());

    X509_NAME *issuer_name = X509_get_issuer_name(x509_.get());

    return get_entries_of(issuer_name, shortnames);
  }

  int get_version() const {
    php_assert(x509_.get());

    auto version = static_cast<int>(X509_get_version(x509_.get()));
    return version;
  }

  int get_time_not_before() const {
    php_assert(x509_.get());

    auto asn_time_not_before = X509_getm_notBefore(x509_.get());
    return (int)convert_asn1_time(asn_time_not_before);
  }

  int get_time_not_after() const {
    php_assert(x509_.get());

    auto asn_time_not_after = X509_getm_notAfter(x509_.get());
    return (int)convert_asn1_time(asn_time_not_after);
  }

  array<mixed> get_purposes(bool shortnames = true) const {
    php_assert(x509_.get());

    array<mixed> res;
    for (int i = 0; i < X509_PURPOSE_get_count(); i++) {
      array<mixed> purposes;

      X509_PURPOSE *purp = X509_PURPOSE_get0(i);
      int id = X509_PURPOSE_get_id(purp);

      auto purpset = static_cast<bool>(X509_check_purpose(x509_.get(), id, 0));
      purposes.push_back(purpset);

      purpset = static_cast<bool>(X509_check_purpose(x509_.get(), id, 1));
      purposes.push_back(purpset);

      char *pname = shortnames ? X509_PURPOSE_get0_sname(purp) : X509_PURPOSE_get0_name(purp);
      purposes.push_back(string(pname));

      res.set_value(id, purposes);
    }

    return res;
  }

  Optional<array<mixed>> parse(bool shortnames = true) const {
    if (!x509_) {
      return false;
    }

    return std::initializer_list<std::pair<string, mixed>>
      {{string("name"),             get_subject_name()},
       {string("subject"),          get_subject(shortnames)},
       {string("hash"),             get_hash()},
       {string("issuer"),           get_issuer(shortnames)},
       {string("version"),          get_version()},
       {string("validFrom_time_t"), get_time_not_before()},
       {string("validTo_time_t"),   get_time_not_after()},
       {string("purposes"),         get_purposes(shortnames)}};
  }

  mixed check_purpose(int64_t purpose) const {
    if (!x509_) {
      return -1;
    }

    X509_STORE_CTX_ptr csc{X509_STORE_CTX_new()};

    if (csc == nullptr) {
      return -1;
    }
    X509_STORE_ptr store{X509_STORE_new()};
    if (store == nullptr) {
      return -1;
    }

    X509_STORE_set_default_paths(store.get());

    if (!X509_STORE_CTX_init(csc.get(), store.get(), x509_.get(), nullptr)) {
      return -1;
    }
    // return value is not checked, as in php
    X509_STORE_CTX_set_purpose(csc.get(), static_cast<int32_t>(purpose));
    int ret = X509_verify_cert(csc.get());
    if (ret != 0 && ret != 1) {
      return ret;
    }
    return static_cast<bool>(ret);
  }

private:
  X509_ptr get_x509_from_data(const string &data) {
    BIO_ptr certBio{BIO_new(BIO_s_mem())};

    if (!certBio) {
      return nullptr;
    }

    bool write_success = BIO_write(certBio.get(), data.c_str(), static_cast<int>(data.size())) != 0;

    if (!write_success) {
      return nullptr;
    }

    return X509_ptr(PEM_read_bio_X509(certBio.get(), nullptr, nullptr, nullptr));
  }

  array<mixed> get_entries_of(X509_NAME *name, bool shortnames = true) const {
    array<mixed> res;

    int count_of_entries = X509_NAME_entry_count(name);

    for (int i = 0; i < count_of_entries; i++) {
      X509_NAME_ENTRY *entry = X509_NAME_get_entry(name, i);

      ASN1_OBJECT *key = X509_NAME_ENTRY_get_object(entry);
      ASN1_STRING *value = X509_NAME_ENTRY_get_data(entry);

      int nid = OBJ_obj2nid(key);
      const char *key_str = shortnames ? OBJ_nid2sn(nid) : OBJ_nid2ln(nid);

      auto value_str = reinterpret_cast<const char *>(ASN1_STRING_get0_data(value));
      int value_len = ASN1_STRING_length(value);

      res.set_value(string(key_str), string(value_str, value_len));
    }

    return res;
  }

  time_t convert_asn1_time(ASN1_TIME *expires) const {
    ASN1_TIME_ptr epoch{ASN1_TIME_new()};
    ASN1_TIME_set(epoch.get(), 0);
    int days = 0;
    int seconds = 0;
    ASN1_TIME_diff(&days, &seconds, epoch.get(), expires);

    return (days * 24 * 60 * 60) + seconds;
  }


private:
  X509_ptr x509_;
};

Optional<array<mixed>> f$openssl_x509_parse(const string &data, bool shortnames /* =true */) {
  return X509_parser(data).parse(shortnames);
}

mixed f$openssl_x509_checkpurpose(const string &data, int64_t purpose) {
  return X509_parser(data).check_purpose(purpose);
}

namespace {

X509_stack_ptr read_x509_cert_stack_from_file(const string &extra_certs) {
  X509_empty_stack_ptr stack{sk_X509_new_null()};
  if (!stack) {
    php_warning("Can't allocate memory for extra certs:\n%s", ssl_get_error_string());
    return {};
  }

  BIO_ptr in{BIO_new_file(extra_certs.c_str(), "rb")};
  if (!in) {
    php_warning("Can't open extra certs file '%s':\n%s", extra_certs.c_str(), ssl_get_error_string());
    return {};
  }

  X509_info_stack_ptr sk{PEM_X509_INFO_read_bio(in.get(), nullptr, nullptr, nullptr)};
  if (!sk) {
    php_warning("Can't read certs file '%s':\n%s", extra_certs.c_str(), ssl_get_error_string());
    return {};
  }

  while (sk_X509_INFO_num(sk.get())) {
    X509_INFO *xi = sk_X509_INFO_shift(sk.get());
    if (xi->x509) {
      sk_X509_push(stack.get(), xi->x509);
      xi->x509 = nullptr;
    }
    X509_INFO_free(xi);
  }

  if (!sk_X509_num(stack.get())) {
    php_warning("Can't find any extra certs in file '%s':\n%s", extra_certs.c_str(), ssl_get_error_string());
    return {};
  }
  return X509_stack_ptr{stack.release()};
}

X509_ptr read_x509_cert(const string &cert_str) noexcept {
  vk::string_view file_prefilx{"file://"};
  if (vk::string_view{cert_str.c_str(), cert_str.size()}.starts_with(file_prefilx)) {
    if (BIO_ptr in{BIO_new_file(cert_str.c_str() + file_prefilx.size(), "rb")}) {
      return X509_ptr{PEM_read_bio_X509(in.get(), nullptr, nullptr, nullptr)};
    }
  } else if (BIO_ptr in{BIO_new_mem_buf(const_cast<char *>(cert_str.c_str()), static_cast<int32_t>(cert_str.size()))}) {
    return X509_ptr{static_cast<X509 *>(PEM_ASN1_read_bio(
      reinterpret_cast<d2i_of_void *>(d2i_X509), PEM_STRING_X509, in.get(),
      nullptr, nullptr, nullptr))
    };
  }
  return {};
}

} // namespace

bool f$openssl_pkcs7_sign(const string &infile, const string &outfile,
                          const string &sign_cert, const string &priv_key,
                          const array<string> &headers, int64_t flags, const string &extra_certs) noexcept {
  dl::CriticalSectionGuard critical_section;
  X509_stack_ptr other_certs;
  if (!extra_certs.empty()) {
    other_certs = read_x509_cert_stack_from_file(extra_certs);
    if (!other_certs) {
      return false;
    }
  }

  bool from_cache = false;
  EVP_PKEY *privkey = openssl_get_private_evp(priv_key, string{}, from_cache);
  if (privkey == nullptr) {
    php_warning("Parameter key cannot be converted into a private key");
    return false;
  }

  auto priv_key_free = vk::finally([from_cache, privkey] {
    if (!from_cache) {
      EVP_PKEY_free(privkey);
    }
  });

  X509_ptr cert = read_x509_cert(sign_cert);
  if (!cert) {
    php_warning("Can't read sign cert:\n%s", ssl_get_error_string());
    return false;
  }

  BIO_ptr infile_bio{BIO_new_file(infile.c_str(), flags & PKCS7_BINARY ? "rb" : "r")};
  if (!infile_bio) {
    php_warning("Can't open input file:\n%s", ssl_get_error_string());
    return false;
  }

  BIO_ptr outfile_bio{BIO_new_file(outfile.c_str(), "wb")};
  if (!outfile_bio) {
    php_warning("Can't open output file:\n%s", ssl_get_error_string());
    return false;
  }

  PKCS7_ptr p7{PKCS7_sign(cert.get(), privkey, other_certs.get(), infile_bio.get(), static_cast<int32_t>(flags))};
  if (!p7) {
    php_warning("Can't create PKCS7 structure:\n%s", ssl_get_error_string());
    return false;
  }

  static_cast<void>(BIO_reset(infile_bio.get()));

  for (auto header : headers) {
    const int ret = header.is_string_key()
                    ? BIO_printf(outfile_bio.get(), "%s: %s\n", header.get_string_key().c_str(), header.get_value().c_str())
                    : BIO_printf(outfile_bio.get(), "%s\n", header.get_value().c_str());
    if (ret < 0) {
      php_warning("Can't add header:\n%s", ssl_get_error_string());
    }
  }

  if (!SMIME_write_PKCS7(outfile_bio.get(), p7.get(), infile_bio.get(), static_cast<int32_t>(flags))) {
    php_warning("Can't write signed data into output file:\n%s", ssl_get_error_string());
    return false;
  }
  return true;
}

namespace {

enum cipher_opts : int {
  OPENSSL_RAW_DATA = 1,
  OPENSSL_ZERO_PADDING = 2,
  OPENSSL_DONT_ZERO_PAD_KEY = 4
};

struct CipherCtx {
  enum cipher_action {
    decrypt = 0,
    encrypt = 1
  };

  CipherCtx(const string &method, int64_t options, cipher_action action) :
    options_(options),
    action_(action) {
    type_ = EVP_get_cipherbyname(method.c_str());
    if (!type_) {
      php_warning("Unknown cipher algorithm '%s'", method.c_str());
      return;
    }
    const int mode = EVP_CIPHER_mode(type_);
    if (mode == EVP_CIPH_GCM_MODE) {
      is_aead_ = true;
      is_single_run_aead_ = false;
      aead_get_tag_flag_ = EVP_CTRL_GCM_GET_TAG;
      aead_set_tag_flag_ = EVP_CTRL_GCM_SET_TAG;
      aead_ivlen_flag_ = EVP_CTRL_GCM_SET_IVLEN;
    } else if (mode == EVP_CIPH_CCM_MODE) {
      is_aead_ = true;
      is_single_run_aead_ = true;
      aead_get_tag_flag_ = EVP_CTRL_CCM_GET_TAG;
      aead_set_tag_flag_ = EVP_CTRL_CCM_SET_TAG;
      aead_ivlen_flag_ = EVP_CTRL_CCM_SET_IVLEN;
    }

    ctx_ = EVP_CIPHER_CTX_new();
    if (!ctx_) {
      php_warning("Failed to create cipher context");
    }
  }

  string flush_result() {
    return std::move(out_);
  };

  explicit operator bool() const { return type_ && ctx_; }

  bool init(string key, string iv, string &tag) {
    const size_t max_iv_len = static_cast<size_t>(EVP_CIPHER_iv_length(type_));
    if (action_ == encrypt && iv.empty() && max_iv_len > 0) {
      php_warning("Using an empty Initialization Vector (iv) is potentially insecure and not recommended");
    }

    if (!EVP_CipherInit_ex(ctx_, type_, nullptr, nullptr, nullptr, action_)) {
      php_warning("Cipher init error:\n%s", ssl_get_error_string());
      return false;
    }

    if (!align_iv(iv, max_iv_len)) {
      return false;
    }

    if (is_single_run_aead_ && action_ == cipher_action::encrypt) {
      if (!EVP_CIPHER_CTX_ctrl(ctx_, aead_set_tag_flag_, static_cast<int32_t>(tag.size()), nullptr)) {
        php_warning("Setting tag length for AEAD cipher failed");
        return false;
      }
    } else if (action_ == cipher_action::decrypt && !tag.empty()) {
      if (!is_aead_) {
        php_warning("The tag cannot be used because the cipher method does not support AEAD");
      } else if (!EVP_CIPHER_CTX_ctrl(ctx_, aead_set_tag_flag_, static_cast<int32_t>(tag.size()), tag.buffer())) {
        php_warning("Setting tag for AEAD cipher decryption failed");
        return false;
      }
    }

    const int cipher_key_len = EVP_CIPHER_key_length(type_);
    if (cipher_key_len > key.size()) {
      if ((OPENSSL_DONT_ZERO_PAD_KEY & options_) && !EVP_CIPHER_CTX_set_key_length(ctx_, key.size())) {
        php_warning("Key length cannot be set for the cipher method:\n%s", ssl_get_error_string());
        return false;
      }
      key.append(cipher_key_len - key.size(), '\0');
    } else if (key.size() > cipher_key_len && !EVP_CIPHER_CTX_set_key_length(ctx_, key.size())) {
      php_warning("Key length cannot be set for the cipher method:\n%s", ssl_get_error_string());
    }

    if (!EVP_CipherInit_ex(ctx_, nullptr, nullptr,
                           reinterpret_cast<const unsigned char *>(key.c_str()),
                           reinterpret_cast<const unsigned char *>(iv.c_str()), action_)) {
      php_warning("Cipher init error:\n%s", ssl_get_error_string());
      return false;
    }
    if (options_ & OPENSSL_ZERO_PADDING) {
      EVP_CIPHER_CTX_set_padding(ctx_, 0);
    }
    return true;
  }

  bool update(const string &data, const string &aad) {
    if (is_single_run_aead_ && !EVP_CipherUpdate(ctx_, nullptr, &out_len_, nullptr, static_cast<int32_t>(data.size()))) {
      php_warning("Setting of data length failed:\n%s", ssl_get_error_string());
      return false;
    }

    if (is_aead_ && !EVP_CipherUpdate(ctx_, nullptr, &out_len_, reinterpret_cast<const unsigned char *>(aad.c_str()), static_cast<int32_t>(aad.size()))) {
      php_warning("Setting of additional application data failed:\n%s", ssl_get_error_string());
      return false;
    }

    out_.assign(data.size() + EVP_CIPHER_block_size(type_), '\0');
    if (!EVP_CipherUpdate(ctx_, reinterpret_cast<unsigned char *>(out_.buffer()), &out_len_,
                          reinterpret_cast<const unsigned char *>(data.c_str()), static_cast<int>(data.size()))) {
      php_warning("Cipher update error:\n%s", ssl_get_error_string());
      return false;
    }
    return true;
  }

  bool finalize() {
    int i = 0;
    const int is_ok = action_ == encrypt
                      ? EVP_EncryptFinal(ctx_, reinterpret_cast<unsigned char *>(out_.buffer()) + out_len_, &i)
                      : EVP_DecryptFinal(ctx_, reinterpret_cast<unsigned char *>(out_.buffer()) + out_len_, &i);
    if (is_ok > 0) {
      out_.shrink(i + out_len_);
    } else {
      php_warning("Cipher finalize error:\n%s", ssl_get_error_string());
    }
    return is_ok;
  }

  bool make_tag(string &tag) {
    if (is_aead_ && !tag.empty()) {
      if (EVP_CIPHER_CTX_ctrl(ctx_, aead_get_tag_flag_, static_cast<int32_t>(tag.size()), tag.buffer()) == 1) {
        return true;
      }
      php_warning("Retrieving verification tag failed:\n%s", ssl_get_error_string());
      return false;
    } else if (!tag.empty()) {
      php_warning("The authenticated tag cannot be provided for cipher that doesn not support AEAD");
    } else if (is_aead_) {
      php_warning("A tag should be provided when using AEAD mode");
      return false;
    }
    tag = string{};
    return true;
  }

  ~CipherCtx() {
    if (ctx_) {
      EVP_CIPHER_CTX_cleanup(ctx_);
      EVP_CIPHER_CTX_free(ctx_);
    }
  }

private:
  bool align_iv(string &iv, size_t iv_required_len) {
    if (iv.size() == iv_required_len) {
      return true;
    }

    if (is_aead_) {
      if (EVP_CIPHER_CTX_ctrl(ctx_, aead_ivlen_flag_, static_cast<int32_t>(iv.size()), nullptr) != 1) {
        php_warning("Setting of IV length for AEAD mode failed");
        return false;
      }
      return true;
    }

    if (iv.empty()) {
      iv.assign(static_cast<std::uint32_t>(iv_required_len), '\0');
      return true;
    }
    if (iv.size() < iv_required_len) {
      php_warning("IV passed is only %d bytes long, cipher expects an IV of precisely %zd bytes, padding with \\0",
                  iv.size(), iv_required_len);
      iv.append(static_cast<string::size_type>(iv_required_len - iv.size()), '\0');
      return true;
    }
    // iv.size() > iv_required_len
    php_warning("IV passed is %d bytes long which is longer than the %zd expected by selected cipher, truncating",
                iv.size(), iv_required_len);
    iv.shrink(static_cast<string::size_type>(iv_required_len));
    return true;
  }

  const EVP_CIPHER *type_{nullptr};
  EVP_CIPHER_CTX *ctx_{nullptr};

  string out_;
  int out_len_{0};

  const int64_t options_{0};
  cipher_action action_;

  bool is_aead_{false};
  bool is_single_run_aead_{false};
  int aead_get_tag_flag_{0};
  int aead_set_tag_flag_{0};
  int aead_ivlen_flag_{0};
};

template<bool allow_alias>
void openssl_add_method(const OBJ_NAME *name, void *arg) {
  if (name->alias == 0 || allow_alias) {
    reinterpret_cast<array<string> *>(arg)->push_back(string{name->name});
  }
}

Optional<string> eval_cipher(CipherCtx::cipher_action action, const string &data, const string &method,
                             const string &key, int64_t options, const string &iv, string &tag, const string &aad) {
  CipherCtx cipher{method, options, action};
  if (cipher && cipher.init(key, iv, tag) && cipher.update(data, aad) && cipher.finalize()) {
    if (action == CipherCtx::cipher_action::encrypt && !cipher.make_tag(tag)) {
      return false;
    }
    return cipher.flush_result();
  }
  return false;
}
} // namespace

array<string> f$openssl_get_cipher_methods(bool aliases) {
  array<string> return_value;
  OBJ_NAME_do_all_sorted(OBJ_NAME_TYPE_CIPHER_METH,
                         aliases ? openssl_add_method<true> : openssl_add_method<false>,
                         &return_value);
  return return_value;
}

Optional<int64_t> f$openssl_cipher_iv_length(const string &method) {
  if (method.empty()) {
    php_warning("Unknown cipher algorithm");
    return false;
  }
  const EVP_CIPHER *cipher_type = EVP_get_cipherbyname(method.c_str());
  if (!cipher_type) {
    php_warning("Unknown cipher algorithm");
    return false;
  }
  return EVP_CIPHER_iv_length(cipher_type);
}

namespace impl_ {
string default_tag_stub;
} // namespace impl_
Optional<string> f$openssl_encrypt(const string &data, const string &method, const string &key, int64_t options,
                                   const string &iv, string &tag, const string &aad, int64_t tag_length) {
  string out_tag;
  if (&tag != &impl_::default_tag_stub) {
    out_tag.assign(static_cast<std::uint32_t>(tag_length), '\0');
  }
  auto result = eval_cipher(CipherCtx::encrypt, data, method, key, options, iv, out_tag, aad);
  if (!result.has_value()) {
    return false;
  }
  if (&tag != &impl_::default_tag_stub) {
    tag = std::move(out_tag);
  }
  return (options & OPENSSL_RAW_DATA)
         ? result
         : f$base64_encode(result.val());
}

Optional<string> f$openssl_decrypt(string data, const string &method, const string &key, int64_t options,
                                   const string &iv, string tag, const string &aad) {
  if (!(options & OPENSSL_RAW_DATA)) {
    Optional<string> decoding_data = f$base64_decode(data, true);
    if (!decoding_data.has_value()) {
      php_warning("Failed to base64 decode the input");
      return false;
    }
    data = std::move(decoding_data.val());
  }
  return eval_cipher(CipherCtx::decrypt, data, method, key, options, iv, tag, aad);
}
