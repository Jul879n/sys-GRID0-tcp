/* Política pura de qué procesos intercepta el MITM de bsd:u. Sin libnx ni
 * stratosphere: se prueba en host (tests/test_policy.cpp). */
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace ztnx::mitm {

    constexpr size_t MaxExtraTitles = 16;

    /* Por debajo de este id están los módulos de sistema (loader, pm, ns, am,
     * nifm, bsd...). Interceptarlos rompe la red de la consola: nunca. Los applets
     * (qlaunch, Álbum...) empiezan aquí y solo se interceptan si el usuario los
     * lista en `extra_titles`. */
    constexpr uint64_t FirstInterceptableId = 0x0100000000001000ull;
    constexpr uint64_t SelfProgramId = 0x4200000000005A54ull;

    struct Policy {
        uint64_t extra[MaxExtraTitles] = {};
        size_t   nExtra = 0;
        bool     homebrew = false;   // mitm_homebrew = 1: toda aplicación (title takeover)
    };

    /* "0100..., 0x0100...; 0100..." -> ids. Ignora entradas que no sean exactamente
     * 16 dígitos hex (con 0x opcional). Devuelve cuántos guardó. */
    inline size_t ParseTitleList(const char *text, uint64_t *out, size_t max)
    {
        size_t n = 0;
        const char *p = text;
        while (p != nullptr && *p != '\0' && n < max) {
            while (*p == ' ' || *p == '\t' || *p == ',' || *p == ';') { ++p; }
            const char *start = p;
            while (*p != '\0' && *p != ' ' && *p != '\t' && *p != ',' && *p != ';' &&
                   *p != '\r' && *p != '\n') { ++p; }
            const char *end = p;
            if (end - start >= 2 && start[0] == '0' && (start[1] == 'x' || start[1] == 'X')) { start += 2; }
            if (end - start == 16) {
                uint64_t v = 0;
                bool ok = true;
                for (const char *c = start; c < end; ++c) {
                    uint64_t d;
                    if (*c >= '0' && *c <= '9') { d = static_cast<uint64_t>(*c - '0'); }
                    else if (*c >= 'a' && *c <= 'f') { d = static_cast<uint64_t>(*c - 'a' + 10); }
                    else if (*c >= 'A' && *c <= 'F') { d = static_cast<uint64_t>(*c - 'A' + 10); }
                    else { ok = false; break; }
                    v = (v << 4) | d;
                }
                if (ok) { out[n++] = v; }
            }
            if (*p == '\r' || *p == '\n') { break; }
        }
        return n;
    }

    inline bool IsExtraTitle(const Policy &pol, uint64_t id)
    {
        for (size_t i = 0; i < pol.nExtra; ++i) { if (pol.extra[i] == id) { return true; } }
        return false;
    }

    /* La primera sesión bsd de un juego oficial nunca recibe RegisterClient:
     * se deja pasar. En libnx (homebrew) es al revés: la sesión #1 (g_bsdSrv) hace
     * RegisterClient y todos los sockets, y la #2 solo StartMonitoring. Dejar pasar la
     * #1 de un homebrew lo deja sin virtualizar (verificado en bsd.log: Sphaira solo
     * tenía la sesión de monitoreo interceptada). */
    inline bool IsDummySession(int sessionNumber, bool isLanTitle)
    {
        return sessionNumber == 1 && isLanTitle;
    }

    /* `isApplicationId`: id de aplicación (0x0100000000010000..0x01FFFF...).
     * `isLanTitle`: está en la lista blanca de juegos LAN (lan_titles.hpp). */
    inline bool ShouldIntercept(uint64_t id, bool isApplicationId, bool isLanTitle, const Policy &pol)
    {
        if (id == SelfProgramId || id < FirstInterceptableId) { return false; }
        if (isApplicationId && isLanTitle) { return true; }
        if (IsExtraTitle(pol, id)) { return true; }
        return pol.homebrew && isApplicationId;
    }

}  // namespace ztnx::mitm
