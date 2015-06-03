/*
 * Copyright (C) 2014 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <fstream>
#include <string>
#include <vector>

#include <hardware/keymaster0.h>
#include <keymaster/soft_keymaster_device.h>
#include <keymaster/softkeymaster.h>

#include "android_keymaster_test_utils.h"

using std::ifstream;
using std::istreambuf_iterator;
using std::string;
using std::vector;
using std::unique_ptr;

extern "C" {
int __android_log_print(int prio, const char* tag, const char* fmt);
int __android_log_print(int prio, const char* tag, const char* fmt) {
    (void)prio, (void)tag, (void)fmt;
    return 0;
}
}  // extern "C"

namespace keymaster {
namespace test {

StdoutLogger logger;

class SoftKeymasterTestInstanceCreator : public Keymaster1TestInstanceCreator {
  public:
    keymaster1_device_t* CreateDevice() const override {
        std::cerr << "Creating software-only device" << std::endl;
        SoftKeymasterDevice* device = new SoftKeymasterDevice;
        return device->keymaster_device();
    }

    bool algorithm_in_hardware(keymaster_algorithm_t) const override { return false; }
    int keymaster0_calls() const override { return 0; }
};

class Keymaster0AdapterTestInstanceCreator : public Keymaster1TestInstanceCreator {
  public:
    Keymaster0AdapterTestInstanceCreator(bool support_ec) : support_ec_(support_ec) {}

    keymaster1_device_t* CreateDevice() const {
        std::cerr << "Creating keymaster0-backed device (with ec: " << std::boolalpha << support_ec_
                  << ")." << std::endl;
        hw_device_t* softkeymaster_device;
        EXPECT_EQ(0, openssl_open(&softkeymaster_module.common, KEYSTORE_KEYMASTER,
                                  &softkeymaster_device));
        // Make the software device pretend to be hardware
        keymaster0_device_t* keymaster0_device =
            reinterpret_cast<keymaster0_device_t*>(softkeymaster_device);
        keymaster0_device->flags &= ~KEYMASTER_SOFTWARE_ONLY;

        if (!support_ec_) {
            // Make the software device pretend not to support EC
            keymaster0_device->flags &= ~KEYMASTER_SUPPORTS_EC;
        }

        counting_keymaster0_device_ = new Keymaster0CountingWrapper(keymaster0_device);

        SoftKeymasterDevice* keymaster = new SoftKeymasterDevice(counting_keymaster0_device_);
        return keymaster->keymaster_device();
    }

    bool algorithm_in_hardware(keymaster_algorithm_t algorithm) const override {
        switch (algorithm) {
        case KM_ALGORITHM_RSA:
            return true;
        case KM_ALGORITHM_EC:
            return support_ec_;
        default:
            return false;
        }
    }
    int keymaster0_calls() const override { return counting_keymaster0_device_->count(); }

  private:
    mutable Keymaster0CountingWrapper* counting_keymaster0_device_;
    bool support_ec_;
};

static auto test_params = testing::Values(
    InstanceCreatorPtr(new SoftKeymasterTestInstanceCreator),
    InstanceCreatorPtr(new Keymaster0AdapterTestInstanceCreator(true /* support_ec */)),
    InstanceCreatorPtr(new Keymaster0AdapterTestInstanceCreator(false /* support_ec */)));

typedef Keymaster1Test CheckSupported;
INSTANTIATE_TEST_CASE_P(AndroidKeymasterTest, CheckSupported, test_params);

TEST_P(CheckSupported, SupportedAlgorithms) {
    EXPECT_EQ(KM_ERROR_OUTPUT_PARAMETER_NULL,
              device()->get_supported_algorithms(device(), NULL, NULL));

    size_t len;
    keymaster_algorithm_t* algorithms;
    EXPECT_EQ(KM_ERROR_OK, device()->get_supported_algorithms(device(), &algorithms, &len));
    EXPECT_TRUE(ResponseContains(
        {KM_ALGORITHM_RSA, KM_ALGORITHM_EC, KM_ALGORITHM_AES, KM_ALGORITHM_HMAC}, algorithms, len));
    free(algorithms);

    EXPECT_EQ(0, GetParam()->keymaster0_calls());
}

TEST_P(CheckSupported, SupportedBlockModes) {
    EXPECT_EQ(KM_ERROR_OUTPUT_PARAMETER_NULL,
              device()->get_supported_block_modes(device(), KM_ALGORITHM_RSA, KM_PURPOSE_ENCRYPT,
                                                  NULL, NULL));

    size_t len;
    keymaster_block_mode_t* modes;
    ASSERT_EQ(KM_ERROR_OK, device()->get_supported_block_modes(device(), KM_ALGORITHM_RSA,
                                                               KM_PURPOSE_ENCRYPT, &modes, &len));
    EXPECT_EQ(0U, len);
    free(modes);

    EXPECT_EQ(KM_ERROR_UNSUPPORTED_PURPOSE,
              device()->get_supported_block_modes(device(), KM_ALGORITHM_EC, KM_PURPOSE_ENCRYPT,
                                                  &modes, &len));

    ASSERT_EQ(KM_ERROR_OK, device()->get_supported_block_modes(device(), KM_ALGORITHM_AES,
                                                               KM_PURPOSE_ENCRYPT, &modes, &len));
    EXPECT_TRUE(ResponseContains({KM_MODE_ECB, KM_MODE_CBC, KM_MODE_CTR, KM_MODE_GCM}, modes, len));
    free(modes);

    EXPECT_EQ(0, GetParam()->keymaster0_calls());
}

TEST_P(CheckSupported, SupportedPaddingModes) {
    EXPECT_EQ(KM_ERROR_OUTPUT_PARAMETER_NULL,
              device()->get_supported_padding_modes(device(), KM_ALGORITHM_RSA, KM_PURPOSE_ENCRYPT,
                                                    NULL, NULL));

    size_t len;
    keymaster_padding_t* modes;
    ASSERT_EQ(KM_ERROR_OK, device()->get_supported_padding_modes(device(), KM_ALGORITHM_RSA,
                                                                 KM_PURPOSE_SIGN, &modes, &len));
    EXPECT_TRUE(
        ResponseContains({KM_PAD_NONE, KM_PAD_RSA_PKCS1_1_5_SIGN, KM_PAD_RSA_PSS}, modes, len));
    free(modes);

    ASSERT_EQ(KM_ERROR_OK, device()->get_supported_padding_modes(device(), KM_ALGORITHM_RSA,
                                                                 KM_PURPOSE_ENCRYPT, &modes, &len));
    EXPECT_TRUE(
        ResponseContains({KM_PAD_NONE, KM_PAD_RSA_OAEP, KM_PAD_RSA_PKCS1_1_5_ENCRYPT}, modes, len));
    free(modes);

    ASSERT_EQ(KM_ERROR_OK, device()->get_supported_padding_modes(device(), KM_ALGORITHM_EC,
                                                                 KM_PURPOSE_SIGN, &modes, &len));
    EXPECT_EQ(0U, len);
    free(modes);

    EXPECT_EQ(KM_ERROR_UNSUPPORTED_PURPOSE,
              device()->get_supported_padding_modes(device(), KM_ALGORITHM_AES, KM_PURPOSE_SIGN,
                                                    &modes, &len));

    EXPECT_EQ(0, GetParam()->keymaster0_calls());
}

TEST_P(CheckSupported, SupportedDigests) {
    EXPECT_EQ(
        KM_ERROR_OUTPUT_PARAMETER_NULL,
        device()->get_supported_digests(device(), KM_ALGORITHM_RSA, KM_PURPOSE_SIGN, NULL, NULL));

    size_t len;
    keymaster_digest_t* digests;
    ASSERT_EQ(KM_ERROR_OK, device()->get_supported_digests(device(), KM_ALGORITHM_RSA,
                                                           KM_PURPOSE_SIGN, &digests, &len));
    EXPECT_TRUE(
        ResponseContains({KM_DIGEST_NONE, KM_DIGEST_MD5, KM_DIGEST_SHA1, KM_DIGEST_SHA_2_224,
                          KM_DIGEST_SHA_2_256, KM_DIGEST_SHA_2_384, KM_DIGEST_SHA_2_512},
                         digests, len));
    free(digests);

    ASSERT_EQ(KM_ERROR_OK, device()->get_supported_digests(device(), KM_ALGORITHM_EC,
                                                           KM_PURPOSE_SIGN, &digests, &len));
    EXPECT_TRUE(
        ResponseContains({KM_DIGEST_NONE, KM_DIGEST_MD5, KM_DIGEST_SHA1, KM_DIGEST_SHA_2_224,
                          KM_DIGEST_SHA_2_256, KM_DIGEST_SHA_2_384, KM_DIGEST_SHA_2_512},
                         digests, len));
    free(digests);

    EXPECT_EQ(KM_ERROR_UNSUPPORTED_PURPOSE,
              device()->get_supported_digests(device(), KM_ALGORITHM_AES, KM_PURPOSE_SIGN, &digests,
                                              &len));

    ASSERT_EQ(KM_ERROR_OK, device()->get_supported_digests(device(), KM_ALGORITHM_HMAC,
                                                           KM_PURPOSE_SIGN, &digests, &len));
    EXPECT_TRUE(ResponseContains({KM_DIGEST_SHA_2_224, KM_DIGEST_SHA_2_256, KM_DIGEST_SHA_2_384,
                                  KM_DIGEST_SHA_2_512, KM_DIGEST_SHA1},
                                 digests, len));
    free(digests);

    EXPECT_EQ(0, GetParam()->keymaster0_calls());
}

TEST_P(CheckSupported, SupportedImportFormats) {
    EXPECT_EQ(KM_ERROR_OUTPUT_PARAMETER_NULL,
              device()->get_supported_import_formats(device(), KM_ALGORITHM_RSA, NULL, NULL));

    size_t len;
    keymaster_key_format_t* formats;
    ASSERT_EQ(KM_ERROR_OK,
              device()->get_supported_import_formats(device(), KM_ALGORITHM_RSA, &formats, &len));
    EXPECT_TRUE(ResponseContains(KM_KEY_FORMAT_PKCS8, formats, len));
    free(formats);

    ASSERT_EQ(KM_ERROR_OK,
              device()->get_supported_import_formats(device(), KM_ALGORITHM_AES, &formats, &len));
    EXPECT_TRUE(ResponseContains(KM_KEY_FORMAT_RAW, formats, len));
    free(formats);

    ASSERT_EQ(KM_ERROR_OK,
              device()->get_supported_import_formats(device(), KM_ALGORITHM_HMAC, &formats, &len));
    EXPECT_TRUE(ResponseContains(KM_KEY_FORMAT_RAW, formats, len));
    free(formats);

    EXPECT_EQ(0, GetParam()->keymaster0_calls());
}

TEST_P(CheckSupported, SupportedExportFormats) {
    EXPECT_EQ(KM_ERROR_OUTPUT_PARAMETER_NULL,
              device()->get_supported_export_formats(device(), KM_ALGORITHM_RSA, NULL, NULL));

    size_t len;
    keymaster_key_format_t* formats;
    ASSERT_EQ(KM_ERROR_OK,
              device()->get_supported_export_formats(device(), KM_ALGORITHM_RSA, &formats, &len));
    EXPECT_TRUE(ResponseContains(KM_KEY_FORMAT_X509, formats, len));
    free(formats);

    ASSERT_EQ(KM_ERROR_OK,
              device()->get_supported_export_formats(device(), KM_ALGORITHM_EC, &formats, &len));
    EXPECT_TRUE(ResponseContains(KM_KEY_FORMAT_X509, formats, len));
    free(formats);

    ASSERT_EQ(KM_ERROR_OK,
              device()->get_supported_export_formats(device(), KM_ALGORITHM_AES, &formats, &len));
    EXPECT_EQ(0U, len);
    free(formats);

    ASSERT_EQ(KM_ERROR_OK,
              device()->get_supported_export_formats(device(), KM_ALGORITHM_AES, &formats, &len));
    EXPECT_EQ(0U, len);
    free(formats);

    ASSERT_EQ(KM_ERROR_OK,
              device()->get_supported_export_formats(device(), KM_ALGORITHM_HMAC, &formats, &len));
    EXPECT_EQ(0U, len);
    free(formats);

    EXPECT_EQ(0, GetParam()->keymaster0_calls());
}

class NewKeyGeneration : public Keymaster1Test {
  protected:
    void CheckBaseParams() {
        AuthorizationSet auths = sw_enforced();
        EXPECT_GT(auths.SerializedSize(), 12U);

        EXPECT_TRUE(contains(auths, TAG_PURPOSE, KM_PURPOSE_SIGN));
        EXPECT_TRUE(contains(auths, TAG_PURPOSE, KM_PURPOSE_VERIFY));
        EXPECT_TRUE(contains(auths, TAG_USER_ID, 7));
        EXPECT_TRUE(contains(auths, TAG_USER_AUTH_TYPE, HW_AUTH_PASSWORD));
        EXPECT_TRUE(contains(auths, TAG_AUTH_TIMEOUT, 300));

        // Verify that App ID, App data and ROT are NOT included.
        EXPECT_FALSE(contains(auths, TAG_ROOT_OF_TRUST));
        EXPECT_FALSE(contains(auths, TAG_APPLICATION_ID));
        EXPECT_FALSE(contains(auths, TAG_APPLICATION_DATA));

        // Just for giggles, check that some unexpected tags/values are NOT present.
        EXPECT_FALSE(contains(auths, TAG_PURPOSE, KM_PURPOSE_ENCRYPT));
        EXPECT_FALSE(contains(auths, TAG_PURPOSE, KM_PURPOSE_DECRYPT));
        EXPECT_FALSE(contains(auths, TAG_AUTH_TIMEOUT, 301));

        // Now check that unspecified, defaulted tags are correct.
        EXPECT_TRUE(contains(auths, TAG_ORIGIN, KM_ORIGIN_GENERATED));
        EXPECT_TRUE(contains(auths, KM_TAG_CREATION_DATETIME));
    }
};
INSTANTIATE_TEST_CASE_P(AndroidKeymasterTest, NewKeyGeneration, test_params);

TEST_P(NewKeyGeneration, Rsa) {
    ASSERT_EQ(KM_ERROR_OK, GenerateKey(AuthorizationSetBuilder()
                                           .RsaSigningKey(256, 3)
                                           .Digest(KM_DIGEST_NONE)
                                           .Padding(KM_PAD_NONE)));
    CheckBaseParams();

    // Check specified tags are all present, and in the right set.
    AuthorizationSet crypto_params;
    AuthorizationSet non_crypto_params;
    if (GetParam()->algorithm_in_hardware(KM_ALGORITHM_RSA)) {
        EXPECT_NE(0U, hw_enforced().size());
        EXPECT_NE(0U, sw_enforced().size());
        crypto_params.push_back(hw_enforced());
        non_crypto_params.push_back(sw_enforced());
    } else {
        EXPECT_EQ(0U, hw_enforced().size());
        EXPECT_NE(0U, sw_enforced().size());
        crypto_params.push_back(sw_enforced());
    }

    EXPECT_TRUE(contains(crypto_params, TAG_ALGORITHM, KM_ALGORITHM_RSA));
    EXPECT_FALSE(contains(non_crypto_params, TAG_ALGORITHM, KM_ALGORITHM_RSA));
    EXPECT_TRUE(contains(crypto_params, TAG_KEY_SIZE, 256));
    EXPECT_FALSE(contains(non_crypto_params, TAG_KEY_SIZE, 256));
    EXPECT_TRUE(contains(crypto_params, TAG_RSA_PUBLIC_EXPONENT, 3));
    EXPECT_FALSE(contains(non_crypto_params, TAG_RSA_PUBLIC_EXPONENT, 3));

    if (GetParam()->algorithm_in_hardware(KM_ALGORITHM_RSA))
        EXPECT_EQ(1, GetParam()->keymaster0_calls());
}

TEST_P(NewKeyGeneration, RsaDefaultSize) {
    ASSERT_EQ(KM_ERROR_UNSUPPORTED_KEY_SIZE,
              GenerateKey(AuthorizationSetBuilder()
                              .Authorization(TAG_ALGORITHM, KM_ALGORITHM_RSA)
                              .Authorization(TAG_RSA_PUBLIC_EXPONENT, 3)
                              .SigningKey()));

    EXPECT_EQ(0, GetParam()->keymaster0_calls());
}

TEST_P(NewKeyGeneration, Ecdsa) {
    ASSERT_EQ(KM_ERROR_OK,
              GenerateKey(AuthorizationSetBuilder().EcdsaSigningKey(224).Digest(KM_DIGEST_NONE)));
    CheckBaseParams();

    // Check specified tags are all present, and in the right set.
    AuthorizationSet crypto_params;
    AuthorizationSet non_crypto_params;
    if (GetParam()->algorithm_in_hardware(KM_ALGORITHM_EC)) {
        EXPECT_NE(0U, hw_enforced().size());
        EXPECT_NE(0U, sw_enforced().size());
        crypto_params.push_back(hw_enforced());
        non_crypto_params.push_back(sw_enforced());
    } else {
        EXPECT_EQ(0U, hw_enforced().size());
        EXPECT_NE(0U, sw_enforced().size());
        crypto_params.push_back(sw_enforced());
    }

    EXPECT_TRUE(contains(crypto_params, TAG_ALGORITHM, KM_ALGORITHM_EC));
    EXPECT_FALSE(contains(non_crypto_params, TAG_ALGORITHM, KM_ALGORITHM_EC));
    EXPECT_TRUE(contains(crypto_params, TAG_KEY_SIZE, 224));
    EXPECT_FALSE(contains(non_crypto_params, TAG_KEY_SIZE, 224));

    if (GetParam()->algorithm_in_hardware(KM_ALGORITHM_EC))
        EXPECT_EQ(1, GetParam()->keymaster0_calls());
}

TEST_P(NewKeyGeneration, EcdsaDefaultSize) {
    ASSERT_EQ(KM_ERROR_UNSUPPORTED_KEY_SIZE,
              GenerateKey(AuthorizationSetBuilder()
                              .Authorization(TAG_ALGORITHM, KM_ALGORITHM_EC)
                              .SigningKey()
                              .Digest(KM_DIGEST_NONE)));

    EXPECT_EQ(0, GetParam()->keymaster0_calls());
}

