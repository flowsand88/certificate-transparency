#include "../include/ct.h"
#include "../merkletree/LogRecord.h"
#include "../merkletree/LogVerifier.h"

#include <fstream>
#include <iostream>
#include <string>

#include <arpa/inet.h>
#include <assert.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

#include <openssl/asn1.h>
#include <openssl/bio.h>
#include <openssl/bn.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

static const char nibble[] = "0123456789abcdef";

static std::string HexString(const bstring &data) {
  std::string ret;
  for (unsigned int i = 0; i < data.size(); ++i) {
    ret.push_back(nibble[(data[i] >> 4) & 0xf]);
    ret.push_back(nibble[data[i] & 0xf]);
  }
  return ret;
}

static void UnknownCommand(const std::string &cmd) {
  std::cerr << "Unknown command: " << cmd << '\n';
}

static void ReadAll(bstring *contents, std::ifstream &in) {
  for ( ; ; ) {
    unsigned char buf[1024];

    size_t n = in.read(reinterpret_cast<char*>(buf), sizeof buf).gcount();
    assert(!in.fail() || in.eof());
    assert(n >= 0);
    contents->append(buf, n);
    if (in.eof())
      return;
  }
}

// Make the object that we use to recognize the extension.
static ASN1_OBJECT *ProofExtensionObject() {
  unsigned char obj_buf[100];
  char oid[] = "1.2.3.4";
  int obj_len = a2d_ASN1_OBJECT(obj_buf, sizeof obj_buf, oid,
                                sizeof oid - 1);
  assert(obj_len > 0);
  ASN1_OBJECT *obj = ASN1_OBJECT_create(0, obj_buf, obj_len, NULL, NULL);
  return obj;
}

static bool VerifyLogSegmentProof(const bstring &proofstring,
                                  const bstring &bundle,
                                  LogVerifier *verifier) {
  assert(verifier != NULL);
  AuditProof proof;
  bool verified = proof.Deserialize(
      SegmentData::LOG_SEGMENT_TREE,
      *(reinterpret_cast<const std::string*>(&proofstring)));
  if (!verified)
    return false;
  return
      verifier->VerifyLogSegmentAuditProof(proof,
                                            *(reinterpret_cast<const
                                              std::string*>(&bundle)));
}

// A generic client.
class CTClient {
public:
  CTClient(const char *server, uint16_t port) {
    fd_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd_ < 0) {
      perror("Socket creation failed");
      exit(3);
    }

    static struct sockaddr_in server_socket;
    memset(&server_socket, 0, sizeof server_socket);
    server_socket.sin_family = AF_INET;
    server_socket.sin_port = htons(port);
    if (inet_aton(server, &server_socket.sin_addr) != 1) {
      std::cerr << "Can't parse server address: " << server << '.' << std::endl;
      exit(5);
    }

    std::cout << "Connecting to " << server << ':' << port << '.' << std::endl;
    int ret = connect(fd_, (struct sockaddr *)&server_socket,
		      sizeof server_socket);
    if (ret < 0) {
      close(fd_);
      perror("Connect failed");
      exit(4);
    }
  }

  ~CTClient() {
    close(fd_);
  }

 protected:
  int fd() const { return fd_; }

 private:
  int fd_;
}; // class CTClient

// A client for talking to the log server.
class LogClient : public CTClient {
 public:
  LogClient(const char *server, uint16_t port) : CTClient(server, port) {}
  // struct {
  //   opaque bundle[ClientCommand.length];
  // } ClientCommandUploadBundle;
  // Uploads the bundle; if the server returns a proof, writes the proof string
  // and returns true; else returns false.
  bool UploadBundle(const bstring &bundle, bstring *proof) {
    CTResponse response = Upload(bundle);
    bool ret = false;
    switch (response.code) {
      case ct::SUBMITTED:
        std::cout << "Token is " << HexString(response.data) << std::endl;
        break;
      case ct::LOGGED:
        std::cout << "Received proof " << HexString(response.data) << std::endl;
        if (proof != NULL) {
          proof->assign(response.data);
          ret = true;
        }
        break;
      default:
        std::cout << "Unknown response code." << std::endl;
    }
    return ret;
  }

private:
  struct CTResponse {
    byte code;
    bstring data;
  };

  // Provisional packet format
  // struct {
  //  uint8 version;
  //  uint8 command;
  //  uint24 length;
  //  opaque fragment[ClientCommand.length];
  // } ClientCommand;

  void write(const void *buf, size_t length) const {
    for (size_t offset = 0; offset < length; ) {
      int n = ::write(fd(), ((char *)buf) + offset, length - offset);
      assert(n > 0);
      offset += n;
    }
  }

