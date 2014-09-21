#!/bin/sh

verbose=false
if [ "x$1" = "x-v" ]; then
    verbose=true
    out=/dev/stdout
    err=/dev/stderr
else
    out=/dev/null
    err=/dev/null
fi

## make & makeopts
if gmake --version > /dev/null 2>&1; then
    make=gmake;
else
    make=make;
fi

makeopts="--quiet --no-print-directory -j"

make_print() {
    echo `$make $makeopts print-$1`
}

## command tools
awk='awk'
bc='bc'
date='date'
grep='grep'
rm='rm -f'
sed='sed'

## symbol table
sym_table='obj/kernel.sym'

## gdb & gdbopts
gdb="$(make_print GDB)"
gdbport='1234'

gdb_in="$(make_print GRADE_GDB_IN)"

## qemu & qemuopts
qemu="$(make_print qemu)"

qemu_out="$(make_print GRADE_QEMU_OUT)"

if $qemu -nographic -help | grep -q '^-gdb'; then
    qemugdb="-gdb tcp::$gdbport"
else
    qemugdb="-s -p $gdbport"
fi

## default variables
default_timeout=30
default_pts=5

pts=5
part=0
part_pos=0
total=0
total_pos=0

## default functions
update_score() {
    total=`expr $total + $part`
    total_pos=`expr $total_pos + $part_pos`
    part=0
    part_pos=0
}

get_time() {
    echo `$date +%s.%N 2> /dev/null`
}

show_part() {
    echo "Part $1 Score: $part/$part_pos"
    echo
    update_score
}

show_final() {
    update_score
    echo "Total Score: $total/$total_pos"
    if [ $total -lt $total_pos ]; then
        exit 1
    fi
}

show_time() {
    t1=$(get_time)
    time=`echo "scale=1; ($t1-$t0)/1" | $sed 's/.N/.0/g' | $bc 2> /dev/null`
    echo "(${time}s)"
}

show_build_tag() {
    echo "$1:" | $awk '{printf "%-24s ", $0}'
}

show_check_tag() {
    echo "$1:" | $awk '{printf "  -%-40s  ", $0}'
}

