/*
 * Copyright (C) 2015, Wazuh Inc.
 *
 * This program is free software; you can redistribute it
 * and/or modify it under the terms of the GNU General Public
 * License (version 2) as published by the FSF - Free Software
 * Foundation.
 */

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>
#include <stdio.h>
#include <stdlib.h>

#include "headers/shared.h"
#include "remoted/remoted.h"
#include "../wrappers/common.h"
#include "../wrappers/linux/socket_wrappers.h"
#include "../wrappers/wazuh/shared/debug_op_wrappers.h"
#include "../wrappers/wazuh/wazuh_db/wdb_metadata_wrappers.h"
#include "../wrappers/wazuh/wazuh_db/wdb_wrappers.h"
#include "../wrappers/libc/stdio_wrappers.h"
#include "../wrappers/posix/stat_wrappers.h"
#include "../wrappers/posix/unistd_wrappers.h"
#include "../wrappers/wazuh/shared/queue_linked_op_wrappers.h"
#include "../wrappers/wazuh/os_crypto/keys_wrappers.h"
#include "../wrappers/wazuh/remoted/queue_wrappers.h"
#include "../wrappers/wazuh/remoted/netbuffer_wrappers.h"
#include "../wrappers/wazuh/remoted/netcounter_wrappers.h"
#include "../wrappers/wazuh/os_crypto/msgs_wrappers.h"
#include "../wrappers/wazuh/remoted/state_wrappers.h"
#include "remoted/secure.c"

extern keystore keys;
extern remoted logr;
extern wnotify_t * notify;

/* Forward declarations */
void * close_fp_main(void * args);
void HandleSecureMessage(const message_t *message, int *wdb_sock);

/* Setup/teardown */

static int setup_config(void **state) {
    w_linked_queue_t *queue = linked_queue_init();
    keys.opened_fp_queue = queue;
    test_mode = 1;
    return 0;
}

static int teardown_config(void **state) {
    linked_queue_free(keys.opened_fp_queue);
    test_mode = 0;
    return 0;
}

static int setup_new_tcp(void **state) {
    test_mode = 1;
    os_calloc(1, sizeof(wnotify_t), notify);
    notify->fd = 0;
    return 0;
}

static int teardown_new_tcp(void **state) {
    test_mode = 0;
    os_free(notify);
    return 0;
}

/* Wrappers */

time_t __wrap_time(int time) {
    check_expected(time);
    return mock();
}

void __wrap_key_lock_write(){
    function_called();
}

void __wrap_key_unlock(){
    function_called();
}

void __wrap_key_lock_read(){
    function_called();
}

int __wrap_close(int __fd) {
    return mock();
}

/*****************WRAPS********************/
int __wrap_w_mutex_lock(pthread_mutex_t *mutex) {
    check_expected_ptr(mutex);
    return 0;
}

int __wrap_w_mutex_unlock(pthread_mutex_t *mutex) {
    check_expected_ptr(mutex);
    return 0;
}

void __wrap_save_controlmsg(__attribute__((unused)) keystore *keys, char *msg, size_t msg_length, __attribute__((unused)) int *wdb_sock) {
    check_expected(msg);
    check_expected(msg_length);
}

/* Tests close_fp_main*/

void test_close_fp_main_queue_empty(void **state)
{
    logr.rids_closing_time = 10;

    // sleep
    expect_value(__wrap_sleep, seconds, 10);

    // key_lock
    expect_function_call(__wrap_key_lock_write);

    expect_string(__wrap__mdebug2, formatted_msg, "Opened rids queue size: 0");

    expect_string(__wrap__mdebug1, formatted_msg, "Rids closer thread started.");

    // key_unlock
    expect_function_call(__wrap_key_unlock);

    close_fp_main(&keys);
    assert_int_equal(keys.opened_fp_queue->elements, 0);
}

void test_close_fp_main_first_node_no_close_first(void **state)
{
    logr.rids_closing_time = 10;

    keyentry *first_node_key = NULL;
    os_calloc(1, sizeof(keyentry), first_node_key);

    int now = 123456789;
    first_node_key->id = strdup("001");
    first_node_key->updating_time = now - 1;

    // Queue with one element
    w_linked_queue_node_t *node1 = linked_queue_push(keys.opened_fp_queue, first_node_key);
    keys.opened_fp_queue->first = node1;

    // sleep
    expect_value(__wrap_sleep, seconds, 10);

    // key_lock
    expect_function_call(__wrap_key_lock_write);

    expect_string(__wrap__mdebug2, formatted_msg, "Opened rids queue size: 1");

    expect_value(__wrap_time, time, 0);
    will_return(__wrap_time, now);

    expect_string(__wrap__mdebug2, formatted_msg, "Checking rids_node of agent 001.");

    expect_string(__wrap__mdebug1, formatted_msg, "Rids closer thread started.");
    // key_unlock
    expect_function_call(__wrap_key_unlock);

    close_fp_main(&keys);
    assert_int_equal(keys.opened_fp_queue->elements, 1);
    os_free(node1);
    os_free(first_node_key->id);
    os_free(first_node_key);
}

