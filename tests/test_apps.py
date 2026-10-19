import pathlib
import subprocess
import tempfile
import textwrap
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]

APPMGR_HEADER = r"""
    #ifndef TEST_PSP2_APPMGR_H
    #define TEST_PSP2_APPMGR_H
    #include <stdint.h>
    typedef int32_t SceInt32;
    typedef int SceUID;
    int sceAppMgrGetRunningAppIdListForShell(SceInt32 *appIds, int count);
    SceUID sceAppMgrGetProcessIdByAppIdForShell(SceInt32 appId);
    int sceAppMgrGetNameById(SceUID pid, char *name);
    #endif
"""


def compile_and_run(source, support_sources, headers, extra_cflags=()):
    with tempfile.TemporaryDirectory() as tmpdir:
        tmp = pathlib.Path(tmpdir)
        source_path = tmp / "test.c"
        executable_path = tmp / "test"
        source_path.write_text(textwrap.dedent(source))
        for relative_path, contents in headers.items():
            header_path = tmp / relative_path
            header_path.parent.mkdir(parents=True, exist_ok=True)
            header_path.write_text(textwrap.dedent(contents))
        subprocess.run(
            [
                "cc",
                "-std=c99",
                "-Wall",
                "-Wextra",
                "-Werror",
                *extra_cflags,
                "-I",
                str(tmp),
                "-I",
                str(ROOT / "src"),
                "-I",
                str(ROOT / "include"),
                str(source_path),
                *(str(ROOT / source_name) for source_name in support_sources),
                "-o",
                str(executable_path),
            ],
            check=True,
            cwd=ROOT,
        )
        subprocess.run([str(executable_path)], check=True, cwd=ROOT)