show_msg() {
    echo $1
    shift
    if [ $# -gt 0 ]; then
        echo "$@" | awk '{printf "   %s\n", $0}'
        echo
    fi
}

pass() {
    show_msg OK "$@"
    part=`expr $part + $pts`
    part_pos=`expr $part_pos + $pts`
}

fail() {
    show_msg WRONG "$@"
    part_pos=`expr $part_pos + $pts`
}

run_qemu() {
    # Run qemu with serial output redirected to $qemu_out. If $brkfun is non-empty,
    # wait until $brkfun is reached or $timeout expires, then kill QEMU
    qemuextra=
    if [ "$brkfun" ]; then
        qemuextra="-S $qemugdb"
    fi

    if [ -z "$timeout" ] || [ $timeout -le 0 ]; then
        timeout=$default_timeout;
    fi

    t0=$(get_time)
    (
        ulimit -t $timeout
        exec $qemu -nographic $qemuopts -serial file:$qemu_out -monitor null -no-reboot $qemuextra
    ) > $out 2> $err &
    pid=$!

    # wait for QEMU to start
    sleep 1

    if [ -n "$brkfun" ]; then
        # find the address of the kernel $brkfun function
        brkaddr=`$grep " $brkfun\$" $sym_table | $sed -e's/ .*$//g'`
        (
            echo "target remote localhost:$gdbport"
            echo "break *0x$brkaddr"
            echo "continue"
        ) > $gdb_in

        $gdb -batch -nx -x $gdb_in > /dev/null 2>&1

        # make sure that QEMU is dead
        # on OS X, exiting gdb doesn't always exit qemu
        kill $pid > /dev/null 2>&1
    fi
}

build_run() {
    # usage: build_run <tag> <args>
    show_build_tag "$1"
    shift

    if $verbose; then
        echo "$make $@ ..."
    fi
    $make $makeopts $@ 'DEFS+=-DDEBUG_GRADE' > $out 2> $err

    if [ $? -ne 0 ]; then
        echo $make $@ failed
        exit 1
    fi

    # now run qemu and save the output
    run_qemu

    show_time
}

check_result() {
    # usage: check_result <tag> <check> <check args...>
    show_check_tag "$1"
    shift

    # give qemu some time to run (for asynchronous mode)
    if [ ! -s $qemu_out ]; then
        sleep 4
    fi

    if [ ! -s $qemu_out ]; then
        fail > /dev/null
        echo 'no $qemu_out'
    else
        check=$1
        shift
        $check "$@"
    fi
}

check_regexps() {
    okay=yes
    not=0
    reg=0
    error=
    for i do
        if [ "x$i" = "x!" ]; then
            not=1
        elif [ "x$i" = "x-" ]; then
            reg=1
        else
            if [ $reg -ne 0 ]; then
                $grep '-E' "^$i\$" $qemu_out > /dev/null
            else
                $grep '-F' "$i" $qemu_out > /dev/null
            fi
            found=$(($? == 0))
            if [ $found -eq $not ]; then
                if [ $found -eq 0 ]; then
                    msg="!! error: missing '$i'"
                else
                    msg="!! error: got unexpected line '$i'"
                fi
                okay=no
                if [ -z "$error" ]; then
                    error="$msg"
                else
                    error="$error\n$msg"
                fi
            fi
            not=0
            reg=0
        fi
    done
    if [ "$okay" = "yes" ]; then
        pass
    else
        fail "$error"
        if $verbose; then
            exit 1
        fi
    fi
}

run_test() {
    # usage: run_test [-tag <tag>] [-prog <prog>] [-Ddef...] [-check <check>] checkargs ...
    tag=
    prog=
    check=check_regexps
    while true; do
        select=
        case $1 in
            -tag|-prog)
                select=`expr substr $1 2 ${#1}`
                eval $select='$2'
                ;;
        esac
        if [ -z "$select" ]; then
            break
        fi
        shift
        shift
    done
    defs=
    while expr "x$1" : "x-D.*" > /dev/null; do
        defs="DEFS+='$1' $defs"
        shift
    done
    if [ "x$1" = "x-check" ]; then
        check=$2
        shift
        shift
    fi

    if [ -z "$prog" ]; then
        $make $makeopts touch > /dev/null 2>&1
        args="$defs"
    else
        if [ -z "$tag" ]; then
            tag="$prog"
        fi
        args="build-$prog $defs"
    fi

    build_run "$tag" "$args"

    check_result 'check result' "$check" "$@"
}

quick_run() {
    # usage: quick_run <tag> [-Ddef...]
    tag="$1"
    shift
    defs=
    while expr "x$1" : "x-D.*" > /dev/null; do
        defs="DEFS+='$1' $defs"
        shift
    done

    $make $makeopts touch > /dev/null 2>&1
    build_run "$tag" "$defs"
}

quick_check() {
    # usage: quick_check <tag> checkargs ...
    tag="$1"
    shift
    check_result "$tag" check_regexps "$@"
}

## kernel image
osimg=$(make_print ucoreimg)

## swap image
swapimg=$(make_print swapimg)

## set default qemu-options
qemuopts="-hda $osimg -drive file=$swapimg,media=disk,cache=writeback"

## set break-function, default is readline
brkfun=readline

default_check() {
    pts=3
    check_regexps "$@"

    pts=2
    quick_check 'check output'                                  \
        'check_alloc_page() succeeded!'                         \
        'check_pgdir() succeeded!'                              \
        'check_boot_pgdir() succeeded!'                         \
        'check_slab() succeeded!'                               \
        'check_vma_struct() succeeded!'                         \
        'check_pgfault() succeeded!'                            \
        'check_vmm() succeeded.'                                \
        'check_swap() succeeded.'                               \
        'check_mm_swap: step1, mm_map ok.'                      \
        'check_mm_swap: step2, mm_unmap ok.'                    \
        'check_mm_swap: step3, exit_mmap ok.'                   \
        'check_mm_swap: step4, dup_mmap ok.'                    \
        'check_mm_swap() succeeded.'                            \
        'check_mm_shm_swap: step1, share memory ok.'            \
        'check_mm_shm_swap: step2, dup_mmap ok.'                \
        'check_mm_shm_swap() succeeded.'                        \
        '++ setup timer interrupts'
}

## check now!!