void test_close_fp_main_close_first(void **state)
{
    logr.rids_closing_time = 10;

    keyentry *first_node_key = NULL;
    os_calloc(1, sizeof(keyentry), first_node_key);

    int now = 123456789;
    first_node_key->id = strdup("001");
    first_node_key->updating_time = now - logr.rids_closing_time - 100;

    first_node_key->fp = (FILE *)1234;

    // Queue with one element
    w_linked_queue_node_t *node1 = linked_queue_push(keys.opened_fp_queue, first_node_key);

    // sleep
    expect_value(__wrap_sleep, seconds, 10);

    // key_lock
    expect_function_call(__wrap_key_lock_write);

    expect_string(__wrap__mdebug2, formatted_msg, "Opened rids queue size: 1");

    expect_value(__wrap_time, time, 0);
    will_return(__wrap_time, now);

    expect_string(__wrap__mdebug2, formatted_msg, "Checking rids_node of agent 001.");

    expect_string(__wrap__mdebug2, formatted_msg, "Pop rids_node of agent 001.");

    expect_string(__wrap__mdebug2, formatted_msg, "Closing rids for agent 001.");

    expect_value(__wrap_fclose, _File, (FILE *)1234);
    will_return(__wrap_fclose, 1);

    expect_string(__wrap__mdebug2, formatted_msg, "Opened rids queue size: 0");

    expect_string(__wrap__mdebug1, formatted_msg, "Rids closer thread started.");

    // key_unlock
    expect_function_call(__wrap_key_unlock);

    close_fp_main(&keys);
    assert_int_equal(keys.opened_fp_queue->elements, 0);
    os_free(first_node_key->id);
    os_free(first_node_key);
}

void test_close_fp_main_close_first_queue_2(void **state)
{
    logr.rids_closing_time = 10;

    keyentry *first_node_key = NULL;
    os_calloc(1, sizeof(keyentry), first_node_key);

    keyentry *second_node_key = NULL;
    os_calloc(1, sizeof(keyentry), second_node_key);

    int now = 123456789;
    first_node_key->id = strdup("001");
    first_node_key->updating_time = now - logr.rids_closing_time - 100;
    first_node_key->fp = (FILE *)1234;

    second_node_key->id = strdup("002");
    second_node_key->updating_time = now - 1;

    // Queue with one element
    w_linked_queue_node_t *node1 = linked_queue_push(keys.opened_fp_queue, first_node_key);
    w_linked_queue_node_t *node2 = linked_queue_push(keys.opened_fp_queue, second_node_key);

    // sleep
    expect_value(__wrap_sleep, seconds, 10);

    // key_lock
    expect_function_call(__wrap_key_lock_write);

    expect_string(__wrap__mdebug2, formatted_msg, "Opened rids queue size: 2");

    expect_value(__wrap_time, time, 0);
    will_return(__wrap_time, now);

    expect_string(__wrap__mdebug2, formatted_msg, "Checking rids_node of agent 001.");

    expect_string(__wrap__mdebug2, formatted_msg, "Pop rids_node of agent 001.");

    expect_string(__wrap__mdebug2, formatted_msg, "Closing rids for agent 001.");

    expect_value(__wrap_fclose, _File, (FILE *)1234);
    will_return(__wrap_fclose, 1);

    expect_string(__wrap__mdebug2, formatted_msg, "Opened rids queue size: 1");

    expect_value(__wrap_time, time, 0);
    will_return(__wrap_time, now);

    expect_string(__wrap__mdebug2, formatted_msg, "Checking rids_node of agent 002.");

    expect_string(__wrap__mdebug1, formatted_msg, "Rids closer thread started.");

    // key_unlock
    expect_function_call(__wrap_key_unlock);

    close_fp_main(&keys);
    assert_int_equal(keys.opened_fp_queue->elements, 1);
    os_free(first_node_key->id);
    os_free(first_node_key);

    os_free(node2);
    os_free(second_node_key->id);
    os_free(second_node_key);
}

