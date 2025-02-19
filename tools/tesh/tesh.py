#! @PYTHON_EXECUTABLE@
# -*- coding: utf-8 -*-
"""

tesh -- testing shell
========================

Copyright (c) 2012-2024. The SimGrid Team. All rights reserved.

This program is free software; you can redistribute it and/or modify it
under the terms of the license (GNU LGPL) which comes with this package.

#TODO: child of child of child that printfs. Does it work?
#TODO: a child dies after its parent. What happen?

#TODO: regular expression in output
#ex: >> Time taken: [0-9]+s
#TODO: linked regular expression in output
#ex:
# >> Bytes sent: ([0-9]+)
# >> Bytes recv: \1
# then, even better:
# ! expect (\1 > 500)

"""

import sys
import errno
import os
import shlex
import re
import difflib
import signal
import argparse
import time

if sys.version_info[0] == 3:
    import subprocess
    import _thread
else:
    raise RuntimeError("This program is expected to run with Python3 only")

##############
#
# Utilities
#
#

# Singleton metaclass that works in Python 2 & 3
# http://stackoverflow.com/questions/6760685/creating-a-singleton-in-python

class _Singleton(type):
    """ A metaclass that creates a Singleton base class when called. """
    _instances = {}

    def __call__(cls, *args, **kwargs):
        if cls not in cls._instances:
            cls._instances[cls] = super(_Singleton, cls).__call__(*args, **kwargs)
        return cls._instances[cls]

class Singleton(_Singleton('SingletonMeta', (object,), {})):
    """ The Singleton base class """
    # pass

SIGNALS_TO_NAMES_DICT = dict((getattr(signal, n), n)
                             for n in dir(signal) if n.startswith('SIG') and '_' not in n)

def tesh_exit(errcode):
    """ Exit correctly """
    # If you do not flush some prints are skipped
    sys.stdout.flush()
    # os._exit exit even when executed within a thread
    # pylint: disable=protected-access
    os._exit(errcode)


def fatal_error(msg):
    """ Exit with error """
    print("[Tesh/CRITICAL] " + str(msg))
    tesh_exit(1)

# retrocompatibility: support ${aaa:=.} variable format
def replace_perl_variables(arg):
    vname = arg.group(1)
    vdefault = arg.group(2)
    if vname in os.environ:
        return "$" + vname
    return vdefault

def setenv(arg):
    """
    Set an environment variable.
    arg must be a string with the format "variable=value"
    """
    if '$' in arg:
        arg = re.sub(r"\${(\w+):=([^}]*)}", replace_perl_variables, arg)
        arg = expandvars2(arg)
    (var, val) = arg.split("=", 1)
    val = val.strip('"') # Remove the "" around the value (but not the ones within the string)
    print("[Tesh/INFO] setenv " + var + "=" + val)
    os.environ[var] = val
    # os.putenv(var, val) does not work
    # see http://stackoverflow.com/questions/17705419/python-os-environ-os-putenv-usr-bin-env


def expandvars2(path):
    """ http://stackoverflow.com/questions/30734967/how-to-expand-environment-variables-in-python-as-bash-does """
    return re.sub(r'(?<!\\)\$[A-Za-z_][A-Za-z0-9_]*', '', os.path.expandvars(path))

##############
#
# Cleanup on signal
#
#

def process_is_dead(pid):
    """ Tests whether the process is dead already """
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return True
    except OSError as err:
        if err.errno == errno.ESRCH: # ESRCH == No such process. The process is now dead
            return True
    return False

def kill_process_group(pid):
    """ This function send TERM signal + KILL signal after 0.2s to the group of the specified process """
    if pid is None:
        # Nobody to kill. We don't have anyone to kill on signal handler
        return

    try:
        pgid = os.getpgid(pid)
    except OSError:
        # os.getpgid failed. Ok, don't cleanup.
        return

    try:
        os.killpg(pgid, signal.SIGTERM)
        if process_is_dead(pid):
            return
        time.sleep(0.2)
        os.killpg(pgid, signal.SIGKILL)
    except OSError:
        # os.killpg failed. OK. Some subprocesses may still be running.
        pass

