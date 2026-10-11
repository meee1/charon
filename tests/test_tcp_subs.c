//MIT License
//
//Copyright (c) 2018 tvelliott
//
//Permission is hereby granted, free of charge, to any person obtaining a copy
//of this software and associated documentation files (the "Software"), to deal
//in the Software without restriction, including without limitation the rights
//to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
//copies of the Software, and to permit persons to whom the Software is
//furnished to do so, subject to the following conditions:
//
//The above copyright notice and this permission notice shall be included in all
//copies or substantial portions of the Software.
//
//THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
//IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
//FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
//AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
//LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
//OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
//SOFTWARE.

// Unit tests for tcp_subs.c — TCP MSS rewriting, window scaling, and checksums

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <arpa/inet.h>
#include "test_harness.h"

// Provide the extern that tcp_subs.c references from config.h
int max_tcp_segs = 2;

// Stub: charon.h may declare things we don't need for these tests.
// We include ethernet.h directly for the struct definitions.
#include "../ethernet.h"

// tcp_subs.c includes charon.h which declares main().  Rename it to avoid
// conflicting with our test main().
#define main charon_main
// Provide stubs for declarations pulled in transitively.
#include "../tcp_subs.c"
#undef main

///////////////////////////////////////////////////////////////////////////////
// Helper: build a minimal TCP SYN frame in a buffer
///////////////////////////////////////////////////////////////////////////////

static void build_tcp_syn_frame(uint8_t *buf, int total_len,
                                uint16_t mss_value, int include_win_scale)
{
    memset(buf, 0, total_len);

    eth_frame *ef = (eth_frame *)buf;

    // Ethernet header: IP type
    ef->ethhdr.eth_type = htons(ETH_IP_TYPE);

    // IP header
    ef->iphdr.version = 0x45;  // IPv4, IHL=5
    ef->iphdr.proto = IP_PROTO_TCP;

    // TCP header
    tcp_hdr *tcp = (tcp_hdr *)&ef->ip_proto_payload;
    tcp->tcp_flags = TCP_SYN;

    // Build TCP options
    uint8_t *opts = tcp->payload;
    int opt_offset = 0;

    // MSS option: kind=2, len=4, value=mss_value
    opts[opt_offset++] = TCP_OPT_MSS;
    opts[opt_offset++] = 4;
    opts[opt_offset++] = (mss_value >> 8) & 0xff;
    opts[opt_offset++] = mss_value & 0xff;

    if (include_win_scale) {
        // Window scale: kind=3, len=3, shift=7
        opts[opt_offset++] = TCP_OPT_WIN_SCALE;
        opts[opt_offset++] = 3;
        opts[opt_offset++] = 7;
        // NOOP padding
        opts[opt_offset++] = TCP_OPT_NOOP;
    }

    // End of options
    opts[opt_offset++] = TCP_OPT_END;

    // Pad to 4-byte boundary
    while (opt_offset % 4 != 0)
        opts[opt_offset++] = TCP_OPT_END;

    // TCP header length: (20 base + options) / 4, in upper nibble
    int tcp_hdr_len = 20 + opt_offset;
    tcp->hdr_len = (tcp_hdr_len / 4) << 4;

    // IP total length: ip_hdr + tcp_hdr_len
    ef->iphdr.len = htons(sizeof(ip_hdr) + tcp_hdr_len);

    // Set window
    tcp->win_size = htons(65535);
}

///////////////////////////////////////////////////////////////////////////////
// Checksum tests
///////////////////////////////////////////////////////////////////////////////

static void test_ipchksum_nonzero(void)
{
    TEST_BEGIN("ipchksum: produces nonzero result for valid header");
    ip_hdr ip;
    memset(&ip, 0, sizeof(ip));
    ip.version = 0x45;
    ip.len = htons(40);
    ip.ttl = 64;
    ip.proto = IP_PROTO_TCP;
    ip.src_ip = 0x0100007f;  // 127.0.0.1
    ip.dst_ip = 0x0100007f;
    uint16_t cksum = ipchksum((uint8_t *)&ip);
    TEST_ASSERT(cksum != 0);
    TEST_PASS();
}

