#pragma once
// Дескриптор модуля crypto + eval-регистратор.
#include "../module.h"
#include "../nvm.h"
#include "../../modules/nlm_crypto.h"
#include "str.h" // sarg

namespace includedlib {

inline void register_crypto(Nvm& vm, const std::string& alias) {
    auto d = std::make_shared<std::map<std::string, NObject>>();
    auto add = [&](const char* n,
                   std::function<NObject(Nvm&, const NArgs&)> f) {
        auto fn = std::make_shared<NFunction>();
        fn->name = n;
        fn->stdimpl = std::move(f);
        (*d)[n] = NObject(fn);
    };
    add("b64encode", [](Nvm&, const NArgs& a) {
        return NObject(nlcrypto_b64encode(sarg(a, 0))); });
    add("b64decode", [](Nvm&, const NArgs& a) {
        return NObject(nlcrypto_b64decode(sarg(a, 0))); });
    add("randomBytes", [](Nvm&, const NArgs& a) {
        return NObject(nlcrypto_randomBytes(
            a.pos.empty() ? 0 : a.pos[0].asInt())); });
    add("keygen", [](Nvm&, const NArgs&) {
        return NObject(nlcrypto_keygen()); });
    add("toHex", [](Nvm&, const NArgs& a) {
        return NObject(nlcrypto_toHex(sarg(a, 0))); });
    add("aes256gcmEncrypt", [](Nvm&, const NArgs& a) {
        return NObject(nlcrypto_aes256gcmEncrypt(sarg(a, 0),
                                                 sarg(a, 1))); });
    add("aes256gcmDecrypt", [](Nvm&, const NArgs& a) {
        return NObject(nlcrypto_aes256gcmDecrypt(sarg(a, 0),
                                                 sarg(a, 1))); });
    add("hkdfSha256", [](Nvm&, const NArgs& a) {
        return NObject(nlcrypto_hkdfSha256(sarg(a, 0), sarg(a, 1),
            sarg(a, 2), a.pos.size() > 3 ? a.pos[3].asInt() : 32)); });
    vm.set(alias, NObject(d));
}

inline const ModuleDescriptor crypto_desc = {
    "crypto", "nlm_crypto.h", {"nlmodules", "bcrypt"},
    &register_crypto, "crypto", nullptr
};

} // namespace includedlib