run_test -prog 'brkfreetest' -check default_check               \
        'kernel_execve: pid = 3, name = "brkfreetest".'         \
        'page fault!!'                                          \
        '  trap 0x0000000e Page Fault'                          \
        '  err  0x00000006'                                     \
      - '  eip  0x008.....'                                     \
      - '  esp  0xaff.....'                                     \
        'killed by kernel.'                                     \
        'brkfreetest pass.'                                     \
        'all user-mode processes have quit.'                    \
        'init check memory pass.'                               \
    ! - 'user panic at .*'

run_test -prog 'brktest' -check default_check                   \
        'kernel_execve: pid = 3, name = "brktest".'             \
        'I am going to eat out all the mem, MU HA HA!!.'        \
        'I ate 5000 slots.'                                     \
        'I ate 10000 slots.'                                    \
        'I ate (at least) 41000000 byte memory.'                \
        'I free all the memory.(0)'                             \
        'brktest pass.'                                         \
        'all user-mode processes have quit.'                    \
        'init check memory pass.'

pts=5
timeout=240

run_test -prog 'sleep'                                          \
        'kernel_execve: pid = 3, name = "sleep".'               \
        'I am child and I will eat out all the memory.'         \
        'I ate 1000 slots.'                                     \
        'I ate 5000 slots.'                                     \
        'sleep 1 x 100 slices.'                                 \
        'sleep 3 x 100 slices.'                                 \
        'sleep 7 x 100 slices.'                                 \
        'sleep 10 x 100 slices.'                                \
      - 'use 1... msecs.'                                       \
        'sleep pass.'                                           \
        'all user-mode processes have quit.'                    \
        'init check memory pass.'                               \
    !   '  trap 0x0000000e Page Fault'                          \
    !   'killed by kernel.'                                     \
    ! - 'user panic at .*'

run_test -prog 'sleepkill'                                      \
        'kernel_execve: pid = 3, name = "sleepkill".'           \
        'sleepkill pass.'                                       \
        'all user-mode processes have quit.'                    \
        'init check memory pass.'                               \
    ! - 'user panic at .*'

pts=10
timeout=

run_test -prog 'mmaptest'                                       \
        'kernel_execve: pid = 3, name = "mmaptest".'            \
        'mmap step1 ok.'                                        \
        'munmap step1 ok.'                                      \
        'mmap step2 ok.'                                        \
        'mmap step3 ok.'                                        \
        'mumap step2 ok.'                                       \
        'mmaptest pass.'                                        \
        'all user-mode processes have quit.'                    \
        'init check memory pass.'                               \
    ! - 'user panic at .*'

run_test -prog 'shmemtest'                                      \
        'kernel_execve: pid = 3, name = "shmemtest".'           \
        'shmemtest pass.'                                       \
        'all user-mode processes have quit.'                    \
        'init check memory pass.'                               \
    ! - 'user panic at .*'

show_part A

pts=15

run_test -prog 'threadtest'                                     \
        'kernel_execve: pid = 3, name = "threadtest".'          \
        'thread ok.'                                            \
        'child ok.'                                             \
        'threadtest pass.'                                      \
        'all user-mode processes have quit.'                    \
        'init check memory pass.'                               \
    ! - 'user panic at .*'

run_test -prog 'threadfork'                                     \
        'kernel_execve: pid = 3, name = "threadfork".'          \
        'threadfork pass.'                                      \
        'all user-mode processes have quit.'                    \
        'init check memory pass.'                               \
    ! - 'user panic at .*'

run_test -prog 'threadwork'                                     \
        'kernel_execve: pid = 3, name = "threadwork".'          \
        'thread ok.'                                            \
      - 'i am 00, .., i got ........'                           \
      - 'i am 07, .., i got ........'                           \
      - 'i am 13, .., i got ........'                           \
      - 'i am 19, .., i got ........'                           \
      - 'i am 23, .., i got ........'                           \
      - 'i am 29, .., i got ........'                           \
      - 'i am 37, .., i got ........'                           \
      - 'i am 43, .., i got ........'                           \
      - 'i am 46, .., i got ........'                           \
      - '  |-- PTE(00001) afd.....-afe..... 00001000 urw'       \
      - '  |-- PTE(00001) afe.....-aff..... 00001000 urw'       \
      - '  |-- PTE(00001) aff.....-b00..... 00001000 urw'       \
        'thread wait ok.'                                       \
        'loop init ok.'                                         \
        'threadwork pass.'                                      \
        'all user-mode processes have quit.'                    \
        'init check memory pass.'                               \
    ! - 'user panic at .*'