static void test_ipchksum_deterministic(void)
{
    TEST_BEGIN("ipchksum: same input produces same checksum");
    ip_hdr ip;
    memset(&ip, 0, sizeof(ip));
    ip.version = 0x45;
    ip.len = htons(40);
    ip.proto = IP_PROTO_TCP;
    ip.src_ip = 0x0A000001;
    ip.dst_ip = 0x0A000002;
    uint16_t c1 = ipchksum((uint8_t *)&ip);
    uint16_t c2 = ipchksum((uint8_t *)&ip);
    TEST_ASSERT(c1 == c2);
    TEST_PASS();
}

static void test_ipchksum_different_addresses(void)
{
    TEST_BEGIN("ipchksum: different addresses produce different checksums");
    ip_hdr ip1, ip2;
    memset(&ip1, 0, sizeof(ip1));
    memset(&ip2, 0, sizeof(ip2));
    ip1.version = ip2.version = 0x45;
    ip1.len = ip2.len = htons(40);
    ip1.proto = ip2.proto = IP_PROTO_TCP;
    ip1.src_ip = 0x01020304;
    ip2.src_ip = 0x05060708;
    uint16_t c1 = ipchksum((uint8_t *)&ip1);
    uint16_t c2 = ipchksum((uint8_t *)&ip2);
    TEST_ASSERT(c1 != c2);
    TEST_PASS();
}

///////////////////////////////////////////////////////////////////////////////
// do_tcp_subs tests
///////////////////////////////////////////////////////////////////////////////

static void test_tcp_subs_non_ip_ignored(void)
{
    TEST_BEGIN("do_tcp_subs: non-IP frame returns 0");
    uint8_t buf[256];
    memset(buf, 0, sizeof(buf));
    eth_frame *ef = (eth_frame *)buf;
    ef->ethhdr.eth_type = htons(ETH_ARP_TYPE);
    int result = do_tcp_subs(buf, sizeof(buf));
    TEST_ASSERT(result == 0);
    TEST_PASS();
}

static void test_tcp_subs_syn_rewrites_mss(void)
{
    TEST_BEGIN("do_tcp_subs: SYN frame gets MSS rewritten to 1460");
    uint8_t buf[256];
    build_tcp_syn_frame(buf, sizeof(buf), 8960, 0);

    int result = do_tcp_subs(buf, sizeof(buf));
    TEST_ASSERT(result > 0);

    // Verify MSS was rewritten: find MSS option in TCP payload
    eth_frame *ef = (eth_frame *)buf;
    tcp_hdr *tcp = (tcp_hdr *)&ef->ip_proto_payload;
    uint8_t *opts = tcp->payload;
    // MSS option: kind=2, len=4, value
    TEST_ASSERT(opts[0] == TCP_OPT_MSS);
    uint16_t new_mss = (opts[2] << 8) | opts[3];
    TEST_ASSERT_MSG(new_mss == 1460, "MSS should be rewritten to 1460");
    TEST_PASS();
}

static void test_tcp_subs_syn_removes_window_scaling(void)
{
    TEST_BEGIN("do_tcp_subs: SYN frame has window scaling replaced with NOOPs");
    uint8_t buf[256];
    build_tcp_syn_frame(buf, sizeof(buf), 1460, 1);

    eth_frame *ef = (eth_frame *)buf;
    tcp_hdr *tcp = (tcp_hdr *)&ef->ip_proto_payload;

    // Before: window scale option should be at offset 4 (after MSS option)
    uint8_t *opts = tcp->payload;
    TEST_ASSERT(opts[4] == TCP_OPT_WIN_SCALE);

    do_tcp_subs(buf, sizeof(buf));

    // After: window scale bytes should be replaced with NOOPs
    TEST_ASSERT_MSG(opts[4] == TCP_OPT_NOOP, "win_scale kind byte should be NOOP");
    TEST_ASSERT_MSG(opts[5] == TCP_OPT_NOOP, "win_scale len byte should be NOOP");
    TEST_ASSERT_MSG(opts[6] == TCP_OPT_NOOP, "win_scale value byte should be NOOP");
    TEST_PASS();
}

