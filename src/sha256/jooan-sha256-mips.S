/*
 * jooan-sha256 (MIPS32 assembly, no libc) — SHA-256 verifier for the
 * JOOAN JA-A12 open-stack installer.
 *
 * The device's busybox has md5sum but no sha256sum, and the OEM OTA container's
 * checks are MD5 over attacker-controlled data. docs/07-EXPLOIT-INSTALL-IMAGE.md
 * requires the project installer to verify its own SHA-256 manifest, so the
 * installer needs a SHA-256 implementation that runs on the device.
 *
 * This is a freestanding build: no libc, no dynamic linking, raw Linux o32
 * syscalls only. A glibc static build of the equivalent C is 574 KB and would
 * eat a quarter of the 2 MiB OTA payload limit and 15% of the 384 KiB /opt
 * partition. This version is a few KB, which is what makes the installer fit
 * the measured persistent layout.
 *
 * SHA-256 is implemented from FIPS 180-4 for correctness and licence clarity:
 * message schedule and state update are the straightforward definitions, with
 * rotation and selection written as instructions rather than adder chains.
 *
 * Usage:
 *   jooan-sha256 <file> [...]               print "<hex>  <file>"
 *
 * Exit: 0 all verified / hashed OK, 1 mismatch or I/O error, 2 usage error.
 *
 * Syscall ABI (o32): v0 = number, a0-a2 = args, result in v0, error = -errno.
 */

    .set    reorder                 /* let gas fill branch delay slots */
    .set    at                      /* pseudo-ops (li/blt/la) need $at */
    .set    nomips16
    .set    mips32r2
    .text
    .globl  _start
    .ent    _start

/* ------------------------------------------------------------------ *
 * Linux/MIPS syscall numbers
 * ------------------------------------------------------------------ */
    .set    SYS_exit,   4001
    .set    SYS_read,   4003
    .set    SYS_write,  4004
    .set    SYS_open,   4005
    .set    SYS_close,  4006

    .set    O_RDONLY,   0
    .set    STDOUT,     1
    .set    STDERR,     2

/* ================================================================== *
 * _start: dispatch on argv[1]
 * ================================================================== */
_start:
    lw      $s0, 0($sp)             /* argc */
    addiu   $s1, $sp, 4             /* argv */
    li      $s2, 2                  /* default exit code: usage error */

    li      $t0, 2
    blt     $s0, $t0, .Lusage       /* need at least one operand */

    /* ---------------- hash mode: files ---------------- *
     * Hash argv[1..argc-1] and print "<64 hex>  <path>\n" for each,
     * matching coreutils `sha256sum` so the output can be diffed against
     * a host-computed manifest.
     */
.Lhash_mode:
    li      $s2, 0                  /* aggregate success until a file fails */
    li      $s5, 1                  /* argv index */
.Lhash_loop:
    bge     $s5, $s0, .Lhash_done   /* argv index must be less than argc */
    nop
    sll     $t1, $s5, 2
    addu    $t1, $s1, $t1
    lw      $a0, 0($t1)
    jal     hash_file
    nop
    bnez    $v0, .Lhash_fail
    nop
    /* print "<hex>  <path>\n" */
    la      $a0, outhex
    jal     puts
    nop
    la      $a0, str_sp
    jal     puts
    nop
    sll     $t1, $s5, 2
    addu    $t1, $s1, $t1
    lw      $a0, 0($t1)
    jal     puts
    nop
    la      $a0, str_nl
    jal     puts
    nop
    b       .Lhash_next
.Lhash_fail:
    li      $s2, 1
.Lhash_next:
    addiu   $s5, $s5, 1
    b       .Lhash_loop
.Lhash_done:
    b       .Lexit

.Ldie_open_manifest:
    la      $a0, msg_no_manifest
    jal     puts_err
    nop
    la      $a0, outhex_argv_path  /* fall through: report the path we tried */
    li      $s2, 1
    b       .Lexit

.Lusage:
    la      $a0, msg_usage
    jal     puts_err
    nop
    li      $s2, 2
    b       .Lexit

.Lexit:
    move    $a0, $s2
    jal     sys_exit
    nop

    .end    _start

/* ================================================================== *
 * hash_file: a0 = path (or "-" for stdin) -> v0 = 0 ok, -1 error
 * outhex[] receives the lowercase hex digest.
 * ================================================================== */
    .globl  hash_file
    .ent    hash_file
