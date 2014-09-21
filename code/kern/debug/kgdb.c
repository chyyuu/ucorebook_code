#include <types.h>
#include <stdio.h>
#include <string.h>
#include <console.h>
#include <trap.h>
#include <mmu.h>
#include <memlayout.h>
#include <kgdb.h>
#include <assert.h>

#define INT3            0xCC
#define PACKETBUFSIZE   2043

static void
kgdb_send_buf(void *buf, size_t size) {
    const char *p = (const char *)buf;
    while (size -- > 0) {
        serial2_putc(*p ++);
    }
}

static void
kgdb_sendc(char c) {
    serial2_putc(c);
}

static struct {
    char buf[PACKETBUFSIZE + 5];
    size_t len;
} packet_out;

static void
kgdb_send(const char *fmt, ...) {
    packet_out.buf[0] = '$';
    char *buf = &packet_out.buf[1];
    va_list ap;
    va_start(ap, fmt);
    size_t cnt = vsnprintf(buf, PACKETBUFSIZE, fmt, ap);
    va_end(ap);
    if (cnt >= PACKETBUFSIZE) {
        panic("packet_out is too big...\n");
    }
    int i = 0;
    uint32_t checksum = 0;
    for (i = 0; i < cnt; i ++) {
        checksum += buf[i];
    }
    snprintf(&buf[cnt], 3 + 1, "#%02x", checksum % 256);
    packet_out.len = cnt + 4;
    kgdb_send_buf(packet_out.buf, packet_out.len);
}

static void
kgdb_resend(void) {
    if (packet_out.len > 0) {
        kgdb_send_buf(packet_out.buf, packet_out.len);
    }
}

static enum {
    PACKET_LEX_INIT = 0,
    PACKET_LEX_DATA,
    PACKET_LEX_CS1,
    PACKET_LEX_CS2,
    PACKET_LEX_END,
    PACKET_LEX_ACK,
    PACKET_LEX_RETRANSMIT,
    PACKET_LEX_INTERRUPT,
    PACKET_LEX_ESCAPE,
} packet_lex_fsm = PACKET_LEX_INIT;

static inline int
is_hex(char c) {
    return (('0' <= c && c <= '9') ||
            ('a' <= c && c <= 'f') ||
            ('A' <= c && c <= 'F'));
}

static struct {
    char buf[PACKETBUFSIZE + 1];
    size_t last;
    uint32_t checksum;
    char cs1, cs2;
} packet;

static int
packet_lex_process(char c) {
    switch (packet_lex_fsm) {
    case PACKET_LEX_INIT:
        switch (c) {
        case '$':
            packet.last = 0;
            packet.checksum = 0;
            packet_lex_fsm = PACKET_LEX_DATA;
            goto correct;
        case '+':
            packet_lex_fsm = PACKET_LEX_ACK;
            goto correct;
        case '-':
            packet_lex_fsm = PACKET_LEX_RETRANSMIT;
            goto correct;
        case 0x03:
            packet_lex_fsm = PACKET_LEX_INTERRUPT;
            goto correct;
        default:
            goto error;
        }
    case PACKET_LEX_ESCAPE:
        packet.buf[packet.last ++] = ((c ^ 0x20) & 0xFF);
        packet.checksum += ((c ^ 0x20) & 0xFF);
        goto correct;
    case PACKET_LEX_DATA:
        switch (c) {
        case '#':
            packet.buf[packet.last ++] = '\0';
            packet_lex_fsm = PACKET_LEX_CS1;
            goto correct;
        case 0x7D:
            packet_lex_fsm = PACKET_LEX_ESCAPE;
            goto correct;
        default:
            packet.buf[packet.last ++] = c;
            packet.checksum += c;
            goto correct;
        }
    case PACKET_LEX_CS1:
        if (is_hex(c)) {
            packet.cs1 = c;
            packet_lex_fsm = PACKET_LEX_CS2;
            goto correct;
        }
        goto error;
    case PACKET_LEX_CS2:
        if (is_hex(c)) {
            packet.cs2 = c;
            packet_lex_fsm = PACKET_LEX_END;
            goto correct;
        }
        goto error;
    default:
        goto error;
    }

    int ret;
correct:
    switch (packet_lex_fsm) {
    case PACKET_LEX_END:
    case PACKET_LEX_ACK:
    case PACKET_LEX_RETRANSMIT:
    case PACKET_LEX_INTERRUPT:
        ret = packet_lex_fsm;
        packet_lex_fsm = PACKET_LEX_INIT;
        return ret;
    default:
        return packet_lex_fsm;
    }

error:
    return -1;
}

