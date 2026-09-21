#pragma once
/*
 * dango_channel_test.h — wire tags for the Phase G channel transport
 * acceptance (linux-artifacts/channel_test). Shared so the guest userspace and
 * any future in-guest checker agree on the message vocabulary; C/C++ compatible
 * (macros only).
 */
#define DANGO_CH_TEST_READY 0x47430001U
#define DANGO_CH_TEST_PING 0x47430002U
#define DANGO_CH_TEST_PONG 0x47430003U
#define DANGO_CH_TEST_DONE 0x47430004U

/* Guest<->host round trips the acceptance test drives. Deliberately unsigned-
 * suffix free: the acceptance marker string is built by stringifying this
 * macro (DANGO_STRINGIFY), and a "U" suffix would print as "512U" and no
 * longer match the numeric value the host harness derives from it. */
#define DANGO_CH_TEST_ROUNDS 512

/* Stringify a macro's value after expansion, so the acceptance marker string is
 * built from the same constant the test drives (no restated literal). */
#define DANGO_STRINGIFY_2(x) #x
#define DANGO_STRINGIFY(x) DANGO_STRINGIFY_2(x)
