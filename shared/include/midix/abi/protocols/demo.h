#pragma once
// Shared opcodes for the boot IPC and supervision demos.
#include "midix/abi/types.h"

constexpr mx_u32_t MSG_PING  = 1;
constexpr mx_u32_t MSG_PONG  = 2;
constexpr mx_u32_t MSG_CRASH = 0xDEAD;
constexpr mx_u32_t MSG_SLEEP_TEST_START = 0x534C4550; // 'SLEP'
constexpr mx_u32_t MSG_SLEEP_TEST_READY = 0x534C4551; // 'SLEQ'
constexpr mx_u32_t MSG_SLEEP_TEST_PING  = 0x534C4552; // 'SLER'
constexpr mx_u32_t MSG_SLEEP_TEST_PONG  = 0x534C4553; // 'SLES'
constexpr mx_u32_t MSG_CPU_QUERY = 0x43505551; // 'CPUQ'
constexpr mx_u32_t MSG_CPU_REPLY = 0x43505552; // 'CPUR'
constexpr mx_u32_t MSG_HOT_START = 0x484F5453; // 'HOTS'
constexpr mx_u32_t MSG_HOT_READY = 0x484F5452; // 'HOTR'