def signal_handler(signo, _frame):
    """ Signal handler """
    print("Caught signal {}".format(SIGNALS_TO_NAMES_DICT[signo]))
    running_pids = TeshState().running_pids # Just in case of interthread conflicts.
    for pid in running_pids:
        kill_process_group(pid)
    TeshState().running_pids.clear()
    tesh_exit(5)


##############
#
# Classes
#
#


class FileReader(Singleton):
    """ Read file line per line (and concat line that ends with "\") """
    def __init__(self, filename=None):
        if filename is None:
            self.filename = "(stdin)"
            self.fileno = sys.stdin
        else:
            self.filename_raw = filename
            self.filename = os.path.basename(filename)
            self.abspath = os.path.abspath(filename)
            self.fileno = open(self.filename_raw)

        self.linenumber = 0

    def __repr__(self):
        return self.filename + ":" + str(self.linenumber)

    def readfullline(self):
        """ Read a full line """
        try:
            line = next(self.fileno)
            self.linenumber += 1
        except StopIteration:
            return None
        if line[-1] == "\n":
            txt = line[0:-1]
        else:
            txt = line
        while len(line) > 1 and line[-2] == "\\":
            txt = txt[0:-1]
            line = next(self.fileno)
            self.linenumber += 1
            txt += line[0:-1]
        return txt


class TeshState(Singleton):
    """ Keep the state of tesh (mostly configuration values) """
    def __init__(self):
        self.running_pids = list() # stores which process group should be killed (or None otherwise)
        self.threads = []
        self.args_suffix = ""
        self.ignore_regexps_common = []
        self.jenkins = False  # not a Jenkins run by default
        self.auto_valgrind = True
        self.timeout = 10  # default value: 10 sec
        self.wrapper = None
        self.keep = False
        self.return_code = 0

    def add_thread(self, thread):
        """ Add another thread to wait for """
        self.threads.append(thread)

    def join_all_threads(self):
        """ Wait for all threads """
        for thread in self.threads:
            thread.acquire()
            thread.release()

    def set_return_code(self, value):
        """ Set exit status """
        if value > self.return_code:
            self.return_code = value