hash_file:
    addiu   $sp, $sp, -24
    sw      $ra, 20($sp)
    sw      $s0, 16($sp)
    sw      $s1, 12($sp)
    sw      $s2, 8($sp)

    move    $s0, $a0                /* path */
    li      $s2, 1                  /* is_stdin = false */
    la      $s1, mf_stdin
    lbu     $t0, 0($s0)             /* byte load: argv strings are unaligned */
    lbu     $t1, 0($s1)
    bne     $t0, $t1, 1f
    nop
    li      $s2, 0                  /* stdin: use fd 0, do not close it */
    b       2f
1:
    move    $a0, $s0
    li      $a1, O_RDONLY
    li      $a2, 0
    jal     sys_open
    nop
    bltz    $v0, .Lhf_err
    nop
    move    $s2, $v0                /* fd, and nonzero => must be closed */
2:
    jal     sha256_init
    nop

.Lhf_read:
    move    $a0, $s2
    la      $a1, iobuf
    li      $a2, 65536
    jal     sys_read
    nop
    bltz    $v0, .Lhf_close_err
    nop
    beqz    $v0, .Lhf_done
    nop
    la      $a0, iobuf
    move    $a1, $v0
    jal     sha256_update
    nop
    b       .Lhf_read

.Lhf_done:
    beqz    $s2, 1f
    nop
    move    $a0, $s2
    jal     sys_close
    nop
1:
    jal     sha256_final
    nop
    /* render 32 bytes -> 64 lowercase hex chars into outhex */
    la      $t0, sha_digest
    la      $t1, outhex
    li      $t2, 32
    la      $t3, hexchars
2:
    lbu     $t4, 0($t0)
    srl     $t5, $t4, 4
    addu    $t6, $t3, $t5
    lbu     $t5, 0($t6)
    sb      $t5, 0($t1)
    andi    $t4, $t4, 0x0f
    addu    $t6, $t3, $t4
    lbu     $t4, 0($t6)
    sb      $t4, 1($t1)
    addiu   $t0, $t0, 1
    addiu   $t1, $t1, 2
    addiu   $t2, $t2, -1
    bnez    $t2, 2b
    nop
    sb      $zero, 0($t1)

    li      $v0, 0
    b       .Lhf_ret

.Lhf_close_err:
    move    $t9, $v0
    beqz    $s2, 1f
    nop
    move    $a0, $s2
    jal     sys_close
    nop
1:
    move    $v0, $t9
.Lhf_err:
    li      $v0, -1
.Lhf_ret:
    lw      $ra, 20($sp)
    lw      $s0, 16($sp)
    lw      $s1, 12($sp)
    lw      $s2, 8($sp)
    addiu   $sp, $sp, 24
    jr      $ra
    nop
    .end    hash_file

/* ================================================================== *
 * verify_stream: a0 = fd, a1 = base dir -> 0 ok, 1 failures
 * Line format: "<64 hex><spaces or *><path>"
 * Paths beginning with '/' are absolute; otherwise base + "/" + path.
 * ================================================================== */
    .globl  verify_stream
    .ent    verify_stream
verify_stream:
    addiu   $sp, $sp, -40
    sw      $ra, 36($sp)
    sw      $s0, 32($sp)            /* fd */
    sw      $s1, 28($sp)            /* base */
    sw      $s2, 24($sp)            /* bad count */
    sw      $s3, 20($sp)            /* total count */
    sw      $s4, 16($sp)

    move    $s0, $a0
    move    $s1, $a1
    li      $s2, 0
    li      $s3, 0

.Lvs_line:
    move    $a0, $s0
    jal     read_line               /* -> v0 = length, -1 eof/err */
    nop
    bltz    $v0, .Lvs_end
    nop
    beqz    $v0, .Lvs_line          /* empty line */

    /* skip leading blanks */
    la      $t0, linebuf
.Lvs_skip:
    lbu     $t1, 0($t0)
    li      $t2, 0x20
    beq     $t1, $t2, 1f
    nop
    li      $t2, 0x09
    bne     $t1, $t2, .Lvs_after_skip
    nop
1:
    addiu   $t0, $t0, 1
    b       .Lvs_skip
.Lvs_after_skip:
    beqz    $t1, .Lvs_line          /* blank */
    li      $t2, 0x23               /* '#' */
    beq     $t1, $t2, .Lvs_line

    /* need at least 64 hex chars */
    la      $t3, linebuf
    subu    $t4, $t0, $t3
    li      $t5, 64
    addu    $t6, $t4, $t5
    la      $t7, line_len
    lw      $t7, 0($t7)
    bgt     $t6, $t7, .Lvs_bad_line
    nop

    /* copy 64 hex chars to wanthex, lowercased */
    la      $t1, wanthex
    li      $t2, 64