static int
byte2int(char c) {
    if ('0' <= c && c <= '9') {
        return c - '0';
    }
    if ('a' <= c && c <= 'f') {
        return c - 'a' + 10;
    }
    if ('A' <= c && c <= 'F') {
        return c - 'A' + 10;
    }
    panic("unknown character %c\n", c);
}

static void
sprintmem(void *buf, uintptr_t addr, int len) {
    const char *s = "0123456789abcdef";
    char *mem = (char *)addr, *p = (char *)buf;
    int i;
    for (i = 0; i < len; i ++, p += 2) {
        char c = *mem ++;
        p[0] = s[(c >> 4) & 0xF];
        p[1] = s[c & 0xF];
    }
}

static void
sprintreg(void *buf, uint32_t reg) {
    const char *s = "0123456789abcdef";
    char *p = (char *)buf;
    int j;
    for (j = 0; j < 4; j ++, p += 2) {
        p[0] = s[(reg >> 4) & 0xF];
        p[1] = s[reg & 0xF];
        reg >>= 8;
    }
}

static enum {
    KGDB_FSM_INIT = 0,
    KGDB_FSM_INTERACTIVE,
    KGDB_FSM_RUNNING,
} kgdb_fsm = KGDB_FSM_INIT;

static const char *
kgdb_get_trapframe(struct trapframe *tf) {
    uint32_t v[77] = {0};
    v[0] = tf->tf_regs.reg_eax;
    v[1] = tf->tf_regs.reg_ecx;
    v[2] = tf->tf_regs.reg_edx;
    v[3] = tf->tf_regs.reg_ebx;

    // suppose we are kgdbing kernel
    v[4] = (uint32_t)&(tf->tf_esp);
    v[5] = tf->tf_regs.reg_ebp;
    v[6] = tf->tf_regs.reg_esi;
    v[7] = tf->tf_regs.reg_edi;

    v[8] = tf->tf_eip;
    v[9] = tf->tf_eflags;

    v[10] = tf->tf_cs;
    // kernel ss
    v[11] = KERNEL_DS;
    v[12] = tf->tf_ds;
    v[13] = tf->tf_es;

    print_trapframe(tf);

    static char buf[77 * 8 + 1];
    char *p = buf;
    int i;
    for (i = 0; i < 77; i ++, p += 8) {
        sprintreg(p, v[i]);
    }
    buf[77 * 8] = '\0';
    return buf;
}

static uint32_t
parsereg(void **arg) {
    uint32_t x = 0;
    char *p = *(char **)arg + 8;
    int i;
    for (i = 0; i < 4; i ++) {
        p -= 2;
        x <<= 8;
        x += (0xFF & ((byte2int(p[0]) << 4) + byte2int(p[1])));
    }
    *(char **)arg = *(char **)arg + 8;
    return x;
}

static uint32_t
parseuint(void **arg, char end) {
    char *p = *(char **)arg;
    uint32_t x = 0;
    while (*p && *p != end) {
        x <<= 4;
        x += byte2int(*p);
        p ++;
    }
    if (*p) {
        p ++;
    }
    *(char **)arg = p;
    return x;
}

static void
kgdb_set_trapframe(struct trapframe *tf, void *buf) {
    tf->tf_regs.reg_eax = parsereg(&buf);
    tf->tf_regs.reg_ecx = parsereg(&buf);
    tf->tf_regs.reg_edx = parsereg(&buf);
    tf->tf_regs.reg_ebx = parsereg(&buf);

    // TODO check if change tf->tf_esp
    tf->tf_regs.reg_ebp = parsereg(&buf);
    tf->tf_regs.reg_esi = parsereg(&buf);
    tf->tf_regs.reg_edi = parsereg(&buf);

    tf->tf_eip = parsereg(&buf);
    tf->tf_eflags = parsereg(&buf);

    tf->tf_cs = parsereg(&buf);
    // TODO check if change tf->tf_ss;
    tf->tf_ds = parsereg(&buf);
    tf->tf_es = parsereg(&buf);
}

