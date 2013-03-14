#include <gflags/gflags.h>
#include <glog/logging.h>
#include <gtest/gtest.h>
#include <openssl/evp.h>
#include <string>

#include "log/cert.h"
#include "log/cert_checker.h"
#include "log/ct_extensions.h"
#include "util/testing.h"
#include "util/util.h"

using std::string;

DEFINE_string(test_certs_dir, "test/testdata", "Path to test certificates");

// Valid certificates.
// Self-signed
static const char kCaCert[] = "ca-cert.pem";
// Issued by ca-cert.pem
static const char kLeafCert[] = "test-cert.pem";
// Issued by ca-cert.pem
static const char kCaPreCert[] = "ca-pre-cert.pem";
// Issued by ca-cert.pem
static const char kPreCert[] = "test-embedded-pre-cert.pem";
// Issued by ca-pre-cert.pem
static const char kPreWithPreCaCert[] = "test-embedded-with-preca-pre-cert.pem";
// Issued by ca-cert.pem
static const char kIntermediateCert[] = "intermediate-cert.pem";
// Issued by intermediate-cert.pem
static const char kChainLeafCert[] = "test-intermediate-cert.pem";

namespace {

class CertCheckerTest : public ::testing::Test {
 protected:
  string leaf_pem_;
  string ca_precert_pem_;
  string precert_pem_;
  string precert_with_preca_pem_;
  string intermediate_pem_;
  string chain_leaf_pem_;
  string ca_pem_;
  CertChecker checker_;
  string cert_dir_;