2:
    lbu     $t4, 0($t0)
    /* lowercase: if 'A' <= c <= 'F' add 32 */
    li      $t5, 0x41
    blt     $t4, $t5, 3f
    nop
    li      $t5, 0x46
    bgt     $t4, $t5, 3f
    nop
    addiu   $t4, $t4, 32
3:
    sb      $t4, 0($t1)
    addiu   $t0, $t0, 1
    addiu   $t1, $t1, 1
    addiu   $t2, $t2, -1
    bnez    $t2, 2b
    nop

    /* skip separators: spaces, tabs, '*' */
.Lvs_sep:
    lbu     $t1, 0($t0)
    li      $t2, 0x20
    beq     $t1, $t2, 1f
    nop
    li      $t2, 0x09
    beq     $t1, $t2, 1f
    nop
    li      $t2, 0x2a               /* '*' */
    bne     $t1, $t2, .Lvs_path
    nop
1:
    addiu   $t0, $t0, 1
    b       .Lvs_sep

.Lvs_path:
    beqz    $t1, .Lvs_line          /* digest but no path */

    /* Build the full path into fullpath.
     *   absolute (leading '/')  -> copy the manifest path verbatim
     *   relative                -> base, then '/', then the manifest path
     */
    la      $t1, fullpath
    lbu     $t2, 0($t0)
    li      $t3, 0x2f               /* '/' */
    beq     $t2, $t3, .Lvs_copy_rel
    nop

    /* relative: copy the base directory first */
    move    $t4, $s1
.Lvs_base_cp:
    lbu     $t5, 0($t4)
    beqz    $t5, .Lvs_base_done
    nop
    sb      $t5, 0($t1)
    addiu   $t4, $t4, 1
    addiu   $t1, $t1, 1
    b       .Lvs_base_cp
.Lvs_base_done:
    /* if the base does not already end in '/', add one */
    la      $t4, fullpath
    beq     $t1, $t4, 1f            /* empty base: no separator needed */
    nop
    lbu     $t5, -1($t1)
    li      $t6, 0x2f
    beq     $t5, $t6, .Lvs_copy_rel
    nop
1:
    li      $t5, 0x2f
    sb      $t5, 0($t1)
    addiu   $t1, $t1, 1

.Lvs_copy_rel:
    lbu     $t5, 0($t0)
    beqz    $t5, .Lvs_copied
    nop
    li      $t6, 0x0d               /* strip CR */
    beq     $t5, $t6, .Lvs_copied
    nop
    li      $t6, 0x0a               /* strip LF */
    beq     $t5, $t6, .Lvs_copied
    nop
    sb      $t5, 0($t1)
    addiu   $t0, $t0, 1
    addiu   $t1, $t1, 1
    b       .Lvs_copy_rel
.Lvs_copied:
    sb      $zero, 0($t1)

    addiu   $s3, $s3, 1
    la      $a0, fullpath
    jal     hash_file
    nop
    bnez    $v0, .Lvs_count_bad
    nop

    /* compare wanthex with outhex */
    la      $t0, wanthex
    la      $t1, outhex
    li      $t2, 64
4:
    lbu     $t3, 0($t0)
    lbu     $t4, 0($t1)
    bne     $t3, $t4, .Lvs_mismatch
    nop
    addiu   $t0, $t0, 1
    addiu   $t1, $t1, 1
    addiu   $t2, $t2, -1
    bnez    $t2, 4b
    nop
    b       .Lvs_line

.Lvs_mismatch:
    la      $a0, msg_mismatch
    jal     puts_err
    nop
    la      $a0, fullpath
    jal     puts_err
    nop
    la      $a0, msg_want
    jal     puts_err
    nop
    la      $a0, wanthex
    jal     puts_err
    nop
    la      $a0, msg_got
    jal     puts_err
    nop
    la      $a0, outhex
    jal     puts_err
    nop
    la      $a0, str_nl
    jal     puts_err
    nop
.Lvs_count_bad:
    addiu   $s2, $s2, 1
    b       .Lvs_line

.Lvs_bad_line:
    addiu   $s2, $s2, 1
    addiu   $s3, $s3, 1
    la      $a0, msg_shortline
    jal     puts_err
    nop
    b       .Lvs_line

.Lvs_end:
    beqz    $s2, .Lvs_ok
    nop
    la      $a0, msg_failed
    jal     puts_err
    nop
    li      $v0, 1
    b       .Lvs_ret
.Lvs_ok:
    la      $a0, msg_verified
    jal     puts
    nop
    move    $a0, $s3
    jal     putnum
    nop
    la      $a0, msg_entries
    jal     puts
    nop
    li      $v0, 0