TEST_P(NewKeyGeneration, EcdsaInvalidSize) {
    if (GetParam()->algorithm_in_hardware(KM_ALGORITHM_EC))
        ASSERT_EQ(
            KM_ERROR_UNKNOWN_ERROR,
            GenerateKey(AuthorizationSetBuilder().EcdsaSigningKey(190).Digest(KM_DIGEST_NONE)));
    else
        ASSERT_EQ(
            KM_ERROR_UNSUPPORTED_KEY_SIZE,
            GenerateKey(AuthorizationSetBuilder().EcdsaSigningKey(190).Digest(KM_DIGEST_NONE)));

    if (GetParam()->algorithm_in_hardware(KM_ALGORITHM_EC))
        EXPECT_EQ(1, GetParam()->keymaster0_calls());
}

TEST_P(NewKeyGeneration, EcdsaAllValidSizes) {
    size_t valid_sizes[] = {224, 256, 384, 521};
    for (size_t size : valid_sizes) {
        EXPECT_EQ(KM_ERROR_OK, GenerateKey(AuthorizationSetBuilder().EcdsaSigningKey(size).Digest(
                                   KM_DIGEST_NONE)))
            << "Failed to generate size: "
            << size;
    }

    if (GetParam()->algorithm_in_hardware(KM_ALGORITHM_EC))
        EXPECT_EQ(4, GetParam()->keymaster0_calls());
}

TEST_P(NewKeyGeneration, HmacSha256) {
    ASSERT_EQ(KM_ERROR_OK,
              GenerateKey(AuthorizationSetBuilder().HmacKey(128).Digest(KM_DIGEST_SHA_2_256)));

    EXPECT_EQ(0, GetParam()->keymaster0_calls());
}

typedef Keymaster1Test GetKeyCharacteristics;
INSTANTIATE_TEST_CASE_P(AndroidKeymasterTest, GetKeyCharacteristics, test_params);

TEST_P(GetKeyCharacteristics, SimpleRsa) {
    ASSERT_EQ(KM_ERROR_OK, GenerateKey(AuthorizationSetBuilder()
                                           .RsaSigningKey(256, 3)
                                           .Digest(KM_DIGEST_NONE)
                                           .Padding(KM_PAD_NONE)));
    AuthorizationSet original(sw_enforced());

    ASSERT_EQ(KM_ERROR_OK, GetCharacteristics());
    EXPECT_EQ(original, sw_enforced());

    if (GetParam()->algorithm_in_hardware(KM_ALGORITHM_RSA))
        EXPECT_EQ(1, GetParam()->keymaster0_calls());
}

typedef Keymaster1Test SigningOperationsTest;
INSTANTIATE_TEST_CASE_P(AndroidKeymasterTest, SigningOperationsTest, test_params);

TEST_P(SigningOperationsTest, RsaSuccess) {
    ASSERT_EQ(KM_ERROR_OK, GenerateKey(AuthorizationSetBuilder()
                                           .RsaSigningKey(256, 3)
                                           .Digest(KM_DIGEST_NONE)
                                           .Padding(KM_PAD_NONE)));
    string message = "12345678901234567890123456789012";
    string signature;
    SignMessage(message, &signature, KM_DIGEST_NONE, KM_PAD_NONE);

    if (GetParam()->algorithm_in_hardware(KM_ALGORITHM_RSA))
        EXPECT_EQ(3, GetParam()->keymaster0_calls());
}

TEST_P(SigningOperationsTest, RsaPssSha256Success) {
    ASSERT_EQ(KM_ERROR_OK, GenerateKey(AuthorizationSetBuilder()
                                           .RsaSigningKey(512, 3)
                                           .Digest(KM_DIGEST_SHA_2_256)
                                           .Padding(KM_PAD_RSA_PSS)));
    // Use large message, which won't work without digesting.
    string message(1024, 'a');
    string signature;
    SignMessage(message, &signature, KM_DIGEST_SHA_2_256, KM_PAD_RSA_PSS);

    if (GetParam()->algorithm_in_hardware(KM_ALGORITHM_RSA))
        EXPECT_EQ(3, GetParam()->keymaster0_calls());
}

TEST_P(SigningOperationsTest, RsaPkcs1Sha256Success) {
    ASSERT_EQ(KM_ERROR_OK, GenerateKey(AuthorizationSetBuilder()
                                           .RsaSigningKey(512, 3)
                                           .Digest(KM_DIGEST_SHA_2_256)
                                           .Padding(KM_PAD_RSA_PKCS1_1_5_SIGN)));
    string message(1024, 'a');
    string signature;
    SignMessage(message, &signature, KM_DIGEST_SHA_2_256, KM_PAD_RSA_PKCS1_1_5_SIGN);

    if (GetParam()->algorithm_in_hardware(KM_ALGORITHM_RSA))
        EXPECT_EQ(3, GetParam()->keymaster0_calls());
}

TEST_P(SigningOperationsTest, RsaPssSha256TooSmallKey) {
    // Key must be at least 10 bytes larger than hash, to provide eight bytes of random salt, so
    // verify that nine bytes larger than hash won't work.
    ASSERT_EQ(KM_ERROR_OK, GenerateKey(AuthorizationSetBuilder()
                                           .RsaSigningKey(256 + 9 * 8, 3)
                                           .Digest(KM_DIGEST_SHA_2_256)
                                           .Padding(KM_PAD_RSA_PSS)));
    string message(1024, 'a');
    string signature;

    AuthorizationSet begin_params(client_params());
    begin_params.push_back(TAG_DIGEST, KM_DIGEST_SHA_2_256);
    begin_params.push_back(TAG_PADDING, KM_PAD_RSA_PSS);
    EXPECT_EQ(KM_ERROR_INCOMPATIBLE_DIGEST, BeginOperation(KM_PURPOSE_SIGN, begin_params));
}

TEST_P(SigningOperationsTest, RsaAbort) {
    ASSERT_EQ(KM_ERROR_OK, GenerateKey(AuthorizationSetBuilder()
                                           .RsaSigningKey(256, 3)
                                           .Digest(KM_DIGEST_NONE)
                                           .Padding(KM_PAD_NONE)));
    AuthorizationSet begin_params(client_params());
    begin_params.push_back(TAG_DIGEST, KM_DIGEST_NONE);
    begin_params.push_back(TAG_PADDING, KM_PAD_NONE);
    ASSERT_EQ(KM_ERROR_OK, BeginOperation(KM_PURPOSE_SIGN, begin_params));
    EXPECT_EQ(KM_ERROR_OK, AbortOperation());
    // Another abort should fail
    EXPECT_EQ(KM_ERROR_INVALID_OPERATION_HANDLE, AbortOperation());

    if (GetParam()->algorithm_in_hardware(KM_ALGORITHM_RSA))
        EXPECT_EQ(2, GetParam()->keymaster0_calls());
}

TEST_P(SigningOperationsTest, RsaUnsupportedDigest) {
    GenerateKey(AuthorizationSetBuilder()
                    .RsaSigningKey(256, 3)
                    .Digest(KM_DIGEST_MD5)
                    .Padding(KM_PAD_RSA_PSS /* supported padding */));
    ASSERT_EQ(KM_ERROR_UNSUPPORTED_DIGEST, BeginOperation(KM_PURPOSE_SIGN));

    if (GetParam()->algorithm_in_hardware(KM_ALGORITHM_RSA))
        EXPECT_EQ(2, GetParam()->keymaster0_calls());
}

TEST_P(SigningOperationsTest, RsaUnsupportedPadding) {
    GenerateKey(AuthorizationSetBuilder()
                    .RsaSigningKey(256, 3)
                    .Digest(KM_DIGEST_SHA_2_256 /* supported digest */)
                    .Padding(KM_PAD_PKCS7));
    AuthorizationSet begin_params(client_params());
    begin_params.push_back(TAG_DIGEST, KM_DIGEST_SHA_2_256);
    ASSERT_EQ(KM_ERROR_UNSUPPORTED_PADDING_MODE, BeginOperation(KM_PURPOSE_SIGN, begin_params));

    if (GetParam()->algorithm_in_hardware(KM_ALGORITHM_RSA))
        EXPECT_EQ(2, GetParam()->keymaster0_calls());
}

TEST_P(SigningOperationsTest, RsaNoDigest) {
    // PSS requires a digest.
    GenerateKey(AuthorizationSetBuilder()
                    .RsaSigningKey(256, 3)
                    .Digest(KM_DIGEST_NONE)
                    .Padding(KM_PAD_RSA_PSS));
    AuthorizationSet begin_params(client_params());
    begin_params.push_back(TAG_DIGEST, KM_DIGEST_NONE);
    begin_params.push_back(TAG_PADDING, KM_PAD_RSA_PSS);
    ASSERT_EQ(KM_ERROR_INCOMPATIBLE_DIGEST, BeginOperation(KM_PURPOSE_SIGN, begin_params));

    if (GetParam()->algorithm_in_hardware(KM_ALGORITHM_RSA))
        EXPECT_EQ(2, GetParam()->keymaster0_calls());
}

TEST_P(SigningOperationsTest, RsaNoPadding) {
    // Padding must be specified
    ASSERT_EQ(KM_ERROR_OK, GenerateKey(AuthorizationSetBuilder().RsaKey(256, 3).SigningKey().Digest(
                               KM_DIGEST_NONE)));
    AuthorizationSet begin_params(client_params());
    begin_params.push_back(TAG_DIGEST, KM_DIGEST_NONE);
    ASSERT_EQ(KM_ERROR_UNSUPPORTED_PADDING_MODE, BeginOperation(KM_PURPOSE_SIGN, begin_params));

    if (GetParam()->algorithm_in_hardware(KM_ALGORITHM_RSA))
        EXPECT_EQ(2, GetParam()->keymaster0_calls());
}

TEST_P(SigningOperationsTest, RsaTooShortMessage) {
    ASSERT_EQ(KM_ERROR_OK, GenerateKey(AuthorizationSetBuilder()
                                           .RsaSigningKey(256, 3)
                                           .Digest(KM_DIGEST_NONE)
                                           .Padding(KM_PAD_NONE)));
    AuthorizationSet begin_params(client_params());
    begin_params.push_back(TAG_DIGEST, KM_DIGEST_NONE);
    begin_params.push_back(TAG_PADDING, KM_PAD_NONE);
    ASSERT_EQ(KM_ERROR_OK, BeginOperation(KM_PURPOSE_SIGN, begin_params));

    string message = "1234567890123456789012345678901";
    string result;
    size_t input_consumed;
    ASSERT_EQ(KM_ERROR_OK, UpdateOperation(message, &result, &input_consumed));
    EXPECT_EQ(0U, result.size());
    EXPECT_EQ(31U, input_consumed);

    string signature;
    ASSERT_EQ(KM_ERROR_UNKNOWN_ERROR, FinishOperation(&signature));
    EXPECT_EQ(0U, signature.length());

    if (GetParam()->algorithm_in_hardware(KM_ALGORITHM_RSA))
        EXPECT_EQ(2, GetParam()->keymaster0_calls());
}

TEST_P(SigningOperationsTest, RsaSignWithEncryptionKey) {
    ASSERT_EQ(KM_ERROR_OK, GenerateKey(AuthorizationSetBuilder()
                                           .RsaEncryptionKey(256, 3)
                                           .Digest(KM_DIGEST_NONE)
                                           .Padding(KM_PAD_NONE)));
    ASSERT_EQ(KM_ERROR_INCOMPATIBLE_PURPOSE, BeginOperation(KM_PURPOSE_SIGN));
    ASSERT_EQ(KM_ERROR_INCOMPATIBLE_PURPOSE, BeginOperation(KM_PURPOSE_VERIFY));

    if (GetParam()->algorithm_in_hardware(KM_ALGORITHM_RSA))
        EXPECT_EQ(3, GetParam()->keymaster0_calls());
}

TEST_P(SigningOperationsTest, EcdsaSuccess) {
    ASSERT_EQ(KM_ERROR_OK,
              GenerateKey(AuthorizationSetBuilder().EcdsaSigningKey(224).Digest(KM_DIGEST_NONE)));
    string message(1024, 'a');
    string signature;
    SignMessage(message, &signature, KM_DIGEST_NONE);

    if (GetParam()->algorithm_in_hardware(KM_ALGORITHM_EC))
        EXPECT_EQ(3, GetParam()->keymaster0_calls());
}

TEST_P(SigningOperationsTest, EcdsaSha256Success) {
    ASSERT_EQ(KM_ERROR_OK, GenerateKey(AuthorizationSetBuilder().EcdsaSigningKey(224).Digest(
                               KM_DIGEST_SHA_2_256)));
    string message(1024, 'a');
    string signature;
    SignMessage(message, &signature, KM_DIGEST_SHA_2_256);

    if (GetParam()->algorithm_in_hardware(KM_ALGORITHM_EC))
        EXPECT_EQ(3, GetParam()->keymaster0_calls());
}

TEST_P(SigningOperationsTest, AesEcbSign) {
    ASSERT_EQ(KM_ERROR_OK,
              GenerateKey(AuthorizationSetBuilder().AesEncryptionKey(128).Authorization(
                  TAG_BLOCK_MODE, KM_MODE_ECB)));
    ASSERT_EQ(KM_ERROR_INCOMPATIBLE_PURPOSE, BeginOperation(KM_PURPOSE_SIGN));
    ASSERT_EQ(KM_ERROR_INCOMPATIBLE_PURPOSE, BeginOperation(KM_PURPOSE_VERIFY));

    EXPECT_EQ(0, GetParam()->keymaster0_calls());
}

TEST_P(SigningOperationsTest, HmacSha1Success) {
    GenerateKey(AuthorizationSetBuilder().HmacKey(128).Digest(KM_DIGEST_SHA1));
    string message = "12345678901234567890123456789012";
    string signature;
    MacMessage(message, &signature, KM_DIGEST_SHA1, 160);
    ASSERT_EQ(20U, signature.size());

    EXPECT_EQ(0, GetParam()->keymaster0_calls());
}

TEST_P(SigningOperationsTest, HmacSha224Success) {
    ASSERT_EQ(KM_ERROR_OK,
              GenerateKey(AuthorizationSetBuilder().HmacKey(128).Digest(KM_DIGEST_SHA_2_224)));
    string message = "12345678901234567890123456789012";
    string signature;
    MacMessage(message, &signature, KM_DIGEST_SHA_2_224, 224);
    ASSERT_EQ(28U, signature.size());

    EXPECT_EQ(0, GetParam()->keymaster0_calls());
}

TEST_P(SigningOperationsTest, HmacSha256Success) {
    ASSERT_EQ(KM_ERROR_OK,
              GenerateKey(AuthorizationSetBuilder().HmacKey(128).Digest(KM_DIGEST_SHA_2_256)));
    string message = "12345678901234567890123456789012";
    string signature;
    MacMessage(message, &signature, KM_DIGEST_SHA_2_256, 256);
    ASSERT_EQ(32U, signature.size());

    EXPECT_EQ(0, GetParam()->keymaster0_calls());
}

TEST_P(SigningOperationsTest, HmacSha384Success) {
    ASSERT_EQ(KM_ERROR_OK,
              GenerateKey(AuthorizationSetBuilder().HmacKey(128).Digest(KM_DIGEST_SHA_2_384)));

    string message = "12345678901234567890123456789012";
    string signature;
    MacMessage(message, &signature, KM_DIGEST_SHA_2_384, 384);
    ASSERT_EQ(48U, signature.size());

    EXPECT_EQ(0, GetParam()->keymaster0_calls());
}

TEST_P(SigningOperationsTest, HmacSha512Success) {
    ASSERT_EQ(KM_ERROR_OK,
              GenerateKey(AuthorizationSetBuilder().HmacKey(128).Digest(KM_DIGEST_SHA_2_512)));
    string message = "12345678901234567890123456789012";
    string signature;
    MacMessage(message, &signature, KM_DIGEST_SHA_2_512, 512);
    ASSERT_EQ(64U, signature.size());

    EXPECT_EQ(0, GetParam()->keymaster0_calls());
}

TEST_P(SigningOperationsTest, HmacLengthInKey) {
    // TODO(swillden): unified API should generate an error on key generation.
    ASSERT_EQ(KM_ERROR_OK, GenerateKey(AuthorizationSetBuilder()
                                           .HmacKey(128)
                                           .Digest(KM_DIGEST_SHA_2_256)
                                           .Authorization(TAG_MAC_LENGTH, 20)));
    string message = "12345678901234567890123456789012";
    string signature;
    MacMessage(message, &signature, KM_DIGEST_SHA_2_256, 240);
    // Size in key was ignored.
    ASSERT_EQ(30U, signature.size());

    EXPECT_EQ(0, GetParam()->keymaster0_calls());
}

TEST_P(SigningOperationsTest, HmacRfc4231TestCase1) {
    uint8_t key_data[] = {
        0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b,
        0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b,
    };
    string message = "Hi There";
    uint8_t sha_224_expected[] = {
        0x89, 0x6f, 0xb1, 0x12, 0x8a, 0xbb, 0xdf, 0x19, 0x68, 0x32, 0x10, 0x7c, 0xd4, 0x9d,
        0xf3, 0x3f, 0x47, 0xb4, 0xb1, 0x16, 0x99, 0x12, 0xba, 0x4f, 0x53, 0x68, 0x4b, 0x22,
    };
    uint8_t sha_256_expected[] = {
        0xb0, 0x34, 0x4c, 0x61, 0xd8, 0xdb, 0x38, 0x53, 0x5c, 0xa8, 0xaf,
        0xce, 0xaf, 0x0b, 0xf1, 0x2b, 0x88, 0x1d, 0xc2, 0x00, 0xc9, 0x83,
        0x3d, 0xa7, 0x26, 0xe9, 0x37, 0x6c, 0x2e, 0x32, 0xcf, 0xf7,
    };
    uint8_t sha_384_expected[] = {
        0xaf, 0xd0, 0x39, 0x44, 0xd8, 0x48, 0x95, 0x62, 0x6b, 0x08, 0x25, 0xf4,
        0xab, 0x46, 0x90, 0x7f, 0x15, 0xf9, 0xda, 0xdb, 0xe4, 0x10, 0x1e, 0xc6,
        0x82, 0xaa, 0x03, 0x4c, 0x7c, 0xeb, 0xc5, 0x9c, 0xfa, 0xea, 0x9e, 0xa9,
        0x07, 0x6e, 0xde, 0x7f, 0x4a, 0xf1, 0x52, 0xe8, 0xb2, 0xfa, 0x9c, 0xb6,
    };
    uint8_t sha_512_expected[] = {
        0x87, 0xaa, 0x7c, 0xde, 0xa5, 0xef, 0x61, 0x9d, 0x4f, 0xf0, 0xb4, 0x24, 0x1a,
        0x1d, 0x6c, 0xb0, 0x23, 0x79, 0xf4, 0xe2, 0xce, 0x4e, 0xc2, 0x78, 0x7a, 0xd0,
        0xb3, 0x05, 0x45, 0xe1, 0x7c, 0xde, 0xda, 0xa8, 0x33, 0xb7, 0xd6, 0xb8, 0xa7,
        0x02, 0x03, 0x8b, 0x27, 0x4e, 0xae, 0xa3, 0xf4, 0xe4, 0xbe, 0x9d, 0x91, 0x4e,
        0xeb, 0x61, 0xf1, 0x70, 0x2e, 0x69, 0x6c, 0x20, 0x3a, 0x12, 0x68, 0x54,
    };

    string key = make_string(key_data);

    CheckHmacTestVector(key, message, KM_DIGEST_SHA_2_224, make_string(sha_224_expected));
    CheckHmacTestVector(key, message, KM_DIGEST_SHA_2_256, make_string(sha_256_expected));
    CheckHmacTestVector(key, message, KM_DIGEST_SHA_2_384, make_string(sha_384_expected));
    CheckHmacTestVector(key, message, KM_DIGEST_SHA_2_512, make_string(sha_512_expected));

    EXPECT_EQ(0, GetParam()->keymaster0_calls());
}

