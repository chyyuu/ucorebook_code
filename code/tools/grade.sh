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

## fs image
fsimg=$(make_print fsimg)
fsroot=$(make_print sfsroot)

## set default qemu-options
qemuopts="-hda $osimg -drive file=$swapimg,media=disk,cache=writeback -drive file=$fsimg,media=disk,cache=writeback"

## set break-function, default is readline
brkfun=readline

default_check() {
    pts=10
    check_regexps "$@"

    pts=10
    quick_check 'check output'                                          \
        'check_alloc_page() succeeded!'                                 \
        'check_pgdir() succeeded!'                                      \
        'check_boot_pgdir() succeeded!'                                 \
        'check_slab() succeeded!'                                       \
        'check_vma_struct() succeeded!'                                 \
        'check_pgfault() succeeded!'                                    \
        'check_vmm() succeeded.'                                        \
        'check_swap() succeeded.'                                       \
        'check_mm_swap: step1, mm_map ok.'                              \
        'check_mm_swap: step2, mm_unmap ok.'                            \
        'check_mm_swap: step3, exit_mmap ok.'                           \
        'check_mm_swap: step4, dup_mmap ok.'                            \
        'check_mm_swap() succeeded.'                                    \
        'check_mm_shm_swap: step1, share memory ok.'                    \
        'check_mm_shm_swap: step2, dup_mmap ok.'                        \
        'check_mm_shm_swap() succeeded.'                                \
        'vfs: mount disk0.'                                             \
        '++ setup timer interrupts'
}

## check now!!

run_test -prog 'hello2' -check default_check                            \
        'kernel_execve: pid = 3, name = "hello2".'                      \
        'Hello world!!.'                                                \
        'I am process 3.'                                               \
        'hello2 pass.'                                                  \
        'all user-mode processes have quit.'                            \
        'init check memory pass.'                                       \
    ! - 'user panic at .*'

run_test -prog 'fwrite_test' -check default_check                       \
        'kernel_execve: pid = 3, name = "fwrite_test".'                 \
        'Hello world!!.'                                                \
        'I am process 3.'                                               \
        'dup fd ok.'                                                    \
        'fork fd ok.'                                                   \
        'fwrite_test pass.'                                             \
        'all user-mode processes have quit.'                            \
        'init check memory pass.'                                       \
    ! - 'user panic at .*'

run_test -prog 'fread_test2' -check default_check                       \
        'kernel_execve: pid = 3, name = "fread_test2".'                 \
        'fread_test2 pass.'                                             \
        'all user-mode processes have quit.'                            \
        'init check memory pass.'                                       \
    ! - 'user panic at .*'

show_part A

pts=30
timeout=300

run_test -prog 'sfs_filetest1'                                          \
        'kernel_execve: pid = 3, name = "sfs_filetest1".'               \
        'init_data ok.'                                                 \
        'random_test ok.'                                               \
        'sfs_filetest1 pass.'                                           \
        'all user-mode processes have quit.'                            \
        'init check memory pass.'                                       \
    ! - 'user panic at .*'

run_test -prog 'sfs_filetest2'                                          \
        'kernel_execve: pid = 3, name = "sfs_filetest2".'               \
        'sfs_filetest2 pass.'                                           \
        'all user-mode processes have quit.'                            \
        'init check memory pass.'                                       \
    ! - 'user panic at .*'

run_test -prog 'sfs_dirtest1'                                           \
        'kernel_execve: pid = 3, name = "sfs_dirtest1".'                \
        '0: current: disk0:/'                                           \
        '1: current: disk0:/'                                           \
        '2: current: disk0:/home/'                                      \
      - '2: d   2   ....        512  .'                                 \
      - '2: d   6   ....       1536  ..'                                \
        '3: current: disk0:/testman/'                                   \
      - '3: d   3   ....       2560  .'                                 \
      - '3: d   6   ....       1536  ..'                                \
        '3: -   1     21      83153  awk'                               \
        '3: d   2      5       1792  coreutils'                         \
        '3: -   1      8      31690  cpp'                               \
        '3: -   1    100     408495  gcc'                               \
        '3: -   1      3       8341  gdb'                               \
        '3: -   1     12      46254  ld'                                \
        '3: -   1      3      10371  sed'                               \
        '3: -   1      5      17354  zsh'                               \
        '4: current: disk0:/testman/coreutils/'                         \
      - '4: d   2   ....       1792  .'                                 \
      - '4: d   3   ....       2560  ..'                                \
        '4: -   1      1       2115  cat'                               \
        '4: -   1      2       5338  cp'                                \
        '4: -   1      2       7487  ls'                                \
        '4: -   1      1       3024  mv'                                \
        '4: -   1      1       3676  rm'                                \
        '5: current: disk0:/testman/'                                   \
      - '5: d   3   ....       2560  .'                                 \
      - '5: d   6   ....       1536  ..'                                \
        '6: current: disk0:/'                                           \
      - '6: d   6   ....       1536  .'                                 \
      - '6: d   6   ....       1536  ..'                                \
      - '6: d   2   ....     ......  bin'                               \
        '6: d   2      0        512  home'                              \
        '6: d   2      1        768  test'                              \
        '6: d   3      8       2560  testman'                           \
        'sfs_dirtest1 pass.'                                            \
        'all user-mode processes have quit.'                            \
        'init check memory pass.'                                       \
    ! - 'user panic at .*'