  void WriteByte(size_t b) const {
    char buf[1];
    buf[0] = b;
    write(buf, 1);
  }

  // Write MSB first.
  void WriteLength(size_t length, size_t length_of_length) const {
    assert(length_of_length <= sizeof length);
    assert(length < 1U << (length_of_length * 8));
    for ( ; length_of_length > 0; --length_of_length) {
      size_t b = length & (0xff << ((length_of_length - 1) * 8));
      WriteByte(b >> ((length_of_length - 1) * 8));
    }
  }

  void WriteCommand(ct::ClientCommand cmd) const {
    WriteByte(VERSION);
    WriteByte(cmd);
  }

  void WriteData(const void *buf, size_t length) const {
    WriteLength(length, 3);
    write(buf, length);
  }

  void read(void *buf, size_t length) const {
    for (size_t offset = 0; offset < length; ) {
      int n = ::read(fd(), ((char *)buf) + offset, length - offset);
      assert(n > 0);
      offset += n;
    }
  }

  byte ReadByte() const {
    byte buf[1];
    read(buf, 1);
    return buf[0];
  }

  size_t ReadLength(size_t length_of_length) const {
    byte buf[length_of_length];
    read(buf, length_of_length);
    size_t length = 0;
    for (size_t n = 0; n < length_of_length; ++n)
      length = (length << 8) + buf[n];
    return length;
  }

  void ReadString(bstring *dst, size_t length) const {
    byte buf[length];
    read(buf, length);
    dst->assign(buf, length);
  }

  void ReadResponse(CTResponse *response) {
    byte version = ReadByte();
    assert(version == VERSION);
    response->code = ReadByte();
    size_t length = ReadLength(3);
    ReadString(&response->data, length);
    std::cout << "Response code is " << (int)response->code << ", data length "
	      << length << std::endl;
  }

  CTResponse Upload(const bstring &bundle) {
    WriteCommand(ct::UPLOAD_BUNDLE);
    WriteData(bundle.data(), bundle.length());
    CTResponse response;
    ReadResponse(&response);
    return response;
  }
  static const byte VERSION = 0;
}; // class LogClient

class SSLClient : public CTClient {
 public:
  SSLClient(const char *server, uint16_t port) : CTClient(server, port) {}

  static int VerifyCallback(X509_STORE_CTX *ctx, void *arg) {
    // Verify the proof, if present.
    LogVerifier *verifier = reinterpret_cast<LogVerifier*>(arg);
    if (verifier == NULL) {
      std::cout << "No log server public key supplied. Dropping connection." <<
          std::endl;
      return 0;
    }

    if (ctx->cert == NULL) {
      std::cout << "No server certificate received. Dropping connection." <<
          std::endl;
      return 0;
    }
    // Read the leaf certificate.
    unsigned char *buf = NULL;
    int cert_len = i2d_X509(ctx->cert, &buf);
    assert(cert_len > 0);
    bstring leaf(buf, cert_len);
    OPENSSL_free(buf);

    bool proof_verified = false;
    // TODO: is there an API call for accessing the bag of certs?
    STACK_OF(X509) *sk = ctx->untrusted;
    if (sk) {
      // Create the object by which we recognize the proof extension.
      ASN1_OBJECT *obj = ProofExtensionObject();
      assert(obj != NULL);
      for (int i = 0; i < sk_X509_num(sk); ++i) {
        X509 *cert = sk_X509_value(sk, i);
        int extension_index = X509_get_ext_by_OBJ(cert, obj, -1);
        if (extension_index != -1) {
          std::cout << "Proof extension found, verifying...";
          X509_EXTENSION *ext = X509_get_ext(cert, extension_index);
          ASN1_OCTET_STRING *ext_data = X509_EXTENSION_get_data(ext);
          bstring proofstring(ext_data->data, ext_data->length);
          proof_verified = VerifyLogSegmentProof(proofstring, leaf, verifier);
          if (proof_verified) {
            std::cout << "OK." << std::endl;
            break;
          } else {
            std::cout << "FAIL." << std::endl;
          }
        }
      }
      ASN1_OBJECT_free(obj);
    }

    if (!proof_verified) {
      std::cout << "No log proof found. Dropping connection." << std::endl;
      return 0;
    }

    std::cout << "Log proof verified." << std::endl;
    int vfy = X509_verify_cert(ctx);
    if (vfy != 1) {
      // Echo a warning, but continue with connection.
      std::cout << "WARNING. Certificate verification failed." << std::endl;
    }
    return 1;
  }