TEST_P(SigningOperationsTest, HmacRfc4231TestCase2) {
    string key = "Jefe";
    string message = "what do ya want for nothing?";
    uint8_t sha_224_expected[] = {
        0xa3, 0x0e, 0x01, 0x09, 0x8b, 0xc6, 0xdb, 0xbf, 0x45, 0x69, 0x0f, 0x3a, 0x7e, 0x9e,
        0x6d, 0x0f, 0x8b, 0xbe, 0xa2, 0xa3, 0x9e, 0x61, 0x48, 0x00, 0x8f, 0xd0, 0x5e, 0x44,
    };
    uint8_t sha_256_expected[] = {
        0x5b, 0xdc, 0xc1, 0x46, 0xbf, 0x60, 0x75, 0x4e, 0x6a, 0x04, 0x24,
        0x26, 0x08, 0x95, 0x75, 0xc7, 0x5a, 0x00, 0x3f, 0x08, 0x9d, 0x27,
        0x39, 0x83, 0x9d, 0xec, 0x58, 0xb9, 0x64, 0xec, 0x38, 0x43,
    };
    uint8_t sha_384_expected[] = {
        0xaf, 0x45, 0xd2, 0xe3, 0x76, 0x48, 0x40, 0x31, 0x61, 0x7f, 0x78, 0xd2,
        0xb5, 0x8a, 0x6b, 0x1b, 0x9c, 0x7e, 0xf4, 0x64, 0xf5, 0xa0, 0x1b, 0x47,
        0xe4, 0x2e, 0xc3, 0x73, 0x63, 0x22, 0x44, 0x5e, 0x8e, 0x22, 0x40, 0xca,
        0x5e, 0x69, 0xe2, 0xc7, 0x8b, 0x32, 0x39, 0xec, 0xfa, 0xb2, 0x16, 0x49,
    };
    uint8_t sha_512_expected[] = {
        0x16, 0x4b, 0x7a, 0x7b, 0xfc, 0xf8, 0x19, 0xe2, 0xe3, 0x95, 0xfb, 0xe7, 0x3b,
        0x56, 0xe0, 0xa3, 0x87, 0xbd, 0x64, 0x22, 0x2e, 0x83, 0x1f, 0xd6, 0x10, 0x27,
        0x0c, 0xd7, 0xea, 0x25, 0x05, 0x54, 0x97, 0x58, 0xbf, 0x75, 0xc0, 0x5a, 0x99,
        0x4a, 0x6d, 0x03, 0x4f, 0x65, 0xf8, 0xf0, 0xe6, 0xfd, 0xca, 0xea, 0xb1, 0xa3,
        0x4d, 0x4a, 0x6b, 0x4b, 0x63, 0x6e, 0x07, 0x0a, 0x38, 0xbc, 0xe7, 0x37,
    };

    CheckHmacTestVector(key, message, KM_DIGEST_SHA_2_224, make_string(sha_224_expected));
    CheckHmacTestVector(key, message, KM_DIGEST_SHA_2_256, make_string(sha_256_expected));
    CheckHmacTestVector(key, message, KM_DIGEST_SHA_2_384, make_string(sha_384_expected));
    CheckHmacTestVector(key, message, KM_DIGEST_SHA_2_512, make_string(sha_512_expected));

    EXPECT_EQ(0, GetParam()->keymaster0_calls());
}

TEST_P(SigningOperationsTest, HmacRfc4231TestCase3) {
    string key(20, 0xaa);
    string message(50, 0xdd);
    uint8_t sha_224_expected[] = {
        0x7f, 0xb3, 0xcb, 0x35, 0x88, 0xc6, 0xc1, 0xf6, 0xff, 0xa9, 0x69, 0x4d, 0x7d, 0x6a,
        0xd2, 0x64, 0x93, 0x65, 0xb0, 0xc1, 0xf6, 0x5d, 0x69, 0xd1, 0xec, 0x83, 0x33, 0xea,
    };
    uint8_t sha_256_expected[] = {
        0x77, 0x3e, 0xa9, 0x1e, 0x36, 0x80, 0x0e, 0x46, 0x85, 0x4d, 0xb8,
        0xeb, 0xd0, 0x91, 0x81, 0xa7, 0x29, 0x59, 0x09, 0x8b, 0x3e, 0xf8,
        0xc1, 0x22, 0xd9, 0x63, 0x55, 0x14, 0xce, 0xd5, 0x65, 0xfe,
    };
    uint8_t sha_384_expected[] = {
        0x88, 0x06, 0x26, 0x08, 0xd3, 0xe6, 0xad, 0x8a, 0x0a, 0xa2, 0xac, 0xe0,
        0x14, 0xc8, 0xa8, 0x6f, 0x0a, 0xa6, 0x35, 0xd9, 0x47, 0xac, 0x9f, 0xeb,
        0xe8, 0x3e, 0xf4, 0xe5, 0x59, 0x66, 0x14, 0x4b, 0x2a, 0x5a, 0xb3, 0x9d,
        0xc1, 0x38, 0x14, 0xb9, 0x4e, 0x3a, 0xb6, 0xe1, 0x01, 0xa3, 0x4f, 0x27,
    };
    uint8_t sha_512_expected[] = {
        0xfa, 0x73, 0xb0, 0x08, 0x9d, 0x56, 0xa2, 0x84, 0xef, 0xb0, 0xf0, 0x75, 0x6c,
        0x89, 0x0b, 0xe9, 0xb1, 0xb5, 0xdb, 0xdd, 0x8e, 0xe8, 0x1a, 0x36, 0x55, 0xf8,
        0x3e, 0x33, 0xb2, 0x27, 0x9d, 0x39, 0xbf, 0x3e, 0x84, 0x82, 0x79, 0xa7, 0x22,
        0xc8, 0x06, 0xb4, 0x85, 0xa4, 0x7e, 0x67, 0xc8, 0x07, 0xb9, 0x46, 0xa3, 0x37,
        0xbe, 0xe8, 0x94, 0x26, 0x74, 0x27, 0x88, 0x59, 0xe1, 0x32, 0x92, 0xfb,
    };

    CheckHmacTestVector(key, message, KM_DIGEST_SHA_2_224, make_string(sha_224_expected));
    CheckHmacTestVector(key, message, KM_DIGEST_SHA_2_256, make_string(sha_256_expected));
    CheckHmacTestVector(key, message, KM_DIGEST_SHA_2_384, make_string(sha_384_expected));
    CheckHmacTestVector(key, message, KM_DIGEST_SHA_2_512, make_string(sha_512_expected));

    EXPECT_EQ(0, GetParam()->keymaster0_calls());
}

TEST_P(SigningOperationsTest, HmacRfc4231TestCase4) {
    uint8_t key_data[25] = {
        0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d,
        0x0e, 0x0f, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19,
    };
    string key = make_string(key_data);
    string message(50, 0xcd);
    uint8_t sha_224_expected[] = {
        0x6c, 0x11, 0x50, 0x68, 0x74, 0x01, 0x3c, 0xac, 0x6a, 0x2a, 0xbc, 0x1b, 0xb3, 0x82,
        0x62, 0x7c, 0xec, 0x6a, 0x90, 0xd8, 0x6e, 0xfc, 0x01, 0x2d, 0xe7, 0xaf, 0xec, 0x5a,
    };
    uint8_t sha_256_expected[] = {
        0x82, 0x55, 0x8a, 0x38, 0x9a, 0x44, 0x3c, 0x0e, 0xa4, 0xcc, 0x81,
        0x98, 0x99, 0xf2, 0x08, 0x3a, 0x85, 0xf0, 0xfa, 0xa3, 0xe5, 0x78,
        0xf8, 0x07, 0x7a, 0x2e, 0x3f, 0xf4, 0x67, 0x29, 0x66, 0x5b,
    };
    uint8_t sha_384_expected[] = {
        0x3e, 0x8a, 0x69, 0xb7, 0x78, 0x3c, 0x25, 0x85, 0x19, 0x33, 0xab, 0x62,
        0x90, 0xaf, 0x6c, 0xa7, 0x7a, 0x99, 0x81, 0x48, 0x08, 0x50, 0x00, 0x9c,
        0xc5, 0x57, 0x7c, 0x6e, 0x1f, 0x57, 0x3b, 0x4e, 0x68, 0x01, 0xdd, 0x23,
        0xc4, 0xa7, 0xd6, 0x79, 0xcc, 0xf8, 0xa3, 0x86, 0xc6, 0x74, 0xcf, 0xfb,
    };
    uint8_t sha_512_expected[] = {
        0xb0, 0xba, 0x46, 0x56, 0x37, 0x45, 0x8c, 0x69, 0x90, 0xe5, 0xa8, 0xc5, 0xf6,
        0x1d, 0x4a, 0xf7, 0xe5, 0x76, 0xd9, 0x7f, 0xf9, 0x4b, 0x87, 0x2d, 0xe7, 0x6f,
        0x80, 0x50, 0x36, 0x1e, 0xe3, 0xdb, 0xa9, 0x1c, 0xa5, 0xc1, 0x1a, 0xa2, 0x5e,
        0xb4, 0xd6, 0x79, 0x27, 0x5c, 0xc5, 0x78, 0x80, 0x63, 0xa5, 0xf1, 0x97, 0x41,
        0x12, 0x0c, 0x4f, 0x2d, 0xe2, 0xad, 0xeb, 0xeb, 0x10, 0xa2, 0x98, 0xdd,
    };

    CheckHmacTestVector(key, message, KM_DIGEST_SHA_2_224, make_string(sha_224_expected));
    CheckHmacTestVector(key, message, KM_DIGEST_SHA_2_256, make_string(sha_256_expected));
    CheckHmacTestVector(key, message, KM_DIGEST_SHA_2_384, make_string(sha_384_expected));
    CheckHmacTestVector(key, message, KM_DIGEST_SHA_2_512, make_string(sha_512_expected));

    EXPECT_EQ(0, GetParam()->keymaster0_calls());
}

TEST_P(SigningOperationsTest, HmacRfc4231TestCase5) {
    string key(20, 0x0c);
    string message = "Test With Truncation";

    uint8_t sha_224_expected[] = {
        0x0e, 0x2a, 0xea, 0x68, 0xa9, 0x0c, 0x8d, 0x37,
        0xc9, 0x88, 0xbc, 0xdb, 0x9f, 0xca, 0x6f, 0xa8,
    };
    uint8_t sha_256_expected[] = {
        0xa3, 0xb6, 0x16, 0x74, 0x73, 0x10, 0x0e, 0xe0,
        0x6e, 0x0c, 0x79, 0x6c, 0x29, 0x55, 0x55, 0x2b,
    };
    uint8_t sha_384_expected[] = {
        0x3a, 0xbf, 0x34, 0xc3, 0x50, 0x3b, 0x2a, 0x23,
        0xa4, 0x6e, 0xfc, 0x61, 0x9b, 0xae, 0xf8, 0x97,
    };
    uint8_t sha_512_expected[] = {
        0x41, 0x5f, 0xad, 0x62, 0x71, 0x58, 0x0a, 0x53,
        0x1d, 0x41, 0x79, 0xbc, 0x89, 0x1d, 0x87, 0xa6,
    };

    CheckHmacTestVector(key, message, KM_DIGEST_SHA_2_224, make_string(sha_224_expected));
    CheckHmacTestVector(key, message, KM_DIGEST_SHA_2_256, make_string(sha_256_expected));
    CheckHmacTestVector(key, message, KM_DIGEST_SHA_2_384, make_string(sha_384_expected));
    CheckHmacTestVector(key, message, KM_DIGEST_SHA_2_512, make_string(sha_512_expected));

    EXPECT_EQ(0, GetParam()->keymaster0_calls());
}

TEST_P(SigningOperationsTest, HmacRfc4231TestCase6) {
    string key(131, 0xaa);
    string message = "Test Using Larger Than Block-Size Key - Hash Key First";

    uint8_t sha_224_expected[] = {
        0x95, 0xe9, 0xa0, 0xdb, 0x96, 0x20, 0x95, 0xad, 0xae, 0xbe, 0x9b, 0x2d, 0x6f, 0x0d,
        0xbc, 0xe2, 0xd4, 0x99, 0xf1, 0x12, 0xf2, 0xd2, 0xb7, 0x27, 0x3f, 0xa6, 0x87, 0x0e,
    };
    uint8_t sha_256_expected[] = {
        0x60, 0xe4, 0x31, 0x59, 0x1e, 0xe0, 0xb6, 0x7f, 0x0d, 0x8a, 0x26,
        0xaa, 0xcb, 0xf5, 0xb7, 0x7f, 0x8e, 0x0b, 0xc6, 0x21, 0x37, 0x28,
        0xc5, 0x14, 0x05, 0x46, 0x04, 0x0f, 0x0e, 0xe3, 0x7f, 0x54,
    };
    uint8_t sha_384_expected[] = {
        0x4e, 0xce, 0x08, 0x44, 0x85, 0x81, 0x3e, 0x90, 0x88, 0xd2, 0xc6, 0x3a,
        0x04, 0x1b, 0xc5, 0xb4, 0x4f, 0x9e, 0xf1, 0x01, 0x2a, 0x2b, 0x58, 0x8f,
        0x3c, 0xd1, 0x1f, 0x05, 0x03, 0x3a, 0xc4, 0xc6, 0x0c, 0x2e, 0xf6, 0xab,
        0x40, 0x30, 0xfe, 0x82, 0x96, 0x24, 0x8d, 0xf1, 0x63, 0xf4, 0x49, 0x52,
    };
    uint8_t sha_512_expected[] = {
        0x80, 0xb2, 0x42, 0x63, 0xc7, 0xc1, 0xa3, 0xeb, 0xb7, 0x14, 0x93, 0xc1, 0xdd,
        0x7b, 0xe8, 0xb4, 0x9b, 0x46, 0xd1, 0xf4, 0x1b, 0x4a, 0xee, 0xc1, 0x12, 0x1b,
        0x01, 0x37, 0x83, 0xf8, 0xf3, 0x52, 0x6b, 0x56, 0xd0, 0x37, 0xe0, 0x5f, 0x25,
        0x98, 0xbd, 0x0f, 0xd2, 0x21, 0x5d, 0x6a, 0x1e, 0x52, 0x95, 0xe6, 0x4f, 0x73,
        0xf6, 0x3f, 0x0a, 0xec, 0x8b, 0x91, 0x5a, 0x98, 0x5d, 0x78, 0x65, 0x98,
    };

    CheckHmacTestVector(key, message, KM_DIGEST_SHA_2_224, make_string(sha_224_expected));
    CheckHmacTestVector(key, message, KM_DIGEST_SHA_2_256, make_string(sha_256_expected));
    CheckHmacTestVector(key, message, KM_DIGEST_SHA_2_384, make_string(sha_384_expected));
    CheckHmacTestVector(key, message, KM_DIGEST_SHA_2_512, make_string(sha_512_expected));

    EXPECT_EQ(0, GetParam()->keymaster0_calls());
}

TEST_P(SigningOperationsTest, HmacRfc4231TestCase7) {
    string key(131, 0xaa);
    string message = "This is a test using a larger than block-size key and a larger than "
                     "block-size data. The key needs to be hashed before being used by the HMAC "
                     "algorithm.";

    uint8_t sha_224_expected[] = {
        0x3a, 0x85, 0x41, 0x66, 0xac, 0x5d, 0x9f, 0x02, 0x3f, 0x54, 0xd5, 0x17, 0xd0, 0xb3,
        0x9d, 0xbd, 0x94, 0x67, 0x70, 0xdb, 0x9c, 0x2b, 0x95, 0xc9, 0xf6, 0xf5, 0x65, 0xd1,
    };
    uint8_t sha_256_expected[] = {
        0x9b, 0x09, 0xff, 0xa7, 0x1b, 0x94, 0x2f, 0xcb, 0x27, 0x63, 0x5f,
        0xbc, 0xd5, 0xb0, 0xe9, 0x44, 0xbf, 0xdc, 0x63, 0x64, 0x4f, 0x07,
        0x13, 0x93, 0x8a, 0x7f, 0x51, 0x53, 0x5c, 0x3a, 0x35, 0xe2,
    };
    uint8_t sha_384_expected[] = {
        0x66, 0x17, 0x17, 0x8e, 0x94, 0x1f, 0x02, 0x0d, 0x35, 0x1e, 0x2f, 0x25,
        0x4e, 0x8f, 0xd3, 0x2c, 0x60, 0x24, 0x20, 0xfe, 0xb0, 0xb8, 0xfb, 0x9a,
        0xdc, 0xce, 0xbb, 0x82, 0x46, 0x1e, 0x99, 0xc5, 0xa6, 0x78, 0xcc, 0x31,
        0xe7, 0x99, 0x17, 0x6d, 0x38, 0x60, 0xe6, 0x11, 0x0c, 0x46, 0x52, 0x3e,
    };
    uint8_t sha_512_expected[] = {
        0xe3, 0x7b, 0x6a, 0x77, 0x5d, 0xc8, 0x7d, 0xba, 0xa4, 0xdf, 0xa9, 0xf9, 0x6e,
        0x5e, 0x3f, 0xfd, 0xde, 0xbd, 0x71, 0xf8, 0x86, 0x72, 0x89, 0x86, 0x5d, 0xf5,
        0xa3, 0x2d, 0x20, 0xcd, 0xc9, 0x44, 0xb6, 0x02, 0x2c, 0xac, 0x3c, 0x49, 0x82,
        0xb1, 0x0d, 0x5e, 0xeb, 0x55, 0xc3, 0xe4, 0xde, 0x15, 0x13, 0x46, 0x76, 0xfb,
        0x6d, 0xe0, 0x44, 0x60, 0x65, 0xc9, 0x74, 0x40, 0xfa, 0x8c, 0x6a, 0x58,
    };

    CheckHmacTestVector(key, message, KM_DIGEST_SHA_2_224, make_string(sha_224_expected));
    CheckHmacTestVector(key, message, KM_DIGEST_SHA_2_256, make_string(sha_256_expected));
    CheckHmacTestVector(key, message, KM_DIGEST_SHA_2_384, make_string(sha_384_expected));
    CheckHmacTestVector(key, message, KM_DIGEST_SHA_2_512, make_string(sha_512_expected));

    EXPECT_EQ(0, GetParam()->keymaster0_calls());
}