  void SetUp() {
    cert_dir_ = FLAGS_test_certs_dir;
    CHECK(util::ReadTextFile(cert_dir_ + "/" + kLeafCert, &leaf_pem_))
        << "Could not read test data from " << cert_dir_
        << ". Wrong --test_certs_dir?";
    CHECK(util::ReadTextFile(cert_dir_ + "/" + kCaPreCert, &ca_precert_pem_));
    CHECK(util::ReadTextFile(cert_dir_ + "/" + kPreCert, &precert_pem_));
    CHECK(util::ReadTextFile(cert_dir_ + "/" + kPreWithPreCaCert,
                             &precert_with_preca_pem_));
    CHECK(util::ReadTextFile(cert_dir_ + "/" + kIntermediateCert,
                             &intermediate_pem_));
    CHECK(util::ReadTextFile(cert_dir_ + "/" + kChainLeafCert,
                             &chain_leaf_pem_));
    CHECK(util::ReadTextFile(cert_dir_ + "/" + kCaCert, &ca_pem_));
  }
};

TEST_F(CertCheckerTest, Certificate) {
  CertChain chain(leaf_pem_);
  ASSERT_TRUE(chain.IsLoaded());

  // Fail as we have no CA certs.
  EXPECT_EQ(CertChecker::ROOT_NOT_IN_LOCAL_STORE,
            checker_.CheckCertChain(&chain));

  // Load CA certs and expect success.
  EXPECT_TRUE(checker_.LoadTrustedCertificate(cert_dir_ + "/" + kCaCert));
  EXPECT_EQ(CertChecker::OK, checker_.CheckCertChain(&chain));
  EXPECT_EQ(2U, chain.Length());
}

TEST_F(CertCheckerTest, CertificateWithRoot) {
  CertChain chain(leaf_pem_);
  ASSERT_TRUE(chain.IsLoaded());
  chain.AddCert(new Cert(ca_pem_));

  // Fail as even though we give a CA cert, it's not in the local store.
  EXPECT_EQ(CertChecker::ROOT_NOT_IN_LOCAL_STORE,
            checker_.CheckCertChain(&chain));

  // Load CA certs and expect success.
  EXPECT_TRUE(checker_.LoadTrustedCertificate(cert_dir_ + "/" + kCaCert));
  EXPECT_EQ(CertChecker::OK, checker_.CheckCertChain(&chain));
  EXPECT_EQ(2U, chain.Length());
}

TEST_F(CertCheckerTest, TrimsRepeatedRoots) {
  CertChain chain(leaf_pem_);
  ASSERT_TRUE(chain.IsLoaded());
  chain.AddCert(new Cert(ca_pem_));
  chain.AddCert(new Cert(ca_pem_));

  // Load CA certs and expect success.
  EXPECT_TRUE(checker_.LoadTrustedCertificate(cert_dir_ + "/" + kCaCert));
  EXPECT_EQ(CertChecker::OK, checker_.CheckCertChain(&chain));
  EXPECT_EQ(2U, chain.Length());
}

TEST_F(CertCheckerTest, Intermediates) {
  // Load CA certs.
  EXPECT_TRUE(checker_.LoadTrustedCertificate(cert_dir_ + "/" + kCaCert));
  // A chain with an intermediate.
  CertChain chain(chain_leaf_pem_);
  ASSERT_TRUE(chain.IsLoaded());
  // Fail as it doesn't chain to a trusted CA.
  EXPECT_EQ(CertChecker::ROOT_NOT_IN_LOCAL_STORE,
            checker_.CheckCertChain(&chain));
  // Add the intermediate and expect success.
  chain.AddCert(new Cert(intermediate_pem_));
  EXPECT_EQ(CertChecker::OK, checker_.CheckCertChain(&chain));
  EXPECT_EQ(3U, chain.Length());

  // An invalid chain, with two certs in wrong order.
  CertChain invalid(intermediate_pem_ + chain_leaf_pem_);
  ASSERT_TRUE(invalid.IsLoaded());
  EXPECT_EQ(CertChecker::INVALID_CERTIFICATE_CHAIN,
            checker_.CheckCertChain(&invalid));
}

TEST_F(CertCheckerTest, PreCert) {
  const string chain_pem = precert_pem_ + ca_pem_;
  PreCertChain chain(chain_pem);

  ASSERT_TRUE(chain.IsLoaded());
  EXPECT_TRUE(chain.IsWellFormed());

  // Fail as we have no CA certs.
  EXPECT_EQ(CertChecker::ROOT_NOT_IN_LOCAL_STORE,
            checker_.CheckPreCertChain(&chain));

  // Load CA certs and expect success.
  checker_.LoadTrustedCertificate(cert_dir_ + "/" + kCaCert);
  EXPECT_EQ(CertChecker::OK, checker_.CheckPreCertChain(&chain));
}

TEST_F(CertCheckerTest, PreCertWithPreCa) {
  const string chain_pem = precert_with_preca_pem_ + ca_precert_pem_;
  PreCertChain chain(chain_pem);

  ASSERT_TRUE(chain.IsLoaded());
  EXPECT_TRUE(chain.IsWellFormed());

  // Fail as we have no CA certs.
  EXPECT_EQ(CertChecker::ROOT_NOT_IN_LOCAL_STORE,
            checker_.CheckPreCertChain(&chain));

  // Load CA certs and expect success.
  checker_.LoadTrustedCertificate(cert_dir_ + "/" + kCaCert);
  EXPECT_EQ(CertChecker::OK, checker_.CheckPreCertChain(&chain));

  // A second, invalid chain, with no CA precert.
  PreCertChain chain2(precert_with_preca_pem_);
  ASSERT_TRUE(chain2.IsLoaded());
  EXPECT_TRUE(chain2.IsWellFormed());
  EXPECT_EQ(CertChecker::ROOT_NOT_IN_LOCAL_STORE,
            checker_.CheckPreCertChain(&chain2));
}

}  // namespace

int main(int argc, char**argv) {
  ct::test::InitTesting(argv[0], &argc, &argv, true);
  OpenSSL_add_all_algorithms();
  ct::LoadCtExtensions();
  return RUN_ALL_TESTS();
}