.Lvs_ret:
    lw      $ra, 36($sp)
    lw      $s0, 32($sp)
    lw      $s1, 28($sp)
    lw      $s2, 24($sp)
    lw      $s3, 20($sp)
    lw      $s4, 16($sp)
    addiu   $sp, $sp, 40
    jr      $ra
    nop
    .end    verify_stream

/* ================================================================== *
 * read_line: a0 = fd. Reads up to LINE_MAX-1 bytes into linebuf,
 * stores the length in line_len. v0 = length, or -1 at EOF.
 * Byte-at-a-time is fine: manifests are a few hundred bytes and this
 * keeps the freestanding code small.
 * ================================================================== */
    .globl  read_line
    .ent    read_line
read_line:
    addiu   $sp, $sp, -24
    sw      $ra, 20($sp)
    sw      $s0, 16($sp)
    sw      $s1, 12($sp)

    move    $s0, $a0
    la      $s1, linebuf
    li      $t9, 0
    la      $t8, line_len

.Lrl_loop:
    li      $t0, 4095
    bge     $t9, $t0, .Lrl_done
    nop
    move    $a0, $s0
    la      $a1, onebyte
    li      $a2, 1
    jal     sys_read
    nop
    beqz    $v0, .Lrl_eof
    nop
    bltz    $v0, .Lrl_eof
    nop
    la      $t1, onebyte
    lbu     $t2, 0($t1)
    li      $t3, 0x0a
    beq     $t2, $t3, .Lrl_done
    nop
    addu    $t1, $s1, $t9
    sb      $t2, 0($t1)
    addiu   $t9, $t9, 1
    b       .Lrl_loop

.Lrl_eof:
    beqz    $t9, .Lrl_eof_empty
    nop
.Lrl_done:
    addu    $t1, $s1, $t9
    sb      $zero, 0($t1)
    sw      $t9, 0($t8)
    move    $v0, $t9
    b       .Lrl_ret
.Lrl_eof_empty:
    li      $v0, -1
.Lrl_ret:
    lw      $ra, 20($sp)
    lw      $s0, 16($sp)
    lw      $s1, 12($sp)
    addiu   $sp, $sp, 24
    jr      $ra
    nop
    .end    read_line

/* ================================================================== *
 * SHA-256 core.  State lives in sha_state[8]; only 32-bit ops are used.
 * ================================================================== */
    .globl  sha256_init
    .ent    sha256_init
sha256_init:
    la      $t0, sha_state
    la      $t1, sha_iv
    li      $t2, 8
1:
    lw      $t3, 0($t1)
    sw      $t3, 0($t0)
    addiu   $t1, $t1, 4
    addiu   $t0, $t0, 4
    addiu   $t2, $t2, -1
    bnez    $t2, 1b
    nop
    la      $t0, sha_bitlen
    sw      $zero, 0($t0)
    sw      $zero, 4($t0)
    la      $t0, sha_buflen
    sw      $zero, 0($t0)
    jr      $ra
    nop
    .end    sha256_init

/* sha256_update: a0 = data, a1 = len. Buffers partial blocks. */
    .globl  sha256_update
    .ent    sha256_update
sha256_update:
    addiu   $sp, $sp, -32
    sw      $ra, 28($sp)
    sw      $s0, 24($sp)            /* data */
    sw      $s1, 20($sp)            /* len */
    sw      $s2, 16($sp)            /* buflen */
    sw      $s3, 12($sp)

    move    $s0, $a0
    move    $s1, $a1

    /* bitlen += len * 8  (64-bit) */
    la      $t0, sha_bitlen
    lw      $t1, 0($t0)
    lw      $t2, 4($t0)
    sll     $t3, $s1, 3
    srl     $t4, $s1, 29
    addu    $t1, $t1, $t3
    sltu    $t5, $t1, $t3
    addu    $t2, $t2, $t4
    addu    $t2, $t2, $t5
    sw      $t1, 0($t0)
    sw      $t2, 4($t0)

    la      $t0, sha_buflen
    lw      $s2, 0($t0)

.Lsu_loop:
    beqz    $s1, .Lsu_fin
    nop
    li      $t0, 64
    subu    $s3, $t0, $s2            /* space in buffer */
    sltu    $t0, $s1, $s3
    beqz    $t0, 1f
    nop
    move    $s3, $s1                 /* copy min(space, len) */
1:
    /* memcpy(sha_buf + buflen, data, s3) */
    la      $t0, sha_buf
    addu    $t0, $t0, $s2
    move    $t1, $s0
    move    $t2, $s3