show_part B

run_test -prog 'sfs_filetest3'                                          \
        'kernel_execve: pid = 3, name = "sfs_filetest3".'               \
        '0: -   1      0          0  testfile'                          \
        '1: -   2      1         14  testfile'                          \
        '1: -   2      1         14  orz'                               \
        'link test ok.'                                                 \
        '2: -   1      1         14  testfile'                          \
        'unlink test ok.'                                               \
        '3: -   1      0          0  testfile'                          \
    ! - '2: .......................  orz'                               \
        'sfs_filetest3 pass.'                                           \
        'all user-mode processes have quit.'                            \
        'init check memory pass.'                                       \
    ! - 'user panic at .*'

run_test -prog 'sfs_dirtest2'                                           \
        'kernel_execve: pid = 3, name = "sfs_dirtest2".'                \
        '0: current: disk0:/test/'                                      \
      - '0: d   2   ....        768  .'                                 \
      - '0: d   6   ....       1536  ..'                                \
        '0: -   1      0          0  testfile'                          \
        '1: current: disk0:/test/'                                      \
      - '1: d   3   ....       1280  .'                                 \
      - '1: d   3   ....        768  dir0'                              \
        '1: -   1      0          0  file1'                             \
        '2: current: disk0:/test/'                                      \
      - '2: d   3   ....       1280  .'                                 \
      - '2: d   3   ....        768  dir0'                              \
        '2: -   2      0          0  file1'                             \
        '3: current: disk0:/test/dir0/dir1/'                            \
      - '3: d   2   ....        768  .'                                 \
      - '3: d   3   ....        768  ..'                                \
        '3: -   2      0          0  file2'                             \
        '4: current: disk0:/test/dir0/'                                 \
      - '4: d   2   ....        512  .'                                 \
      - '4: d   3   ....       1280  ..'                                \
        '5: current: disk0:/test/'                                      \
      - '5: d   2   ....        768  .'                                 \
      - '5: d   6   ....       1536  ..'                                \
        'sfs_dirtest2 pass.'                                            \
        'all user-mode processes have quit.'                            \
        'init check memory pass.'                                       \
    ! - 'user panic at .*'

run_test -prog 'sfs_dirtest3'                                           \
        'kernel_execve: pid = 3, name = "sfs_dirtest3".'                \
        '0: current: disk0:/test/'                                      \
      - '0: d   2   ....        768  .'                                 \
      - '0: d   6   ....       1536  ..'                                \
        '0: -   1      0          0  testfile'                          \
        '1: current: disk0:/test/dir0/dir1/'                            \
      - '1: d   2   ....        512  .'                                 \
      - '1: d   3   ....       1024  ..'                                \
        '2: current: disk0:/test/dir0/dir1/'                            \
      - '2: d   2   ....        768  .'                                 \
      - '2: d   3   ....        768  ..'                                \
        '2: -   1      1         28  file2'                             \
        '3: current: disk0:/test/'                                      \
      - '3: d   4   ....       1280  .'                                 \
      - '3: d   6   ....       1536  ..'                                \
      - '3: d   2   ....        768  dir2'                              \
      - '3: d   2   ....        512  dir0'                              \
        '4: current: disk0:/test/'                                      \
      - '4: d   2   ....        768  .'                                 \
      - '4: d   6   ....       1536  ..'                                \
        'sfs_dirtest3 pass.'                                            \
        'all user-mode processes have quit.'                            \
        'init check memory pass.'                                       \
    ! - 'user panic at .*'

show_part C

run_test -prog 'sfs_exectest1'                                          \
        'kernel_execve: pid = 3, name = "sfs_exectest1".'               \
        '00: Hello world!!.'                                            \
        '01: Hello world!!.'                                            \
        '03: Hello world!!.'                                            \
        '05: Hello world!!.'                                            \
        '07: Hello world!!.'                                            \
        '09: Hello world!!.'                                            \
        '11: Hello world!!.'                                            \
        '13: Hello world!!.'                                            \
        '15: Hello world!!.'                                            \
        '17: Hello world!!.'                                            \
        '19: Hello world!!.'                                            \
        '21: Hello world!!.'                                            \
        '23: Hello world!!.'                                            \
        '25: Hello world!!.'                                            \
        '27: Hello world!!.'                                            \
        '29: Hello world!!.'                                            \
        '31: Hello world!!.'                                            \
        'sfs_exectest1 pass.'                                           \
        'all user-mode processes have quit.'                            \
        'init check memory pass.'                                       \
    ! - 'user panic at .*'

show_part D

## print final-score
show_final