TEST_P(SigningOperationsTest, HmacSha256TooLargeMacLength) {
    ASSERT_EQ(KM_ERROR_OK,
              GenerateKey(AuthorizationSetBuilder().HmacKey(128).Digest(KM_DIGEST_SHA_2_256)));
    AuthorizationSet begin_params(client_params());
    begin_params.push_back(TAG_MAC_LENGTH, 264);
    begin_params.push_back(TAG_DIGEST, KM_DIGEST_SHA_2_256);
    ASSERT_EQ(KM_ERROR_OK,
              BeginOperation(KM_PURPOSE_SIGN, begin_params, nullptr /* output_params */));
    string message = "1234567890123456789012345678901";
    string result;
    size_t input_consumed;
    ASSERT_EQ(KM_ERROR_OK, UpdateOperation(message, &result, &input_consumed));
    ASSERT_EQ(KM_ERROR_UNSUPPORTED_MAC_LENGTH, FinishOperation(&result));

    EXPECT_EQ(0, GetParam()->keymaster0_calls());
}

// TODO(swillden): Add more verification failure tests.

typedef Keymaster1Test VerificationOperationsTest;
INSTANTIATE_TEST_CASE_P(AndroidKeymasterTest, VerificationOperationsTest, test_params);

TEST_P(VerificationOperationsTest, RsaSuccess) {
    ASSERT_EQ(KM_ERROR_OK, GenerateKey(AuthorizationSetBuilder()
                                           .RsaSigningKey(256, 3)
                                           .Digest(KM_DIGEST_NONE)
                                           .Padding(KM_PAD_NONE)));
    string message = "12345678901234567890123456789012";
    string signature;
    SignMessage(message, &signature, KM_DIGEST_NONE, KM_PAD_NONE);
    VerifyMessage(message, signature, KM_DIGEST_NONE, KM_PAD_NONE);

    if (GetParam()->algorithm_in_hardware(KM_ALGORITHM_RSA))
        EXPECT_EQ(4, GetParam()->keymaster0_calls());
}

TEST_P(VerificationOperationsTest, RsaPssSha256Success) {
    ASSERT_EQ(KM_ERROR_OK, GenerateKey(AuthorizationSetBuilder()
                                           .RsaSigningKey(512, 3)
                                           .Digest(KM_DIGEST_SHA_2_256)
                                           .Padding(KM_PAD_RSA_PSS)));
    // Use large message, which won't work without digesting.
    string message(1024, 'a');
    string signature;
    SignMessage(message, &signature, KM_DIGEST_SHA_2_256, KM_PAD_RSA_PSS);
    VerifyMessage(message, signature, KM_DIGEST_SHA_2_256, KM_PAD_RSA_PSS);

    if (GetParam()->algorithm_in_hardware(KM_ALGORITHM_RSA))
        EXPECT_EQ(4, GetParam()->keymaster0_calls());
}

TEST_P(VerificationOperationsTest, RsaPssSha256CorruptSignature) {
    GenerateKey(AuthorizationSetBuilder()
                    .RsaSigningKey(512, 3)
                    .Digest(KM_DIGEST_SHA_2_256)
                    .Padding(KM_PAD_RSA_PSS));
    string message(1024, 'a');
    string signature;
    SignMessage(message, &signature, KM_DIGEST_SHA_2_256, KM_PAD_RSA_PSS);
    ++signature[signature.size() / 2];

    AuthorizationSet begin_params(client_params());
    begin_params.push_back(TAG_DIGEST, KM_DIGEST_SHA_2_256);
    begin_params.push_back(TAG_PADDING, KM_PAD_RSA_PSS);
    EXPECT_EQ(KM_ERROR_OK, BeginOperation(KM_PURPOSE_VERIFY, begin_params));

    string result;
    size_t input_consumed;
    EXPECT_EQ(KM_ERROR_OK, UpdateOperation(message, &result, &input_consumed));
    EXPECT_EQ(message.size(), input_consumed);
    EXPECT_EQ(KM_ERROR_VERIFICATION_FAILED, FinishOperation(signature, &result));

    if (GetParam()->algorithm_in_hardware(KM_ALGORITHM_RSA))
        EXPECT_EQ(4, GetParam()->keymaster0_calls());
}

TEST_P(VerificationOperationsTest, RsaPssSha256CorruptInput) {
    ASSERT_EQ(KM_ERROR_OK, GenerateKey(AuthorizationSetBuilder()
                                           .RsaSigningKey(512, 3)
                                           .Digest(KM_DIGEST_SHA_2_256)
                                           .Padding(KM_PAD_RSA_PSS)));
    // Use large message, which won't work without digesting.
    string message(1024, 'a');
    string signature;
    SignMessage(message, &signature, KM_DIGEST_SHA_2_256, KM_PAD_RSA_PSS);
    ++message[message.size() / 2];

    AuthorizationSet begin_params(client_params());
    begin_params.push_back(TAG_DIGEST, KM_DIGEST_SHA_2_256);
    begin_params.push_back(TAG_PADDING, KM_PAD_RSA_PSS);
    EXPECT_EQ(KM_ERROR_OK, BeginOperation(KM_PURPOSE_VERIFY, begin_params));

    string result;
    size_t input_consumed;
    EXPECT_EQ(KM_ERROR_OK, UpdateOperation(message, &result, &input_consumed));
    EXPECT_EQ(message.size(), input_consumed);
    EXPECT_EQ(KM_ERROR_VERIFICATION_FAILED, FinishOperation(signature, &result));

    if (GetParam()->algorithm_in_hardware(KM_ALGORITHM_RSA))
        EXPECT_EQ(4, GetParam()->keymaster0_calls());
}

TEST_P(VerificationOperationsTest, RsaPkcs1Sha256Success) {
    GenerateKey(AuthorizationSetBuilder()
                    .RsaSigningKey(512, 3)
                    .Digest(KM_DIGEST_SHA_2_256)
                    .Padding(KM_PAD_RSA_PKCS1_1_5_SIGN));
    string message(1024, 'a');
    string signature;
    SignMessage(message, &signature, KM_DIGEST_SHA_2_256, KM_PAD_RSA_PKCS1_1_5_SIGN);
    VerifyMessage(message, signature, KM_DIGEST_SHA_2_256, KM_PAD_RSA_PKCS1_1_5_SIGN);

    if (GetParam()->algorithm_in_hardware(KM_ALGORITHM_RSA))
        EXPECT_EQ(4, GetParam()->keymaster0_calls());
}

TEST_P(VerificationOperationsTest, RsaPkcs1Sha256CorruptSignature) {
    GenerateKey(AuthorizationSetBuilder()
                    .RsaSigningKey(512, 3)
                    .Digest(KM_DIGEST_SHA_2_256)
                    .Padding(KM_PAD_RSA_PKCS1_1_5_SIGN));
    string message(1024, 'a');
    string signature;
    SignMessage(message, &signature, KM_DIGEST_SHA_2_256, KM_PAD_RSA_PKCS1_1_5_SIGN);
    ++signature[signature.size() / 2];

    AuthorizationSet begin_params(client_params());
    begin_params.push_back(TAG_DIGEST, KM_DIGEST_SHA_2_256);
    begin_params.push_back(TAG_PADDING, KM_PAD_RSA_PKCS1_1_5_SIGN);
    EXPECT_EQ(KM_ERROR_OK, BeginOperation(KM_PURPOSE_VERIFY, begin_params));

    string result;
    size_t input_consumed;
    EXPECT_EQ(KM_ERROR_OK, UpdateOperation(message, &result, &input_consumed));
    EXPECT_EQ(message.size(), input_consumed);
    EXPECT_EQ(KM_ERROR_VERIFICATION_FAILED, FinishOperation(signature, &result));

    if (GetParam()->algorithm_in_hardware(KM_ALGORITHM_RSA))
        EXPECT_EQ(4, GetParam()->keymaster0_calls());
}

TEST_P(VerificationOperationsTest, RsaPkcs1Sha256CorruptInput) {
    ASSERT_EQ(KM_ERROR_OK, GenerateKey(AuthorizationSetBuilder()
                                           .RsaSigningKey(512, 3)
                                           .Digest(KM_DIGEST_SHA_2_256)
                                           .Padding(KM_PAD_RSA_PKCS1_1_5_SIGN)));
    // Use large message, which won't work without digesting.
    string message(1024, 'a');
    string signature;
    SignMessage(message, &signature, KM_DIGEST_SHA_2_256, KM_PAD_RSA_PKCS1_1_5_SIGN);
    ++message[message.size() / 2];

    AuthorizationSet begin_params(client_params());
    begin_params.push_back(TAG_DIGEST, KM_DIGEST_SHA_2_256);
    begin_params.push_back(TAG_PADDING, KM_PAD_RSA_PKCS1_1_5_SIGN);
    EXPECT_EQ(KM_ERROR_OK, BeginOperation(KM_PURPOSE_VERIFY, begin_params));

    string result;
    size_t input_consumed;
    EXPECT_EQ(KM_ERROR_OK, UpdateOperation(message, &result, &input_consumed));
    EXPECT_EQ(message.size(), input_consumed);
    EXPECT_EQ(KM_ERROR_VERIFICATION_FAILED, FinishOperation(signature, &result));

    if (GetParam()->algorithm_in_hardware(KM_ALGORITHM_RSA))
        EXPECT_EQ(4, GetParam()->keymaster0_calls());
}

template <typename T> vector<T> make_vector(const T* array, size_t len) {
    return vector<T>(array, array + len);
}

TEST_P(VerificationOperationsTest, RsaAllDigestAndPadCombinations) {
    // Get all supported digests and padding modes.
    size_t digests_len;
    keymaster_digest_t* digests;
    ASSERT_EQ(KM_ERROR_OK,
              device()->get_supported_digests(device(), KM_ALGORITHM_RSA, KM_PURPOSE_SIGN, &digests,
                                              &digests_len));

    size_t padding_modes_len;
    keymaster_padding_t* padding_modes;
    ASSERT_EQ(KM_ERROR_OK,
              device()->get_supported_padding_modes(device(), KM_ALGORITHM_RSA, KM_PURPOSE_SIGN,
                                                    &padding_modes, &padding_modes_len));

    // Try them.
    int trial_count = 0;
    for (keymaster_padding_t padding_mode : make_vector(padding_modes, padding_modes_len)) {
        for (keymaster_digest_t digest : make_vector(digests, digests_len)) {
            if (digest != KM_DIGEST_NONE && padding_mode == KM_PAD_NONE)
                // Digesting requires padding
                continue;

            // Compute key & message size that will work.
            size_t key_bits = 0;
            size_t message_len = 1000;

            if (digest == KM_DIGEST_NONE) {
                key_bits = 256;
                switch (padding_mode) {
                case KM_PAD_NONE:
                    // Match key size.
                    message_len = key_bits / 8;
                    break;
                case KM_PAD_RSA_PKCS1_1_5_SIGN:
                    message_len = key_bits / 8 - 11;
                    break;
                case KM_PAD_RSA_PSS:
                    // PSS requires a digest.
                    continue;
                default:
                    FAIL() << "Missing padding";
                    break;
                }
            } else {
                size_t digest_bits;
                switch (digest) {
                case KM_DIGEST_MD5:
                    digest_bits = 128;
                    break;
                case KM_DIGEST_SHA1:
                    digest_bits = 160;
                    break;
                case KM_DIGEST_SHA_2_224:
                    digest_bits = 224;
                    break;
                case KM_DIGEST_SHA_2_256:
                    digest_bits = 256;
                    break;
                case KM_DIGEST_SHA_2_384:
                    digest_bits = 384;
                    break;
                case KM_DIGEST_SHA_2_512:
                    digest_bits = 512;
                    break;
                default:
                    FAIL() << "Missing digest";
                }

                switch (padding_mode) {
                case KM_PAD_RSA_PKCS1_1_5_SIGN:
                    key_bits = digest_bits + 8 * (11 + 19);
                    break;
                case KM_PAD_RSA_PSS:
                    key_bits = digest_bits + 8 * 10;
                    break;
                default:
                    FAIL() << "Missing padding";
                    break;
                }
            }

            GenerateKey(AuthorizationSetBuilder()
                            .RsaSigningKey(key_bits, 3)
                            .Digest(digest)
                            .Padding(padding_mode));
            string message(message_len, 'a');
            string signature;
            SignMessage(message, &signature, digest, padding_mode);
            VerifyMessage(message, signature, digest, padding_mode);
            ++trial_count;
        }
    }

    free(padding_modes);
    free(digests);

    if (GetParam()->algorithm_in_hardware(KM_ALGORITHM_RSA))
        EXPECT_EQ(trial_count * 4, GetParam()->keymaster0_calls());
}

TEST_P(VerificationOperationsTest, EcdsaSuccess) {
    ASSERT_EQ(KM_ERROR_OK,
              GenerateKey(AuthorizationSetBuilder().EcdsaSigningKey(256).Digest(KM_DIGEST_NONE)));
    string message = "123456789012345678901234567890123456789012345678";
    string signature;
    SignMessage(message, &signature, KM_DIGEST_NONE);
    VerifyMessage(message, signature, KM_DIGEST_NONE);

    if (GetParam()->algorithm_in_hardware(KM_ALGORITHM_EC))
        EXPECT_EQ(4, GetParam()->keymaster0_calls());
}

TEST_P(VerificationOperationsTest, EcdsaSha256Success) {
    ASSERT_EQ(KM_ERROR_OK, GenerateKey(AuthorizationSetBuilder()
                                           .EcdsaSigningKey(256)
                                           .Digest(KM_DIGEST_SHA_2_256)
                                           .Digest(KM_DIGEST_NONE)));
    string message = "123456789012345678901234567890123456789012345678";
    string signature;
    SignMessage(message, &signature, KM_DIGEST_SHA_2_256);
    VerifyMessage(message, signature, KM_DIGEST_SHA_2_256);

    if (GetParam()->algorithm_in_hardware(KM_ALGORITHM_EC))
        EXPECT_EQ(4, GetParam()->keymaster0_calls());

    // Just for giggles, try verifying with the wrong digest.
    AuthorizationSet begin_params(client_params());
    begin_params.push_back(TAG_DIGEST, KM_DIGEST_NONE);
    EXPECT_EQ(KM_ERROR_OK, BeginOperation(KM_PURPOSE_VERIFY, begin_params));

    string result;
    size_t input_consumed;
    EXPECT_EQ(KM_ERROR_OK, UpdateOperation(message, &result, &input_consumed));
    EXPECT_EQ(message.size(), input_consumed);
    EXPECT_EQ(KM_ERROR_VERIFICATION_FAILED, FinishOperation(signature, &result));
}

TEST_P(VerificationOperationsTest, HmacSha1Success) {
    GenerateKey(AuthorizationSetBuilder().HmacKey(128).Digest(KM_DIGEST_SHA1));
    string message = "123456789012345678901234567890123456789012345678";
    string signature;
    MacMessage(message, &signature, KM_DIGEST_SHA1, 160);
    VerifyMessage(message, signature, KM_DIGEST_SHA1);

    EXPECT_EQ(0, GetParam()->keymaster0_calls());
}

TEST_P(VerificationOperationsTest, HmacSha224Success) {
    GenerateKey(AuthorizationSetBuilder().HmacKey(128).Digest(KM_DIGEST_SHA_2_224));
    string message = "123456789012345678901234567890123456789012345678";
    string signature;
    MacMessage(message, &signature, KM_DIGEST_SHA_2_224, 224);
    VerifyMessage(message, signature, KM_DIGEST_SHA_2_224);

    EXPECT_EQ(0, GetParam()->keymaster0_calls());
}

TEST_P(VerificationOperationsTest, HmacSha256Success) {
    GenerateKey(AuthorizationSetBuilder().HmacKey(128).Digest(KM_DIGEST_SHA_2_256));
    string message = "123456789012345678901234567890123456789012345678";
    string signature;
    MacMessage(message, &signature, KM_DIGEST_SHA_2_256, 256);
    VerifyMessage(message, signature, KM_DIGEST_SHA_2_256);

    EXPECT_EQ(0, GetParam()->keymaster0_calls());
}

TEST_P(VerificationOperationsTest, HmacSha384Success) {
    GenerateKey(AuthorizationSetBuilder().HmacKey(128).Digest(KM_DIGEST_SHA_2_384));
    string message = "123456789012345678901234567890123456789012345678";
    string signature;
    MacMessage(message, &signature, KM_DIGEST_SHA_2_384, 384);
    VerifyMessage(message, signature, KM_DIGEST_SHA_2_384);

    EXPECT_EQ(0, GetParam()->keymaster0_calls());
}

