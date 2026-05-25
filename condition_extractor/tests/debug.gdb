set pagination off
set print thread-events off
set breakpoint pending on

# Stop on fatal signals BEFORE Catch2's signal handler swallows them.
# `nopass` keeps the signal from being delivered, so we stay stopped at the
# faulting frame instead of bouncing into Catch2's handler / abort.
handle SIGABRT stop print nopass
handle SIGSEGV stop print nopass

# Auto-print a backtrace whenever we stop.
define hook-stop
  bt 30
end

# Run the test inline (no fork) so gdb stays with the test process throughout.
set environment LIBERATOR_TEST_NO_FORK=1

# IMPORTANT: do NOT set follow-fork-mode child here. The test no longer forks
# itself, but it still calls system("dot -Tpng ...") which fork+execs a `dot`
# subprocess. Following that child loses our debugging context.
set follow-fork-mode parent
set detach-on-fork on

#break my_proc
break AccessType.cpp:1483

run "svf test classes"