2:
    beqz    $t2, 3f
    nop
    lbu     $t3, 0($t1)
    sb      $t3, 0($t0)
    addiu   $t1, $t1, 1
    addiu   $t0, $t0, 1
    addiu   $t2, $t2, -1
    b       2b
    nop
3:
    addu    $s2, $s2, $s3
    addu    $s0, $s0, $s3
    subu    $s1, $s1, $s3

    li      $t0, 64
    bne     $s2, $t0, .Lsu_loop
    nop
    la      $a0, sha_buf
    jal     sha256_block
    nop
    li      $s2, 0
    b       .Lsu_loop

.Lsu_fin:
    la      $t0, sha_buflen
    sw      $s2, 0($t0)
    lw      $ra, 28($sp)
    lw      $s0, 24($sp)
    lw      $s1, 20($sp)
    lw      $s2, 16($sp)
    lw      $s3, 12($sp)
    addiu   $sp, $sp, 32
    jr      $ra
    nop
    .end    sha256_update

/* sha256_final: appends padding, writes 32-byte digest to sha_digest. */
    .globl  sha256_final
    .ent    sha256_final
sha256_final:
    addiu   $sp, $sp, -24
    sw      $ra, 20($sp)
    sw      $s0, 16($sp)

    la      $t0, sha_buflen
    lw      $s0, 0($t0)

    /* append 0x80 */
    la      $t1, sha_buf
    addu    $t1, $t1, $s0
    li      $t2, 0x80
    sb      $t2, 0($t1)
    addiu   $s0, $s0, 1

    li      $t2, 56
    ble     $s0, $t2, .Lsf_pad
    nop
    /* fill to 64, block, then start a fresh zeroed block */
    la      $t1, sha_buf
.Lsf_fill1:
    li      $t2, 64
    bge     $s0, $t2, 1f
    nop
    addu    $t3, $t1, $s0
    sb      $zero, 0($t3)
    addiu   $s0, $s0, 1
    b       .Lsf_fill1
1:
    la      $a0, sha_buf
    jal     sha256_block
    nop
    li      $s0, 0
.Lsf_pad:
    la      $t1, sha_buf
.Lsf_zero:
    li      $t2, 56
    bge     $s0, $t2, .Lsf_len
    nop
    addu    $t3, $t1, $s0
    sb      $zero, 0($t3)
    addiu   $s0, $s0, 1
    b       .Lsf_zero

.Lsf_len:
    /* append the 64-bit big-endian bit length at offset 56 */
    la      $t0, sha_bitlen
    lw      $t1, 0($t0)             /* low */
    lw      $t2, 4($t0)             /* high */
    la      $t3, sha_buf
    srl     $t4, $t2, 24
    sb      $t4, 56($t3)
    srl     $t4, $t2, 16
    sb      $t4, 57($t3)
    srl     $t4, $t2, 8
    sb      $t4, 58($t3)
    sb      $t2, 59($t3)
    srl     $t4, $t1, 24
    sb      $t4, 60($t3)
    srl     $t4, $t1, 16
    sb      $t4, 61($t3)
    srl     $t4, $t1, 8
    sb      $t4, 62($t3)
    sb      $t1, 63($t3)

    la      $a0, sha_buf
    jal     sha256_block
    nop

    /* store state big-endian into sha_digest */
    la      $t0, sha_state
    la      $t1, sha_digest
    li      $t2, 8
.Lsf_out:
    lw      $t3, 0($t0)
    srl     $t4, $t3, 24
    sb      $t4, 0($t1)
    srl     $t4, $t3, 16
    sb      $t4, 1($t1)
    srl     $t4, $t3, 8
    sb      $t4, 2($t1)
    sb      $t3, 3($t1)
    addiu   $t0, $t0, 4
    addiu   $t1, $t1, 4
    addiu   $t2, $t2, -1
    bnez    $t2, .Lsf_out
    nop

    lw      $ra, 20($sp)
    lw      $s0, 16($sp)
    addiu   $sp, $sp, 24
    jr      $ra
    nop
    .end    sha256_final

/* sha256_block: a0 -> 64-byte block. Uses sha_w[64] in bss.
 *
 * The state update is the literal FIPS 180-4 definition:
 *   Ch(e,f,g)  = (e&f) ^ (~e&g)
 *   Maj(a,b,c) = (a&b) ^ (a&c) ^ (b&c)
 * written with instructions rather than adder chains, so there is no
 * carry-propagation subtlety to reason about.
 */
    .globl  sha256_block
    .ent    sha256_block