TEST_P(VerificationOperationsTest, HmacSha512Success) {
    GenerateKey(AuthorizationSetBuilder().HmacKey(128).Digest(KM_DIGEST_SHA_2_512));
    string message = "123456789012345678901234567890123456789012345678";
    string signature;
    MacMessage(message, &signature, KM_DIGEST_SHA_2_512, 512);
    VerifyMessage(message, signature, KM_DIGEST_SHA_2_512);

    EXPECT_EQ(0, GetParam()->keymaster0_calls());
}

typedef Keymaster1Test ExportKeyTest;
INSTANTIATE_TEST_CASE_P(AndroidKeymasterTest, ExportKeyTest, test_params);

TEST_P(ExportKeyTest, RsaSuccess) {
    ASSERT_EQ(KM_ERROR_OK, GenerateKey(AuthorizationSetBuilder()
                                           .RsaSigningKey(256, 3)
                                           .Digest(KM_DIGEST_NONE)
                                           .Padding(KM_PAD_NONE)));
    string export_data;
    ASSERT_EQ(KM_ERROR_OK, ExportKey(KM_KEY_FORMAT_X509, &export_data));
    EXPECT_GT(export_data.length(), 0U);

    // TODO(swillden): Verify that the exported key is actually usable to verify signatures.

    if (GetParam()->algorithm_in_hardware(KM_ALGORITHM_RSA))
        EXPECT_EQ(2, GetParam()->keymaster0_calls());
}

TEST_P(ExportKeyTest, EcdsaSuccess) {
    ASSERT_EQ(KM_ERROR_OK,
              GenerateKey(AuthorizationSetBuilder().EcdsaSigningKey(224).Digest(KM_DIGEST_NONE)));
    string export_data;
    ASSERT_EQ(KM_ERROR_OK, ExportKey(KM_KEY_FORMAT_X509, &export_data));
    EXPECT_GT(export_data.length(), 0U);

    // TODO(swillden): Verify that the exported key is actually usable to verify signatures.

    if (GetParam()->algorithm_in_hardware(KM_ALGORITHM_EC))
        EXPECT_EQ(2, GetParam()->keymaster0_calls());
}

TEST_P(ExportKeyTest, RsaUnsupportedKeyFormat) {
    ASSERT_EQ(KM_ERROR_OK, GenerateKey(AuthorizationSetBuilder()
                                           .RsaSigningKey(256, 3)
                                           .Digest(KM_DIGEST_NONE)
                                           .Padding(KM_PAD_NONE)));
    string export_data;
    ASSERT_EQ(KM_ERROR_UNSUPPORTED_KEY_FORMAT, ExportKey(KM_KEY_FORMAT_PKCS8, &export_data));

    if (GetParam()->algorithm_in_hardware(KM_ALGORITHM_RSA))
        EXPECT_EQ(2, GetParam()->keymaster0_calls());
}

TEST_P(ExportKeyTest, RsaCorruptedKeyBlob) {
    ASSERT_EQ(KM_ERROR_OK, GenerateKey(AuthorizationSetBuilder()
                                           .RsaSigningKey(256, 3)
                                           .Digest(KM_DIGEST_NONE)
                                           .Padding(KM_PAD_NONE)));
    corrupt_key_blob();
    string export_data;
    ASSERT_EQ(KM_ERROR_INVALID_KEY_BLOB, ExportKey(KM_KEY_FORMAT_X509, &export_data));

    if (GetParam()->algorithm_in_hardware(KM_ALGORITHM_RSA))
        EXPECT_EQ(2, GetParam()->keymaster0_calls());
}

TEST_P(ExportKeyTest, AesKeyExportFails) {
    ASSERT_EQ(KM_ERROR_OK, GenerateKey(AuthorizationSetBuilder().AesEncryptionKey(128)));
    string export_data;

    EXPECT_EQ(KM_ERROR_UNSUPPORTED_KEY_FORMAT, ExportKey(KM_KEY_FORMAT_X509, &export_data));
    EXPECT_EQ(KM_ERROR_UNSUPPORTED_KEY_FORMAT, ExportKey(KM_KEY_FORMAT_PKCS8, &export_data));
    EXPECT_EQ(KM_ERROR_UNSUPPORTED_KEY_FORMAT, ExportKey(KM_KEY_FORMAT_RAW, &export_data));

    EXPECT_EQ(0, GetParam()->keymaster0_calls());
}

static string read_file(const string& file_name) {
    ifstream file_stream(file_name, std::ios::binary);
    istreambuf_iterator<char> file_begin(file_stream);
    istreambuf_iterator<char> file_end;
    return string(file_begin, file_end);
}

typedef Keymaster1Test ImportKeyTest;
INSTANTIATE_TEST_CASE_P(AndroidKeymasterTest, ImportKeyTest, test_params);

TEST_P(ImportKeyTest, RsaSuccess) {
    string pk8_key = read_file("rsa_privkey_pk8.der");
    ASSERT_EQ(633U, pk8_key.size());

    ASSERT_EQ(KM_ERROR_OK, ImportKey(AuthorizationSetBuilder()
                                         .RsaSigningKey(1024, 65537)
                                         .Digest(KM_DIGEST_NONE)
                                         .Padding(KM_PAD_NONE),
                                     KM_KEY_FORMAT_PKCS8, pk8_key));

    // Check values derived from the key.
    EXPECT_TRUE(contains(GetParam()->algorithm_in_hardware(KM_ALGORITHM_RSA) ? hw_enforced()
                                                                             : sw_enforced(),
                         TAG_ALGORITHM, KM_ALGORITHM_RSA));
    EXPECT_TRUE(contains(GetParam()->algorithm_in_hardware(KM_ALGORITHM_RSA) ? hw_enforced()
                                                                             : sw_enforced(),
                         TAG_KEY_SIZE, 1024));
    EXPECT_TRUE(contains(GetParam()->algorithm_in_hardware(KM_ALGORITHM_RSA) ? hw_enforced()
                                                                             : sw_enforced(),
                         TAG_RSA_PUBLIC_EXPONENT, 65537U));

    // And values provided by AndroidKeymaster
    EXPECT_TRUE(contains(sw_enforced(), TAG_ORIGIN, KM_ORIGIN_IMPORTED));
    EXPECT_TRUE(contains(sw_enforced(), KM_TAG_CREATION_DATETIME));

    string message(1024 / 8, 'a');
    string signature;
    SignMessage(message, &signature, KM_DIGEST_NONE, KM_PAD_NONE);
    VerifyMessage(message, signature, KM_DIGEST_NONE, KM_PAD_NONE);

    if (GetParam()->algorithm_in_hardware(KM_ALGORITHM_RSA))
        EXPECT_EQ(4, GetParam()->keymaster0_calls());
}

TEST_P(ImportKeyTest, OldApiRsaSuccess) {
    string pk8_key = read_file("rsa_privkey_pk8.der");
    ASSERT_EQ(633U, pk8_key.size());

    // NOTE: This will break when the keymaster0 APIs are removed from keymaster1.  But at that
    // point softkeymaster will no longer support keymaster0 APIs anyway.
    uint8_t* key_blob;
    size_t key_blob_length;
    ASSERT_EQ(0,
              device()->import_keypair(device(), reinterpret_cast<const uint8_t*>(pk8_key.data()),
                                       pk8_key.size(), &key_blob, &key_blob_length));
    set_key_blob(key_blob, key_blob_length);

    string message(1024 / 8, 'a');
    AuthorizationSet begin_params;  // Don't use client data.
    begin_params.push_back(TAG_DIGEST, KM_DIGEST_NONE);
    begin_params.push_back(TAG_PADDING, KM_PAD_NONE);
    AuthorizationSet update_params;
    AuthorizationSet output_params;
    string signature =
        ProcessMessage(KM_PURPOSE_SIGN, message, begin_params, update_params, &output_params);
    ProcessMessage(KM_PURPOSE_VERIFY, message, signature, begin_params, update_params,
                   &output_params);

    if (GetParam()->algorithm_in_hardware(KM_ALGORITHM_RSA))
        EXPECT_EQ(4, GetParam()->keymaster0_calls());
}

TEST_P(ImportKeyTest, RsaKeySizeMismatch) {
    string pk8_key = read_file("rsa_privkey_pk8.der");
    ASSERT_EQ(633U, pk8_key.size());
    ASSERT_EQ(KM_ERROR_IMPORT_PARAMETER_MISMATCH,
              ImportKey(AuthorizationSetBuilder()
                            .RsaSigningKey(2048 /* Doesn't match key */, 3)
                            .Digest(KM_DIGEST_NONE)
                            .Padding(KM_PAD_NONE),
                        KM_KEY_FORMAT_PKCS8, pk8_key));

    EXPECT_EQ(0, GetParam()->keymaster0_calls());
}

TEST_P(ImportKeyTest, RsaPublicExponenMismatch) {
    string pk8_key = read_file("rsa_privkey_pk8.der");
    ASSERT_EQ(633U, pk8_key.size());
    ASSERT_EQ(KM_ERROR_IMPORT_PARAMETER_MISMATCH,
              ImportKey(AuthorizationSetBuilder()
                            .RsaSigningKey(256, 3 /* Doesnt' match key */)
                            .Digest(KM_DIGEST_NONE)
                            .Padding(KM_PAD_NONE),
                        KM_KEY_FORMAT_PKCS8, pk8_key));

    EXPECT_EQ(0, GetParam()->keymaster0_calls());
}

TEST_P(ImportKeyTest, EcdsaSuccess) {
    string pk8_key = read_file("ec_privkey_pk8.der");
    ASSERT_EQ(138U, pk8_key.size());

    ASSERT_EQ(KM_ERROR_OK,
              ImportKey(AuthorizationSetBuilder().EcdsaSigningKey(256).Digest(KM_DIGEST_NONE),
                        KM_KEY_FORMAT_PKCS8, pk8_key));

    // Check values derived from the key.
    EXPECT_TRUE(
        contains(GetParam()->algorithm_in_hardware(KM_ALGORITHM_EC) ? hw_enforced() : sw_enforced(),
                 TAG_ALGORITHM, KM_ALGORITHM_EC));
    EXPECT_TRUE(
        contains(GetParam()->algorithm_in_hardware(KM_ALGORITHM_EC) ? hw_enforced() : sw_enforced(),
                 TAG_KEY_SIZE, 256));

    // And values provided by AndroidKeymaster
    EXPECT_TRUE(contains(sw_enforced(), TAG_ORIGIN, KM_ORIGIN_IMPORTED));
    EXPECT_TRUE(contains(sw_enforced(), KM_TAG_CREATION_DATETIME));

    string message(1024 / 8, 'a');
    string signature;
    SignMessage(message, &signature, KM_DIGEST_NONE);
    VerifyMessage(message, signature, KM_DIGEST_NONE);

    if (GetParam()->algorithm_in_hardware(KM_ALGORITHM_EC))
        EXPECT_EQ(4, GetParam()->keymaster0_calls());
}

TEST_P(ImportKeyTest, EcdsaSizeSpecified) {
    string pk8_key = read_file("ec_privkey_pk8.der");
    ASSERT_EQ(138U, pk8_key.size());

    ASSERT_EQ(KM_ERROR_OK,
              ImportKey(AuthorizationSetBuilder().EcdsaSigningKey(256).Digest(KM_DIGEST_NONE),
                        KM_KEY_FORMAT_PKCS8, pk8_key));

    // Check values derived from the key.
    EXPECT_TRUE(
        contains(GetParam()->algorithm_in_hardware(KM_ALGORITHM_EC) ? hw_enforced() : sw_enforced(),
                 TAG_ALGORITHM, KM_ALGORITHM_EC));
    EXPECT_TRUE(
        contains(GetParam()->algorithm_in_hardware(KM_ALGORITHM_EC) ? hw_enforced() : sw_enforced(),
                 TAG_KEY_SIZE, 256));

    // And values provided by AndroidKeymaster
    EXPECT_TRUE(contains(sw_enforced(), TAG_ORIGIN, KM_ORIGIN_IMPORTED));
    EXPECT_TRUE(contains(sw_enforced(), KM_TAG_CREATION_DATETIME));

    string message(1024 / 8, 'a');
    string signature;
    SignMessage(message, &signature, KM_DIGEST_NONE);
    VerifyMessage(message, signature, KM_DIGEST_NONE);

    if (GetParam()->algorithm_in_hardware(KM_ALGORITHM_EC))
        EXPECT_EQ(4, GetParam()->keymaster0_calls());
}

TEST_P(ImportKeyTest, EcdsaSizeMismatch) {
    string pk8_key = read_file("ec_privkey_pk8.der");
    ASSERT_EQ(138U, pk8_key.size());
    ASSERT_EQ(KM_ERROR_IMPORT_PARAMETER_MISMATCH,
              ImportKey(AuthorizationSetBuilder()
                            .EcdsaSigningKey(224 /* Doesn't match key */)
                            .Digest(KM_DIGEST_NONE),
                        KM_KEY_FORMAT_PKCS8, pk8_key));

    EXPECT_EQ(0, GetParam()->keymaster0_calls());
}