void test_close_fp_main_close_first_queue_2_close_2(void **state)
{
    logr.rids_closing_time = 10;

    keyentry *first_node_key = NULL;
    os_calloc(1, sizeof(keyentry), first_node_key);

    keyentry *second_node_key = NULL;
    os_calloc(1, sizeof(keyentry), second_node_key);

    int now = 123456789;
    first_node_key->id = strdup("001");
    first_node_key->updating_time = now - logr.rids_closing_time - 100;
    first_node_key->fp = (FILE *)1234;

    second_node_key->id = strdup("002");
    second_node_key->updating_time = now - logr.rids_closing_time - 99;
    second_node_key->fp = (FILE *)1234;

    // Queue with one element
    w_linked_queue_node_t *node1 = linked_queue_push(keys.opened_fp_queue, first_node_key);
    w_linked_queue_node_t *node2 = linked_queue_push(keys.opened_fp_queue, second_node_key);

    // sleep
    expect_value(__wrap_sleep, seconds, 10);

    // key_lock
    expect_function_call(__wrap_key_lock_write);

    expect_string(__wrap__mdebug2, formatted_msg, "Opened rids queue size: 2");

    expect_value(__wrap_time, time, 0);
    will_return(__wrap_time, now);

    expect_string(__wrap__mdebug2, formatted_msg, "Checking rids_node of agent 001.");

    expect_string(__wrap__mdebug2, formatted_msg, "Pop rids_node of agent 001.");

    expect_string(__wrap__mdebug2, formatted_msg, "Closing rids for agent 001.");

    expect_value(__wrap_fclose, _File, (FILE *)1234);
    will_return(__wrap_fclose, 1);

    expect_string(__wrap__mdebug2, formatted_msg, "Opened rids queue size: 1");

    expect_value(__wrap_time, time, 0);
    will_return(__wrap_time, now);

    expect_string(__wrap__mdebug2, formatted_msg, "Checking rids_node of agent 002.");

    expect_string(__wrap__mdebug2, formatted_msg, "Pop rids_node of agent 002.");

    expect_string(__wrap__mdebug2, formatted_msg, "Closing rids for agent 002.");

    expect_value(__wrap_fclose, _File, (FILE *)1234);
    will_return(__wrap_fclose, 1);

    expect_string(__wrap__mdebug2, formatted_msg, "Opened rids queue size: 0");

    expect_string(__wrap__mdebug1, formatted_msg, "Rids closer thread started.");

    // key_unlock
    expect_function_call(__wrap_key_unlock);

    close_fp_main(&keys);
    assert_int_equal(keys.opened_fp_queue->elements, 0);
    os_free(first_node_key->id);
    os_free(first_node_key);

    os_free(second_node_key->id);
    os_free(second_node_key);
}

void test_close_fp_main_close_fp_null(void **state)
{
    logr.rids_closing_time = 10;

    keyentry *first_node_key = NULL;
    os_calloc(1, sizeof(keyentry), first_node_key);

    int now = 123456789;
    first_node_key->id = strdup("001");
    first_node_key->updating_time = now - logr.rids_closing_time - 100;
    first_node_key->fp = NULL;

    // Queue with one element
    w_linked_queue_node_t *node1 = linked_queue_push(keys.opened_fp_queue, first_node_key);

    // sleep
    expect_value(__wrap_sleep, seconds, 10);

    // key_lock
    expect_function_call(__wrap_key_lock_write);

    expect_string(__wrap__mdebug2, formatted_msg, "Opened rids queue size: 1");

    expect_value(__wrap_time, time, 0);
    will_return(__wrap_time, now);

    expect_string(__wrap__mdebug2, formatted_msg, "Checking rids_node of agent 001.");

    expect_string(__wrap__mdebug2, formatted_msg, "Pop rids_node of agent 001.");

    expect_string(__wrap__mdebug2, formatted_msg, "Opened rids queue size: 0");

    expect_string(__wrap__mdebug1, formatted_msg, "Rids closer thread started.");

    // key_unlock
    expect_function_call(__wrap_key_unlock);

    close_fp_main(&keys);
    assert_int_equal(keys.opened_fp_queue->elements, 0);
    os_free(first_node_key->id);
    os_free(first_node_key);
}

void test_HandleSecureMessage_shutdown_message(void **state)
{
    char buffer[OS_MAXSTR + 1] = "#!-agent shutdown ";
    message_t message = { .buffer = buffer, .size = 18, .sock = 1, .counter = 10 };
    struct sockaddr_in peer_info;
    int wdb_sock;

    keyentry** keyentries;
    os_calloc(1, sizeof(keyentry*), keyentries);
    keys.keyentries = keyentries;

    keyentry *key = NULL;
    os_calloc(1, sizeof(keyentry), key);

    key->id = strdup("009");
    key->sock = 1;
    key->keyid = 1;

    keys.keyentries[0] = key;

    global_counter = 0;

    peer_info.sin_family = AF_INET;
    peer_info.sin_addr.s_addr = 0x0100007F;
    memcpy(&message.addr, &peer_info, sizeof(peer_info));

    expect_function_call(__wrap_key_lock_read);

    expect_string(__wrap_OS_IsAllowedIP, srcip, "127.0.0.1");
    will_return(__wrap_OS_IsAllowedIP, 0);

    expect_string(__wrap_ReadSecMSG, buffer, "#!-agent shutdown ");
    will_return(__wrap_ReadSecMSG, message.size);
    will_return(__wrap_ReadSecMSG, "#!-agent shutdown ");
    will_return(__wrap_ReadSecMSG, KS_VALID);

    expect_value(__wrap_rem_getCounter, fd, 1);
    will_return(__wrap_rem_getCounter, 10);

    expect_value(__wrap_time, time, 0);
    will_return(__wrap_time, (time_t)123456789);

    expect_value(__wrap_rem_getCounter, fd, 1);
    will_return(__wrap_rem_getCounter, 10);

    expect_function_call(__wrap_key_unlock);

    expect_string(__wrap_save_controlmsg, msg, "agent shutdown ");
    expect_value(__wrap_save_controlmsg, msg_length, message.size - 3);

    expect_string(__wrap_rem_inc_recv_ctrl, agent_id, key->id);

    HandleSecureMessage(&message, &wdb_sock);

    os_free(key->id);
    os_free(key);
    os_free(keyentries);
}