static void test_tcp_subs_window_size_rewritten(void)
{
    TEST_BEGIN("do_tcp_subs: TCP window size rewritten to max_tcp_segs * MSS");
    uint8_t buf[256];
    build_tcp_syn_frame(buf, sizeof(buf), 1460, 0);

    eth_frame *ef = (eth_frame *)buf;
    tcp_hdr *tcp = (tcp_hdr *)&ef->ip_proto_payload;

    // Before
    TEST_ASSERT(htons(tcp->win_size) == 65535);

    do_tcp_subs(buf, sizeof(buf));

    // After: should be max_tcp_segs * 1460 = 2 * 1460 = 2920
    uint16_t expected = max_tcp_segs * 1460;
    uint16_t actual = htons(tcp->win_size);
    TEST_ASSERT_MSG(actual == expected, "window should be max_tcp_segs * MSS");
    TEST_PASS();
}

static void test_tcp_subs_checksums_recalculated(void)
{
    TEST_BEGIN("do_tcp_subs: IP and TCP checksums are recalculated");
    uint8_t buf[256];
    build_tcp_syn_frame(buf, sizeof(buf), 8960, 0);

    eth_frame *ef = (eth_frame *)buf;
    tcp_hdr *tcp = (tcp_hdr *)&ef->ip_proto_payload;

    // Set checksums to known bad values
    ef->iphdr.chksum = 0xFFFF;
    tcp->chksum = 0xFFFF;

    do_tcp_subs(buf, sizeof(buf));

    // After rewrite, checksums should have been recalculated (not 0xFFFF)
    TEST_ASSERT_MSG(ef->iphdr.chksum != 0xFFFF, "IP checksum should be recalculated");
    TEST_ASSERT_MSG(tcp->chksum != 0xFFFF, "TCP checksum should be recalculated");
    TEST_PASS();
}

static void test_tcp_subs_non_syn_only_window(void)
{
    TEST_BEGIN("do_tcp_subs: non-SYN TCP frame only gets window rewritten");
    uint8_t buf[256];
    memset(buf, 0, sizeof(buf));

    eth_frame *ef = (eth_frame *)buf;
    ef->ethhdr.eth_type = htons(ETH_IP_TYPE);
    ef->iphdr.version = 0x45;
    ef->iphdr.proto = IP_PROTO_TCP;
    ef->iphdr.len = htons(sizeof(ip_hdr) + 20);  // minimal TCP, no options

    tcp_hdr *tcp = (tcp_hdr *)&ef->ip_proto_payload;
    tcp->tcp_flags = TCP_ACK;  // Not SYN
    tcp->hdr_len = (20 / 4) << 4;
    tcp->win_size = htons(65535);

    int result = do_tcp_subs(buf, sizeof(buf));

    // Window should be rewritten
    uint16_t expected = max_tcp_segs * 1460;
    TEST_ASSERT(htons(tcp->win_size) == expected);
    // Result should be nonzero (checksum recalculated)
    TEST_ASSERT(result > 0);
    TEST_PASS();
}

static void test_tcp_subs_synack_rewrites_mss(void)
{
    TEST_BEGIN("do_tcp_subs: SYN+ACK frame also gets MSS rewritten");
    uint8_t buf[256];
    build_tcp_syn_frame(buf, sizeof(buf), 8960, 0);

    // Change flags to SYN|ACK
    eth_frame *ef = (eth_frame *)buf;
    tcp_hdr *tcp = (tcp_hdr *)&ef->ip_proto_payload;
    tcp->tcp_flags = TCP_SYN | TCP_ACK;

    do_tcp_subs(buf, sizeof(buf));

    // MSS should be rewritten
    uint8_t *opts = tcp->payload;
    uint16_t new_mss = (opts[2] << 8) | opts[3];
    TEST_ASSERT_MSG(new_mss == 1460, "SYN+ACK MSS should be rewritten to 1460");
    TEST_PASS();
}