class AppListTests(unittest.TestCase):
    def test_running_apps_are_listed_with_names(self):
        compile_and_run(
            r"""
            #include "apps.h"

            #include <psp2/appmgr.h>
            #include <stdio.h>
            #include <string.h>

            static int list_result;
            static SceInt32 running[3];
            static int name_lookups;

            int sceAppMgrGetRunningAppIdListForShell(SceInt32 *appIds,
                int count)
            {
                int index;

                /* The real syscall refuses more than 80 bytes. */
                if (count * (int)sizeof(SceInt32) > 80)
                    return (int)0x8002710B;
                if (list_result <= 0)
                    return list_result;
                for (index = 0; index < list_result && index < count;
                    ++index)
                    appIds[index] = running[index];
                return list_result;
            }

            SceUID sceAppMgrGetProcessIdByAppIdForShell(SceInt32 appId)
            {
                if (appId == 0x203)
                    return (SceUID)0x80800001;
                return 0x40010000 + appId;
            }

            int sceAppMgrGetNameById(SceUID pid, char *name)
            {
                if (pid == 0x40010101)
                    strcpy(name, "VITASHELL");
                else if (pid == 0x40010102)
                    strcpy(name, "NPXS10015");
                else
                    return -1;
                return 0;
            }

            int promote_read_sfo_string(const char *path, const char *key,
                char *destination, size_t capacity)
            {
                ++name_lookups;
                if (strcmp(key, "TITLE") != 0)
                    return -2;
                if (strcmp(path, "ux0:app/VITASHELL/sce_sys/param.sfo") == 0)
                {
                    snprintf(destination, capacity, "Vita\nShell");
                    return 1;
                }
                /* System apps live on vs0:, so ux0: and gro0: miss first. */
                if (strcmp(path, "vs0:app/NPXS10015/sce_sys/param.sfo") == 0)
                {
                    snprintf(destination, capacity, "Settings");
                    return 1;
                }
                return -3;
            }

            int main(void)
            {
                char buffer[2048];
                char small[100];
                const char expected[] =
                    "TITLEID    APPID       PID         NAME\n"
                    "VITASHELL  0x00000101  0x40010101  Vita Shell\n"
                    "NPXS10015  0x00000102  0x40010102  Settings\n"
                    "?          0x00000203  0x80800001  \n";
                int result;

                list_result = 0;
                result = apps_format_list(buffer, sizeof(buffer));
                if (result <= 0 || strcmp(buffer, "No apps running.\n") != 0)
                    return 1;

                list_result = (int)0x80800002;
                result = apps_format_list(buffer, sizeof(buffer));
                if (result != (int)0x80800002 || buffer[0] != '\0')
                    return 2;

                running[0] = 0x101;
                running[1] = 0x102;
                running[2] = 0x203;
                list_result = 3;
                name_lookups = 0;
                result = apps_format_list(buffer, sizeof(buffer));
                if (result != (int)strlen(expected) ||
                    strcmp(buffer, expected) != 0)
                    return 3;
                /* One hit on ux0:, three tries for vs0:, none for "?". */
                if (name_lookups != 4)
                    return 4;

                /* Only whole lines are kept when the buffer runs out. */
                result = apps_format_list(small, sizeof(small));
                if (result != (int)strlen(small) ||
                    strcmp(small,
                        "TITLEID    APPID       PID         NAME\n"
                        "VITASHELL  0x00000101  0x40010101  Vita Shell\n")
                        != 0)
                    return 5;

                return 0;
            }
            """,
            ("src/apps.c",),
            {"psp2/appmgr.h": APPMGR_HEADER},
        )

    def test_ps_and_quit_all_report_errors(self):
        compile_and_run(
            r"""
            #include "cmd_definitions.h"

            #include <stdbool.h>
            #include <stdint.h>
            #include <stdio.h>
            #include <string.h>
            #include <vitacompanion_input.h>

            volatile int run = 1;
            volatile int net_connected = 1;

            static int list_result;
            static int destroy_other_result;

            int apps_format_list(char *buffer, size_t capacity)
            {
                if (list_result < 0)
                    return list_result;
                return snprintf(buffer, capacity, "No apps running.\n");
            }
            int sceAppMgrDestroyOtherApp(void) { return destroy_other_result; }
            int sceAppMgrDestroyAppByName(const char *name)
            {
                (void)name;
                return 0;
            }
            int sceAppMgrLaunchAppByUri(int flags, const char *uri)
            {
                (void)flags;
                (void)uri;
                return 0;
            }
            int scePowerRequestDisplayOn(void) { return 0; }
            int scePowerRequestDisplayOff(void) { return 0; }
            int sceKernelDelayThread(unsigned int delay)
            {
                (void)delay;
                return 0;
            }
            void nosleep_set_enabled(bool enabled) { (void)enabled; }
            bool nosleep_is_enabled(void) { return false; }
            bool input_is_ready(void) { return false; }
            int input_apply(const vitacompanion_input_action *action)
            {
                (void)action;
                return 0;
            }
            int vitacompanion_parse_press(char **arguments,
                size_t argument_count, vitacompanion_input_action *action)
            {
                (void)arguments;
                (void)argument_count;
                (void)action;
                return 0;
            }
            int vitacompanion_parse_release(char **arguments,
                size_t argument_count, vitacompanion_input_action *action)
            {
                (void)arguments;
                (void)argument_count;
                (void)action;
                return 0;
            }
            const char *vitacompanion_input_parse_error(int result)
            {
                (void)result;
                return "";
            }
            bool parse_wait_duration_ms(const char *value,
                uint32_t *duration_ms)
            {
                (void)value;
                (void)duration_ms;
                return false;
            }
            int promote_directory(const char *path)
            {
                (void)path;
                return 0;
            }
            int reboot_request(void) { return 0; }
            int version_format(char *buffer, size_t capacity)
            {
                (void)buffer;
                (void)capacity;
                return 0;
            }

            int main(void)
            {
                char response[2048];
                char quit[] = "quit";
                char all[] = "all";
                char *quit_all[] = { quit, all };

                if (strcmp(cmd_get_definition("ps")->name, "ps") != 0)
                    return 1;

                list_result = 0;
                cmd_ps(0, 0, response);
                if (strcmp(response, "No apps running.\n") != 0)
                    return 2;

                list_result = (int)0x80800002;
                cmd_ps(0, 0, response);
                if (strcmp(response,
                    "Error: Could not list running apps (0x80800002).\n")
                    != 0)
                    return 3;

                destroy_other_result = 0;
                cmd_quit(quit_all, 1, response);
                if (strcmp(response, "Apps quit.\n") != 0)
                    return 4;

                destroy_other_result = (int)0x80800003;
                cmd_quit(quit_all, 1, response);
                if (strcmp(response,
                    "Error: Could not quit apps (0x80800003).\n") != 0)
                    return 5;

                return 0;
            }
            """,
            ("src/cmd_definitions.c",),
            {
                "vitasdk.h": r"""
                    #ifndef TEST_VITASDK_H
                    #define TEST_VITASDK_H
                    int sceAppMgrDestroyOtherApp(void);
                    int sceAppMgrDestroyAppByName(const char *name);
                    int sceAppMgrLaunchAppByUri(int flags,
                        const char *uri);
                    int scePowerRequestDisplayOn(void);
                    int scePowerRequestDisplayOff(void);
                    int sceKernelDelayThread(unsigned int delay);
                    #endif
                """,
            },
            extra_cflags=("-Wno-unused-parameter",),
        )

    def test_ps_is_built_and_documented(self):
        cmake_source = (ROOT / "CMakeLists.txt").read_text()
        readme = (ROOT / "README.md").read_text()

        self.assertIn("src/apps.c", cmake_source)
        self.assertIn("| `ps`", readme)


if __name__ == "__main__":
    unittest.main()