void test_HandleSecureMessage_NewMessage_NoShutdownMessage(void **state)
{
    char buffer[OS_MAXSTR + 1] = "#!-agent startup ";
    message_t message = { .buffer = buffer, .size = 17, .sock = 1, .counter = 11 };
    struct sockaddr_in peer_info;
    int wdb_sock;

    keyentry** keyentries;
    os_calloc(1, sizeof(keyentry*), keyentries);
    keys.keyentries = keyentries;

    keyentry *key = NULL;
    os_calloc(1, sizeof(keyentry), key);

    key->id = strdup("009");
    key->sock = 1;
    key->keyid = 1;

    keys.keyentries[0] = key;

    global_counter = 0;

    peer_info.sin_family = AF_INET;
    peer_info.sin_addr.s_addr = 0x0100007F;
    memcpy(&message.addr, &peer_info, sizeof(peer_info));

    expect_function_call(__wrap_key_lock_read);

    expect_string(__wrap_OS_IsAllowedIP, srcip, "127.0.0.1");
    will_return(__wrap_OS_IsAllowedIP, 0);

    expect_string(__wrap_ReadSecMSG, buffer, "#!-agent startup ");
    will_return(__wrap_ReadSecMSG, message.size);
    will_return(__wrap_ReadSecMSG, "#!-agent startup ");
    will_return(__wrap_ReadSecMSG, KS_VALID);

    expect_value(__wrap_rem_getCounter, fd, 1);
    will_return(__wrap_rem_getCounter, 10);

    expect_value(__wrap_time, time, 0);
    will_return(__wrap_time, (time_t)123456789);

    expect_value(__wrap_rem_getCounter, fd, 1);
    will_return(__wrap_rem_getCounter, 10);


    expect_value(__wrap_OS_AddSocket, i, 0);
    expect_value(__wrap_OS_AddSocket, sock, message.sock);
    will_return(__wrap_OS_AddSocket, 2);

    expect_string(__wrap__mdebug2, formatted_msg, "TCP socket 1 added to keystore.");

    expect_function_call(__wrap_key_unlock);

    expect_string(__wrap_save_controlmsg, msg, "agent startup ");
    expect_value(__wrap_save_controlmsg, msg_length, message.size - 3);

    expect_string(__wrap_rem_inc_recv_ctrl, agent_id, key->id);

    HandleSecureMessage(&message, &wdb_sock);

    os_free(key->id);
    os_free(key);
    os_free(keyentries);
}


void test_HandleSecureMessage_OldMessage_NoShutdownMessage(void **state)
{
    char buffer[OS_MAXSTR + 1] = "#!-agent startup ";
    message_t message = { .buffer = buffer, .size = 17, .sock = 1, .counter = 5 };
    struct sockaddr_in peer_info;
    int wdb_sock;

    keyentry** keyentries;
    os_calloc(1, sizeof(keyentry*), keyentries);
    keys.keyentries = keyentries;

    keyentry *key = NULL;
    os_calloc(1, sizeof(keyentry), key);

    key->id = strdup("009");
    key->sock = 1;
    key->keyid = 1;

    keys.keyentries[0] = key;

    global_counter = 0;

    peer_info.sin_family = AF_INET;
    peer_info.sin_addr.s_addr = 0x0100007F;
    memcpy(&message.addr, &peer_info, sizeof(peer_info));

    expect_function_call(__wrap_key_lock_read);

    expect_string(__wrap_OS_IsAllowedIP, srcip, "127.0.0.1");
    will_return(__wrap_OS_IsAllowedIP, 0);

    expect_string(__wrap_ReadSecMSG, buffer, "#!-agent startup ");
    will_return(__wrap_ReadSecMSG, message.size);
    will_return(__wrap_ReadSecMSG, "#!-agent startup ");
    will_return(__wrap_ReadSecMSG, KS_VALID);

    expect_value(__wrap_rem_getCounter, fd, 1);
    will_return(__wrap_rem_getCounter, 10);

    expect_function_call(__wrap_key_unlock);
    HandleSecureMessage(&message, &wdb_sock);

    os_free(key->id);
    os_free(key);
    os_free(keyentries);
}

