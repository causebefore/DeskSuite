#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "app_network_hub_url.h"

static void assert_hub_url_success(const char *input, const char *expected)
{
    char output[APP_NETWORK_HUB_URL_MAX_LENGTH + 1U] = { 0 };
    assert(app_network_hub_url_parse_copy(input, output) == ESP_OK);
    assert(strcmp(output, expected) == 0);
}

static void assert_hub_url_rejected(const char *input)
{
    char output[APP_NETWORK_HUB_URL_MAX_LENGTH + 1U] = "unchanged";
    assert(app_network_hub_url_parse_copy(input, output) != ESP_OK);
    assert(output[0] == '\0');
}

static void test_normalizes_supported_authorities(void)
{
    assert_hub_url_success("HTTP://Example.COM/", "http://example.com");
    assert_hub_url_success("http://192.168.1.2:8765/", "http://192.168.1.2:8765");
    assert_hub_url_success("http://hub.example:8765/", "http://hub.example:8765");
    assert_hub_url_success("http://LOCALHOST", "http://localhost");
}

static void test_accepts_exact_normalized_ascii_limit(void)
{
    char input[APP_NETWORK_HUB_URL_MAX_LENGTH + 2U]    = "http://";
    char expected[APP_NETWORK_HUB_URL_MAX_LENGTH + 1U] = "http://";
    memset(input + 7U, 'a', 63U);
    input[70U] = '.';
    memset(input + 71U, 'b', 56U);
    input[127U] = '/';
    input[128U] = '\0';
    memcpy(expected + 7U, input + 7U, 120U);
    expected[127U] = '\0';

    assert_hub_url_success(input, expected);
}

static void test_rejects_unsupported_url_parts(void)
{
    assert_hub_url_rejected("https://hub.example");
    assert_hub_url_rejected("http://user@hub.example");
    assert_hub_url_rejected("http://hub.example?ready=1");
    assert_hub_url_rejected("http://hub.example#status");
    assert_hub_url_rejected("http://hub.example/api");
    assert_hub_url_rejected("http://hub.example//");
}

static void test_rejects_whitespace_and_non_ascii(void)
{
    const char non_ascii[] = "http://hub.\x80";
    assert_hub_url_rejected(" http://hub.example");
    assert_hub_url_rejected("http://hub.example ");
    assert_hub_url_rejected("http://hub example");
    assert_hub_url_rejected(non_ascii); /* 非 ASCII */
}

static void test_rejects_invalid_hosts_and_ports(void)
{
    assert_hub_url_rejected("http://");
    assert_hub_url_rejected("hub.example");
    assert_hub_url_rejected("http://-hub.example");
    assert_hub_url_rejected("http://hub-.example");
    assert_hub_url_rejected("http://hub..example");
    assert_hub_url_rejected("http://hub_example");
    assert_hub_url_rejected("http://256.1.2.3");
    assert_hub_url_rejected("http://[192.168.1.2]");
    assert_hub_url_rejected("http://hub.example:");
    assert_hub_url_rejected("http://hub.example:0");
    assert_hub_url_rejected("http://hub.example:65536");
    assert_hub_url_rejected("http://hub.example:http");
}

static void test_rejects_overlong_normalized_output(void)
{
    char overlong[APP_NETWORK_HUB_URL_MAX_LENGTH + 2U] = "http://";
    memset(overlong + 7U, 'a', 63U);
    overlong[70U] = '.';
    memset(overlong + 71U, 'b', 57U);
    overlong[128U] = '\0';
    assert_hub_url_rejected(overlong);
}

static void test_rejects_null_arguments(void)
{
    char output[APP_NETWORK_HUB_URL_MAX_LENGTH + 1U] = { 0 };
    assert(app_network_hub_url_parse_copy(NULL, output) != ESP_OK);
    assert(app_network_hub_url_parse_copy("http://hub.example", NULL) != ESP_OK);
}

int main(void)
{
    test_normalizes_supported_authorities();
    test_accepts_exact_normalized_ascii_limit();
    test_rejects_unsupported_url_parts();
    test_rejects_whitespace_and_non_ascii();
    test_rejects_invalid_hosts_and_ports();
    test_rejects_overlong_normalized_output();
    test_rejects_null_arguments();
    puts("app_network_hub_url host tests passed");
    return 0;
}