class Cmd:
    """ Command line object """
    def __init__(self):
        self.input_pipe = []
        self.output_pipe_stdout = []
        self.output_pipe_stderr = []
        self.timeout = TeshState().timeout
        self.args = None
        self.linenumber = -1

        self.background = False
        # Python threads loose the cwd
        self.cwd = os.getcwd()

        self.ignore_output = False
        self.expect_return = [0]

        self.output_display = False

        self.sort = -1
        self.rerun_with_valgrind = False

        self.ignore_regexps = TeshState().ignore_regexps_common

    def add_input_pipe(self, line):
        """ Add a line to stdin input """
        self.input_pipe.append(line)

    def add_output_pipe_stdout(self, line):
        """ Add a line to stdout output """
        self.output_pipe_stdout.append(line)

    def add_output_pipe_stderr(self, line):
        """ Add a line to stderr output """
        self.output_pipe_stderr.append(line)

    def set_cmd(self, args, linenumber):
        """ Set command line """
        self.args = args
        self.linenumber = linenumber

    def add_ignore(self, txt):
        """ Add regexp to ignore lines """
        self.ignore_regexps.append(re.compile(txt))

    def remove_ignored_lines(self, lines):
        """ Remove ignored lines """
        for ign in self.ignore_regexps:
            lines = [l for l in lines if not ign.match(l)]
        return lines

    def _cmd_mkfile(self, argline):
        filename = argline[len("mkfile "):]
        file = open(filename, "w")
        if file is None:
            fatal_error("Unable to create file " + filename)
        file.write("\n".join(self.input_pipe))
        file.write("\n")
        file.close()

    def _cmd_cd(self, argline): # pylint: disable=no-self-use
        args = shlex.split(argline)
        if len(args) != 2:
            fatal_error("Too many arguments to cd")
        try:
            os.chdir(args[1])
            print("[Tesh/INFO] change directory to " + args[1])
        except FileNotFoundError:
            print("Chdir to " + args[1] + " failed: No such file or directory")
            print("Test suite `" + FileReader().filename + "': NOK (system error)")
            tesh_exit(4)

    def run_if_possible(self):
        """
        Run the Cmd if possible.
        Return False if nothing has been ran.
        """
        if not self.can_run():
            return False
        if self.background:
            lock = _thread.allocate_lock()
            lock.acquire()
            TeshState().add_thread(lock)
            _thread.start_new_thread(Cmd._run, (self, lock))
        else:
            self._run()
            if self.rerun_with_valgrind and TeshState().auto_valgrind:
                print('\n\n\nXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX')
                print(      'XXXXXXXXX Rerunning this test with valgrind to help debugging it XXXXXXXXX')
                print(      'XXXXXXXX (this will fail if valgrind is not installed, of course) XXXXXXXX')
                print(      'XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX\n\n\n')

                self.args = "valgrind " + self.args
                self._run()
        return True


    def _run(self, lock=None):
        # Python threads loose the cwd
        os.chdir(self.cwd)

        self.args = re.sub(r"\${(\w+):=([^}]*)}", replace_perl_variables, self.args)

        # replace bash environment variables ($THINGS) to their values
        self.args = expandvars2(self.args)

        if re.match("^mkfile ", self.args) is not None:
            self._cmd_mkfile(self.args)
            if lock is not None:
                lock.release()
            return

        if re.match("^cd ", self.args) is not None:
            self._cmd_cd(self.args)
            if lock is not None:
                lock.release()
            return

        if TeshState().wrapper is not None:
            self.timeout *= 20
            self.args = TeshState().wrapper + self.args
        elif re.match(".*smpirun.*", self.args) is not None:
            self.args = "sh " + self.args
        if TeshState().jenkins and self.timeout is not None:
            self.timeout *= 10

        self.args += TeshState().args_suffix

        logs = list()
        msg = "[{file}:{number}] {args}".format(file=FileReader().filename, number=self.linenumber, args=self.args)
        if self.background:
            logs.append(msg)
        else:
            print(msg, flush=True)

        args = shlex.split(self.args)

        local_pid = None

        try:
            preexec_function = lambda: os.setpgid(0, 0)
            proc = subprocess.Popen( # pylint: disable=subprocess-popen-preexec-fn
                args,
                bufsize=1,
                stdin=subprocess.PIPE,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                universal_newlines=True,
                preexec_fn=preexec_function)
            local_pid = proc.pid
            TeshState().running_pids.append(local_pid)
        except PermissionError:
            logs.append("[{file}:{number}] Cannot start '{cmd}': The binary is not executable.".format(
                file=FileReader().filename, number=self.linenumber, cmd=args[0]))
            logs.append("[{file}:{number}] Current dir: {dir}".format(file=FileReader().filename,
                                                                      number=self.linenumber, dir=os.getcwd()))
            TeshState().set_return_code(3)
            print('\n'.join(logs))
            return
        except NotADirectoryError:
            logs.append("[{file}:{number}] Cannot start '{cmd}': The path to binary does not exist.".format(
                file=FileReader().filename, number=self.linenumber, cmd=args[0]))
            logs.append("[{file}:{number}] Current dir: {dir}".format(file=FileReader().filename,
                                                                      number=self.linenumber, dir=os.getcwd()))
            TeshState().set_return_code(3)
            print('\n'.join(logs))
            return
        except FileNotFoundError:
            logs.append("[{file}:{number}] Cannot start '{cmd}': File not found.".format(
                file=FileReader().filename, number=self.linenumber, cmd=args[0]))
            TeshState().set_return_code(3)
            print('\n'.join(logs))
            return
        except OSError as err:
            if err.errno == 8:
                err.strerror += \
                    "\nOSError: [Errno 8] Executed scripts should start with shebang line (like #!/usr/bin/env sh)"
            raise err

        cmd_name = FileReader().filename + ":" + str(self.linenumber)
        try:
            (stdout_data, _stderr_data) = proc.communicate("\n".join(self.input_pipe), self.timeout)
            timeout_reached = False
        except subprocess.TimeoutExpired:
            timeout_reached = True
            logs.append("Test suite `{file}': NOK (<{cmd}> timeout after {timeout} sec)".format(
                file=FileReader().filename, cmd=cmd_name, timeout=self.timeout))
            TeshState().running_pids.remove(local_pid)
            kill_process_group(local_pid)
            # Try to get the output of the timeout process, to help in debugging.
            try:
                (stdout_data, _stderr_data) = proc.communicate(timeout=1)
            except subprocess.TimeoutExpired:
                logs.append("[{file}:{number}] Could not retrieve output. Killing the process group failed?".format(
                    file=FileReader().filename, number=self.linenumber))
                TeshState().set_return_code(3)
                print('\n'.join(logs))
                return

        # remove text colors
        ansi_escape = re.compile(r'\x1b[^m]*m')
        stdout_data = ansi_escape.sub('', stdout_data)

        if self.output_display:
            logs.append(str(stdout_data))

        if self.rerun_with_valgrind:
            print(str(stdout_data), file=sys.stderr)
            return

        if self.ignore_output:
            logs.append("(ignoring the output of <{cmd}> as requested)".format(cmd=cmd_name))
        else:
            stdouta = stdout_data.split("\n")
            stdouta = self.remove_ignored_lines(stdouta)
            while stdouta and stdouta[-1] == "":
                del stdouta[-1]
            stdcpy = stdouta[:]

            # Mimic the "sort" bash command, which is case unsensitive.
            if self.sort == 0:
                stdouta.sort(key=lambda x: x.lower())
                self.output_pipe_stdout.sort(key=lambda x: x.lower())
            elif self.sort > 0:
                stdouta.sort(key=lambda x: x[:self.sort].lower())
                self.output_pipe_stdout.sort(key=lambda x: x[:self.sort].lower())

            diff = list(
                difflib.unified_diff(
                    self.output_pipe_stdout,
                    stdouta,
                    lineterm="",
                    fromfile='expected',
                    tofile='obtained'))
            if diff:
                logs.append("Output of <{cmd}> mismatch:".format(cmd=cmd_name))
                if self.sort >= 0:  # If sorted, truncate the diff output and show the unsorted version
                    difflen = 0
                    for line in diff:
                        if difflen < 50:
                            print(line)
                        difflen += 1
                    if difflen > 50:
                        logs.append("(diff truncated after 50 lines)")
                    logs.append("Unsorted observed output:\n")
                    for line in stdcpy:
                        logs.append(line)
                else:  # If not sorted, just display the diff
                    for line in diff:
                        logs.append(line)

                logs.append("Test suite `{file}': NOK (<{cmd}> output mismatch)".format(
                    file=FileReader().filename, cmd=cmd_name))

                # Also report any failed return code and/or signal we got in case of output mismatch
                if not proc.returncode in self.expect_return:
                    if proc.returncode >= 0:
                        logs.append("In addition, <{cmd}> returned code {code}.".format(
                            cmd=cmd_name, code=proc.returncode))
                    else:
                        logs.append("In addition, <{cmd}> got signal {sig}.".format(cmd=cmd_name,
                            sig=SIGNALS_TO_NAMES_DICT[-proc.returncode]))
                    if proc.returncode == -signal.SIGSEGV:
                        self.rerun_with_valgrind = True

                if lock is not None:
                    lock.release()
                if TeshState().keep:
                    file = open('obtained', 'w')
                    obtained = stdout_data.split("\n")
                    while obtained and obtained[-1] == "":
                        del obtained[-1]
                    obtained = self.remove_ignored_lines(obtained)
                    for line in obtained:
                        file.write("> " + line + "\n")
                    file.close()
                    logs.append("Obtained output kept as requested: {path}".format(path=os.path.abspath("obtained")))
                TeshState().set_return_code(2)
                print('\n'.join(logs))
                return

        if timeout_reached:
            TeshState().set_return_code(3)
            print('\n'.join(logs))
            return

        if not proc.returncode in self.expect_return:
            if proc.returncode >= 0:
                logs.append("Test suite `{file}': NOK (<{cmd}> returned code {code})".format(
                    file=FileReader().filename, cmd=cmd_name, code=proc.returncode))
                if lock is not None:
                    lock.release()
                TeshState().set_return_code(2)
                print('\n'.join(logs))
                return

            logs.append("Test suite `{file}': NOK (<{cmd}> got signal {sig})".format(
                file=FileReader().filename, cmd=cmd_name,
                sig=SIGNALS_TO_NAMES_DICT[-proc.returncode]))

            if proc.returncode == -signal.SIGSEGV:
                self.rerun_with_valgrind = True

            if lock is not None:
                lock.release()
            TeshState().set_return_code(max(-proc.returncode, 1))
            print('\n'.join(logs))
            return

        if lock is not None:
            lock.release()

        print('\n'.join(logs))

    def can_run(self):
        """ Check if ready to run """
        return self.args is not None