void test_HandleSecureMessage_unvalid_message(void **state)
{
    char buffer[OS_MAXSTR + 1] = "!1234!";
    message_t message = { .buffer = buffer, .size = 6, .sock = 1};
    struct sockaddr_in peer_info;
    int wdb_sock;

    keyentry** keyentries;
    os_calloc(1, sizeof(keyentry*), keyentries);
    keys.keyentries = keyentries;

    keyentry *key = NULL;
    os_calloc(1, sizeof(keyentry), key);

    key->id = strdup("001");
    key->sock = 1;
    key->keyid = 1;

    keys.keyentries[0] = key;

    global_counter = 0;

    peer_info.sin_family = AF_INET;
    peer_info.sin_addr.s_addr = 0x0100007F;
    memcpy(&message.addr, &peer_info, sizeof(peer_info));

    expect_function_call(__wrap_key_lock_read);

    // OS_IsAllowedDynamicID
    expect_string(__wrap_OS_IsAllowedDynamicID, id, "1234");
    expect_string(__wrap_OS_IsAllowedDynamicID, srcip, "127.0.0.1");
    will_return(__wrap_OS_IsAllowedDynamicID, 0);

    expect_string(__wrap__mwarn, formatted_msg, "Received message is empty");

    expect_function_call(__wrap_key_unlock);

    expect_function_call(__wrap_key_lock_read);

    // OS_DeleteSocket
    expect_value(__wrap_OS_DeleteSocket, sock, message.sock);
    will_return(__wrap_OS_DeleteSocket, 0);

    expect_function_call(__wrap_key_unlock);

    will_return(__wrap_close, 0);

    // nb_close
    expect_value(__wrap_nb_close, sock, message.sock);
    expect_value(__wrap_nb_close, sock, message.sock);
    expect_function_call(__wrap_rem_dec_tcp);

    // rem_setCounter
    expect_value(__wrap_rem_setCounter, fd, 1);
    expect_value(__wrap_rem_setCounter, counter, 0);

    expect_string(__wrap__mdebug1, formatted_msg, "TCP peer disconnected [1]");

    expect_function_call(__wrap_rem_inc_recv_unknown);

    HandleSecureMessage(&message, &wdb_sock);

    os_free(key->id);
    os_free(key);
    os_free(keyentries);
}

void test_HandleSecureMessage_different_sock(void **state)
{
    char buffer[OS_MAXSTR + 1] = "!12!";
    message_t message = { .buffer = buffer, .size = 4, .sock = 1};
    struct sockaddr_in peer_info;
    int wdb_sock;

    keyentry** keyentries;
    os_calloc(1, sizeof(keyentry*), keyentries);
    keys.keyentries = keyentries;

    keyentry *key = NULL;
    os_calloc(1, sizeof(keyentry), key);

    key->id = strdup("001");
    key->sock = 4;
    key->keyid = 1;

    keys.keyentries[0] = key;

    global_counter = 0;

    peer_info.sin_family = AF_INET;
    peer_info.sin_addr.s_addr = inet_addr("127.0.0.1");
    memcpy(&message.addr, &peer_info, sizeof(peer_info));

    expect_function_call(__wrap_key_lock_read);

    // OS_IsAllowedDynamicID
    expect_string(__wrap_OS_IsAllowedDynamicID, id, "12");
    expect_string(__wrap_OS_IsAllowedDynamicID, srcip, "127.0.0.1");
    will_return(__wrap_OS_IsAllowedDynamicID, 0);

    expect_function_call(__wrap_key_unlock);

    expect_string(__wrap__mwarn, formatted_msg, "Agent key already in use: agent ID '001'");

    expect_function_call(__wrap_key_lock_read);

    // OS_DeleteSocket
    expect_value(__wrap_OS_DeleteSocket, sock, message.sock);
    will_return(__wrap_OS_DeleteSocket, 0);

    expect_function_call(__wrap_key_unlock);

    will_return(__wrap_close, 0);

    // nb_close
    expect_value(__wrap_nb_close, sock, message.sock);
    expect_value(__wrap_nb_close, sock, message.sock);
    expect_function_call(__wrap_rem_dec_tcp);

    // rem_setCounter
    expect_value(__wrap_rem_setCounter, fd, 1);
    expect_value(__wrap_rem_setCounter, counter, 0);

    expect_string(__wrap__mdebug1, formatted_msg, "TCP peer disconnected [1]");

    expect_function_call(__wrap_rem_inc_recv_unknown);

    HandleSecureMessage(&message, &wdb_sock);

    os_free(key->id);
    os_free(key);
    os_free(keyentries);
}

void test_HandleSecureMessage_different_sock_2(void **state)
{
    char buffer[OS_MAXSTR + 1] = "12!";
    message_t message = { .buffer = buffer, .size = 4, .sock = 1};
    struct sockaddr_in peer_info;
    int wdb_sock;

    keyentry** keyentries;
    os_calloc(1, sizeof(keyentry*), keyentries);
    keys.keyentries = keyentries;

    keyentry *key = NULL;
    os_calloc(1, sizeof(keyentry), key);

    key->id = strdup("001");
    key->sock = 4;
    key->keyid = 1;

    keys.keyentries[0] = key;

    global_counter = 0;

    peer_info.sin_family = AF_INET;
    peer_info.sin_addr.s_addr = inet_addr("127.0.0.1");
    memcpy(&message.addr, &peer_info, sizeof(peer_info));

    expect_function_call(__wrap_key_lock_read);

    // OS_IsAllowedDynamicID
    expect_string(__wrap_OS_IsAllowedIP, srcip, "127.0.0.1");
    will_return(__wrap_OS_IsAllowedIP, 0);

    expect_function_call(__wrap_key_unlock);

    expect_string(__wrap__mwarn, formatted_msg, "Agent key already in use: agent ID '001'");

    expect_function_call(__wrap_key_lock_read);

    // OS_DeleteSocket
    expect_value(__wrap_OS_DeleteSocket, sock, message.sock);
    will_return(__wrap_OS_DeleteSocket, 0);

    expect_function_call(__wrap_key_unlock);

    will_return(__wrap_close, 0);

    // nb_close
    expect_value(__wrap_nb_close, sock, message.sock);
    expect_value(__wrap_nb_close, sock, message.sock);
    expect_function_call(__wrap_rem_dec_tcp);

    // rem_setCounter
    expect_value(__wrap_rem_setCounter, fd, 1);
    expect_value(__wrap_rem_setCounter, counter, 0);

    expect_string(__wrap__mdebug1, formatted_msg, "TCP peer disconnected [1]");

    expect_function_call(__wrap_rem_inc_recv_unknown);

    HandleSecureMessage(&message, &wdb_sock);

    os_free(key->id);
    os_free(key);
    os_free(keyentries);
}