TEST_P(ImportKeyTest, AesKeySuccess) {
    char key_data[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    string key(key_data, sizeof(key_data));
    ASSERT_EQ(KM_ERROR_OK,
              ImportKey(AuthorizationSetBuilder().AesEncryptionKey(128).EcbMode().Authorization(
                            TAG_PADDING, KM_PAD_PKCS7),
                        KM_KEY_FORMAT_RAW, key));

    EXPECT_TRUE(contains(sw_enforced(), TAG_ORIGIN, KM_ORIGIN_IMPORTED));
    EXPECT_TRUE(contains(sw_enforced(), KM_TAG_CREATION_DATETIME));

    string message = "Hello World!";
    string ciphertext = EncryptMessage(message, KM_MODE_ECB, KM_PAD_PKCS7);
    string plaintext = DecryptMessage(ciphertext, KM_MODE_ECB, KM_PAD_PKCS7);
    EXPECT_EQ(message, plaintext);

    EXPECT_EQ(0, GetParam()->keymaster0_calls());
}

TEST_P(ImportKeyTest, HmacSha256KeySuccess) {
    char key_data[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    string key(key_data, sizeof(key_data));
    ASSERT_EQ(KM_ERROR_OK, ImportKey(AuthorizationSetBuilder()
                                         .HmacKey(sizeof(key_data) * 8)
                                         .Digest(KM_DIGEST_SHA_2_256)
                                         .Authorization(TAG_MAC_LENGTH, 32),
                                     KM_KEY_FORMAT_RAW, key));

    EXPECT_TRUE(contains(sw_enforced(), TAG_ORIGIN, KM_ORIGIN_IMPORTED));
    EXPECT_TRUE(contains(sw_enforced(), KM_TAG_CREATION_DATETIME));

    string message = "Hello World!";
    string signature;
    MacMessage(message, &signature, KM_DIGEST_SHA_2_256, 32);
    VerifyMessage(message, signature, KM_DIGEST_SHA_2_256);

    EXPECT_EQ(0, GetParam()->keymaster0_calls());
}

typedef Keymaster1Test EncryptionOperationsTest;
INSTANTIATE_TEST_CASE_P(AndroidKeymasterTest, EncryptionOperationsTest, test_params);

TEST_P(EncryptionOperationsTest, RsaNoPaddingSuccess) {
    ASSERT_EQ(KM_ERROR_OK,
              GenerateKey(AuthorizationSetBuilder().RsaEncryptionKey(256, 3).Padding(KM_PAD_NONE)));

    string message = "12345678901234567890123456789012";
    string ciphertext1 = EncryptMessage(string(message), KM_PAD_NONE);
    EXPECT_EQ(256U / 8, ciphertext1.size());

    string ciphertext2 = EncryptMessage(string(message), KM_PAD_NONE);
    EXPECT_EQ(256U / 8, ciphertext2.size());

    // Unpadded RSA is deterministic
    EXPECT_EQ(ciphertext1, ciphertext2);

    if (GetParam()->algorithm_in_hardware(KM_ALGORITHM_RSA))
        EXPECT_EQ(3, GetParam()->keymaster0_calls());
}

TEST_P(EncryptionOperationsTest, RsaNoPaddingTooShort) {
    ASSERT_EQ(KM_ERROR_OK,
              GenerateKey(AuthorizationSetBuilder().RsaEncryptionKey(256, 3).Padding(KM_PAD_NONE)));

    string message = "1234567890123456789012345678901";

    AuthorizationSet begin_params(client_params());
    begin_params.push_back(TAG_PADDING, KM_PAD_NONE);
    EXPECT_EQ(KM_ERROR_OK, BeginOperation(KM_PURPOSE_ENCRYPT, begin_params));

    string result;
    size_t input_consumed;
    EXPECT_EQ(KM_ERROR_OK, UpdateOperation(message, &result, &input_consumed));
    EXPECT_EQ(KM_ERROR_INVALID_INPUT_LENGTH, FinishOperation(&result));
    EXPECT_EQ(0U, result.size());

    if (GetParam()->algorithm_in_hardware(KM_ALGORITHM_RSA))
        EXPECT_EQ(2, GetParam()->keymaster0_calls());
}

TEST_P(EncryptionOperationsTest, RsaNoPaddingTooLong) {
    ASSERT_EQ(KM_ERROR_OK,
              GenerateKey(AuthorizationSetBuilder().RsaEncryptionKey(256, 3).Padding(KM_PAD_NONE)));

    string message = "123456789012345678901234567890123";

    AuthorizationSet begin_params(client_params());
    begin_params.push_back(TAG_PADDING, KM_PAD_NONE);
    EXPECT_EQ(KM_ERROR_OK, BeginOperation(KM_PURPOSE_ENCRYPT, begin_params));

    string result;
    size_t input_consumed;
    EXPECT_EQ(KM_ERROR_OK, UpdateOperation(message, &result, &input_consumed));
    EXPECT_EQ(KM_ERROR_INVALID_INPUT_LENGTH, FinishOperation(&result));
    EXPECT_EQ(0U, result.size());

    if (GetParam()->algorithm_in_hardware(KM_ALGORITHM_RSA))
        EXPECT_EQ(2, GetParam()->keymaster0_calls());
}

TEST_P(EncryptionOperationsTest, RsaOaepSuccess) {
    ASSERT_EQ(KM_ERROR_OK, GenerateKey(AuthorizationSetBuilder().RsaEncryptionKey(512, 3).Padding(
                               KM_PAD_RSA_OAEP)));

    string message = "Hello World!";
    string ciphertext1 = EncryptMessage(string(message), KM_PAD_RSA_OAEP);
    EXPECT_EQ(512U / 8, ciphertext1.size());

    string ciphertext2 = EncryptMessage(string(message), KM_PAD_RSA_OAEP);
    EXPECT_EQ(512U / 8, ciphertext2.size());

    // OAEP randomizes padding so every result should be different.
    EXPECT_NE(ciphertext1, ciphertext2);

    if (GetParam()->algorithm_in_hardware(KM_ALGORITHM_RSA))
        EXPECT_EQ(3, GetParam()->keymaster0_calls());
}

TEST_P(EncryptionOperationsTest, RsaOaepRoundTrip) {
    ASSERT_EQ(KM_ERROR_OK, GenerateKey(AuthorizationSetBuilder().RsaEncryptionKey(512, 3).Padding(
                               KM_PAD_RSA_OAEP)));
    string message = "Hello World!";
    string ciphertext = EncryptMessage(string(message), KM_PAD_RSA_OAEP);
    EXPECT_EQ(512U / 8, ciphertext.size());

    string plaintext = DecryptMessage(ciphertext, KM_PAD_RSA_OAEP);
    EXPECT_EQ(message, plaintext);

    if (GetParam()->algorithm_in_hardware(KM_ALGORITHM_RSA))
        EXPECT_EQ(4, GetParam()->keymaster0_calls());
}

TEST_P(EncryptionOperationsTest, RsaOaepTooLarge) {
    ASSERT_EQ(KM_ERROR_OK, GenerateKey(AuthorizationSetBuilder().RsaEncryptionKey(512, 3).Padding(
                               KM_PAD_RSA_OAEP)));
    string message = "12345678901234567890123";
    string result;
    size_t input_consumed;

    AuthorizationSet begin_params(client_params());
    begin_params.push_back(TAG_PADDING, KM_PAD_RSA_OAEP);
    EXPECT_EQ(KM_ERROR_OK, BeginOperation(KM_PURPOSE_ENCRYPT, begin_params));
    EXPECT_EQ(KM_ERROR_OK, UpdateOperation(message, &result, &input_consumed));
    EXPECT_EQ(KM_ERROR_INVALID_INPUT_LENGTH, FinishOperation(&result));
    EXPECT_EQ(0U, result.size());

    if (GetParam()->algorithm_in_hardware(KM_ALGORITHM_RSA))
        EXPECT_EQ(2, GetParam()->keymaster0_calls());
}

TEST_P(EncryptionOperationsTest, RsaOaepCorruptedDecrypt) {
    ASSERT_EQ(KM_ERROR_OK, GenerateKey(AuthorizationSetBuilder().RsaEncryptionKey(512, 3).Padding(
                               KM_PAD_RSA_OAEP)));
    string message = "Hello World!";
    string ciphertext = EncryptMessage(string(message), KM_PAD_RSA_OAEP);
    EXPECT_EQ(512U / 8, ciphertext.size());

    // Corrupt the ciphertext
    ciphertext[512 / 8 / 2]++;

    string result;
    size_t input_consumed;
    AuthorizationSet begin_params(client_params());
    begin_params.push_back(TAG_PADDING, KM_PAD_RSA_OAEP);
    EXPECT_EQ(KM_ERROR_OK, BeginOperation(KM_PURPOSE_DECRYPT, begin_params));
    EXPECT_EQ(KM_ERROR_OK, UpdateOperation(ciphertext, &result, &input_consumed));
    EXPECT_EQ(KM_ERROR_UNKNOWN_ERROR, FinishOperation(&result));
    EXPECT_EQ(0U, result.size());

    if (GetParam()->algorithm_in_hardware(KM_ALGORITHM_RSA))
        EXPECT_EQ(4, GetParam()->keymaster0_calls());
}

TEST_P(EncryptionOperationsTest, RsaPkcs1Success) {
    ASSERT_EQ(KM_ERROR_OK, GenerateKey(AuthorizationSetBuilder().RsaEncryptionKey(512, 3).Padding(
                               KM_PAD_RSA_PKCS1_1_5_ENCRYPT)));
    string message = "Hello World!";
    string ciphertext1 = EncryptMessage(string(message), KM_PAD_RSA_PKCS1_1_5_ENCRYPT);
    EXPECT_EQ(512U / 8, ciphertext1.size());

    string ciphertext2 = EncryptMessage(string(message), KM_PAD_RSA_PKCS1_1_5_ENCRYPT);
    EXPECT_EQ(512U / 8, ciphertext2.size());

    // PKCS1 v1.5 randomizes padding so every result should be different.
    EXPECT_NE(ciphertext1, ciphertext2);

    if (GetParam()->algorithm_in_hardware(KM_ALGORITHM_RSA))
        EXPECT_EQ(3, GetParam()->keymaster0_calls());
}

TEST_P(EncryptionOperationsTest, RsaPkcs1RoundTrip) {
    ASSERT_EQ(KM_ERROR_OK, GenerateKey(AuthorizationSetBuilder().RsaEncryptionKey(512, 3).Padding(
                               KM_PAD_RSA_PKCS1_1_5_ENCRYPT)));
    string message = "Hello World!";
    string ciphertext = EncryptMessage(string(message), KM_PAD_RSA_PKCS1_1_5_ENCRYPT);
    EXPECT_EQ(512U / 8, ciphertext.size());

    string plaintext = DecryptMessage(ciphertext, KM_PAD_RSA_PKCS1_1_5_ENCRYPT);
    EXPECT_EQ(message, plaintext);

    if (GetParam()->algorithm_in_hardware(KM_ALGORITHM_RSA))
        EXPECT_EQ(4, GetParam()->keymaster0_calls());
}

TEST_P(EncryptionOperationsTest, RsaPkcs1TooLarge) {
    ASSERT_EQ(KM_ERROR_OK, GenerateKey(AuthorizationSetBuilder().RsaEncryptionKey(512, 3).Padding(
                               KM_PAD_RSA_PKCS1_1_5_ENCRYPT)));
    string message = "123456789012345678901234567890123456789012345678901234";
    string result;
    size_t input_consumed;

    AuthorizationSet begin_params(client_params());
    begin_params.push_back(TAG_PADDING, KM_PAD_RSA_PKCS1_1_5_ENCRYPT);
    EXPECT_EQ(KM_ERROR_OK, BeginOperation(KM_PURPOSE_ENCRYPT, begin_params));
    EXPECT_EQ(KM_ERROR_OK, UpdateOperation(message, &result, &input_consumed));
    EXPECT_EQ(KM_ERROR_INVALID_INPUT_LENGTH, FinishOperation(&result));
    EXPECT_EQ(0U, result.size());

    if (GetParam()->algorithm_in_hardware(KM_ALGORITHM_RSA))
        EXPECT_EQ(2, GetParam()->keymaster0_calls());
}

TEST_P(EncryptionOperationsTest, RsaPkcs1CorruptedDecrypt) {
    ASSERT_EQ(KM_ERROR_OK, GenerateKey(AuthorizationSetBuilder().RsaEncryptionKey(512, 3).Padding(
                               KM_PAD_RSA_PKCS1_1_5_ENCRYPT)));
    string message = "Hello World!";
    string ciphertext = EncryptMessage(string(message), KM_PAD_RSA_PKCS1_1_5_ENCRYPT);
    EXPECT_EQ(512U / 8, ciphertext.size());

    // Corrupt the ciphertext
    ciphertext[512 / 8 / 2]++;

    string result;
    size_t input_consumed;
    AuthorizationSet begin_params(client_params());
    begin_params.push_back(TAG_PADDING, KM_PAD_RSA_PKCS1_1_5_ENCRYPT);
    EXPECT_EQ(KM_ERROR_OK, BeginOperation(KM_PURPOSE_DECRYPT, begin_params));
    EXPECT_EQ(KM_ERROR_OK, UpdateOperation(ciphertext, &result, &input_consumed));
    EXPECT_EQ(KM_ERROR_UNKNOWN_ERROR, FinishOperation(&result));
    EXPECT_EQ(0U, result.size());

    if (GetParam()->algorithm_in_hardware(KM_ALGORITHM_RSA))
        EXPECT_EQ(4, GetParam()->keymaster0_calls());
}

TEST_P(EncryptionOperationsTest, RsaEncryptWithSigningKey) {
    ASSERT_EQ(KM_ERROR_OK, GenerateKey(AuthorizationSetBuilder()
                                           .RsaSigningKey(256, 3)
                                           .Digest(KM_DIGEST_NONE)
                                           .Padding(KM_PAD_NONE)));
    ASSERT_EQ(KM_ERROR_INCOMPATIBLE_PURPOSE, BeginOperation(KM_PURPOSE_ENCRYPT));
    ASSERT_EQ(KM_ERROR_INCOMPATIBLE_PURPOSE, BeginOperation(KM_PURPOSE_DECRYPT));

    if (GetParam()->algorithm_in_hardware(KM_ALGORITHM_RSA))
        EXPECT_EQ(3, GetParam()->keymaster0_calls());
}

TEST_P(EncryptionOperationsTest, EcdsaEncrypt) {
    ASSERT_EQ(KM_ERROR_OK,
              GenerateKey(AuthorizationSetBuilder().EcdsaSigningKey(224).Digest(KM_DIGEST_NONE)));
    ASSERT_EQ(KM_ERROR_INCOMPATIBLE_PURPOSE, BeginOperation(KM_PURPOSE_ENCRYPT));
    ASSERT_EQ(KM_ERROR_INCOMPATIBLE_PURPOSE, BeginOperation(KM_PURPOSE_DECRYPT));

    if (GetParam()->algorithm_in_hardware(KM_ALGORITHM_EC))
        EXPECT_EQ(3, GetParam()->keymaster0_calls());
}

TEST_P(EncryptionOperationsTest, HmacEncrypt) {
    ASSERT_EQ(
        KM_ERROR_OK,
        GenerateKey(
            AuthorizationSetBuilder().HmacKey(128).Digest(KM_DIGEST_NONE).Padding(KM_PAD_NONE)));
    ASSERT_EQ(KM_ERROR_INCOMPATIBLE_PURPOSE, BeginOperation(KM_PURPOSE_ENCRYPT));
    ASSERT_EQ(KM_ERROR_INCOMPATIBLE_PURPOSE, BeginOperation(KM_PURPOSE_DECRYPT));

    EXPECT_EQ(0, GetParam()->keymaster0_calls());
}

TEST_P(EncryptionOperationsTest, AesEcbRoundTripSuccess) {
    ASSERT_EQ(KM_ERROR_OK, GenerateKey(AuthorizationSetBuilder()
                                           .AesEncryptionKey(128)
                                           .Authorization(TAG_BLOCK_MODE, KM_MODE_ECB)
                                           .Padding(KM_PAD_NONE)));
    // Two-block message.
    string message = "12345678901234567890123456789012";
    string ciphertext1 = EncryptMessage(message, KM_MODE_ECB, KM_PAD_NONE);
    EXPECT_EQ(message.size(), ciphertext1.size());

    string ciphertext2 = EncryptMessage(string(message), KM_MODE_ECB, KM_PAD_NONE);
    EXPECT_EQ(message.size(), ciphertext2.size());

    // ECB is deterministic.
    EXPECT_EQ(ciphertext1, ciphertext2);

    string plaintext = DecryptMessage(ciphertext1, KM_MODE_ECB, KM_PAD_NONE);
    EXPECT_EQ(message, plaintext);

    EXPECT_EQ(0, GetParam()->keymaster0_calls());
}

TEST_P(EncryptionOperationsTest, AesEcbNoPaddingWrongInputSize) {
    ASSERT_EQ(KM_ERROR_OK, GenerateKey(AuthorizationSetBuilder()
                                           .AesEncryptionKey(128)
                                           .Authorization(TAG_BLOCK_MODE, KM_MODE_ECB)
                                           .Padding(KM_PAD_NONE)));
    // Message is slightly shorter than two blocks.
    string message = "1234567890123456789012345678901";

    AuthorizationSet begin_params(client_params());
    begin_params.push_back(TAG_BLOCK_MODE, KM_MODE_ECB);
    begin_params.push_back(TAG_PADDING, KM_PAD_NONE);
    EXPECT_EQ(KM_ERROR_OK, BeginOperation(KM_PURPOSE_ENCRYPT, begin_params));
    string ciphertext;
    size_t input_consumed;
    EXPECT_EQ(KM_ERROR_OK, UpdateOperation(message, &ciphertext, &input_consumed));
    EXPECT_EQ(message.size(), input_consumed);
    EXPECT_EQ(KM_ERROR_INVALID_INPUT_LENGTH, FinishOperation(&ciphertext));

    EXPECT_EQ(0, GetParam()->keymaster0_calls());
}

TEST_P(EncryptionOperationsTest, AesEcbPkcs7Padding) {
    ASSERT_EQ(KM_ERROR_OK, GenerateKey(AuthorizationSetBuilder()
                                           .AesEncryptionKey(128)
                                           .Authorization(TAG_BLOCK_MODE, KM_MODE_ECB)
                                           .Authorization(TAG_PADDING, KM_PAD_PKCS7)));

    // Try various message lengths; all should work.
    for (size_t i = 0; i < 32; ++i) {
        string message(i, 'a');
        string ciphertext = EncryptMessage(message, KM_MODE_ECB, KM_PAD_PKCS7);
        EXPECT_EQ(i + 16 - (i % 16), ciphertext.size());
        string plaintext = DecryptMessage(ciphertext, KM_MODE_ECB, KM_PAD_PKCS7);
        EXPECT_EQ(message, plaintext);
    }

    EXPECT_EQ(0, GetParam()->keymaster0_calls());
}

TEST_P(EncryptionOperationsTest, AesEcbPkcs7PaddingCorrupted) {
    ASSERT_EQ(KM_ERROR_OK, GenerateKey(AuthorizationSetBuilder()
                                           .AesEncryptionKey(128)
                                           .Authorization(TAG_BLOCK_MODE, KM_MODE_ECB)
                                           .Authorization(TAG_PADDING, KM_PAD_PKCS7)));

    string message = "a";
    string ciphertext = EncryptMessage(message, KM_MODE_ECB, KM_PAD_PKCS7);
    EXPECT_EQ(16U, ciphertext.size());
    EXPECT_NE(ciphertext, message);
    ++ciphertext[ciphertext.size() / 2];

    AuthorizationSet begin_params(client_params());
    begin_params.push_back(TAG_BLOCK_MODE, KM_MODE_ECB);
    begin_params.push_back(TAG_PADDING, KM_PAD_PKCS7);
    EXPECT_EQ(KM_ERROR_OK, BeginOperation(KM_PURPOSE_DECRYPT, begin_params));
    string plaintext;
    size_t input_consumed;
    EXPECT_EQ(KM_ERROR_OK, UpdateOperation(ciphertext, &plaintext, &input_consumed));
    EXPECT_EQ(ciphertext.size(), input_consumed);
    EXPECT_EQ(KM_ERROR_INVALID_ARGUMENT, FinishOperation(&plaintext));

    EXPECT_EQ(0, GetParam()->keymaster0_calls());
}

TEST_P(EncryptionOperationsTest, AesCtrRoundTripSuccess) {
    ASSERT_EQ(KM_ERROR_OK, GenerateKey(AuthorizationSetBuilder()
                                           .AesEncryptionKey(128)
                                           .Authorization(TAG_BLOCK_MODE, KM_MODE_CTR)
                                           .Padding(KM_PAD_NONE)));
    string message = "123";
    string iv1;
    string ciphertext1 = EncryptMessage(message, KM_MODE_CTR, KM_PAD_NONE, &iv1);
    EXPECT_EQ(message.size(), ciphertext1.size());
    EXPECT_EQ(16U, iv1.size());

    string iv2;
    string ciphertext2 = EncryptMessage(message, KM_MODE_CTR, KM_PAD_NONE, &iv2);
    EXPECT_EQ(message.size(), ciphertext2.size());
    EXPECT_EQ(16U, iv2.size());

    // IVs should be random, so ciphertexts should differ.
    EXPECT_NE(iv1, iv2);
    EXPECT_NE(ciphertext1, ciphertext2);

    string plaintext = DecryptMessage(ciphertext1, KM_MODE_CTR, KM_PAD_NONE, iv1);
    EXPECT_EQ(message, plaintext);

    EXPECT_EQ(0, GetParam()->keymaster0_calls());
}

TEST_P(EncryptionOperationsTest, AesCtrIncremental) {
    ASSERT_EQ(KM_ERROR_OK, GenerateKey(AuthorizationSetBuilder()
                                           .AesEncryptionKey(128)
                                           .Authorization(TAG_BLOCK_MODE, KM_MODE_CTR)
                                           .Padding(KM_PAD_NONE)));

    int increment = 15;
    string message(239, 'a');
    AuthorizationSet input_params(client_params());
    input_params.push_back(TAG_BLOCK_MODE, KM_MODE_CTR);
    input_params.push_back(TAG_PADDING, KM_PAD_NONE);
    AuthorizationSet output_params;
    EXPECT_EQ(KM_ERROR_OK, BeginOperation(KM_PURPOSE_ENCRYPT, input_params, &output_params));

    string ciphertext;
    size_t input_consumed;
    for (size_t i = 0; i < message.size(); i += increment)
        EXPECT_EQ(KM_ERROR_OK,
                  UpdateOperation(message.substr(i, increment), &ciphertext, &input_consumed));
    EXPECT_EQ(KM_ERROR_OK, FinishOperation(&ciphertext));
    EXPECT_EQ(message.size(), ciphertext.size());

    // Move TAG_NONCE into input_params
    input_params.Reinitialize(output_params);
    input_params.push_back(client_params());
    input_params.push_back(TAG_BLOCK_MODE, KM_MODE_CTR);
    input_params.push_back(TAG_PADDING, KM_PAD_NONE);
    output_params.Clear();

    EXPECT_EQ(KM_ERROR_OK, BeginOperation(KM_PURPOSE_DECRYPT, input_params, &output_params));
    string plaintext;
    for (size_t i = 0; i < ciphertext.size(); i += increment)
        EXPECT_EQ(KM_ERROR_OK,
                  UpdateOperation(ciphertext.substr(i, increment), &plaintext, &input_consumed));
    EXPECT_EQ(KM_ERROR_OK, FinishOperation(&plaintext));
    EXPECT_EQ(ciphertext.size(), plaintext.size());
    EXPECT_EQ(message, plaintext);

    EXPECT_EQ(0, GetParam()->keymaster0_calls());
}

struct AesCtrSp80038aTestVector {
    const char* key;
    const char* nonce;
    const char* plaintext;
    const char* ciphertext;
};

// These test vectors are taken from
// http://csrc.nist.gov/publications/nistpubs/800-38a/sp800-38a.pdf, section F.5.
static const AesCtrSp80038aTestVector kAesCtrSp80038aTestVectors[] = {
    // AES-128
    {
        "2b7e151628aed2a6abf7158809cf4f3c", "f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff",
        "6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e51"
        "30c81c46a35ce411e5fbc1191a0a52eff69f2445df4f9b17ad2b417be66c3710",
        "874d6191b620e3261bef6864990db6ce9806f66b7970fdff8617187bb9fffdff"
        "5ae4df3edbd5d35e5b4f09020db03eab1e031dda2fbe03d1792170a0f3009cee",
    },
    // AES-192
    {
        "8e73b0f7da0e6452c810f32b809079e562f8ead2522c6b7b", "f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff",
        "6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e51"
        "30c81c46a35ce411e5fbc1191a0a52eff69f2445df4f9b17ad2b417be66c3710",
        "1abc932417521ca24f2b0459fe7e6e0b090339ec0aa6faefd5ccc2c6f4ce8e94"
        "1e36b26bd1ebc670d1bd1d665620abf74f78a7f6d29809585a97daec58c6b050",
    },
    // AES-256
    {
        "603deb1015ca71be2b73aef0857d77811f352c073b6108d72d9810a30914dff4",
        "f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff",
        "6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e51"
        "30c81c46a35ce411e5fbc1191a0a52eff69f2445df4f9b17ad2b417be66c3710",
        "601ec313775789a5b7a7f504bbf3d228f443e3ca4d62b59aca84e990cacaf5c5"
        "2b0930daa23de94ce87017ba2d84988ddfc9c58db67aada613c2dd08457941a6",
    },
};

TEST_P(EncryptionOperationsTest, AesCtrSp80038aTestVector) {
    for (size_t i = 0; i < 3; i++) {
        const AesCtrSp80038aTestVector& test(kAesCtrSp80038aTestVectors[i]);
        const string key = hex2str(test.key);
        const string nonce = hex2str(test.nonce);
        const string plaintext = hex2str(test.plaintext);
        const string ciphertext = hex2str(test.ciphertext);
        CheckAesCtrTestVector(key, nonce, plaintext, ciphertext);
    }

    EXPECT_EQ(0, GetParam()->keymaster0_calls());
}

TEST_P(EncryptionOperationsTest, AesCtrInvalidPaddingMode) {
    ASSERT_EQ(KM_ERROR_OK, GenerateKey(AuthorizationSetBuilder()
                                           .AesEncryptionKey(128)
                                           .Authorization(TAG_BLOCK_MODE, KM_MODE_CTR)
                                           .Authorization(TAG_PADDING, KM_PAD_PKCS7)));
    AuthorizationSet begin_params(client_params());
    begin_params.push_back(TAG_BLOCK_MODE, KM_MODE_CTR);
    begin_params.push_back(TAG_PADDING, KM_PAD_NONE);
    EXPECT_EQ(KM_ERROR_INCOMPATIBLE_PADDING_MODE, BeginOperation(KM_PURPOSE_ENCRYPT, begin_params));

    EXPECT_EQ(0, GetParam()->keymaster0_calls());
}

TEST_P(EncryptionOperationsTest, AesCtrInvalidCallerNonce) {
    ASSERT_EQ(KM_ERROR_OK, GenerateKey(AuthorizationSetBuilder()
                                           .AesEncryptionKey(128)
                                           .Authorization(TAG_BLOCK_MODE, KM_MODE_CTR)
                                           .Authorization(TAG_CALLER_NONCE)
                                           .Padding(KM_PAD_NONE)));

    AuthorizationSet input_params(client_params());
    input_params.push_back(TAG_BLOCK_MODE, KM_MODE_CTR);
    input_params.push_back(TAG_PADDING, KM_PAD_NONE);
    input_params.push_back(TAG_NONCE, "123", 3);
    EXPECT_EQ(KM_ERROR_INVALID_NONCE, BeginOperation(KM_PURPOSE_ENCRYPT, input_params));

    EXPECT_EQ(0, GetParam()->keymaster0_calls());
}

TEST_P(EncryptionOperationsTest, AesCbcRoundTripSuccess) {
    ASSERT_EQ(KM_ERROR_OK, GenerateKey(AuthorizationSetBuilder()
                                           .AesEncryptionKey(128)
                                           .Authorization(TAG_BLOCK_MODE, KM_MODE_CBC)
                                           .Padding(KM_PAD_NONE)));
    // Two-block message.
    string message = "12345678901234567890123456789012";
    string iv1;
    string ciphertext1 = EncryptMessage(message, KM_MODE_CBC, KM_PAD_NONE, &iv1);
    EXPECT_EQ(message.size(), ciphertext1.size());

    string iv2;
    string ciphertext2 = EncryptMessage(message, KM_MODE_CBC, KM_PAD_NONE, &iv2);
    EXPECT_EQ(message.size(), ciphertext2.size());

    // IVs should be random, so ciphertexts should differ.
    EXPECT_NE(iv1, iv2);
    EXPECT_NE(ciphertext1, ciphertext2);

    string plaintext = DecryptMessage(ciphertext1, KM_MODE_CBC, KM_PAD_NONE, iv1);
    EXPECT_EQ(message, plaintext);

    EXPECT_EQ(0, GetParam()->keymaster0_calls());
}

TEST_P(EncryptionOperationsTest, AesCallerNonce) {
    ASSERT_EQ(KM_ERROR_OK, GenerateKey(AuthorizationSetBuilder()
                                           .AesEncryptionKey(128)
                                           .Authorization(TAG_BLOCK_MODE, KM_MODE_CBC)
                                           .Authorization(TAG_CALLER_NONCE)
                                           .Padding(KM_PAD_NONE)));
    string message = "12345678901234567890123456789012";
    string iv1;
    // Don't specify nonce, should get a random one.
    string ciphertext1 = EncryptMessage(message, KM_MODE_CBC, KM_PAD_NONE, &iv1);
    EXPECT_EQ(message.size(), ciphertext1.size());
    EXPECT_EQ(16U, iv1.size());

    string plaintext = DecryptMessage(ciphertext1, KM_MODE_CBC, KM_PAD_NONE, iv1);
    EXPECT_EQ(message, plaintext);

    // Now specify a nonce, should also work.
    AuthorizationSet input_params(client_params());
    AuthorizationSet update_params;
    AuthorizationSet output_params;
    input_params.push_back(TAG_NONCE, "abcdefghijklmnop", 16);
    input_params.push_back(TAG_BLOCK_MODE, KM_MODE_CBC);
    input_params.push_back(TAG_PADDING, KM_PAD_NONE);
    string ciphertext2 =
        ProcessMessage(KM_PURPOSE_ENCRYPT, message, input_params, update_params, &output_params);

    // Decrypt with correct nonce.
    plaintext = ProcessMessage(KM_PURPOSE_DECRYPT, ciphertext2, input_params, update_params,
                               &output_params);
    EXPECT_EQ(message, plaintext);

    // Now try with wrong nonce.
    input_params.Reinitialize(client_params());
    input_params.push_back(TAG_BLOCK_MODE, KM_MODE_CBC);
    input_params.push_back(TAG_PADDING, KM_PAD_NONE);
    input_params.push_back(TAG_NONCE, "aaaaaaaaaaaaaaaa", 16);
    plaintext = ProcessMessage(KM_PURPOSE_DECRYPT, ciphertext2, input_params, update_params,
                               &output_params);
    EXPECT_NE(message, plaintext);

    EXPECT_EQ(0, GetParam()->keymaster0_calls());
}

TEST_P(EncryptionOperationsTest, AesCallerNonceProhibited) {
    ASSERT_EQ(KM_ERROR_OK, GenerateKey(AuthorizationSetBuilder()
                                           .AesEncryptionKey(128)
                                           .Authorization(TAG_BLOCK_MODE, KM_MODE_CBC)
                                           .Padding(KM_PAD_NONE)));

    string message = "12345678901234567890123456789012";
    string iv1;
    // Don't specify nonce, should get a random one.
    string ciphertext1 = EncryptMessage(message, KM_MODE_CBC, KM_PAD_NONE, &iv1);
    EXPECT_EQ(message.size(), ciphertext1.size());
    EXPECT_EQ(16U, iv1.size());

    string plaintext = DecryptMessage(ciphertext1, KM_MODE_CBC, KM_PAD_NONE, iv1);
    EXPECT_EQ(message, plaintext);

    // Now specify a nonce, should fail.
    AuthorizationSet input_params(client_params());
    AuthorizationSet update_params;
    AuthorizationSet output_params;
    input_params.push_back(TAG_NONCE, "abcdefghijklmnop", 16);
    input_params.push_back(TAG_BLOCK_MODE, KM_MODE_CBC);
    input_params.push_back(TAG_PADDING, KM_PAD_NONE);

    EXPECT_EQ(KM_ERROR_CALLER_NONCE_PROHIBITED,
              BeginOperation(KM_PURPOSE_ENCRYPT, input_params, &output_params));

    EXPECT_EQ(0, GetParam()->keymaster0_calls());
}

TEST_P(EncryptionOperationsTest, AesCbcIncrementalNoPadding) {
    ASSERT_EQ(KM_ERROR_OK, GenerateKey(AuthorizationSetBuilder()
                                           .AesEncryptionKey(128)
                                           .Authorization(TAG_BLOCK_MODE, KM_MODE_CBC)
                                           .Padding(KM_PAD_NONE)));

    int increment = 15;
    string message(240, 'a');
    AuthorizationSet input_params(client_params());
    input_params.push_back(TAG_BLOCK_MODE, KM_MODE_CBC);
    input_params.push_back(TAG_PADDING, KM_PAD_NONE);
    AuthorizationSet output_params;
    EXPECT_EQ(KM_ERROR_OK, BeginOperation(KM_PURPOSE_ENCRYPT, input_params, &output_params));

    string ciphertext;
    size_t input_consumed;
    for (size_t i = 0; i < message.size(); i += increment)
        EXPECT_EQ(KM_ERROR_OK,
                  UpdateOperation(message.substr(i, increment), &ciphertext, &input_consumed));
    EXPECT_EQ(KM_ERROR_OK, FinishOperation(&ciphertext));
    EXPECT_EQ(message.size(), ciphertext.size());

    // Move TAG_NONCE into input_params
    input_params.Reinitialize(output_params);
    input_params.push_back(client_params());
    input_params.push_back(TAG_BLOCK_MODE, KM_MODE_CBC);
    input_params.push_back(TAG_PADDING, KM_PAD_NONE);
    output_params.Clear();

    EXPECT_EQ(KM_ERROR_OK, BeginOperation(KM_PURPOSE_DECRYPT, input_params, &output_params));
    string plaintext;
    for (size_t i = 0; i < ciphertext.size(); i += increment)
        EXPECT_EQ(KM_ERROR_OK,
                  UpdateOperation(ciphertext.substr(i, increment), &plaintext, &input_consumed));
    EXPECT_EQ(KM_ERROR_OK, FinishOperation(&plaintext));
    EXPECT_EQ(ciphertext.size(), plaintext.size());
    EXPECT_EQ(message, plaintext);

    EXPECT_EQ(0, GetParam()->keymaster0_calls());
}

TEST_P(EncryptionOperationsTest, AesCbcPkcs7Padding) {
    ASSERT_EQ(KM_ERROR_OK, GenerateKey(AuthorizationSetBuilder()
                                           .AesEncryptionKey(128)
                                           .Authorization(TAG_BLOCK_MODE, KM_MODE_CBC)
                                           .Authorization(TAG_PADDING, KM_PAD_PKCS7)));

    // Try various message lengths; all should work.
    for (size_t i = 0; i < 32; ++i) {
        string message(i, 'a');
        string iv;
        string ciphertext = EncryptMessage(message, KM_MODE_CBC, KM_PAD_PKCS7, &iv);
        EXPECT_EQ(i + 16 - (i % 16), ciphertext.size());
        string plaintext = DecryptMessage(ciphertext, KM_MODE_CBC, KM_PAD_PKCS7, iv);
        EXPECT_EQ(message, plaintext);
    }

    EXPECT_EQ(0, GetParam()->keymaster0_calls());
}

TEST_P(EncryptionOperationsTest, AesGcmRoundTripSuccess) {
    ASSERT_EQ(KM_ERROR_OK, GenerateKey(AuthorizationSetBuilder()
                                           .AesEncryptionKey(128)
                                           .Authorization(TAG_BLOCK_MODE, KM_MODE_GCM)
                                           .Authorization(TAG_PADDING, KM_PAD_NONE)));
    string aad = "foobar";
    string message = "123456789012345678901234567890123456";
    AuthorizationSet begin_params(client_params());
    begin_params.push_back(TAG_BLOCK_MODE, KM_MODE_GCM);
    begin_params.push_back(TAG_PADDING, KM_PAD_NONE);
    begin_params.push_back(TAG_MAC_LENGTH, 128);
    AuthorizationSet begin_out_params;

    AuthorizationSet update_params;
    update_params.push_back(TAG_ASSOCIATED_DATA, aad.data(), aad.size());
    AuthorizationSet update_out_params;

    AuthorizationSet finish_params;
    AuthorizationSet finish_out_params;

    string ciphertext;
    string discard;
    string plaintext;

    size_t input_consumed;

    // Encrypt
    EXPECT_EQ(KM_ERROR_OK, BeginOperation(KM_PURPOSE_ENCRYPT, begin_params, &begin_out_params));
    EXPECT_EQ(KM_ERROR_OK, UpdateOperation(update_params, message, &update_out_params, &ciphertext,
                                           &input_consumed));
    EXPECT_EQ(message.size(), input_consumed);
    EXPECT_EQ(KM_ERROR_OK, FinishOperation(finish_params, "", &finish_out_params, &discard));

    // Grab nonce & tag.
    EXPECT_NE(-1, begin_out_params.find(TAG_NONCE));
    EXPECT_NE(-1, finish_out_params.find(TAG_AEAD_TAG));
    begin_params.push_back(begin_out_params);
    update_params.push_back(finish_out_params);

    // Decrypt.
    EXPECT_EQ(KM_ERROR_OK, BeginOperation(KM_PURPOSE_DECRYPT, begin_params, &begin_out_params));
    EXPECT_EQ(KM_ERROR_OK, UpdateOperation(update_params, ciphertext, &update_out_params,
                                           &plaintext, &input_consumed));
    EXPECT_EQ(ciphertext.size(), input_consumed);
    EXPECT_EQ(KM_ERROR_OK, FinishOperation(&discard));

    EXPECT_EQ(message, plaintext);
    EXPECT_EQ(0, GetParam()->keymaster0_calls());
}

TEST_P(EncryptionOperationsTest, AesGcmMultiPartAad) {
    ASSERT_EQ(KM_ERROR_OK, GenerateKey(AuthorizationSetBuilder()
                                           .AesEncryptionKey(128)
                                           .Authorization(TAG_BLOCK_MODE, KM_MODE_GCM)
                                           .Authorization(TAG_PADDING, KM_PAD_NONE)));
    string message = "123456789012345678901234567890123456";
    AuthorizationSet begin_params(client_params());
    begin_params.push_back(TAG_BLOCK_MODE, KM_MODE_GCM);
    begin_params.push_back(TAG_PADDING, KM_PAD_NONE);
    begin_params.push_back(TAG_MAC_LENGTH, 128);
    AuthorizationSet begin_out_params;

    AuthorizationSet update_params;
    update_params.push_back(TAG_ASSOCIATED_DATA, "foo", 3);
    AuthorizationSet update_out_params;

    AuthorizationSet finish_params;
    AuthorizationSet finish_out_params;

    string ciphertext;
    string discard;
    string plaintext;

    size_t input_consumed;

    EXPECT_EQ(KM_ERROR_OK, BeginOperation(KM_PURPOSE_ENCRYPT, begin_params, &begin_out_params));

    // No data, AAD only.
    EXPECT_EQ(KM_ERROR_OK, UpdateOperation(update_params, "", &update_out_params, &ciphertext,
                                           &input_consumed));

    // AAD and data.
    EXPECT_EQ(KM_ERROR_OK, UpdateOperation(update_params, message, &update_out_params, &ciphertext,
                                           &input_consumed));
    EXPECT_EQ(message.size(), input_consumed);
    EXPECT_EQ(KM_ERROR_OK, FinishOperation(finish_params, "", &finish_out_params, &discard));

    // Grab nonce & tag.
    EXPECT_NE(-1, begin_out_params.find(TAG_NONCE));
    begin_params.push_back(begin_out_params);

    EXPECT_NE(-1, finish_out_params.find(TAG_AEAD_TAG));
    update_params.push_back(finish_out_params);

    // All of the AAD in one.

    // Decrypt.
    EXPECT_EQ(KM_ERROR_OK, BeginOperation(KM_PURPOSE_DECRYPT, begin_params, &begin_out_params));
    EXPECT_EQ(KM_ERROR_OK, UpdateOperation(update_params, "", &update_out_params, &ciphertext,
                                           &input_consumed));
    EXPECT_EQ(KM_ERROR_OK, UpdateOperation(update_params, ciphertext, &update_out_params,
                                           &plaintext, &input_consumed));
    EXPECT_EQ(ciphertext.size(), input_consumed);
    EXPECT_EQ(KM_ERROR_OK, FinishOperation(&discard));

    EXPECT_EQ(message, plaintext);
    EXPECT_EQ(0, GetParam()->keymaster0_calls());
}

TEST_P(EncryptionOperationsTest, AesGcmBadAad) {
    ASSERT_EQ(KM_ERROR_OK, GenerateKey(AuthorizationSetBuilder()
                                           .AesEncryptionKey(128)
                                           .Authorization(TAG_BLOCK_MODE, KM_MODE_GCM)
                                           .Authorization(TAG_PADDING, KM_PAD_NONE)));
    string aad = "foobar";
    string message = "12345678901234567890123456789012";
    AuthorizationSet begin_params(client_params());
    begin_params.push_back(TAG_BLOCK_MODE, KM_MODE_GCM);
    begin_params.push_back(TAG_PADDING, KM_PAD_NONE);
    begin_params.push_back(TAG_MAC_LENGTH, 128);
    AuthorizationSet begin_out_params;

    AuthorizationSet update_params;
    update_params.push_back(TAG_ASSOCIATED_DATA, aad.data(), aad.size());
    AuthorizationSet update_out_params;

    AuthorizationSet finish_params;
    AuthorizationSet finish_out_params;

    string ciphertext;
    string discard;
    string plaintext;

    size_t input_consumed;

    // Encrypt
    EXPECT_EQ(KM_ERROR_OK, BeginOperation(KM_PURPOSE_ENCRYPT, begin_params, &begin_out_params));
    EXPECT_EQ(KM_ERROR_OK, UpdateOperation(update_params, message, &update_out_params, &ciphertext,
                                           &input_consumed));
    EXPECT_EQ(message.size(), input_consumed);
    EXPECT_EQ(KM_ERROR_OK, FinishOperation(finish_params, "", &finish_out_params, &discard));

    // Grab nonce & tag.
    EXPECT_NE(-1, begin_out_params.find(TAG_NONCE));
    EXPECT_NE(-1, finish_out_params.find(TAG_AEAD_TAG));
    begin_params.push_back(begin_out_params);
    update_params.Clear();
    update_params.push_back(TAG_ASSOCIATED_DATA, "barfoo" /* Wrong AAD */, 6);
    update_params.push_back(finish_out_params);

    // Decrypt.
    EXPECT_EQ(KM_ERROR_OK, BeginOperation(KM_PURPOSE_DECRYPT, begin_params, &begin_out_params));
    EXPECT_EQ(KM_ERROR_OK, UpdateOperation(update_params, ciphertext, &update_out_params,
                                           &plaintext, &input_consumed));
    EXPECT_EQ(ciphertext.size(), input_consumed);
    EXPECT_EQ(KM_ERROR_VERIFICATION_FAILED, FinishOperation(&discard));

    EXPECT_EQ(message, plaintext);
    EXPECT_EQ(0, GetParam()->keymaster0_calls());
}

TEST_P(EncryptionOperationsTest, AesGcmWrongNonce) {
    ASSERT_EQ(KM_ERROR_OK, GenerateKey(AuthorizationSetBuilder()
                                           .AesEncryptionKey(128)
                                           .Authorization(TAG_BLOCK_MODE, KM_MODE_GCM)
                                           .Authorization(TAG_PADDING, KM_PAD_NONE)));
    string aad = "foobar";
    string message = "12345678901234567890123456789012";
    AuthorizationSet begin_params(client_params());
    begin_params.push_back(TAG_BLOCK_MODE, KM_MODE_GCM);
    begin_params.push_back(TAG_PADDING, KM_PAD_NONE);
    begin_params.push_back(TAG_MAC_LENGTH, 128);
    AuthorizationSet begin_out_params;

    AuthorizationSet update_params;
    update_params.push_back(TAG_ASSOCIATED_DATA, aad.data(), aad.size());
    AuthorizationSet update_out_params;

    AuthorizationSet finish_params;
    AuthorizationSet finish_out_params;

    string ciphertext;
    string discard;
    string plaintext;

    size_t input_consumed;

    // Encrypt
    EXPECT_EQ(KM_ERROR_OK, BeginOperation(KM_PURPOSE_ENCRYPT, begin_params, &begin_out_params));
    EXPECT_EQ(KM_ERROR_OK, UpdateOperation(update_params, message, &update_out_params, &ciphertext,
                                           &input_consumed));
    EXPECT_EQ(message.size(), input_consumed);
    EXPECT_EQ(KM_ERROR_OK, FinishOperation(finish_params, "", &finish_out_params, &discard));

    EXPECT_NE(-1, finish_out_params.find(TAG_AEAD_TAG));
    update_params.push_back(finish_out_params);
    begin_params.push_back(TAG_NONCE, "123456789012", 12);

    EXPECT_EQ(KM_ERROR_OK, BeginOperation(KM_PURPOSE_DECRYPT, begin_params, &begin_out_params));
    EXPECT_EQ(KM_ERROR_OK, UpdateOperation(update_params, ciphertext, &update_out_params,
                                           &plaintext, &input_consumed));
    EXPECT_EQ(ciphertext.size(), input_consumed);
    EXPECT_EQ(KM_ERROR_VERIFICATION_FAILED, FinishOperation(&discard));

    // With wrong nonce, should have gotten garbage plaintext.
    EXPECT_NE(message, plaintext);
    EXPECT_EQ(0, GetParam()->keymaster0_calls());
}

TEST_P(EncryptionOperationsTest, AesGcmCorruptTag) {
    ASSERT_EQ(KM_ERROR_OK, GenerateKey(AuthorizationSetBuilder()
                                           .AesEncryptionKey(128)
                                           .Authorization(TAG_BLOCK_MODE, KM_MODE_GCM)
                                           .Authorization(TAG_PADDING, KM_PAD_NONE)));
    string aad = "foobar";
    string message = "123456789012345678901234567890123456";
    AuthorizationSet begin_params(client_params());
    begin_params.push_back(TAG_BLOCK_MODE, KM_MODE_GCM);
    begin_params.push_back(TAG_PADDING, KM_PAD_NONE);
    begin_params.push_back(TAG_MAC_LENGTH, 128);
    AuthorizationSet begin_out_params;

    AuthorizationSet update_params;
    update_params.push_back(TAG_ASSOCIATED_DATA, aad.data(), aad.size());
    AuthorizationSet update_out_params;

    AuthorizationSet finish_params;
    AuthorizationSet finish_out_params;

    string ciphertext;
    string discard;
    string plaintext;

    size_t input_consumed;

    // Encrypt
    EXPECT_EQ(KM_ERROR_OK, BeginOperation(KM_PURPOSE_ENCRYPT, begin_params, &begin_out_params));
    EXPECT_EQ(KM_ERROR_OK, UpdateOperation(update_params, message, &update_out_params, &ciphertext,
                                           &input_consumed));
    EXPECT_EQ(message.size(), input_consumed);
    EXPECT_EQ(KM_ERROR_OK, FinishOperation(finish_params, "", &finish_out_params, &discard));

    // Grab nonce & tag; corrupt tag.
    EXPECT_NE(-1, begin_out_params.find(TAG_NONCE));
    begin_params.push_back(begin_out_params);
    keymaster_blob_t tag;
    EXPECT_TRUE(finish_out_params.GetTagValue(TAG_AEAD_TAG, &tag));
    const_cast<uint8_t*>(tag.data)[tag.data_length / 2]++;
    update_params.push_back(TAG_AEAD_TAG, tag);

    // Decrypt.
    EXPECT_EQ(KM_ERROR_OK, BeginOperation(KM_PURPOSE_DECRYPT, begin_params, &begin_out_params));
    EXPECT_EQ(KM_ERROR_OK, UpdateOperation(update_params, ciphertext, &update_out_params,
                                           &plaintext, &input_consumed));
    EXPECT_EQ(ciphertext.size(), input_consumed);
    EXPECT_EQ(KM_ERROR_VERIFICATION_FAILED, FinishOperation(&discard));

    EXPECT_EQ(message, plaintext);
    EXPECT_EQ(0, GetParam()->keymaster0_calls());
}

TEST_P(EncryptionOperationsTest, AesGcmShortTag) {
    ASSERT_EQ(KM_ERROR_OK, GenerateKey(AuthorizationSetBuilder()
                                           .AesEncryptionKey(128)
                                           .Authorization(TAG_BLOCK_MODE, KM_MODE_GCM)
                                           .Authorization(TAG_PADDING, KM_PAD_NONE)));
    string aad = "foobar";
    string message = "123456789012345678901234567890123456";
    AuthorizationSet begin_params(client_params());
    begin_params.push_back(TAG_BLOCK_MODE, KM_MODE_GCM);
    begin_params.push_back(TAG_PADDING, KM_PAD_NONE);
    begin_params.push_back(TAG_MAC_LENGTH, 128);
    AuthorizationSet begin_out_params;

    AuthorizationSet update_params;
    update_params.push_back(TAG_ASSOCIATED_DATA, aad.data(), aad.size());
    AuthorizationSet update_out_params;

    AuthorizationSet finish_params;
    AuthorizationSet finish_out_params;

    string ciphertext;
    string discard;
    string plaintext;

    size_t input_consumed;

    // Encrypt
    EXPECT_EQ(KM_ERROR_OK, BeginOperation(KM_PURPOSE_ENCRYPT, begin_params, &begin_out_params));
    EXPECT_EQ(KM_ERROR_OK, UpdateOperation(update_params, message, &update_out_params, &ciphertext,
                                           &input_consumed));
    EXPECT_EQ(message.size(), input_consumed);
    EXPECT_EQ(KM_ERROR_OK, FinishOperation(finish_params, "", &finish_out_params, &discard));

    EXPECT_NE(-1, begin_out_params.find(TAG_NONCE));
    begin_params.push_back(begin_out_params);
    keymaster_blob_t tag;
    EXPECT_TRUE(finish_out_params.GetTagValue(TAG_AEAD_TAG, &tag));
    tag.data_length = 11;
    update_params.push_back(TAG_AEAD_TAG, tag);

    // Decrypt.
    EXPECT_EQ(KM_ERROR_OK, BeginOperation(KM_PURPOSE_DECRYPT, begin_params, &begin_out_params));
    EXPECT_EQ(KM_ERROR_UNSUPPORTED_MAC_LENGTH,
              UpdateOperation(update_params, ciphertext, &update_out_params, &plaintext,
                              &input_consumed));

    EXPECT_EQ(0, GetParam()->keymaster0_calls());
}

typedef Keymaster1Test AddEntropyTest;
INSTANTIATE_TEST_CASE_P(AndroidKeymasterTest, AddEntropyTest, test_params);

TEST_P(AddEntropyTest, AddEntropy) {
    // There's no obvious way to test that entropy is actually added, but we can test that the API
    // doesn't blow up or return an error.
    EXPECT_EQ(KM_ERROR_OK,
              device()->add_rng_entropy(device(), reinterpret_cast<const uint8_t*>("foo"), 3));

    EXPECT_EQ(0, GetParam()->keymaster0_calls());
}

typedef Keymaster1Test Keymaster0AdapterTest;
INSTANTIATE_TEST_CASE_P(
    AndroidKeymasterTest, Keymaster0AdapterTest,
    ::testing::Values(
        InstanceCreatorPtr(new Keymaster0AdapterTestInstanceCreator(true /* support_ec */)),
        InstanceCreatorPtr(new Keymaster0AdapterTestInstanceCreator(false /* support_ec */))));

TEST_P(Keymaster0AdapterTest, OldSoftwareKeymaster1RsaBlob) {
    // Load and use an old-style Keymaster1 software key blob.  These blobs contain OCB-encrypted
    // key data.
    string km1_sw = read_file("km1_sw_rsa_512.blob");
    EXPECT_EQ(486U, km1_sw.length());

    uint8_t* key_data = reinterpret_cast<uint8_t*>(malloc(km1_sw.length()));
    memcpy(key_data, km1_sw.data(), km1_sw.length());
    set_key_blob(key_data, km1_sw.length());

    string message(64, 'a');
    string signature;
    SignMessage(message, &signature, KM_DIGEST_NONE, KM_PAD_NONE);

    EXPECT_EQ(0, GetParam()->keymaster0_calls());
}

TEST_P(Keymaster0AdapterTest, OldSoftwareKeymaster1EcdsaBlob) {
    // Load and use an old-style Keymaster1 software key blob.  These blobs contain OCB-encrypted
    // key data.
    string km1_sw = read_file("km1_sw_ecdsa_256.blob");
    EXPECT_EQ(270U, km1_sw.length());

    uint8_t* key_data = reinterpret_cast<uint8_t*>(malloc(km1_sw.length()));
    memcpy(key_data, km1_sw.data(), km1_sw.length());
    set_key_blob(key_data, km1_sw.length());

    string message(64, 'a');
    string signature;
    SignMessage(message, &signature, KM_DIGEST_NONE, KM_PAD_NONE);

    EXPECT_EQ(0, GetParam()->keymaster0_calls());
}

struct Malloc_Delete {
    void operator()(void* p) { free(p); }
};

TEST_P(Keymaster0AdapterTest, OldSoftwareKeymaster0RsaBlob) {
    // Load and use an old softkeymaster blob.  These blobs contain PKCS#8 key data.
    string km0_sw = read_file("km0_sw_rsa_512.blob");
    EXPECT_EQ(333U, km0_sw.length());

    uint8_t* key_data = reinterpret_cast<uint8_t*>(malloc(km0_sw.length()));
    memcpy(key_data, km0_sw.data(), km0_sw.length());
    set_key_blob(key_data, km0_sw.length());

    string message(64, 'a');
    string signature;
    SignMessage(message, &signature, KM_DIGEST_NONE, KM_PAD_NONE);

    EXPECT_EQ(0, GetParam()->keymaster0_calls());
}

TEST_P(Keymaster0AdapterTest, OldSwKeymaster0RsaBlobGetCharacteristics) {
    // Load and use an old softkeymaster blob.  These blobs contain PKCS#8 key data.
    string km0_sw = read_file("km0_sw_rsa_512.blob");
    EXPECT_EQ(333U, km0_sw.length());

    uint8_t* key_data = reinterpret_cast<uint8_t*>(malloc(km0_sw.length()));
    memcpy(key_data, km0_sw.data(), km0_sw.length());
    set_key_blob(key_data, km0_sw.length());

    EXPECT_EQ(KM_ERROR_OK, GetCharacteristics());
    EXPECT_TRUE(contains(sw_enforced(), TAG_ALGORITHM, KM_ALGORITHM_RSA));
    EXPECT_TRUE(contains(sw_enforced(), TAG_KEY_SIZE, 512));
    EXPECT_TRUE(contains(sw_enforced(), TAG_RSA_PUBLIC_EXPONENT, 3));
    EXPECT_TRUE(contains(sw_enforced(), TAG_DIGEST, KM_DIGEST_NONE));
    EXPECT_TRUE(contains(sw_enforced(), TAG_PADDING, KM_PAD_NONE));
    EXPECT_TRUE(contains(sw_enforced(), TAG_PURPOSE, KM_PURPOSE_SIGN));
    EXPECT_TRUE(contains(sw_enforced(), TAG_PURPOSE, KM_PURPOSE_VERIFY));
    EXPECT_TRUE(sw_enforced().GetTagValue(TAG_ALL_USERS));
    EXPECT_TRUE(sw_enforced().GetTagValue(TAG_NO_AUTH_REQUIRED));

    EXPECT_EQ(0, GetParam()->keymaster0_calls());
}

TEST_P(Keymaster0AdapterTest, OldHwKeymaster0RsaBlob) {
    // Load and use an old softkeymaster blob.  These blobs contain PKCS#8 key data.
    string km0_sw = read_file("km0_sw_rsa_512.blob");
    EXPECT_EQ(333U, km0_sw.length());

    // The keymaster0 wrapper swaps the old softkeymaster leading 'P' for a 'Q' to make the key not
    // be recognized as a software key.  Do the same here to pretend this is a hardware key.
    EXPECT_EQ('P', km0_sw[0]);
    km0_sw[0] = 'Q';

    uint8_t* key_data = reinterpret_cast<uint8_t*>(malloc(km0_sw.length()));
    memcpy(key_data, km0_sw.data(), km0_sw.length());
    set_key_blob(key_data, km0_sw.length());

    string message(64, 'a');
    string signature;
    SignMessage(message, &signature, KM_DIGEST_NONE, KM_PAD_NONE);
    VerifyMessage(message, signature, KM_DIGEST_NONE, KM_PAD_NONE);

    EXPECT_EQ(5, GetParam()->keymaster0_calls());
}

TEST_P(Keymaster0AdapterTest, OldHwKeymaster0RsaBlobGetCharacteristics) {
    // Load and use an old softkeymaster blob.  These blobs contain PKCS#8 key data.
    string km0_sw = read_file("km0_sw_rsa_512.blob");
    EXPECT_EQ(333U, km0_sw.length());

    // The keymaster0 wrapper swaps the old softkeymaster leading 'P' for a 'Q' to make the key not
    // be recognized as a software key.  Do the same here to pretend this is a hardware key.
    EXPECT_EQ('P', km0_sw[0]);
    km0_sw[0] = 'Q';

    uint8_t* key_data = reinterpret_cast<uint8_t*>(malloc(km0_sw.length()));
    memcpy(key_data, km0_sw.data(), km0_sw.length());
    set_key_blob(key_data, km0_sw.length());

    EXPECT_EQ(KM_ERROR_OK, GetCharacteristics());
    EXPECT_TRUE(contains(hw_enforced(), TAG_ALGORITHM, KM_ALGORITHM_RSA));
    EXPECT_TRUE(contains(hw_enforced(), TAG_KEY_SIZE, 512));
    EXPECT_TRUE(contains(hw_enforced(), TAG_RSA_PUBLIC_EXPONENT, 3));
    EXPECT_TRUE(contains(hw_enforced(), TAG_DIGEST, KM_DIGEST_NONE));
    EXPECT_TRUE(contains(hw_enforced(), TAG_PADDING, KM_PAD_NONE));
    EXPECT_EQ(5U, hw_enforced().size());

    EXPECT_TRUE(contains(sw_enforced(), TAG_PURPOSE, KM_PURPOSE_SIGN));
    EXPECT_TRUE(contains(sw_enforced(), TAG_PURPOSE, KM_PURPOSE_VERIFY));
    EXPECT_TRUE(sw_enforced().GetTagValue(TAG_ALL_USERS));
    EXPECT_TRUE(sw_enforced().GetTagValue(TAG_NO_AUTH_REQUIRED));

    EXPECT_FALSE(contains(sw_enforced(), TAG_ALGORITHM, KM_ALGORITHM_RSA));
    EXPECT_FALSE(contains(sw_enforced(), TAG_KEY_SIZE, 512));
    EXPECT_FALSE(contains(sw_enforced(), TAG_RSA_PUBLIC_EXPONENT, 3));
    EXPECT_FALSE(contains(sw_enforced(), TAG_DIGEST, KM_DIGEST_NONE));
    EXPECT_FALSE(contains(sw_enforced(), TAG_PADDING, KM_PAD_NONE));

    EXPECT_EQ(1, GetParam()->keymaster0_calls());
}

}  // namespace test
}  // namespace keymaster