##############
#
# Main
#
#

def main():
    """ main function """
    signal.signal(signal.SIGINT, signal_handler)
    signal.signal(signal.SIGTERM, signal_handler)

    parser = argparse.ArgumentParser(description='tesh -- testing shell')
    group1 = parser.add_argument_group('Options')
    group1.add_argument('teshfile', nargs='?', help='Name of teshfile, stdin if omitted')
    group1.add_argument(
        '--cd',
        metavar='some/directory',
        help='ask tesh to switch the working directory before launching the tests')
    group1.add_argument('--setenv', metavar='var=value', action='append', help='set a specific environment variable')
    group1.add_argument('--cfg', metavar='arg', action='append', help='add parameter --cfg=arg to each command line')
    group1.add_argument('--log', metavar='arg', action='append', help='add parameter --log=arg to each command line')
    group1.add_argument(
        '--ignore-jenkins',
        action='store_true',
        help='ignore all cruft generated on SimGrid continuous integration servers')
    group1.add_argument(
        '--no-auto-valgrind',
        action='store_true',
        help='do not automaticall launch segfaulting commands in valgrind')
    group1.add_argument('--wrapper', metavar='arg', help='Run each command in the provided wrapper (eg valgrind)')
    group1.add_argument(
        '--keep',
        action='store_true',
        help='Keep the obtained output when it does not match the expected one')

    options = parser.parse_args()

    if options.cd is not None:
        print("[Tesh/INFO] change directory to " + options.cd)
        os.chdir(options.cd)

    if options.ignore_jenkins:
        print("Ignore all cruft seen on SimGrid's continuous integration servers")
        # Note: regexps should match at the beginning of lines
        TeshState().ignore_regexps_common = [
            re.compile(r"profiling:"),
            re.compile(r"Unable to clean temporary file C:"),
            re.compile(r".*Configuration change: Set 'contexts/"),
            re.compile(r".*Configuration change: Set 'smpi/tmpdir"),
            re.compile(r"==[0-9]+== ?WARNING: ASan doesn't fully support"),
            re.compile(r"==[0-9]+== ?WARNING: ASan is ignoring requested __asan_handle_no_return: stack "),
            re.compile(r"False positive error reports may follow"),
            re.compile(r"For details see http://code\.google\.com/p/address-sanitizer/issues/detail\?id=189"),
            re.compile(r"For details see https://github\.com/google/sanitizers/issues/189"),
            re.compile(r"Python runtime initialized with LC_CTYPE=C .*"),
            re.compile(r"sthread is intercepting the execution of \.*"),
            # Seen on CircleCI
            re.compile(r"cmake: /usr/local/lib/libcurl\.so\.4: no version information available \(required by cmake\)"),
            re.compile(
                r".*mmap broken on FreeBSD, but dlopen\+thread broken too\. Switching to dlopen\+raw contexts\."),
            re.compile(r".*dlopen\+thread broken on Apple and BSD\. Switching to raw contexts\."),
            # Supurious warning from OpenJDK with -Xcheck:jni
            # https://forums.oracle.com/ords/apexds/post/xcheck-jni-with-jdk-1-8-0-60-now-causes-a-flood-of-warning-6661
            re.compile(r"WARNING: JNI local refs: .*"),
            re.compile(r"\s*at \S*"), # This RE is maybe too generic but I struggled to make it as specific as possible
        ]
        TeshState().jenkins = True  # This is a Jenkins build

    if options.no_auto_valgrind:
        TeshState().auto_valgrind = False

    if options.teshfile is None:
        file = FileReader(None)
        print("Test suite from stdin")
    else:
        if not os.path.isfile(options.teshfile):
            print("Cannot open teshfile '" + options.teshfile + "': File not found")
            tesh_exit(3)
        file = FileReader(options.teshfile)
        print("Test suite '" + file.abspath + "'")

    if options.setenv is not None:
        for env in options.setenv:
            setenv(env)

    if options.cfg is not None:
        for cfg in options.cfg:
            TeshState().args_suffix += " --cfg=" + cfg
    if options.log is not None:
        for log in options.log:
            TeshState().args_suffix += " --log=" + log

    if options.wrapper is not None:
        TeshState().wrapper = options.wrapper

    if options.keep:
        TeshState().keep = True

    # cmd holds the current command line
    # tech commands will add some parameters to it
    # when ready, we execute it.
    cmd = Cmd()

    line = file.readfullline()
    while line is not None:
        # print(">>============="+line+"==<<")
        if not line:
            #print ("END CMD block")
            if cmd.run_if_possible():
                cmd = Cmd()

        elif line[0] == "#":
            pass

        elif line[0:2] == "p ":
            print("[" + str(FileReader()) + "] " + line[2:])

        elif line[0:2] == "< ":
            cmd.add_input_pipe(line[2:])
        elif line[0:1] == "<":
            cmd.add_input_pipe(line[1:])

        elif line[0:2] == "> ":
            cmd.add_output_pipe_stdout(line[2:])
        elif line[0:1] == ">":
            cmd.add_output_pipe_stdout(line[1:])

        elif line[0:2] == "$ ":
            if cmd.run_if_possible():
                cmd = Cmd()
            cmd.set_cmd(line[2:], file.linenumber)

        elif line[0:2] == "& ":
            if cmd.run_if_possible():
                cmd = Cmd()
            cmd.set_cmd(line[2:], file.linenumber)
            cmd.background = True

        elif line[0:15] == "! output ignore":
            cmd.ignore_output = True
            #print("cmd.ignore_output = True")
        elif line[0:16] == "! output display":
            cmd.output_display = True
            cmd.ignore_output = True
        elif line[0:15] == "! expect return":
            try:
                cmd.expect_return = [int(line[16:])]
            except ValueError as err:
                fatal_error("Invalid expect return value: \""+(line[16:])+"\"")
            #print("expect return "+str(int(line[16:])))
        elif line[0:15] == "! expect signal":
            cmd.expect_return = []
            for sig in (line[16:]).split("|"):
                # get the signal integer value from the signal module
                if sig not in signal.__dict__:
                    fatal_error("unrecognized signal '" + sig + "'")
                sig = int(signal.__dict__[sig])
                # popen return -signal when a process ends with a signal
                cmd.expect_return.append(-sig)
        elif line[0:len("! timeout ")] == "! timeout ":
            if "no" in line[len("! timeout "):]:
                cmd.timeout = None
            else:
                try:
                    cmd.timeout = int(line[len("! timeout "):])
                except ValueError as err:
                    fatal_error("Invalid timeout value: \""+(line[len("! timeout "):])+"\"")

        elif line[0:len("! output sort")] == "! output sort":
            if len(line) >= len("! output sort "):
                try:
                    sort = int(line[len("! output sort "):])
                except ValueError as err:
                    fatal_error("Invalid sort value: \""+(line[len("! output sort "):])+"\"")
            else:
                sort = 0
            cmd.sort = sort
        elif line[0:len("! setenv ")] == "! setenv ":
            setenv(line[len("! setenv "):])

        elif line[0:len("! ignore ")] == "! ignore ":
            cmd.add_ignore(line[len("! ignore "):])

        else:
            fatal_error(f"UNRECOGNIZED OPTION LINE: {line}\n"
            "Valid requests:\n"
            "   ! output ignore\n"
            "   ! output sort\n"
            "   ! output display\n"
            "   ! setenv XX=YY\n"
            "   ! ignore XYZ\n"
            "   ! expect return NN\n"
            "   ! expect signal NN\n"
            "   ! timeout NN\n")

        line = file.readfullline()

    cmd.run_if_possible()

    TeshState().join_all_threads()

    if TeshState().return_code == 0:
        if file.filename == "(stdin)":
            print("Test suite from stdin OK")
        else:
            print("Test suite `" + file.filename + "' OK")
    tesh_exit(TeshState().return_code)

if __name__ == '__main__':
    main()