  // VerifyCallback uses this verifier for verifying log proofs.
  void SSLConnect(LogVerifier *verifier) {
    SSL_CTX *ctx = SSL_CTX_new(SSLv3_client_method());
    assert(ctx != NULL);
    // SSL_VERIFY_PEER makes the connection abort immediately
    // if verification fails.
    SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, NULL);
    SSL_CTX_set_cert_verify_callback(ctx, &VerifyCallback, verifier);
    SSL *ssl = SSL_new(ctx);
    assert(ssl != NULL);
    BIO *bio = BIO_new_socket(fd(), BIO_NOCLOSE);
    assert(bio != NULL);
    // Takes ownership of bio.
    SSL_set_bio(ssl, bio, bio);
    int ret = SSL_connect(ssl);
    // TODO: check and report certificate verification errors.
    if (ret == 1)
      std::cout << "Connected." << std::endl;
    else
      std::cout << "Connection failed." << std::endl;
    if (ssl) {
      SSL_shutdown(ssl);
      SSL_free(ssl);
    }
    SSL_CTX_free(ctx);
  }
}; // class SSLCLient

static void UploadHelp() {
    std::cerr << "upload <file> <server> <port> [-server_key key_file] " <<
        "[-out proof_file]" << std::endl;
}

static void Upload(int argc, const char **argv) {
  if (argc < 4) {
    UploadHelp();
    exit(1);
  }
  const char *file = argv[1];
  const char *server_name = argv[2];
  uint16_t port = atoi(argv[3]);

  const char *key_file = NULL;
  const char *proof_file = NULL;

  EVP_PKEY *pkey = NULL;
  FILE *out = NULL;

  argc -= 4;
  argv += 4;

  while (argc >= 2) {
    if (strcmp(argv[0], "-server_key") == 0) {
      if (key_file != NULL) {
        UploadHelp();
        exit(1);
      }
      key_file = argv[1];
      argc -= 2;
      argv += 2;
    } else if (strcmp(argv[0], "-out") == 0) {
      if (proof_file != NULL) {
        UploadHelp();
        exit(1);
      }
      proof_file = argv[1];
      argc -= 2;
      argv += 2;
    } else {
      UploadHelp();
      exit(1);
    }
  }

  if (argc) {
    UploadHelp();
    exit(1);
  }

  std::ifstream in(file);
  if (!in.is_open()) {
    perror(file);
    exit(2);
  }

  if (key_file != NULL) {
    FILE *fp = fopen(key_file, "r");
    if (fp == NULL) {
      perror(key_file);
      exit(2);
    }
    if (PEM_read_PUBKEY(fp, &pkey, NULL, NULL) == NULL) {
      std::cerr << "Could not read log server public key" << std::endl;
      exit(6);
    }
    fclose(fp);
  }

  if (proof_file != NULL) {
    out = fopen(proof_file, "wb");
    if (out == NULL) {
      perror(proof_file);
      exit(2);
    }
  }

  std::cout << "Uploading certificate bundle from " << file << '.' << std::endl;

  // Assume for now that we get a single leaf cert.
  // TODO: properly encode the submission with length prefixes,
  // to match the spec.
  bstring contents;
  ReadAll(&contents, in);
  std::cout << file << " is " << contents.length() << " bytes." << std::endl;

  X509 *cert = NULL;
  const unsigned char *dataptr = contents.data();
  if ((cert = d2i_X509(&cert, &dataptr, contents.size())) == NULL) {
    std::cerr << "Input is not a valid DER-encoded certificate." << std::endl;
    exit(1);
  }
  X509_free(cert);

  LogClient client(server_name, port);
  bstring proof;
  if(!client.UploadBundle(contents, &proof)) {
    std::cout << "No log proof received. Try again later." << std::endl;
    if (out != NULL) {
      fclose(out);
      remove(proof_file);
    }
    if (pkey != NULL)
      EVP_PKEY_free(pkey);
  } else {
    if (pkey == NULL) {
      std::cout << "WARNING: no log server key supplied. Cannot verify proof."
                << std::endl;
    } else {
      LogVerifier verifier(pkey);
      if (VerifyLogSegmentProof(proof, contents, &verifier))
        std::cout << "Proof verified." << std::endl;
      else
        std::cout << "ERROR: invalid proof." << std::endl;
    }
    if (out != NULL) {
      fwrite(proof.data(), 1, proof.size(), out);
      fclose(out);
      std::cout << "Wrote proof to " << proof_file << std::endl;
    }
  }
}