static int
str2int(const char *p) {
    int g = 0, g0 = 1;
    if (*p == '-') {
        g0 = -1;
        p ++;
    }
    while (*p) {
        g <<= 4;
        g += byte2int(*p);
        p ++;
    }
    return g * g0;
}

static const char *
kgdb_get_mem(void *msg) {
    uintptr_t addr = parseuint(&msg, ',');
    uint32_t len = parseuint(&msg, ':');
    static char buf[PACKETBUFSIZE + 1];
    sprintmem(buf, addr, len);
    buf[len * 2] = '\0';
    return buf;
}

static void
kgdb_set_mem(void *msg) {
    uintptr_t addr = parseuint(&msg, ',');
    uint32_t len = parseuint(&msg, ':');
    char *mem = (char *)addr, *p = (char *)msg;
    int i;
    for (i = 0; i < len; i ++, mem ++) {
        *mem = (byte2int(p[0]) << 4) + byte2int(p[1]);
        p += 2;
    }
}

static void
kgdb_set_mem_bin(void *msg) {
    uintptr_t addr = parseuint(&msg, ',');
    uint32_t len = parseuint(&msg, ':');
    char *mem = (char *)addr, *p = (char *)msg;
    while (len -- > 0) {
        *mem ++ = *p ++;
    }
}

static void
kgdb_set_register(struct trapframe *tf, void *msg) {
    uint32_t num = parseuint(&msg, '=');
    uint32_t value = parsereg(&msg);
    switch (num) {
    case 0:
        tf->tf_regs.reg_eax = value;
        break;
    case 1:
        tf->tf_regs.reg_ecx = value;
        break;
    case 2:
        tf->tf_regs.reg_edx = value;
        break;
    case 3:
        tf->tf_regs.reg_ebx = value;
        break;
    case 5:
        tf->tf_regs.reg_ebp = value;
        break;
    case 6:
        tf->tf_regs.reg_esi = value;
        break;
    case 7:
        tf->tf_regs.reg_edi = value;
        break;
    case 8:
        tf->tf_eip = value;
        break;
    case 9:
        tf->tf_eflags = value;
        break;
    case 10:
        tf->tf_cs = value;
        break;
    case 12:
        tf->tf_ds = value;
        break;
    case 13:
        tf->tf_es = value;
        break;
    }
}

#define MAX_BREAKPOINTS     10

static struct {
    uintptr_t eip;
    char orig;
    bool enabled;
} breakpoints[MAX_BREAKPOINTS];

static int
kgdb_remove_bp(void *msg) {
    uintptr_t addr = parseuint(&msg, ',');
    int i;
    for (i = 0; i < MAX_BREAKPOINTS; i ++) {
        if (breakpoints[i].enabled != 0 && breakpoints[i].eip == addr) {
            breakpoints[i].enabled = 0;
            *(char *)addr = breakpoints[i].orig;
            return 0;
        }
    }
    return -1;
}

static int
kgdb_insert_bp(void *msg) {
    uintptr_t addr = parseuint(&msg, ',');
    int i;
    for (i = 0; i < MAX_BREAKPOINTS; i ++) {
        if (breakpoints[i].enabled == 0) {
            breakpoints[i].eip = addr;
            breakpoints[i].orig = *(char *)addr;
            breakpoints[i].enabled = 1;
            *(char *)addr = INT3;
            return 0;
        }
    }
    return -1;
}

