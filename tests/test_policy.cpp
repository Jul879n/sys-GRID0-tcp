/* Pruebas de host de la política del MITM (qué procesos se interceptan). */
#include "../source/mitm/mitm_policy.hpp"

#include <cstdio>

using namespace ztnx::mitm;

static int g_checks = 0, g_failures = 0;
#define CHECK(cond) do { ++g_checks; if (!(cond)) { ++g_failures; std::printf("  FAIL  %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)

int main()
{
    uint64_t ids[MaxExtraTitles];
    CHECK(ParseTitleList("", ids, MaxExtraTitles) == 0);
    CHECK(ParseTitleList("0100000000001234", ids, MaxExtraTitles) == 1 && ids[0] == 0x0100000000001234ull);
    CHECK(ParseTitleList("0x01000000000010AB, 0100ABCDEF012345;  zz  0100", ids, MaxExtraTitles) == 2);
    CHECK(ids[0] == 0x01000000000010ABull && ids[1] == 0x0100ABCDEF012345ull);
    CHECK(ParseTitleList("0100000000001234 0100000000001235", ids, 1) == 1);
    CHECK(ParseTitleList("01000000000012g4", ids, MaxExtraTitles) == 0);
    CHECK(ParseTitleList("0100000000001234\n0100000000001235", ids, MaxExtraTitles) == 1);

    CHECK(IsDummySession(1, true));      // juego oficial: sesión #1 sin RegisterClient
    CHECK(!IsDummySession(1, false));    // homebrew libnx: la #1 lleva los sockets
    CHECK(!IsDummySession(2, true) && !IsDummySession(2, false));

    Policy off;
    const uint64_t mk8 = 0x0100152000022000ull, album = 0x010000000000100Dull, homebrewApp = 0x0100000000012345ull;
    const uint64_t ns = 0x010000000000001Full, nifm = 0x010000000000000Full;
    CHECK(ShouldIntercept(mk8, true, true, off));                 // juego LAN: igual que antes
    CHECK(!ShouldIntercept(homebrewApp, true, false, off));        // aplicación fuera de la lista: no
    CHECK(!ShouldIntercept(album, false, false, off));             // applet: no
    CHECK(!ShouldIntercept(ns, false, false, off));                // sistema: no

    Policy extra;
    extra.nExtra = ParseTitleList("010000000000100D 0100000000012345", extra.extra, MaxExtraTitles);
    CHECK(extra.nExtra == 2);
    CHECK(ShouldIntercept(album, false, false, extra));            // applet listado
    CHECK(ShouldIntercept(homebrewApp, true, false, extra));       // aplicación listada
    CHECK(!ShouldIntercept(0x0100000000999999ull, true, false, extra));

    Policy hb;
    hb.homebrew = true;
    CHECK(ShouldIntercept(homebrewApp, true, false, hb));          // title takeover
    CHECK(!ShouldIntercept(album, false, false, hb));              // applets siguen exigiendo extra_titles

    // Los módulos de sistema nunca, ni listados.
    Policy evil;
    evil.homebrew = true;
    evil.extra[0] = ns; evil.extra[1] = nifm; evil.extra[2] = SelfProgramId; evil.nExtra = 3;
    CHECK(!ShouldIntercept(ns, false, false, evil));
    CHECK(!ShouldIntercept(nifm, false, false, evil));
    CHECK(!ShouldIntercept(SelfProgramId, true, true, evil));
    CHECK(!ShouldIntercept(0, false, false, evil));

    std::printf("policy tests\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