static void test_tcp_subs_opt_end_terminates(void)
{
    TEST_BEGIN("do_tcp_subs: options parsing stops at END marker");
    uint8_t buf[256];
    memset(buf, 0, sizeof(buf));

    eth_frame *ef = (eth_frame *)buf;
    ef->ethhdr.eth_type = htons(ETH_IP_TYPE);
    ef->iphdr.version = 0x45;
    ef->iphdr.proto = IP_PROTO_TCP;

    tcp_hdr *tcp = (tcp_hdr *)&ef->ip_proto_payload;
    tcp->tcp_flags = TCP_SYN;

    // Options: END immediately, then garbage
    uint8_t *opts = tcp->payload;
    opts[0] = TCP_OPT_END;
    opts[1] = 0xFF;  // garbage after END
    opts[2] = 0xFF;
    opts[3] = 0xFF;

    int tcp_hdr_len = 20 + 4;
    tcp->hdr_len = (tcp_hdr_len / 4) << 4;
    ef->iphdr.len = htons(sizeof(ip_hdr) + tcp_hdr_len);

    // Should not crash — END terminates parsing before garbage
    do_tcp_subs(buf, sizeof(buf));

    // Garbage bytes should be untouched
    TEST_ASSERT(opts[1] == 0xFF);
    TEST_PASS();
}

///////////////////////////////////////////////////////////////////////////////
// Additional do_tcp_subs tests
///////////////////////////////////////////////////////////////////////////////

static void test_tcp_subs_udp_ip_not_rewritten(void)
{
    TEST_BEGIN("do_tcp_subs: UDP IP frame returns 0 (no TCP rewriting)");
    uint8_t buf[256];
    memset(buf, 0, sizeof(buf));

    eth_frame *ef = (eth_frame *)buf;
    ef->ethhdr.eth_type = htons(ETH_IP_TYPE);
    ef->iphdr.version = 0x45;
    ef->iphdr.proto = IP_PROTO_UDP;
    ef->iphdr.len = htons(sizeof(ip_hdr) + 8);  // minimal UDP

    int result = do_tcp_subs(buf, sizeof(buf));
    TEST_ASSERT_MSG(result == 0, "UDP frame should return 0 from do_tcp_subs");
    TEST_PASS();
}

static void test_tcp_subs_tcp_rst_no_mss_rewrite(void)
{
    TEST_BEGIN("do_tcp_subs: TCP RST frame gets window rewritten but not MSS");
    uint8_t buf[256];
    // Build a SYN frame first so MSS option is present, then change flags to RST
    build_tcp_syn_frame(buf, sizeof(buf), 8960, 0);

    eth_frame *ef = (eth_frame *)buf;
    tcp_hdr *tcp = (tcp_hdr *)&ef->ip_proto_payload;
    tcp->tcp_flags = TCP_RST;  // RST is neither SYN nor SYN|ACK, so sub_mss() is skipped but the window is still rewritten

    do_tcp_subs(buf, sizeof(buf));

    // MSS should NOT have been rewritten (only SYN/SYN-ACK triggers sub_mss)
    uint8_t *opts = tcp->payload;
    TEST_ASSERT(opts[0] == TCP_OPT_MSS);
    uint16_t mss = (opts[2] << 8) | opts[3];
    TEST_ASSERT_MSG(mss == 8960, "MSS should be unchanged for non-SYN TCP frame");

    // Window SHOULD have been rewritten
    uint16_t expected_win = max_tcp_segs * 1460;
    TEST_ASSERT_MSG(htons(tcp->win_size) == expected_win,
                    "window should be rewritten even for RST");
    TEST_PASS();
}

static void test_tcp_subs_max_segs_one(void)
{
    TEST_BEGIN("do_tcp_subs: max_tcp_segs=1 sets window to exactly one MSS");
    uint8_t buf[256];
    build_tcp_syn_frame(buf, sizeof(buf), 1460, 0);

    eth_frame *ef = (eth_frame *)buf;
    tcp_hdr *tcp = (tcp_hdr *)&ef->ip_proto_payload;

    int saved_max = max_tcp_segs;
    max_tcp_segs = 1;
    do_tcp_subs(buf, sizeof(buf));
    max_tcp_segs = saved_max;

    uint16_t actual = htons(tcp->win_size);
    TEST_ASSERT_MSG(actual == 1460, "window should be 1 * 1460 = 1460 bytes");
    TEST_PASS();
}