static int
kgdb_perform(struct trapframe *tf) {
    char *msg = packet.buf;
    switch (msg[0]) {
    case '?':
        kgdb_send("T05thread:01;");
        return 0;
    case 'g':
        kgdb_send(kgdb_get_trapframe(tf));
        return 0;
    case 'G':
        kgdb_set_trapframe(tf, msg + 1);
        kgdb_send("OK");
        return 0;
    case 'q':
        if (strcmp(msg, "qSupported") == 0) {
            kgdb_send("PacketSize=%d", PACKETBUFSIZE - 1);
            return 0;
        }
        else if (strcmp(msg, "qSymbol::") == 0) {
            kgdb_send("qSymbol:65786563");
            return 0;
        }
        else if (strncmp(msg, "qSymbol", strlen("qSymbol")) == 0) {
            kgdb_send("OK");
            return 0;
        }
        else if (strncmp(msg, "qfThreadInfo", strlen("qfThreadInfo")) == 0) {
            kgdb_send("m1l");
            return 0;
        }
        goto unknown_request;
    case 'H':
        if (msg[1] == 'c' || msg[1] == 'g') {
            kgdb_send("OK");
            return 0;
        }
        goto unknown_request;
    case 'm':
        kgdb_send(kgdb_get_mem(msg + 1));
        return 0;
    case 'M':
        kgdb_set_mem(msg + 1);
        kgdb_send("OK");
        return 0;
    case 'X':
        kgdb_set_mem_bin(msg + 1);
        kgdb_send("OK");
        return 0;
    case 'P':
        kgdb_set_register(tf, msg + 1);
        kgdb_send("OK");
        return 0;
    case 'p':
        switch (str2int(msg + 1)) {
        case 8:
            kgdb_send("%4l", tf->tf_eip);
            return 0;
        default:
            kgdb_send("0f010f01");
            return 0;
        }
    case 'T':
        kgdb_send("OK");
        return 0;
    case 'k':
        return 'k';
    case 'c':
        return 'c';
    case 'v':
        if (strcmp(msg, "vCont?") == 0) {
            kgdb_send("vCont;c;C;s;S");
            return 0;
        }
        if (strcmp(msg, "vCont;c") == 0) {
            return 'c';
        }
        if (strcmp(msg, "vCont;s:1") == 0 || strcmp(msg, "vCont;s:1;c") == 0) {
            tf->tf_eflags |= FL_TF;
            return 'c';
        }
        goto unknown_request;
    case 'z':
    case 'Z':
        if (msg[1] == '0') {
            if (msg[0] == 'z') {
                if (kgdb_remove_bp(msg + 3) == 0) {
                    kgdb_send("OK");
                }
                else {
                    kgdb_send("E01");
                }
            }
            else {
                if (kgdb_insert_bp(msg + 3) == 0) {
                    kgdb_send("OK");
                }
                else {
                    kgdb_send("E01");
                }
            }
            return 0;
        }
        goto unknown_request;
    default:
unknown_request:
        kgdb_send("");
        return 0;
    }
}

void
kgdb_interactive(struct trapframe *tf, int received) {
    if (kgdb_fsm == KGDB_FSM_RUNNING) {
        kgdb_send("T05thread:01;");
    }
    kgdb_fsm = KGDB_FSM_INTERACTIVE;

    tf->tf_eflags &= ~ FL_TF;

    while (1) {
        if (received == 0) {
            while (1) {
                char c;
                while (serial2_getc(&c) != 0);
                int packet_state = packet_lex_process(c);
                if (packet_state == PACKET_LEX_END) {
                    break;
                }
                else if (packet_state == PACKET_LEX_RETRANSMIT) {
                    kgdb_resend();
                }
            }
        }
        uint32_t checksum = (byte2int(packet.cs1) << 4) + byte2int(packet.cs2);
        if (packet.checksum % 256 != checksum) {
            kgdb_sendc('-');
            continue ;
        }
        kgdb_sendc('+');
        if (kgdb_perform(tf) != 0) {
            kgdb_sendc('+');
            break;
        }
        received = 0;
    }

    kgdb_fsm = KGDB_FSM_RUNNING;
}

void
kgdb_debug(struct trapframe *tf) {
    kgdb_interactive(tf, 0);
}

void
kgdb_intr(struct trapframe *tf) {
    char c;
    if (serial2_getc(&c) != 0) {
        return ;
    }
    int packet_state = packet_lex_process(c);
    if (kgdb_fsm == KGDB_FSM_RUNNING && packet_state == PACKET_LEX_INTERRUPT) {
        kgdb_send("S05");
        kgdb_interactive(tf, 0);
    }
    else {
        if (packet_state == PACKET_LEX_RETRANSMIT) {
            kgdb_resend();
        }
        else if (packet_state == PACKET_LEX_END) {
            if (kgdb_fsm == KGDB_FSM_INIT) {
                kgdb_interactive(tf, 1);
            }
        }
    }
}

void
kgdb_init(void) {
    kgdb_sendc(0xEF);
    packet.last = 0;
    packet.checksum = 0;
    packet_out.len = 0;
    int i;
    for (i = 0; i < MAX_BREAKPOINTS; i ++) {
        breakpoints[i].enabled = 0;
    }
}