run_test -prog 'threadgroup1'                                   \
        'kernel_execve: pid = 3, name = "threadgroup1".'        \
        'thread ok.'                                            \
        'yield 0.'                                              \
        'yield 1.'                                              \
        'yield 2.'                                              \
        'exit thread group now.'                                \
        'all user-mode processes have quit.'                    \
        'init check memory pass.'                               \
    ! - 'user panic at .*'

run_test -prog 'threadgroup2'                                   \
        'kernel_execve: pid = 3, name = "threadgroup2".'        \
        'thread ok.'                                            \
        'yield 0.'                                              \
        'yield 1.'                                              \
        'yield 2.'                                              \
        'exit thread group now.'                                \
        'all user-mode processes have quit.'                    \
        'init check memory pass.'                               \
    ! - 'user panic at .*'

pts=20

run_test -prog 'buggy_wait'                                     \
        'kernel_execve: pid = 3, name = "buggy_wait".'          \
        'child munmap ok.'                                      \
        'buggy_wait pass.'                                      \
        'all user-mode processes have quit.'                    \
        'init check memory pass.'                               \
    ! - 'user panic at .*'

run_test -prog 'buggy_wait2'                                    \
        'kernel_execve: pid = 3, name = "buggy_wait2".'         \
        'child fork ok.'                                        \
        'buggy_wait2 pass.'                                     \
        'all user-mode processes have quit.'                    \
        'init check memory pass.'                               \
    ! - 'user panic at .*'

pts=25
timeout=500

run_test -prog 'primer'                                         \
        'kernel_execve: pid = 3, name = "primer".'              \
        'sharemem init ok.'                                     \
        '5 is a primer.'                                        \
        '71 is a primer.'                                       \
        '223 is a primer.'                                      \
        '409 is a primer.'                                      \
        '601 is a primer.'                                      \
        '881 is a primer.'                                      \
        '1163 is a primer.'                                     \
        '1451 is a primer.'                                     \
        '1733 is a primer.'                                     \
        '2069 is a primer.'                                     \
        '2383 is a primer.'                                     \
        '2729 is a primer.'                                     \
        '3079 is a primer.'                                     \
        '3413 is a primer.'                                     \
        '3767 is a primer.'                                     \
        '4219 is a primer.'                                     \
        '4561 is a primer.'                                     \
        '4937 is a primer.'                                     \
        '5387 is a primer.'                                     \
        '5779 is a primer.'                                     \
        '6247 is a primer.'                                     \
        '6659 is a primer.'                                     \
        '7069 is a primer.'                                     \
        '7529 is a primer.'                                     \
      - '...... 7 quit.'                                        \
      - '...... 23 quit.'                                       \
      - '...... 43 quit.'                                       \
      - '...... 67 quit.'                                       \
      - '...... 89 quit.'                                       \
      - '...... 109 quit.'                                      \
      - '...... 139 quit.'                                      \
      - '...... 167 quit.'                                      \
      - '...... 193 quit.'                                      \
      - '...... 227 quit.'                                      \
      - '...... 251 quit.'                                      \
      - '...... 277 quit.'                                      \
      - '...... 311 quit.'                                      \
      - '...... 347 quit.'                                      \
      - '...... 373 quit.'                                      \
      - '...... 401 quit.'                                      \
      - '...... 433 quit.'                                      \
      - '...... 461 quit.'                                      \
      - '...... 491 quit.'                                      \
      - '...... 523 quit.'                                      \
      - '...... 569 quit.'                                      \
      - '...... 599 quit.'                                      \
      - '...... 619 quit.'                                      \
      - '...... 653 quit.'                                      \
      - '...... 683 quit.'                                      \
      - '...... 727 quit.'                                      \
      - '...... 757 quit.'                                      \
      - '...... 797 quit.'                                      \
      - '...... 827 quit.'                                      \
      - '...... 859 quit.'                                      \
      - '...... 887 quit.'                                      \
      - '...... 937 quit.'                                      \
      - '...... 971 quit.'                                      \
        'primer pass.'                                          \
        'all user-mode processes have quit.'                    \
        'init check memory pass.'                               \
    ! - 'user panic at .*'

show_part B

## print final-score
show_final