void test_handle_new_tcp_connection_success(void **state)
{
    struct sockaddr_in peer_info;
    int sock_client = 12;

    peer_info.sin_family = AF_INET;
    peer_info.sin_addr.s_addr = 0x0A00A8C0;

    will_return(__wrap_accept, AF_INET);
    will_return(__wrap_accept, sock_client);

    // nb_open
    expect_value(__wrap_nb_open, sock, sock_client);
    expect_value(__wrap_nb_open, peer_info, (struct sockaddr_storage *)&peer_info);
    expect_value(__wrap_nb_open, sock, sock_client);
    expect_value(__wrap_nb_open, peer_info, (struct sockaddr_storage *)&peer_info);

    expect_function_call(__wrap_rem_inc_tcp);

    expect_string(__wrap__mdebug1, formatted_msg, "New TCP connection [12]");

    // wnotify_add
    expect_value(__wrap_wnotify_add, notify, notify);
    expect_value(__wrap_wnotify_add, fd, sock_client);
    expect_value(__wrap_wnotify_add, op, WO_READ);
    will_return(__wrap_wnotify_add, 0);

    handle_new_tcp_connection(notify, (struct sockaddr_storage *)&peer_info);
}

void test_handle_new_tcp_connection_wnotify_fail(void **state)
{
    struct sockaddr_in peer_info;
    int sock_client = 12;

    peer_info.sin_family = AF_INET;
    peer_info.sin_addr.s_addr = 0x0A00A8C0;

    will_return(__wrap_accept, AF_INET);
    will_return(__wrap_accept, sock_client);

    // nb_open
    expect_value(__wrap_nb_open, sock, sock_client);
    expect_value(__wrap_nb_open, peer_info, (struct sockaddr_storage *)&peer_info);
    expect_value(__wrap_nb_open, sock, sock_client);
    expect_value(__wrap_nb_open, peer_info, (struct sockaddr_storage *)&peer_info);

    expect_function_call(__wrap_rem_inc_tcp);

    expect_string(__wrap__mdebug1, formatted_msg, "New TCP connection [12]");

    // wnotify_add
    expect_value(__wrap_wnotify_add, notify, notify);
    expect_value(__wrap_wnotify_add, fd, sock_client);
    expect_value(__wrap_wnotify_add, op, WO_READ);
    will_return(__wrap_wnotify_add, -1);

    expect_string(__wrap__merror, formatted_msg, "wnotify_add(0, 12): Success (0)");

    expect_function_call(__wrap_key_lock_read);

    // OS_DeleteSocket
    expect_value(__wrap_OS_DeleteSocket, sock, sock_client);
    will_return(__wrap_OS_DeleteSocket, 0);

    expect_function_call(__wrap_key_unlock);

    will_return(__wrap_close, 0);

    // nb_close
    expect_value(__wrap_nb_close, sock, sock_client);
    expect_value(__wrap_nb_close, sock, sock_client);
    expect_function_call(__wrap_rem_dec_tcp);

    // rem_setCounter
    expect_value(__wrap_rem_setCounter, fd, sock_client);
    expect_value(__wrap_rem_setCounter, counter, 0);

    expect_string(__wrap__mdebug1, formatted_msg, "TCP peer disconnected [12]");

    handle_new_tcp_connection(notify, (struct sockaddr_storage *)&peer_info);
}

void test_handle_new_tcp_connection_socket_fail(void **state)
{
    struct sockaddr_in peer_info;
    int sock_client = 12;

    peer_info.sin_family = AF_INET;
    peer_info.sin_addr.s_addr = 0x0A00A8C0;

    will_return(__wrap_accept, AF_INET);
    will_return(__wrap_accept, -1);
    errno = -1;
    expect_string(__wrap__merror, formatted_msg, "(1242): Couldn't accept TCP connections: Unknown error -1 (-1)");

    handle_new_tcp_connection(notify, (struct sockaddr_storage *)&peer_info);
}

void test_handle_new_tcp_connection_socket_fail_err(void **state)
{
    struct sockaddr_in peer_info;
    int sock_client = 12;

    peer_info.sin_family = AF_INET;
    peer_info.sin_addr.s_addr = 0x0A00A8C0;

    will_return(__wrap_accept, AF_INET);
    will_return(__wrap_accept, -1);
    errno = ECONNABORTED;
    expect_string(__wrap__mdebug1, formatted_msg, "(1242): Couldn't accept TCP connections: Software caused connection abort (103)");

    handle_new_tcp_connection(notify, (struct sockaddr_storage *)&peer_info);
}

