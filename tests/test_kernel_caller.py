import pathlib
import re
import unittest

from test_command_features import ROOT, compile_and_run


STUB_HEADERS = {
    "psp2kern/types.h": r"""
        #pragma once
        typedef int SceUID;
        typedef unsigned int SceSize;
    """,
    "psp2kern/kernel/acmgr.h": r"""
        #pragma once
        #include <psp2kern/types.h>
        int ksceSblACMgrIsSystemProgram(SceUID pid);
    """,
    "psp2kern/kernel/sysroot.h": r"""
        #pragma once
        #include <psp2kern/types.h>
        int ksceKernelSysrootGetProcessTitleId(
            SceUID pid, char *titleid, SceSize len);
    """,
}

STUBS = r"""
    #include "caller.h"

    #include <psp2kern/kernel/acmgr.h>
    #include <psp2kern/kernel/sysroot.h>
    #include <string.h>

    static const char *title_id = "main";
    static int title_result;
    static int system_program = 1;
    static int title_calls;

    int ksceKernelSysrootGetProcessTitleId(
        SceUID pid, char *titleid, SceSize len)
    {
        (void)pid;
        ++title_calls;
        if (title_result < 0)
            return title_result;
        strncpy(titleid, title_id, len);
        return 0;
    }

    int ksceSblACMgrIsSystemProgram(SceUID pid)
    {
        (void)pid;
        return system_program;
    }
"""


class KernelCallerTests(unittest.TestCase):
    def run_caller(self, body):
        compile_and_run(
            STUBS + "\nint main(void)\n{\n" + body + "\n}\n",
            ("kernel/caller.c",),
            headers=STUB_HEADERS,
            include_dirs=("kernel",),
        )

    def test_accepts_shell_and_caches_its_pid(self):
        self.run_caller(r"""
            if (!vitacompanion_caller_allowed(0x10005) || title_calls != 1)
                return 1;
            if (!vitacompanion_caller_allowed(0x10005) || title_calls != 1)
                return 2;
            return 0;
        """)

    def test_rejects_other_titles_and_non_system_programs(self):
        self.run_caller(r"""
            title_id = "VITASHELL";
            if (vitacompanion_caller_allowed(0x20001))
                return 1;
            title_id = "mainx";
            if (vitacompanion_caller_allowed(0x20001))
                return 2;
            title_id = "main";
            system_program = 0;
            if (vitacompanion_caller_allowed(0x20001))
                return 3;
            system_program = 1;
            title_result = -1;
            if (vitacompanion_caller_allowed(0x20001))
                return 4;
            return 0;
        """)

    def test_rejected_caller_does_not_reuse_the_cached_pid(self):
        self.run_caller(r"""
            if (!vitacompanion_caller_allowed(0x10005))
                return 1;
            title_id = "PCSE00001";
            if (vitacompanion_caller_allowed(0x30003))
                return 2;
            if (!vitacompanion_caller_allowed(0x10005))
                return 3;
            return 0;
        """)

    def test_restarted_shell_is_revalidated(self):
        self.run_caller(r"""
            if (!vitacompanion_caller_allowed(0x10005))
                return 1;
            if (!vitacompanion_caller_allowed(0x10777) || title_calls != 2)
                return 2;
            title_id = "PCSE00001";
            if (vitacompanion_caller_allowed(0x10005))
                return 3;
            return 0;
        """)

    def test_negative_pid_is_never_cached(self):
        self.run_caller(r"""
            title_id = "PCSE00001";
            if (vitacompanion_caller_allowed(-1))
                return 1;
            return 0;
        """)

    def test_bypass_flag_allows_anyone(self):
        compile_and_run(
            STUBS + r"""
            int main(void)
            {
                title_id = "PCSE00001";
                system_program = 0;
                return vitacompanion_caller_allowed(0x30003) ? 0 : 1;
            }
            """,
            ("kernel/caller.c",),
            headers=STUB_HEADERS,
            include_dirs=("kernel",),
            extra_cflags=("-DVITACOMPANION_KERNEL_ALLOW_ANY_CALLER",
                          "-Wno-unused-function",
                          "-Wno-unused-variable"),
        )

    def test_guarded_exports(self):
        source = (ROOT / "kernel" / "main.c").read_text()
        guarded = (
            "vitaCompanionKernelSetButtons",
            "vitaCompanionKernelSetAnalog",
            "vitaCompanionKernelSetTouch",
            "vitaCompanionKernelReset",
            "vitaCompanionKernelRequestReboot",
            "vitaCompanionKernelForceReboot",
        )
        for name in guarded + ("vitaCompanionKernelGetApiVersion",):
            match = re.search(
                r"^int " + name + r"\([^)]*\)\n\{\n(.*?)^\}",
                source, re.S | re.M)
            self.assertIsNotNone(match, name)
            body = match.group(1)
            if name in guarded:
                guard = body.index("caller_allowed()")
                self.assertLess(guard, body.find("state_lock()")
                                if "state_lock()" in body else len(body),
                                name)
                self.assertIn("VITACOMPANION_KERNEL_ERROR_NOT_PERMITTED",
                              body, name)
            else:
                self.assertNotIn("caller_allowed", body, name)

    def test_build_links_the_caller_check(self):
        cmake = (ROOT / "CMakeLists.txt").read_text()
        self.assertIn("kernel/caller.c", cmake)
        self.assertIn("SceSblACMgrForDriver_stub", cmake)
        self.assertIn("SceSysrootForKernel_stub", cmake)
        self.assertIn("VITACOMPANION_KERNEL_ALLOW_ANY_CALLER", cmake)


if __name__ == "__main__":
    unittest.main()