sha256_block:
    addiu   $sp, $sp, -48
    sw      $s0, 44($sp)
    sw      $s1, 40($sp)
    sw      $s2, 36($sp)            /* a */
    sw      $s3, 32($sp)            /* b */
    sw      $s4, 28($sp)            /* c */
    sw      $s5, 24($sp)            /* d */
    sw      $s6, 20($sp)            /* e */
    sw      $s7, 16($sp)            /* f */
    sw      $fp, 12($sp)            /* g */
    sw      $ra, 8($sp)             /* also holds h in the loop */

    la      $t0, sha_w
    move    $t1, $a0
    li      $t2, 16
.Lwb_load:
    lbu     $t3, 0($t1)
    lbu     $t4, 1($t1)
    lbu     $t5, 2($t1)
    lbu     $t6, 3($t1)
    sll     $t3, $t3, 24
    sll     $t4, $t4, 16
    sll     $t5, $t5, 8
    or      $t3, $t3, $t4
    or      $t3, $t3, $t5
    or      $t3, $t3, $t6
    sw      $t3, 0($t0)
    addiu   $t1, $t1, 4
    addiu   $t0, $t0, 4
    addiu   $t2, $t2, -1
    bnez    $t2, .Lwb_load
    nop

    /* message schedule: w[i] = s1(w[i-2]) + w[i-7] + s0(w[i-15]) + w[i-16] */
    la      $t0, sha_w
    addiu   $t0, $t0, 64             /* &w[16] */
    li      $t2, 48
.Lwb_sched:
    lw      $t3, -60($t0)            /* w[i-15] */
    /* s0 = ror7 ^ ror18 ^ shr3 */
    srl     $t4, $t3, 7
    sll     $t5, $t3, 25
    or      $t4, $t4, $t5
    srl     $t5, $t3, 18
    sll     $t6, $t3, 14
    or      $t5, $t5, $t6
    xor     $t4, $t4, $t5
    srl     $t5, $t3, 3
    xor     $t4, $t4, $t5
    lw      $t5, -8($t0)             /* w[i-2] */
    /* s1 = ror17 ^ ror19 ^ shr10 */
    srl     $t6, $t5, 17
    sll     $t7, $t5, 15
    or      $t6, $t6, $t7
    srl     $t7, $t5, 19
    sll     $t8, $t5, 13
    or      $t7, $t7, $t8
    xor     $t6, $t6, $t7
    srl     $t7, $t5, 10
    xor     $t6, $t6, $t7
    addu    $t4, $t4, $t6
    lw      $t5, -28($t0)            /* w[i-7] */
    addu    $t4, $t4, $t5
    lw      $t5, -64($t0)            /* w[i-16] */
    addu    $t4, $t4, $t5
    sw      $t4, 0($t0)
    addiu   $t0, $t0, 4
    addiu   $t2, $t2, -1
    bnez    $t2, .Lwb_sched
    nop

    /* load working vars */
    la      $t0, sha_state
    lw      $s2, 0($t0)              /* a */
    lw      $s3, 4($t0)              /* b */
    lw      $s4, 8($t0)              /* c */
    lw      $s5, 12($t0)             /* d */
    lw      $s6, 16($t0)             /* e */
    lw      $s7, 20($t0)             /* f */
    lw      $fp, 24($t0)             /* g */
    lw      $ra, 28($t0)             /* h */

    la      $t9, sha_w
    la      $t8, sha_k
    li      $t7, 64
