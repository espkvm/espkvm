/*
 * SPDX-FileCopyrightText: 2026 Evgenij Spitsyn and ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 * Reading chats out of getUpdates, and the bot's name out of getMe.
 */
#include "tg_chats.h"

#include <stdio.h>
#include <string.h>

static int g_fail;
#define CHECK(c)                                                     \
    do {                                                             \
        if (!(c)) {                                                  \
            printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c);       \
            g_fail++;                                                \
        }                                                            \
    } while (0)

int main(void)
{
    char out[1024];

    /* Nothing yet. */
    CHECK(tg_chats_from_updates("{\"ok\":true,\"result\":[]}", out, sizeof(out), 10) == 0);
    CHECK(strcmp(out, "[]") == 0);

    /* A private chat, written to twice, and a reply that repeats the chat. */
    const char *priv =
        "{\"ok\":true,\"result\":[{\"update_id\":1,\"message\":{\"message_id\":5,"
        "\"from\":{\"id\":42,\"is_bot\":false,\"first_name\":\"Ann\"},"
        "\"chat\":{\"id\":42,\"first_name\":\"Ann\",\"last_name\":\"Lee\",\"username\":\"ann\",\"type\":\"private\"},"
        "\"date\":1,\"text\":\"/start\"}},"
        "{\"update_id\":2,\"message\":{\"message_id\":6,"
        "\"chat\":{\"id\":42,\"first_name\":\"Ann\",\"type\":\"private\"},"
        "\"reply_to_message\":{\"chat\":{\"id\":42,\"type\":\"private\"}},\"text\":\"hi\"}}]}";
    CHECK(tg_chats_from_updates(priv, out, sizeof(out), 10) == 1);
    CHECK(strcmp(out, "[{\"id\":\"42\",\"type\":\"private\",\"name\":\"Ann Lee\"}]") == 0);

    /* A group added by my_chat_member, with a title full of traps, and a
       message text that looks like a chat but is inside a string. */
    const char *grp =
        "{\"ok\":true,\"result\":[{\"update_id\":3,\"my_chat_member\":{"
        "\"chat\":{\"id\":-1001234567890,\"title\":\"Ops \\\"}{\\\" \\u0434\\u0435\\u0432\",\"type\":\"supergroup\"},"
        "\"from\":{\"id\":42}}},"
        "{\"update_id\":4,\"message\":{\"chat\":{\"id\":7,\"username\":\"bob\",\"type\":\"private\"},"
        "\"text\":\"\\\"chat\\\":{\\\"id\\\":99}\"}},"
        "{\"update_id\":5,\"channel_post\":{\"sender_chat\":{\"id\":-100555,\"title\":\"no\"},"
        "\"chat\":{\"id\":-100555,\"title\":\"News\",\"type\":\"channel\"}}}]}";
    CHECK(tg_chats_from_updates(grp, out, sizeof(out), 10) == 3);
    CHECK(strcmp(out,
                 "[{\"id\":\"-1001234567890\",\"type\":\"supergroup\",\"name\":\"Ops \\\"}{\\\" \\u0434\\u0435\\u0432\"},"
                 "{\"id\":\"7\",\"type\":\"private\",\"name\":\"bob\"},"
                 "{\"id\":\"-100555\",\"type\":\"channel\",\"name\":\"News\"}]") == 0);

    /* The cap on the count. */
    CHECK(tg_chats_from_updates(grp, out, sizeof(out), 2) == 2);

    /* Too small a buffer is an error, not a cut array. */
    char tiny[40];
    CHECK(tg_chats_from_updates(grp, tiny, sizeof(tiny), 10) == -1);

    /* A reply cut short in the middle of a chat. */
    CHECK(tg_chats_from_updates("{\"result\":[{\"message\":{\"chat\":{\"id\":5,\"ti", out, sizeof(out), 10) == 0);

    /* A long name is cut on a character, never in the middle of an escape. */
    char big[1024] = "{\"chat\":{\"id\":1,\"title\":\"";
    for (int i = 0; i < 40; i++) {
        strcat(big, "\\u0444");
    }
    strcat(big, "\",\"type\":\"group\"}}");
    CHECK(tg_chats_from_updates(big, out, sizeof(out), 10) == 1);
    const char *nm = strstr(out, "\"name\":\"") + 8;
    const size_t nlen = (size_t)(strchr(nm, '"') - nm);
    CHECK(nlen == 126); /* 21 whole escapes of 6 bytes fit in 128 */

    /* getMe. */
    char user[64];
    CHECK(tg_bot_username("{\"ok\":true,\"result\":{\"id\":1,\"is_bot\":true,\"first_name\":\"KVM\","
                          "\"username\":\"my_kvm_bot\"}}", user, sizeof(user)));
    CHECK(strcmp(user, "my_kvm_bot") == 0);
    CHECK(!tg_bot_username("{\"ok\":false,\"error_code\":401}", user, sizeof(user)));

    if (g_fail) {
        printf("%d failed\n", g_fail);
        return 1;
    }
    printf("tg_chats: all passed\n");
    return 0;
}
