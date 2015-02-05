/*
 * Copyright 2014 The Android Open Source Project
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

#ifndef SYSTEM_KEYMASTER_AES_KEY_H_
#define SYSTEM_KEYMASTER_AES_KEY_H_

#include <openssl/aes.h>

#include "symmetric_key.h"

namespace keymaster {

class AesKey : public SymmetricKey {
  public:
    AesKey(const AuthorizationSet& auths, const Logger& logger) : SymmetricKey(auths, logger) {}
    AesKey(const UnencryptedKeyBlob& blob, const Logger& logger, keymaster_error_t* error)
        : SymmetricKey(blob, logger, error) {}


    virtual Operation* CreateOperation(keymaster_purpose_t, keymaster_error_t* error);

  private:
    Operation* CreateOcbOperation(keymaster_purpose_t, keymaster_error_t* error);
};

}  // namespace keymaster

#endif  // SYSTEM_KEYMASTER_AES_KEY_H_