static void test_tcp_subs_sack_option_skipped(void)
{
    TEST_BEGIN("do_tcp_subs: SACK-permitted option (kind=4,len=2) is skipped cleanly");
    uint8_t buf[256];
    memset(buf, 0, sizeof(buf));

    eth_frame *ef = (eth_frame *)buf;
    ef->ethhdr.eth_type = htons(ETH_IP_TYPE);
    ef->iphdr.version = 0x45;
    ef->iphdr.proto = IP_PROTO_TCP;

    tcp_hdr *tcp = (tcp_hdr *)&ef->ip_proto_payload;
    tcp->tcp_flags = TCP_SYN;
    tcp->win_size = htons(65535);

    // Options: MSS(4 bytes) | SACK-PERMITTED(2 bytes) | END | pad
    uint8_t *opts = tcp->payload;
    int off = 0;
    opts[off++] = TCP_OPT_MSS;   // kind=2
    opts[off++] = 4;
    opts[off++] = (8960 >> 8) & 0xff;
    opts[off++] = 8960 & 0xff;
    opts[off++] = TCP_OPT_SACK;  // kind=4 (SACK-permitted)
    opts[off++] = 2;              // len=2
    opts[off++] = TCP_OPT_END;
    opts[off++] = 0;              // pad

    int tcp_hdr_len = 20 + off;
    tcp->hdr_len = (tcp_hdr_len / 4) << 4;
    ef->iphdr.len = htons(sizeof(ip_hdr) + tcp_hdr_len);

    // Must not crash and must rewrite MSS
    do_tcp_subs(buf, sizeof(buf));

    uint16_t new_mss = (opts[2] << 8) | opts[3];
    TEST_ASSERT_MSG(new_mss == 1460, "MSS should be rewritten even with SACK option present");

    // SACK option bytes should be unchanged (the default case skips them)
    TEST_ASSERT_MSG(opts[4] == TCP_OPT_SACK, "SACK kind byte should be untouched");
    TEST_PASS();
}

///////////////////////////////////////////////////////////////////////////////
// Direct checksum function tests
///////////////////////////////////////////////////////////////////////////////

static void test_tcpchksum_called_for_complete_frame(void)
{
    TEST_BEGIN("tcpchksum: produces nonzero value for a non-SYN TCP frame");
    uint8_t buf[256];
    memset(buf, 0, sizeof(buf));

    eth_frame *ef = (eth_frame *)buf;
    ef->ethhdr.eth_type = htons(ETH_IP_TYPE);
    ef->iphdr.version = 0x45;
    ef->iphdr.proto = IP_PROTO_TCP;
    // Set real source/dest to get a non-trivial checksum
    ef->iphdr.src_ip = 0x0100007f;  // 127.0.0.1
    ef->iphdr.dst_ip = 0x0200007f;  // 127.0.0.2
    ef->iphdr.len = htons(sizeof(ip_hdr) + 20);  // minimal TCP

    tcp_hdr *tcp = (tcp_hdr *)&ef->ip_proto_payload;
    tcp->tcp_flags = TCP_ACK;
    tcp->hdr_len = (20 / 4) << 4;
    tcp->src_port = htons(12345);
    tcp->dst_port = htons(80);

    uint16_t cksum = tcpchksum(buf);
    TEST_ASSERT_MSG(cksum != 0, "tcpchksum should be nonzero for a real frame");
    TEST_PASS();
}

///////////////////////////////////////////////////////////////////////////////
// main
///////////////////////////////////////////////////////////////////////////////

///////////////////////////////////////////////////////////////////////////////
// Regression tests for defects found with the RF channel simulator
///////////////////////////////////////////////////////////////////////////////

// Independent reference: one's-complement TCP checksum over pseudo header +
// segment (handles odd lengths without touching the buffer).
static uint16_t ref_tcp_checksum(const uint8_t *frame)
{
    const uint8_t *ip = frame + 14;
    int ip_len = (ip[2] << 8) | ip[3];
    int seg_len = ip_len - 20;
    uint32_t sum = 0;
    for (int i = 12; i < 20; i += 2) sum += (ip[i] << 8) | ip[i + 1];
    sum += 6 + seg_len;
    const uint8_t *seg = ip + 20;
    for (int i = 0; i < seg_len; i += 2) {
        if (i == 16) continue;   // checksum field itself
        sum += (seg[i] << 8) | (i + 1 < seg_len ? seg[i + 1] : 0);
    }
    while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
    return (uint16_t)~sum;
}

