import unittest

from test_command_features import compile_and_run


HEADERS = {
    "psp2/net/netctl.h": r"""
        #ifndef TEST_NETCTL_H
        #define TEST_NETCTL_H
        enum {
            SCE_NETCTL_STATE_DISCONNECTED,
            SCE_NETCTL_STATE_CONNECTING,
            SCE_NETCTL_STATE_FINALIZING,
            SCE_NETCTL_STATE_CONNECTED
        };
        int sceNetCtlInetGetState(int *state);
        #endif
    """,
    "psp2/registrymgr.h": r"""
        #ifndef TEST_REGISTRYMGR_H
        #define TEST_REGISTRYMGR_H
        int sceRegMgrGetKeyInt(const char *category, const char *name,
            int *buf);
        #endif
    """,
}

# A fake SceWlan driver: request 0 clears the "on" bit, request 1 sets it
# unless the test makes it ignore that many requests. Every request is logged.
FAKE_DRIVER = r"""
    #include "wifi.h"

    #include <stdio.h>
    #include <string.h>

    static unsigned int config = 0x1;
    static int requests[64];
    static int request_count;
    static int ignore_on_requests;
    static int config_error;

    int sceWlanSetConfiguration(int request)
    {
        requests[request_count++] = request;
        if (request == 0)
            config &= ~0x1u;
        else if (request == 1 && ignore_on_requests > 0)
            ignore_on_requests--;
        else if (request == 1)
            config |= 0x1;
        return 0;
    }

    unsigned int sceWlanGetConfiguration(void)
    {
        return config_error ? 0x80410D01u : config;
    }

    int sceRegMgrGetKeyInt(const char *category, const char *name, int *buf)
    {
        (void)category;
        *buf = strcmp(name, "wifi_flag") == 0 ? 1 : 0;
        return 0;
    }

    int sceNetCtlInetGetState(int *state)
    {
        *state = (config & 0x1) ? 3 : 0;
        return 0;
    }

    void LOG(const char *str, ...) { (void)str; }

    static void ticks(int count)
    {
        while (count-- > 0)
            wifi_tick();
    }

    #define CHECK(condition) \
        do { if (!(condition)) { printf("line %d\n", __LINE__); \
            return __LINE__ % 250 + 1; } } while (0)
"""


def run_scenario(body):
    compile_and_run(
        FAKE_DRIVER + "\nint main(void)\n{\n" + body + "\n    return 0;\n}\n",
        ("src/wifi.c",),
        HEADERS,
    )


class WifiTests(unittest.TestCase):
    def test_off_is_refused_until_the_tick_loop_runs(self):
        run_scenario(r"""
            CHECK(wifi_off_for(10) == -1);
            CHECK(request_count == 0);
            ticks(1);
            CHECK(wifi_off_for(10) == 0);
        """)

    def test_timed_off_waits_for_the_reply_then_restores_itself(self):
        run_scenario(r"""
            wifi_tick();
            CHECK(wifi_off_for(10) == 0);
            CHECK(wifi_off_for(10) == -2);

            ticks(2);
            CHECK(request_count == 0);
            ticks(1);
            CHECK(request_count == 1 && requests[0] == 0);
            CHECK(!(config & 0x1));

            ticks(9);
            CHECK(request_count == 1);
            ticks(1);
            CHECK(request_count == 2 && requests[1] == 1);
            CHECK(config & 0x1);

            ticks(1);
            CHECK(wifi_off_for(5) == 0);
        """)

    def test_restore_retries_until_wifi_is_back_on(self):
        run_scenario(r"""
            wifi_tick();
            CHECK(wifi_off_for(1) == 0);
            ignore_on_requests = 2;
            ticks(3);
            CHECK(request_count == 1);

            ticks(1);
            CHECK(request_count == 2 && requests[1] == 1);
            ticks(5);
            CHECK(request_count == 2);
            ticks(1);
            CHECK(request_count == 3 && requests[2] == 1);
            ticks(6);
            CHECK(request_count == 4 && (config & 0x1));
            ticks(10);
            CHECK(request_count == 4);
        """)

    def test_flight_mode_is_never_requested(self):
        run_scenario(r"""
            int i;

            wifi_tick();
            wifi_force_set(1);
            CHECK(wifi_off_for(3) == 0);
            ticks(30);
            config = 0;
            ticks(30);
            for (i = 0; i < request_count; i++)
                CHECK(requests[i] == 0 || requests[i] == 1);
        """)

    def test_force_turns_wifi_back_on_at_its_interval(self):
        run_scenario(r"""
            wifi_force_set(3);
            config = 0;
            ticks(2);
            CHECK(request_count == 0);
            ticks(1);
            CHECK(request_count == 1 && requests[0] == 1);
            CHECK(config & 0x1);
            ticks(9);
            CHECK(request_count == 1);

            wifi_force_set(0);
            config = 0;
            ticks(10);
            CHECK(request_count == 1);
        """)

    def test_force_leaves_flight_mode_power_events_and_timed_off_alone(self):
        run_scenario(r"""
            wifi_force_set(1);
            config = 0x2;
            ticks(5);
            config = 0x200;
            ticks(5);
            config_error = 1;
            ticks(5);
            CHECK(request_count == 0);

            config_error = 0;
            config = 0x1;
            CHECK(wifi_off_for(20) == 0);
            ticks(10);
            CHECK(request_count == 1 && requests[0] == 0);
        """)

    def test_stopping_the_plugin_during_a_timed_off_turns_wifi_on(self):
        run_scenario(r"""
            wifi_tick();
            CHECK(wifi_off_for(60) == 0);
            ticks(3);
            CHECK(!(config & 0x1));
            wifi_end();
            CHECK(config & 0x1);
            CHECK(wifi_off_for(5) == -1);
        """)

    def test_status_reports_driver_registry_network_and_timers(self):
        run_scenario(r"""
            char status[512];

            wifi_tick();
            wifi_force_set(30);
            config = 0x101;
            wifi_status(status, sizeof(status));
            CHECK(strstr(status, "Wi-Fi: on (config 0x00000101: on power-save)"));
            CHECK(strstr(status, "wifi_flag=1 wifi_power_save=0 flight_mode=0"));
            CHECK(strstr(status, "Network: connected"));
            CHECK(strstr(status, "Timed off: none"));
            CHECK(strstr(status, "Force: every 30s"));

            CHECK(wifi_off_for(10) == 0);
            ticks(3);
            wifi_status(status, sizeof(status));
            CHECK(strstr(status, "Wi-Fi: off"));
            CHECK(strstr(status, "Network: disconnected"));
            CHECK(strstr(status, "Timed off: back on in 10s"));

            config_error = 1;
            wifi_status(status, 16);
            CHECK(strlen(status) == 15);
        """)


if __name__ == "__main__":
    unittest.main()