void test_handle_incoming_data_from_udp_socket_0(void **state)
{
    struct sockaddr_in peer_info;
    logr.udp_sock = 1;

    peer_info.sin_family = AF_INET;
    peer_info.sin_addr.s_addr = 0x0A00A8C0;

    will_return(__wrap_recvfrom, 0);

    handle_incoming_data_from_udp_socket((struct sockaddr_storage *)&peer_info);
}

void test_handle_incoming_data_from_udp_socket_success(void **state)
{
    struct sockaddr_in peer_info;
    logr.udp_sock = 1;

    peer_info.sin_family = AF_INET;
    peer_info.sin_addr.s_addr = 0x0A00A8C0;

    will_return(__wrap_recvfrom, 10);

    expect_value(__wrap_rem_msgpush, size, 10);
    expect_value(__wrap_rem_msgpush, addr, (struct sockaddr_storage *)&peer_info);
    expect_value(__wrap_rem_msgpush, sock, USING_UDP_NO_CLIENT_SOCKET);
    will_return(__wrap_rem_msgpush, 0);

    expect_value(__wrap_rem_add_recv, bytes, 10);

    handle_incoming_data_from_udp_socket((struct sockaddr_storage *)&peer_info);
}

void test_handle_incoming_data_from_tcp_socket_too_big_message(void **state)
{
    int sock_client = 8;

    expect_value(__wrap_nb_recv, sock, sock_client);
    will_return(__wrap_nb_recv, -2);

    expect_string(__wrap__mwarn, formatted_msg, "Too big message size from socket [8].");

    expect_function_call(__wrap_key_lock_read);

    // OS_DeleteSocket
    expect_value(__wrap_OS_DeleteSocket, sock, sock_client);
    will_return(__wrap_OS_DeleteSocket, 0);

    expect_function_call(__wrap_key_unlock);

    will_return(__wrap_close, 0);

    // nb_close
    expect_value(__wrap_nb_close, sock, sock_client);
    expect_value(__wrap_nb_close, sock, sock_client);
    expect_function_call(__wrap_rem_dec_tcp);

    // rem_setCounter
    expect_value(__wrap_rem_setCounter, fd, sock_client);
    expect_value(__wrap_rem_setCounter, counter, 0);

    expect_string(__wrap__mdebug1, formatted_msg, "TCP peer disconnected [8]");

    handle_incoming_data_from_tcp_socket(sock_client);
}

void test_handle_incoming_data_from_tcp_socket_case_0(void **state)
{
    int sock_client = 7;

    expect_value(__wrap_nb_recv, sock, sock_client);
    will_return(__wrap_nb_recv, 0);

    expect_string(__wrap__mdebug1, formatted_msg, "handle incoming close socket [7].");

    expect_function_call(__wrap_key_lock_read);

    // OS_DeleteSocket
    expect_value(__wrap_OS_DeleteSocket, sock, sock_client);
    will_return(__wrap_OS_DeleteSocket, 0);

    expect_function_call(__wrap_key_unlock);

    will_return(__wrap_close, 0);

    // nb_close
    expect_value(__wrap_nb_close, sock, sock_client);
    expect_value(__wrap_nb_close, sock, sock_client);
    expect_function_call(__wrap_rem_dec_tcp);

    // rem_setCounter
    expect_value(__wrap_rem_setCounter, fd, sock_client);
    expect_value(__wrap_rem_setCounter, counter, 0);

    expect_string(__wrap__mdebug1, formatted_msg, "TCP peer disconnected [7]");

    handle_incoming_data_from_tcp_socket(sock_client);
}

void test_handle_incoming_data_from_tcp_socket_case_1(void **state)
{
    int sock_client = 7;

    expect_value(__wrap_nb_recv, sock, sock_client);
    will_return(__wrap_nb_recv, -1);

    errno = ETIMEDOUT;

    expect_string(__wrap__mdebug1, formatted_msg, "TCP peer [7]: Connection timed out (110)");

    expect_string(__wrap__mdebug1, formatted_msg, "handle incoming close socket [7].");

    expect_function_call(__wrap_key_lock_read);

    // OS_DeleteSocket
    expect_value(__wrap_OS_DeleteSocket, sock, sock_client);
    will_return(__wrap_OS_DeleteSocket, 0);

    expect_function_call(__wrap_key_unlock);

    will_return(__wrap_close, 0);

    // nb_close
    expect_value(__wrap_nb_close, sock, sock_client);
    expect_value(__wrap_nb_close, sock, sock_client);
    expect_function_call(__wrap_rem_dec_tcp);

    // rem_setCounter
    expect_value(__wrap_rem_setCounter, fd, sock_client);
    expect_value(__wrap_rem_setCounter, counter, 0);

    expect_string(__wrap__mdebug1, formatted_msg, "TCP peer disconnected [7]");

    handle_incoming_data_from_tcp_socket(sock_client);
}

void test_handle_incoming_data_from_tcp_socket_success(void **state)
{
    int sock_client = 12;

    expect_value(__wrap_nb_recv, sock, sock_client);
    will_return(__wrap_nb_recv, 100);

    expect_value(__wrap_rem_add_recv, bytes, 100);

    handle_incoming_data_from_tcp_socket(sock_client);
}