// Linux SYN: MSS, SACK_PERM, TS, NOP, WS (as captured in the simulator)
static int build_linux_syn(uint8_t *buf, uint16_t mss)
{
    static const uint8_t opts[20] = {
        2, 4, 0, 0,                       // MSS (filled below)
        4, 2,                             // SACK permitted
        8, 10, 0xa7, 0x48, 0x5d, 0xe4, 0, 0, 0, 0,  // TS val, ecr
        1,                                // NOP
        3, 3, 10                          // window scale 10
    };
    memset(buf, 0, 256);
    eth_frame *ef = (eth_frame *)buf;
    ef->ethhdr.eth_type = htons(ETH_IP_TYPE);
    ef->iphdr.version = 0x45;
    ef->iphdr.proto = IP_PROTO_TCP;
    ef->iphdr.len = htons(20 + 40);
    ef->iphdr.src_ip = htonl(0x0a000001);
    ef->iphdr.dst_ip = htonl(0x0a000002);
    tcp_hdr *tcp = (tcp_hdr *)&ef->ip_proto_payload;
    tcp->hdr_len = (40 / 4) << 4;
    tcp->tcp_flags = TCP_SYN;
    memcpy(tcp->payload, opts, sizeof(opts));
    tcp->payload[2] = mss >> 8;
    tcp->payload[3] = mss & 0xff;
    return 14 + 60;
}

static void test_linux_syn_options_preserved(void)
{
    TEST_BEGIN("do_tcp_subs: Linux SYN keeps SACK/TS intact, lowers MSS, NOPs WS");
    uint8_t buf[256];
    int len = build_linux_syn(buf, 2302);
    TEST_ASSERT(do_tcp_subs(buf, len) == len);
    tcp_hdr *tcp = (tcp_hdr *)&((eth_frame *)buf)->ip_proto_payload;
    uint8_t *o = tcp->payload;
    TEST_ASSERT(o[0] == 2 && o[1] == 4 && ((o[2] << 8) | o[3]) == 1460);
    TEST_ASSERT(o[4] == 4 && o[5] == 2);
    // timestamp option untouched (was corrupted to length 5 before)
    TEST_ASSERT(o[6] == 8 && o[7] == 10 && o[8] == 0xa7 && o[9] == 0x48 && o[10] == 0x5d && o[11] == 0xe4);
    TEST_ASSERT(o[16] == TCP_OPT_NOOP);
    TEST_ASSERT(o[17] == TCP_OPT_NOOP && o[18] == TCP_OPT_NOOP && o[19] == TCP_OPT_NOOP);
    TEST_ASSERT(ntohs(tcp->chksum) == ref_tcp_checksum(buf));
    TEST_PASS();
}

static void test_small_mss_not_raised(void)
{
    TEST_BEGIN("do_tcp_subs: a smaller advertised MSS is left alone");
    uint8_t buf[256];
    int len = build_linux_syn(buf, 536);
    do_tcp_subs(buf, len);
    tcp_hdr *tcp = (tcp_hdr *)&((eth_frame *)buf)->ip_proto_payload;
    TEST_ASSERT(((tcp->payload[2] << 8) | tcp->payload[3]) == 536);
    TEST_PASS();
}

static void test_bad_ip_length_ignored(void)
{
    TEST_BEGIN("do_tcp_subs: IP length beyond the frame (3001) or below 40 (10) is ignored");
    uint8_t buf[4096];
    int len = build_linux_syn(buf, 1460);
    memset(buf + len, 0xa5, sizeof(buf) - len);          // canary past the frame
    ((eth_frame *)buf)->iphdr.len = htons(3001);
    TEST_ASSERT(do_tcp_subs(buf, len) == 0);
    for (size_t i = len; i < sizeof(buf); i++) TEST_ASSERT(buf[i] == 0xa5);
    ((eth_frame *)buf)->iphdr.len = htons(10);
    TEST_ASSERT(do_tcp_subs(buf, len) == 0);              // used to hang in inet_chksum
    TEST_PASS();
}