.Lwb_round:
    /* S1 = ror6(e) ^ ror11(e) ^ ror25(e) */
    srl     $t0, $s6, 6
    sll     $t1, $s6, 26
    or      $t0, $t0, $t1
    srl     $t1, $s6, 11
    sll     $t2, $s6, 21
    or      $t1, $t1, $t2
    xor     $t0, $t0, $t1
    srl     $t1, $s6, 25
    sll     $t2, $s6, 7
    or      $t1, $t1, $t2
    xor     $t0, $t0, $t1            /* S1 */
    /* Ch = (e&f) ^ (~e&g) */
    and     $t1, $s6, $s7
    nor     $t2, $s6, $zero
    and     $t2, $t2, $fp
    xor     $t1, $t1, $t2            /* Ch */
    addu    $t0, $t0, $t1
    addu    $t0, $t0, $ra            /* + h */
    lw      $t1, 0($t8)              /* + K[i] */
    addu    $t0, $t0, $t1
    lw      $t1, 0($t9)              /* + w[i] */
    addu    $t0, $t0, $t1            /* t1 */
    /* S0 = ror2(a) ^ ror13(a) ^ ror22(a) */
    srl     $t1, $s2, 2
    sll     $t2, $s2, 30
    or      $t1, $t1, $t2
    srl     $t2, $s2, 13
    sll     $t3, $s2, 19
    or      $t2, $t2, $t3
    xor     $t1, $t1, $t2
    srl     $t2, $s2, 22
    sll     $t3, $s2, 10
    or      $t2, $t2, $t3
    xor     $t1, $t1, $t2            /* S0 */
    /* Maj = (a&b) ^ (a&c) ^ (b&c) */
    and     $t2, $s2, $s3
    and     $t3, $s2, $s4
    xor     $t2, $t2, $t3
    and     $t3, $s3, $s4
    xor     $t2, $t2, $t3            /* Maj */
    addu    $t1, $t1, $t2            /* t2 */
    /* rotate */
    move    $ra, $fp                 /* h = g */
    move    $fp, $s7                 /* g = f */
    move    $s7, $s6                 /* f = e */
    addu    $s6, $s5, $t0            /* e = d + t1 */
    move    $s5, $s4                 /* d = c */
    move    $s4, $s3                 /* c = b */
    move    $s3, $s2                 /* b = a */
    addu    $s2, $t0, $t1            /* a = t1 + t2 */

    addiu   $t9, $t9, 4
    addiu   $t8, $t8, 4
    addiu   $t7, $t7, -1
    bnez    $t7, .Lwb_round
    nop

    /* state += working vars */
    la      $t0, sha_state
    lw      $t1, 0($t0)
    addu    $t1, $t1, $s2
    sw      $t1, 0($t0)
    lw      $t1, 4($t0)
    addu    $t1, $t1, $s3
    sw      $t1, 4($t0)
    lw      $t1, 8($t0)
    addu    $t1, $t1, $s4
    sw      $t1, 8($t0)
    lw      $t1, 12($t0)
    addu    $t1, $t1, $s5
    sw      $t1, 12($t0)
    lw      $t1, 16($t0)
    addu    $t1, $t1, $s6
    sw      $t1, 16($t0)
    lw      $t1, 20($t0)
    addu    $t1, $t1, $s7
    sw      $t1, 20($t0)
    lw      $t1, 24($t0)
    addu    $t1, $t1, $fp
    sw      $t1, 24($t0)
    lw      $t1, 28($t0)
    addu    $t1, $t1, $ra
    sw      $t1, 28($t0)

    lw      $s0, 44($sp)
    lw      $s1, 40($sp)
    lw      $s2, 36($sp)
    lw      $s3, 32($sp)
    lw      $s4, 28($sp)
    lw      $s5, 24($sp)
    lw      $s6, 20($sp)
    lw      $s7, 16($sp)
    lw      $fp, 12($sp)
    lw      $ra, 8($sp)
    addiu   $sp, $sp, 48
    jr      $ra
    nop
    .end    sha256_block

/* ================================================================== *
 * Small I/O and string helpers
 * ================================================================== */

/* puts: a0 = NUL-terminated string -> stdout */
    .globl  puts
    .ent    puts
puts:
    addiu   $sp, $sp, -24
    sw      $ra, 20($sp)
    sw      $s0, 16($sp)
    move    $s0, $a0
    li      $t0, 0
1:
    addu    $t1, $s0, $t0
    lbu     $t2, 0($t1)
    beqz    $t2, 2f
    nop
    addiu   $t0, $t0, 1
    b       1b
    nop
2:
    beqz    $t0, 3f
    nop
    li      $a0, STDOUT
    move    $a1, $s0
    move    $a2, $t0
    jal     sys_write
    nop
3:
    lw      $ra, 20($sp)
    lw      $s0, 16($sp)
    addiu   $sp, $sp, 24
    jr      $ra
    nop
    .end    puts

/* puts_err: a0 = string -> stderr */
    .globl  puts_err
    .ent    puts_err
puts_err:
    addiu   $sp, $sp, -24
    sw      $ra, 20($sp)
    sw      $s0, 16($sp)
    move    $s0, $a0
    li      $t0, 0
1:
    addu    $t1, $s0, $t0
    lbu     $t2, 0($t1)
    beqz    $t2, 2f
    nop
    addiu   $t0, $t0, 1
    b       1b
    nop
2:
    beqz    $t0, 3f
    nop
    li      $a0, STDERR
    move    $a1, $s0
    move    $a2, $t0
    jal     sys_write
    nop
3:
    lw      $ra, 20($sp)
    lw      $s0, 16($sp)
    addiu   $sp, $sp, 24
    jr      $ra
    nop
    .end    puts_err

/* putnum: a0 = unsigned value, prints decimal + newline to stdout */
    .globl  putnum
    .ent    putnum
