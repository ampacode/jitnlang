#pragma once
// Дескриптор модуля tcp + eval-регистратор (глобальные Tcp*).
#include "../module.h"
#include "../nvm.h"
#include "../../modules/nlm_tcp.h"
#include "str.h" // sarg
#include "physics.h" // darg

namespace includedlib {

inline void register_tcp(Nvm& vm) {
    auto add = [&](const char* n,
                   std::function<NObject(Nvm&, const NArgs&)> f) {
        auto fn = std::make_shared<NFunction>();
        fn->name = n;
        fn->stdimpl = std::move(f);
        vm.set(n, NObject(fn));
    };
    add("TcpListen", [](Nvm&, const NArgs& a) {
        return NObject((long long)TcpListen((int)darg(a, 0))); });
    add("TcpAccept", [](Nvm&, const NArgs& a) {
        return NObject((long long)TcpAccept(
            (intptr_t)darg(a, 0))); });
    add("TcpConnect", [](Nvm&, const NArgs& a) {
        return NObject((long long)TcpConnect(sarg(a, 0),
                                             (int)darg(a, 1))); });
    add("TcpSend", [](Nvm&, const NArgs& a) {
        TcpSend((intptr_t)darg(a, 0), sarg(a, 1));
        return NObject(); });
    add("TcpRecvLine", [](Nvm&, const NArgs& a) {
        return NObject(TcpRecvLine((intptr_t)darg(a, 0))); });
    add("TcpRecvAll", [](Nvm&, const NArgs& a) {
        return NObject(TcpRecvAll((intptr_t)darg(a, 0))); });
    add("TcpSetTimeout", [](Nvm&, const NArgs& a) {
        TcpSetTimeout((intptr_t)darg(a, 0), (int)darg(a, 1));
        return NObject(); });
    add("TcpClose", [](Nvm&, const NArgs& a) {
        TcpClose((intptr_t)darg(a, 0));
        return NObject(); });
    add("TcpSleepMs", [](Nvm&, const NArgs& a) {
        TcpSleepMs((int)darg(a, 0));
        return NObject(); });
    add("TcpPeerIp", [](Nvm&, const NArgs& a) {
        return NObject(TcpPeerIp((intptr_t)darg(a, 0))); });
}

inline const ModuleDescriptor tcp_desc = {
    "tcp", "nlm_tcp.h",
#ifdef _WIN32
    {"ws2_32"},
#else
    {},
#endif
    [](Nvm& vm, const std::string&) { register_tcp(vm); },
    "tcp", nullptr
};

} // namespace includedlib