void test_handle_outgoing_data_to_tcp_socket_case_1_EAGAIN(void **state)
{
    int sock_client = 10;

    expect_value(__wrap_nb_send, sock, sock_client);
    will_return(__wrap_nb_send, -1);

    errno = EAGAIN;

    expect_string(__wrap__mdebug1, formatted_msg, "TCP peer [10]: Resource temporarily unavailable (11)");

    handle_outgoing_data_to_tcp_socket(sock_client);
}

void test_handle_outgoing_data_to_tcp_socket_case_1_EPIPE(void **state)
{
    int sock_client = 10;

    expect_value(__wrap_nb_send, sock, sock_client);
    will_return(__wrap_nb_send, -1);

    errno = EPIPE;

    expect_string(__wrap__mdebug1, formatted_msg, "TCP peer [10]: Broken pipe (32)");

    expect_string(__wrap__mdebug1, formatted_msg, "handle outgoing close socket [10].");

    expect_function_call(__wrap_key_lock_read);

    // OS_DeleteSocket
    expect_value(__wrap_OS_DeleteSocket, sock, sock_client);
    will_return(__wrap_OS_DeleteSocket, 0);

    expect_function_call(__wrap_key_unlock);

    will_return(__wrap_close, 0);

    // nb_close
    expect_value(__wrap_nb_close, sock, sock_client);
    expect_value(__wrap_nb_close, sock, sock_client);
    expect_function_call(__wrap_rem_dec_tcp);

    // rem_setCounter
    expect_value(__wrap_rem_setCounter, fd, sock_client);
    expect_value(__wrap_rem_setCounter, counter, 0);

    expect_string(__wrap__mdebug1, formatted_msg, "TCP peer disconnected [10]");

    handle_outgoing_data_to_tcp_socket(sock_client);
}

void test_handle_outgoing_data_to_tcp_socket_success(void **state)
{
    int sock_client = 10;

    expect_value(__wrap_nb_send, sock, sock_client);
    will_return(__wrap_nb_send, 100);

    expect_value(__wrap_rem_add_send, bytes, 100);

    handle_outgoing_data_to_tcp_socket(sock_client);
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        // Tests close_fp_main
        cmocka_unit_test_setup_teardown(test_close_fp_main_queue_empty, setup_config, teardown_config),
        cmocka_unit_test_setup_teardown(test_close_fp_main_first_node_no_close_first, setup_config, teardown_config),
        cmocka_unit_test_setup_teardown(test_close_fp_main_close_first, setup_config, teardown_config),
        cmocka_unit_test_setup_teardown(test_close_fp_main_close_first_queue_2, setup_config, teardown_config),
        cmocka_unit_test_setup_teardown(test_close_fp_main_close_first_queue_2_close_2, setup_config, teardown_config),
        cmocka_unit_test_setup_teardown(test_close_fp_main_close_fp_null, setup_config, teardown_config),
        // Tests HandleSecureMessage
        cmocka_unit_test(test_HandleSecureMessage_unvalid_message),
        cmocka_unit_test(test_HandleSecureMessage_shutdown_message),
        cmocka_unit_test(test_HandleSecureMessage_NewMessage_NoShutdownMessage),
        cmocka_unit_test(test_HandleSecureMessage_OldMessage_NoShutdownMessage),
        cmocka_unit_test(test_HandleSecureMessage_different_sock),
        cmocka_unit_test(test_HandleSecureMessage_different_sock_2),
        // Tests handle_new_tcp_connection
        cmocka_unit_test_setup_teardown(test_handle_new_tcp_connection_success, setup_new_tcp, teardown_new_tcp),
        cmocka_unit_test_setup_teardown(test_handle_new_tcp_connection_wnotify_fail, setup_new_tcp, teardown_new_tcp),
        cmocka_unit_test_setup_teardown(test_handle_new_tcp_connection_socket_fail, setup_new_tcp, teardown_new_tcp),
        cmocka_unit_test_setup_teardown(test_handle_new_tcp_connection_socket_fail_err, setup_new_tcp, teardown_new_tcp),
        // Tests handle_incoming_data_from_udp_socket
        cmocka_unit_test(test_handle_incoming_data_from_udp_socket_0),
        cmocka_unit_test(test_handle_incoming_data_from_udp_socket_success),
        // Tests handle_incoming_data_from_tcp_socket
        cmocka_unit_test(test_handle_incoming_data_from_tcp_socket_too_big_message),
        cmocka_unit_test(test_handle_incoming_data_from_tcp_socket_case_0),
        cmocka_unit_test(test_handle_incoming_data_from_tcp_socket_case_1),
        cmocka_unit_test(test_handle_incoming_data_from_tcp_socket_success),
        // Tests handle_outgoing_data_to_tcp_socket
        cmocka_unit_test(test_handle_outgoing_data_to_tcp_socket_case_1_EAGAIN),
        cmocka_unit_test(test_handle_outgoing_data_to_tcp_socket_case_1_EPIPE),
        cmocka_unit_test(test_handle_outgoing_data_to_tcp_socket_success),

        };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