putnum:
    addiu   $sp, $sp, -32
    sw      $ra, 28($sp)
    move    $t0, $a0
    la      $t1, numbuf
    addiu   $t2, $t1, 19
    sb      $zero, 0($t2)
    li      $t3, 10
1:
    divu    $t0, $t3
    mflo    $t0
    mfhi    $t4
    addiu   $t4, $t4, 48
    addiu   $t2, $t2, -1
    sb      $t4, 0($t2)
    bnez    $t0, 1b
    nop
    /* print digits then a newline */
    li      $a0, STDOUT
    move    $a1, $t2
    la      $t4, numbuf
    addiu   $t4, $t4, 19
    subu    $a2, $t4, $t2
    jal     sys_write
    nop
    lw      $ra, 28($sp)
    addiu   $sp, $sp, 32
    jr      $ra
    nop
    .end    putnum

/* strcmp: a0, a1 -> v0 = 0 if equal */
    .globl  strcmp
    .ent    strcmp
strcmp:
1:
    lbu     $t0, 0($a0)
    lbu     $t1, 0($a1)
    bne     $t0, $t1, 2f
    nop
    beqz    $t0, 3f
    nop
    addiu   $a0, $a0, 1
    addiu   $a1, $a1, 1
    b       1b
    nop
2:
    li      $v0, 1
    jr      $ra
    nop
3:
    li      $v0, 0
    jr      $ra
    nop
    .end    strcmp

/* ---- raw syscalls ------------------------------------------------- */
    .globl  sys_write
    .ent    sys_write
sys_write:
    li      $v0, SYS_write
    syscall
    beqz    $a3, 1f
    nop
    subu    $v0, $zero, $v0
1:
    jr      $ra
    nop
    .end    sys_write

    .globl  sys_read
    .ent    sys_read
sys_read:
    li      $v0, SYS_read
    syscall
    beqz    $a3, 1f
    nop
    subu    $v0, $zero, $v0
1:
    jr      $ra
    nop
    .end    sys_read

    .globl  sys_open
    .ent    sys_open
sys_open:
    li      $v0, SYS_open
    syscall
    beqz    $a3, 1f
    nop
    subu    $v0, $zero, $v0
1:
    jr      $ra
    nop
    .end    sys_open

    .globl  sys_close
    .ent    sys_close
sys_close:
    li      $v0, SYS_close
    syscall
    beqz    $a3, 1f
    nop
    subu    $v0, $zero, $v0
1:
    jr      $ra
    nop
    .end    sys_close

    .globl  sys_exit
    .ent    sys_exit
sys_exit:
    li      $v0, SYS_exit
    syscall
    nop
    .end    sys_exit

/* ================================================================== *
 * Read-only data
 * ================================================================== */
    .rdata
    .align  4
sha_iv:
    .word   0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a
    .word   0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19

    .align  4
sha_k:
    .word   0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5
    .word   0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5
    .word   0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3
    .word   0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174
    .word   0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc
    .word   0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da
    .word   0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7
    .word   0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967
    .word   0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13
    .word   0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85
    .word   0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3
    .word   0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070
    .word   0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5
    .word   0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3
    .word   0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208
    .word   0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2

hexchars:
    .ascii  "0123456789abcdef"

str_c:          .asciz  "-c"
str_dot:        .asciz  "."
str_sp:         .asciz  "  "          /* coreutils sha256sum field separator */
str_nl:         .asciz  "\n"
msg_usage:      .asciz  "usage: jooan-sha256 <file> [...]\n"
msg_no_manifest:.asciz  "jooan-sha256: cannot open manifest\n"
msg_mismatch:   .asciz  "jooan-sha256: MISMATCH "
msg_want:       .asciz  "\n  want "
msg_got:        .asciz  "\n  got  "
msg_shortline:  .asciz  "jooan-sha256: malformed manifest line\n"
msg_failed:     .asciz  "jooan-sha256: verification FAILED\n"
msg_verified:   .asciz  "jooan-sha256: verified "
msg_entries:    .asciz  " entries\n"

/* ================================================================== *
 * Writable data
 * ================================================================== */
    .data
    .align  4
sha_state:      .space  32
sha_bitlen:     .space  8
sha_buflen:     .space  4
sha_buf:        .space  64
sha_w:          .space  256
sha_digest:     .space  32
outhex:         .space  80
outhex_argv_path: .space 4
wanthex:        .space  80
fullpath:       .space  8192
linebuf:        .space  4096
line_len:       .space  4
onebyte:        .space  4
numbuf:         .space  32
mf_stdin:       .asciz  "-"

    .bss
    .align  4
iobuf:          .space  65536