// FIXME: fix all the memory leaks in this code.
static void MakeCert(int argc, const char **argv) {
  if (argc != 3) {
    std::cerr << argv[0] << " <input proof> <output certificate>\n";
    exit(7);
  }
  const char *proof_file = argv[1];
  const char *cert_file = argv[2];

  int proof_fd = open(proof_file, O_RDONLY);
  assert(proof_fd >= 0);
  unsigned char proof[2048];
  ssize_t proof_len = read(proof_fd, proof, sizeof proof);
  assert(proof_len >= 0);
  assert(proof_len < (ssize_t)sizeof proof);

  int cert_fd = open(cert_file, O_CREAT | O_TRUNC | O_WRONLY, 0666);
  assert(cert_fd >= 0);
  BIO *out = BIO_new_fd(cert_fd, BIO_CLOSE);

  X509 *x = X509_new();

  // X509v3 (== 2)
  X509_set_version(x, 2);

  // Random 128 bit serial number
  BIGNUM *serial = BN_new();
  BN_rand(serial, 128, 0, 0);
  BN_to_ASN1_INTEGER(serial, X509_get_serialNumber(x));
  BN_free(serial);

  // Set signature algorithm
  // FIXME: is there an opaque way to get the algorithm structure?
  x->cert_info->signature->algorithm = OBJ_nid2obj(NID_sha1WithRSAEncryption);
  x->cert_info->signature->parameter = NULL;

  // Set the start date to now
  X509_gmtime_adj(X509_get_notBefore(x), 0);
  // End date to now + 1 second
  X509_gmtime_adj(X509_get_notAfter(x), 1);

  // Create the issuer name
  X509_NAME *issuer = X509_NAME_new();
  X509_NAME_add_entry_by_NID(issuer, NID_commonName, V_ASN1_PRINTABLESTRING,
			     const_cast<unsigned char *>(
			       reinterpret_cast<const unsigned char *>("Test")),
			     4, 0, -1);
  X509_set_issuer_name(x, issuer);

  // Create the subject name
  X509_NAME *subject = X509_NAME_new();
  X509_NAME_add_entry_by_NID(subject, NID_commonName, V_ASN1_PRINTABLESTRING,
			     const_cast<unsigned char *>(
			       reinterpret_cast<const unsigned char *>("tseT")),
			     4, 0, -1);
  X509_set_subject_name(x, subject);

  // Public key
  RSA *rsa = RSA_new();
  static const unsigned char bits[1] = { 3 };
  rsa->n = BN_bin2bn(bits, 1, NULL);
  rsa->e = BN_bin2bn(bits, 1, NULL);
  EVP_PKEY *evp_pkey = EVP_PKEY_new();
  EVP_PKEY_assign_RSA(evp_pkey, rsa);
  X509_PUBKEY_set(&X509_get_X509_PUBKEY(x) , evp_pkey);

  // And finally, the proof in an extension
  ASN1_OBJECT *obj = ProofExtensionObject();
  ASN1_OCTET_STRING *data = ASN1_OCTET_STRING_new();
  ASN1_OCTET_STRING_set(data, proof, proof_len);
  X509_EXTENSION *ext = X509_EXTENSION_new();
  X509_EXTENSION_set_object(ext, obj);
  X509_EXTENSION_set_critical(ext, 1);
  X509_EXTENSION_set_data(ext, data);
  X509_add_ext(x, ext, -1);

  int i = i2d_X509_bio(out, x);
  assert(i != 0);

  BIO_free(out);
}

static void Connect(int argc, const char **argv) {
  if (argc < 3) {
    std::cerr << argv[0] << " <server> <port>\n";
    exit(2);
  }
  const char *server_name = argv[1];
  unsigned port = atoi(argv[2]);

  EVP_PKEY *pkey = NULL;

  if (argc > 3) {
    FILE *fp = fopen(argv[3], "r");
    if (fp == NULL || PEM_read_PUBKEY(fp, &pkey, NULL, NULL) == NULL) {
      std::cerr << "Could not read log server public key.\n";
      exit(1);
    }
    fclose(fp);
  }

  LogVerifier *verifier = NULL;
  if (pkey != NULL)
    verifier = new LogVerifier(pkey);
  SSLClient client(server_name, port);
  client.SSLConnect(verifier);
  delete verifier;
}

int main(int argc, const char **argv) {
  if (argc < 2) {
    std::cerr << argv[0] << " <command> ...\n";
    return 1;
  }

  const std::string cmd(argv[1]);
  if (cmd == "upload")
    Upload(argc - 1, argv + 1);
  else if (cmd == "certificate")
    MakeCert(argc - 1, argv + 1);
  else if (cmd == "connect") {
    SSL_library_init();
    Connect(argc - 1, argv + 1);
  }
  else
    UnknownCommand(cmd);
}