static void test_fragment_and_ip_options_ignored(void)
{
    TEST_BEGIN("do_tcp_subs: fragments and IPv4 options are passed through untouched");
    uint8_t buf[256], orig[256];
    int len = build_linux_syn(buf, 2302);
    ((eth_frame *)buf)->iphdr.offset = htons(0x2000);   // MF set
    memcpy(orig, buf, sizeof(buf));
    TEST_ASSERT(do_tcp_subs(buf, len) == 0);
    TEST_ASSERT(memcmp(orig, buf, sizeof(buf)) == 0);
    len = build_linux_syn(buf, 2302);
    ((eth_frame *)buf)->iphdr.version = 0x46;            // IHL 6
    memcpy(orig, buf, sizeof(buf));
    TEST_ASSERT(do_tcp_subs(buf, len) == 0);
    TEST_ASSERT(memcmp(orig, buf, sizeof(buf)) == 0);
    TEST_PASS();
}

static void test_odd_length_checksum_no_overrun(void)
{
    TEST_BEGIN("do_tcp_subs: odd-length segment checksum is correct, no write past frame");
    uint8_t buf[256];
    int len = build_linux_syn(buf, 1460);
    eth_frame *ef = (eth_frame *)buf;
    tcp_hdr *tcp = (tcp_hdr *)&ef->ip_proto_payload;
    tcp->tcp_flags = TCP_ACK;
    tcp->hdr_len = (20 / 4) << 4;
    ef->iphdr.len = htons(20 + 20 + 7);                  // 7 data bytes
    len = 14 + 47;
    memcpy(tcp->payload, "abcdefg", 7);
    buf[len] = 0x5a;                                     // canary right after the frame
    TEST_ASSERT(do_tcp_subs(buf, len) == len);
    TEST_ASSERT(buf[len] == 0x5a);
    TEST_ASSERT(ntohs(tcp->chksum) == ref_tcp_checksum(buf));
    TEST_PASS();
}

static void test_malformed_option_length_stops(void)
{
    TEST_BEGIN("do_tcp_subs: malformed option length stops the option walk");
    uint8_t buf[256];
    int len = build_linux_syn(buf, 2302);
    tcp_hdr *tcp = (tcp_hdr *)&((eth_frame *)buf)->ip_proto_payload;
    tcp->payload[5] = 0;                                 // SACK_PERM with length 0
    TEST_ASSERT(do_tcp_subs(buf, len) == len);
    TEST_ASSERT(((tcp->payload[2] << 8) | tcp->payload[3]) == 1460);   // MSS before it still done
    TEST_ASSERT(tcp->payload[17] == 3);                  // WS after it untouched
    TEST_PASS();
}

int main(void)
{
    fprintf(stderr, "=== TCP Substitution Tests ===\n");

    RUN_TEST(test_ipchksum_nonzero);
    RUN_TEST(test_ipchksum_deterministic);
    RUN_TEST(test_ipchksum_different_addresses);

    RUN_TEST(test_tcp_subs_non_ip_ignored);
    RUN_TEST(test_tcp_subs_syn_rewrites_mss);
    RUN_TEST(test_tcp_subs_syn_removes_window_scaling);
    RUN_TEST(test_tcp_subs_window_size_rewritten);
    RUN_TEST(test_tcp_subs_checksums_recalculated);
    RUN_TEST(test_tcp_subs_non_syn_only_window);
    RUN_TEST(test_tcp_subs_synack_rewrites_mss);
    RUN_TEST(test_tcp_subs_opt_end_terminates);
    RUN_TEST(test_tcp_subs_udp_ip_not_rewritten);
    RUN_TEST(test_tcp_subs_tcp_rst_no_mss_rewrite);
    RUN_TEST(test_tcp_subs_max_segs_one);
    RUN_TEST(test_tcp_subs_sack_option_skipped);
    RUN_TEST(test_tcpchksum_called_for_complete_frame);

    RUN_TEST(test_linux_syn_options_preserved);
    RUN_TEST(test_small_mss_not_raised);
    RUN_TEST(test_bad_ip_length_ignored);
    RUN_TEST(test_fragment_and_ip_options_ignored);
    RUN_TEST(test_odd_length_checksum_no_overrun);
    RUN_TEST(test_malformed_option_length_stops);
    TEST_SUMMARY();
    return TEST_EXIT_CODE();
}
