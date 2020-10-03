/*
Copyright (c) 2020 Roger Light <roger@atchoo.org>

All rights reserved. This program and the accompanying materials
are made available under the terms of the Eclipse Public License v1.0
and Eclipse Distribution License v1.0 which accompany this distribution.

The Eclipse Public License is available at
   http://www.eclipse.org/legal/epl-v10.html
and the Eclipse Distribution License is available at
  http://www.eclipse.org/org/documents/edl-v10.php.

Contributors:
   Roger Light - initial implementation and documentation.
*/

#include "mosquitto_broker_internal.h"
#include "mosquitto_broker.h"
#include "mosquitto_plugin.h"
#include "mosquitto.h"

#include <CUnit/CUnit.h>
#include <CUnit/Basic.h>

#include <ctype.h>
#include <stdio.h>
#include <cJSON.h>
#include <uthash.h>
#include <utlist.h>
#include <time.h>

#define ANSI_RED "\e[0;31m"
#define ANSI_GREEN "\e[0;32m"
#define ANSI_YELLOW "\e[0;33m"
#define ANSI_BLUE "\e[0;34m"
#define ANSI_MAGENTA "\e[0;35m"
#define ANSI_CYAN "\e[0;36m"
#define ANSI_WHITE "\e[0;37m"
#define ANSI_RESET "\e[0m"

/* 00000000-0000-0000-0000-000000000000 */

struct expected_publish{
	struct expected_publish *prev, *next;

	char topic[200];
	char *payload;
	bool random;
};

static struct expected_publish *expected_publishes = NULL;
const char room_uuid[] = "00000000-0000-0000-0000-000000000000";
const char room_uuid2[] = "00000000-0000-0000-0000-000000000001";
const char player1_uuid[] = "00000000-0000-0000-0000-000000000001";
const char player2_uuid[] = "00000000-0000-0000-0000-000000000002";
const char player3_uuid[] = "00000000-0000-0000-0000-000000000003";
const char player1_name[] = "Player 1";
const char player2_name[] = "Player 2";
const char player3_name[] = "Player 3";
char player1_payload[1000];
char player2_payload[1000];
char player3_payload[1000];
int publ = 0;
int random_count = 0;

struct mosquitto client1, client2, client3;

void check_expected_publish(const char *topic, int payloadlen, const char *payload);

/* ======================================================================/
 *
 * Replacement functions
 *
 * ====================================================================== */

int RAND_bytes(unsigned char bytes, int count)
{
	random_count += count;
	return count;
}

const char *mosquitto_client_id(const struct mosquitto *client)
{
	if(client == &client1){
		return player1_name;
	}else if(client == &client2){
		return player2_name;
	}else if(client == &client3){
		return player3_name;
	}else{
		return "unknown";
	}
}


int mosquitto_broker_publish_copy(
		const char *client_id,
		const char *topic,
		int payloadlen,
		const void *payload,
		int qos,
		bool retain,
		mosquitto_property *properties)
{
	// FIXME - implement queue of messages for comparison
	struct mosquitto_acl_msg msg;

	check_expected_publish(topic, payloadlen, payload);

	memset(&msg, 0, sizeof(struct mosquitto_acl_msg));
	msg.topic = topic;
	msg.payloadlen = payloadlen;
	msg.payload = payload;
	msg.qos = qos;
	msg.retain = retain;
	mosquitto_auth_acl_check(NULL, MOSQ_ACL_READ, NULL, &msg);
	publ++;
	return 0;
}

int mosquitto_broker_publish(
		const char *client_id,
		const char *topic,
		int payloadlen,
		void *payload,
		int qos,
		bool retain,
		mosquitto_property *properties)
{
	int rc;

	rc = mosquitto_broker_publish_copy( client_id, topic,
			payloadlen, payload, qos, retain, properties);
	free(payload);
	return rc;
}




/* ======================================================================/
 *
 * Helper functions
 *
 * ====================================================================== */

void add_expected_publish(const char *topic_cmd, const char *payload, bool random)
{
	struct expected_publish *ep;

	ep = malloc(sizeof(struct expected_publish));
	snprintf(ep->topic, sizeof(ep->topic), "tfdg/%s/%s", room_uuid, topic_cmd);
	ep->payload = strdup(payload);
	ep->random = random;
	DL_APPEND(expected_publishes, ep);
}


void check_expected_publish(const char *topic, int payloadlen, const char *payload)
{
	struct expected_publish *ep;

	CU_ASSERT_PTR_NOT_NULL(expected_publishes);
	if(expected_publishes == NULL){
		printf("%s || %s\n", topic, payload);
		return;
	}

	ep = expected_publishes;
	DL_DELETE(expected_publishes, ep);
	CU_ASSERT_STRING_EQUAL(topic, ep->topic);
	if(strcmp(topic, ep->topic)){
		printf("%s || %s\n", topic, ep->topic);
	}
	if(ep->random == false){
		CU_ASSERT_NSTRING_EQUAL(payload, ep->payload, payloadlen);
		if(strncmp(payload, ep->payload, payloadlen)){
			printf("%s\n%s\n", payload, ep->payload);
		}
	}
	free(ep->payload);
	free(ep);
}


void easy_acl_check(const char *room, struct mosquitto *client, const char *topic_cmd, const char *payload, int mode)
{
	int rc;
	char topic[1000];
	struct mosquitto_acl_msg msg;

	memset(&msg, 0, sizeof(struct mosquitto_acl_msg));
	snprintf(topic, sizeof(topic), "tfdg/%s/%s", room, topic_cmd);

	msg.topic = topic;
	msg.payload = payload;
	msg.payloadlen = strlen(payload);

	rc = mosquitto_auth_acl_check(NULL, mode, client, &msg);
	CU_ASSERT_EQUAL(rc, MOSQ_ERR_ACL_DENIED);
}

/* ======================================================================/
 *
 * Tests
 *
 * ====================================================================== */

void TEST_non_tfdg_topic(void)
{
	struct mosquitto client;
	struct mosquitto_acl_msg msg;
	char topic[1000];
	char payload[1000];
	int rc;

	unlink("tfdg-state.json");
	mosquitto_auth_plugin_init(NULL, NULL, 0);

	memset(&client, 0, sizeof(struct mosquitto));
	memset(&msg, 0, sizeof(struct mosquitto_acl_msg));
	msg.topic = topic;
	msg.payload = payload;

	snprintf(topic, sizeof(topic), "123456/7890");
	msg.payloadlen = snprintf(payload, sizeof(payload), "{\"name\":\"%s\", \"uuid\":\"%s\"}", player1_name, player1_uuid);
	rc = mosquitto_auth_acl_check(NULL, MOSQ_ACL_READ, &client, &msg);
	CU_ASSERT_EQUAL(rc, MOSQ_ERR_PLUGIN_DEFER);

	mosquitto_auth_plugin_cleanup(NULL, NULL, 0);
}


void TEST_subscribe_success(void)
{
	struct mosquitto_acl_msg msg;
	char topic[1000];
	int rc;

	unlink("tfdg-state.json");
	mosquitto_auth_plugin_init(NULL, NULL, 0);

	memset(&msg, 0, sizeof(struct mosquitto_acl_msg));
	msg.topic = topic;
	msg.payload = player1_payload;
	msg.payloadlen = strlen(player1_payload);

	snprintf(topic, sizeof(topic), "tfdg/#");
	rc = mosquitto_auth_acl_check(NULL, MOSQ_ACL_SUBSCRIBE, &client1, &msg);
	CU_ASSERT_EQUAL(rc, MOSQ_ERR_SUCCESS);

	mosquitto_auth_plugin_cleanup(NULL, NULL, 0);
}


void TEST_subscribe_fail(void)
{
	struct mosquitto_acl_msg msg;
	char topic[1000];
	int rc;

	unlink("tfdg-state.json");
	mosquitto_auth_plugin_init(NULL, NULL, 0);

	memset(&msg, 0, sizeof(struct mosquitto_acl_msg));
	msg.topic = topic;
	msg.payload = player1_payload;
	msg.payloadlen = strlen(player1_payload);

	snprintf(topic, sizeof(topic), "tfdg/%s/login", room_uuid);
	rc = mosquitto_auth_acl_check(NULL, MOSQ_ACL_SUBSCRIBE, &client1, &msg);
	CU_ASSERT_EQUAL(rc, MOSQ_ERR_ACL_DENIED);

	mosquitto_auth_plugin_cleanup(NULL, NULL, 0);
}


void TEST_topic_tokenise(void)
{
	struct mosquitto_acl_msg msg;
	char topic[1000];
	int rc;

	add_expected_publish("lobby-players",
			"{\"players\":[{\"name\":\"Player 1\",\"uuid\":\"00000000-0000-0000-0000-000000000001\"}],"
			"\"options\":{\"losers-see-dice\":true,\"allow-calza\":true,\"max-dice\":5,\"max-dice-value\":6,\"show-results-table\":true}}",
			false);

	add_expected_publish("host", player1_payload, false);

	unlink("tfdg-state.json");
	mosquitto_auth_plugin_init(NULL, NULL, 0);

	memset(&msg, 0, sizeof(struct mosquitto_acl_msg));
	msg.topic = topic;
	msg.payload = player1_payload;
	msg.payloadlen = strlen(player1_payload);

	snprintf(topic, sizeof(topic), "tfdg/no-room");
	rc = mosquitto_auth_acl_check(NULL, MOSQ_ACL_WRITE, &client1, &msg);
	CU_ASSERT_EQUAL(rc, MOSQ_ERR_ACL_DENIED);

	snprintf(topic, sizeof(topic), "tfdg/%s/login", room_uuid);
	rc = mosquitto_auth_acl_check(NULL, MOSQ_ACL_WRITE, &client1, &msg);
	CU_ASSERT_EQUAL(rc, MOSQ_ERR_ACL_DENIED);

	snprintf(topic, sizeof(topic), "tfdg/bad-room/login");
	rc = mosquitto_auth_acl_check(NULL, MOSQ_ACL_WRITE, &client1, &msg);
	CU_ASSERT_EQUAL(rc, MOSQ_ERR_ACL_DENIED);

	snprintf(topic, sizeof(topic), "tfdg/%s/login/overlong", room_uuid);
	rc = mosquitto_auth_acl_check(NULL, MOSQ_ACL_WRITE, &client1, &msg);
	CU_ASSERT_EQUAL(rc, MOSQ_ERR_ACL_DENIED);

	mosquitto_auth_plugin_cleanup(NULL, NULL, 0);
}


void TEST_single_login_bad_payload(void)
{
	char payload[1000];

	unlink("tfdg-state.json");
	mosquitto_auth_plugin_init(NULL, NULL, 0);

	snprintf(payload, sizeof(payload), "{\"name\":\"%s\"}", player1_name);
	easy_acl_check(room_uuid, &client1, "login", payload, MOSQ_ACL_WRITE);

	snprintf(payload, sizeof(payload), "{\"uuid\":\"%s\"}", player1_uuid);
	easy_acl_check(room_uuid, &client1, "login", payload, MOSQ_ACL_WRITE);

	mosquitto_auth_plugin_cleanup(NULL, NULL, 0);
}


void TEST_single_login_login_logout_logout(void)
{
	unlink("tfdg-state.json");
	mosquitto_auth_plugin_init(NULL, NULL, 0);

	add_expected_publish("lobby-players",
			"{\"players\":[{\"name\":\"Player 1\",\"uuid\":\"00000000-0000-0000-0000-000000000001\"}],"
			"\"options\":{\"losers-see-dice\":true,\"allow-calza\":true,\"max-dice\":5,\"max-dice-value\":6,\"show-results-table\":true}}",
			false);
	add_expected_publish("host", player1_payload, false);

	add_expected_publish("lobby-players",
			"{\"players\":[{\"name\":\"Player 1\",\"uuid\":\"00000000-0000-0000-0000-000000000001\"}],"
			"\"options\":{\"losers-see-dice\":true,\"allow-calza\":true,\"max-dice\":5,\"max-dice-value\":6,\"show-results-table\":true}}",
			false);
	add_expected_publish("host", player1_payload, false);

	easy_acl_check(room_uuid, &client1, "login",  player1_payload, MOSQ_ACL_WRITE);
	// FIXME easy_acl_check(room_uuid, &client2, "login",  player2_payload, MOSQ_ACL_WRITE);
	easy_acl_check(room_uuid, &client1, "login",  player1_payload, MOSQ_ACL_WRITE);
	easy_acl_check(room_uuid, &client1, "logout", player1_payload, MOSQ_ACL_WRITE);
	easy_acl_check(room_uuid, &client1, "logout", player1_payload, MOSQ_ACL_WRITE);
	// FIXME easy_acl_check(room_uuid, &client2, "logout", player2_payload, MOSQ_ACL_WRITE);

	mosquitto_auth_plugin_cleanup(NULL, NULL, 0);
}


void TEST_single_login_logout(void)
{
	unlink("tfdg-state.json");
	mosquitto_auth_plugin_init(NULL, NULL, 0);

	add_expected_publish("lobby-players",
			"{\"players\":[{\"name\":\"Player 1\",\"uuid\":\"00000000-0000-0000-0000-000000000001\"}],"
			"\"options\":{\"losers-see-dice\":true,\"allow-calza\":true,\"max-dice\":5,\"max-dice-value\":6,\"show-results-table\":true}}",
			false);
	add_expected_publish("host", player1_payload, false);

	easy_acl_check(room_uuid, &client1, "login",  player1_payload, MOSQ_ACL_WRITE);
	easy_acl_check(room_uuid, &client1, "logout", player1_payload, MOSQ_ACL_WRITE);

	mosquitto_auth_plugin_cleanup(NULL, NULL, 0);
}


void TEST_single_login_logout_logout(void)
{
	unlink("tfdg-state.json");
	mosquitto_auth_plugin_init(NULL, NULL, 0);

	add_expected_publish("lobby-players",
			"{\"players\":[{\"name\":\"Player 1\",\"uuid\":\"00000000-0000-0000-0000-000000000001\"}],"
			"\"options\":{\"losers-see-dice\":true,\"allow-calza\":true,\"max-dice\":5,\"max-dice-value\":6,\"show-results-table\":true}}",
			false);
	add_expected_publish("host", player1_payload, false);

	easy_acl_check(room_uuid, &client1, "login",  player1_payload, MOSQ_ACL_WRITE);
	easy_acl_check(room_uuid, &client1, "logout", player1_payload, MOSQ_ACL_WRITE);
	easy_acl_check(room_uuid, &client1, "logout", player1_payload, MOSQ_ACL_WRITE);

	mosquitto_auth_plugin_cleanup(NULL, NULL, 0);
}


void TEST_single_login_leave_game_logout(void)
{
	unlink("tfdg-state.json");
	mosquitto_auth_plugin_init(NULL, NULL, 0);

	add_expected_publish("lobby-players",
			"{\"players\":[{\"name\":\"Player 1\",\"uuid\":\"00000000-0000-0000-0000-000000000001\"}],"
			"\"options\":{\"losers-see-dice\":true,\"allow-calza\":true,\"max-dice\":5,\"max-dice-value\":6,\"show-results-table\":true}}",
			false);
	add_expected_publish("host", player1_payload, false);

	easy_acl_check(room_uuid, &client1, "login",      player1_payload, MOSQ_ACL_WRITE);
	easy_acl_check(room_uuid, &client1, "leave-game", player1_payload, MOSQ_ACL_WRITE);
	easy_acl_check(room_uuid, &client1, "logout",     player1_payload, MOSQ_ACL_WRITE);

	mosquitto_auth_plugin_cleanup(NULL, NULL, 0);
}


void TEST_set_option_non_matching_player(void)
{
	char payload[1000];

	unlink("tfdg-state.json");
	mosquitto_auth_plugin_init(NULL, NULL, 0);

	add_expected_publish("lobby-players",
			"{\"players\":[{\"name\":\"Player 1\",\"uuid\":\"00000000-0000-0000-0000-000000000001\"}],"
			"\"options\":{\"losers-see-dice\":true,\"allow-calza\":true,\"max-dice\":5,\"max-dice-value\":6,\"show-results-table\":true}}",
			false);
	add_expected_publish("host", player1_payload, false);

	easy_acl_check(room_uuid, &client1, "login",      player1_payload, MOSQ_ACL_WRITE);

	snprintf(payload, sizeof(payload), "{\"name\":\"%s\", \"uuid\":\"%s\", \"option\":\"roll-dice-at-start\", \"value\":false}", player2_name, player2_uuid);
	easy_acl_check(room_uuid, &client1, "set-option",     payload, MOSQ_ACL_WRITE);

	easy_acl_check(room_uuid, &client1, "leave-game", player1_payload, MOSQ_ACL_WRITE);
	easy_acl_check(room_uuid, &client1, "logout",     player1_payload, MOSQ_ACL_WRITE);

	mosquitto_auth_plugin_cleanup(NULL, NULL, 0);
}

static void two_player_game(void)
{
	char payload[1000];
	int i;

	add_expected_publish("lobby-players",
			"{\"players\":[{\"name\":\"Player 1\",\"uuid\":\"00000000-0000-0000-0000-000000000001\"}],"
			"\"options\":{\"losers-see-dice\":true,\"allow-calza\":true,\"max-dice\":5,\"max-dice-value\":6,\"show-results-table\":true}}",
			false);

	add_expected_publish("host", player1_payload, false);
	easy_acl_check(room_uuid, &client1, "login", player1_payload, MOSQ_ACL_WRITE);
	easy_acl_check(room_uuid, &client2, "login", player2_payload, MOSQ_ACL_WRITE);

	snprintf(payload, sizeof(payload), "{\"name\":\"%s\", \"uuid\":\"%s\", \"option\":\"roll-dice-at-start\", \"value\":false}", player1_name, player1_uuid);
	easy_acl_check(room_uuid, &client1, "set-option",     payload, MOSQ_ACL_WRITE);

	easy_acl_check(room_uuid, &client1, "start-game", player1_payload, MOSQ_ACL_WRITE);

	for(i=0; i<5; i++){
		easy_acl_check(room_uuid, &client1, "roll-dice", player1_payload, MOSQ_ACL_WRITE);
		easy_acl_check(room_uuid, &client2, "roll-dice", player2_payload, MOSQ_ACL_WRITE);

		easy_acl_check(room_uuid, &client1, "call-dudo", player1_payload, MOSQ_ACL_WRITE);
		easy_acl_check(room_uuid, &client1, "i-lost",    player1_payload, MOSQ_ACL_WRITE);
		printf("EOR %i\n", i);
	}


	//easy_acl_check(room_uuid, &client1, "logout", player1_payload, MOSQ_ACL_WRITE);
	//easy_acl_check(room_uuid, &client2, "logout", player2_payload, MOSQ_ACL_WRITE);
	//easy_acl_check(room_uuid, &client3, "logout", player3_payload, MOSQ_ACL_WRITE);
}


void TEST_two_player_game(void)
{
	unlink("tfdg-state.json");
	mosquitto_auth_plugin_init(NULL, NULL, 0);

	two_player_game();

	mosquitto_auth_plugin_cleanup(NULL, NULL, 0);
}


static void three_player_game(void)
{
	char payload[1000];
	int i;

	add_expected_publish("lobby-players",
			"{\"players\":[{\"name\":\"Player 1\",\"uuid\":\"00000000-0000-0000-0000-000000000001\"}],"
			"\"options\":{\"losers-see-dice\":true,\"allow-calza\":true,\"max-dice\":5,\"max-dice-value\":6,\"show-results-table\":true}}",
			false);

	add_expected_publish("host", player1_payload, false);
	easy_acl_check(room_uuid, &client1, "login", player1_payload, MOSQ_ACL_WRITE);
	easy_acl_check(room_uuid, &client2, "login", player2_payload, MOSQ_ACL_WRITE);
	easy_acl_check(room_uuid, &client3, "login", player3_payload, MOSQ_ACL_WRITE);

	snprintf(payload, sizeof(payload), "{\"name\":\"%s\", \"uuid\":\"%s\", \"option\":\"roll-dice-at-start\", \"value\":false}", player1_name, player1_uuid);
	easy_acl_check(room_uuid, &client1, "set-option",     payload, MOSQ_ACL_WRITE);

	easy_acl_check(room_uuid, &client1, "start-game", player1_payload, MOSQ_ACL_WRITE);

	for(i=0; i<5; i++){
		easy_acl_check(room_uuid, &client1, "roll-dice", player1_payload, MOSQ_ACL_WRITE);
		easy_acl_check(room_uuid, &client2, "roll-dice", player2_payload, MOSQ_ACL_WRITE);
		easy_acl_check(room_uuid, &client3, "roll-dice", player3_payload, MOSQ_ACL_WRITE);

		easy_acl_check(room_uuid, &client1, "call-dudo", player1_payload, MOSQ_ACL_WRITE);
		easy_acl_check(room_uuid, &client1, "i-lost",    player1_payload, MOSQ_ACL_WRITE);
	}

	for(i=0; i<5; i++){
		easy_acl_check(room_uuid, &client2, "roll-dice", player2_payload, MOSQ_ACL_WRITE);
		easy_acl_check(room_uuid, &client3, "roll-dice", player3_payload, MOSQ_ACL_WRITE);

		easy_acl_check(room_uuid, &client2, "call-dudo", player2_payload, MOSQ_ACL_WRITE);
		easy_acl_check(room_uuid, &client2, "i-lost",    player2_payload, MOSQ_ACL_WRITE);
	}

	//easy_acl_check(room_uuid, &client1, "logout", player1_payload, MOSQ_ACL_WRITE);
	//easy_acl_check(room_uuid, &client2, "logout", player2_payload, MOSQ_ACL_WRITE);
	//easy_acl_check(room_uuid, &client3, "logout", player3_payload, MOSQ_ACL_WRITE);
}


void TEST_three_player_game(void)
{
	unlink("tfdg-state.json");
	mosquitto_auth_plugin_init(NULL, NULL, 0);

	three_player_game();

	mosquitto_auth_plugin_cleanup(NULL, NULL, 0);
}


void TEST_three_player_game_multiple(void)
{
	int i;

	unlink("tfdg-state.json");
	mosquitto_auth_plugin_init(NULL, NULL, 0);

	for(i=0; i<2; i++){
		three_player_game();
	}

	mosquitto_auth_plugin_cleanup(NULL, NULL, 0);
}


void TEST_three_player_game_rejoin(void)
{
	char payload[1000];
	int i;

	unlink("tfdg-state.json");
	mosquitto_auth_plugin_init(NULL, NULL, 0);

	easy_acl_check(room_uuid, &client1, "login", player1_payload, MOSQ_ACL_WRITE);
	easy_acl_check(room_uuid, &client2, "login", player2_payload, MOSQ_ACL_WRITE);
	easy_acl_check(room_uuid, &client3, "login", player3_payload, MOSQ_ACL_WRITE);

	snprintf(payload, sizeof(payload), "{\"name\":\"%s\", \"uuid\":\"%s\", \"option\":\"roll-dice-at-start\", \"value\":false}", player1_name, player1_uuid);
	easy_acl_check(room_uuid, &client1, "set-option", payload, MOSQ_ACL_WRITE);

	easy_acl_check(room_uuid, &client1, "start-game", player1_payload, MOSQ_ACL_WRITE);
	{
		easy_acl_check(room_uuid, &client1, "roll-dice", player1_payload, MOSQ_ACL_WRITE);
		easy_acl_check(room_uuid, &client2, "roll-dice", player2_payload, MOSQ_ACL_WRITE);
		easy_acl_check(room_uuid, &client3, "roll-dice", player3_payload, MOSQ_ACL_WRITE);

		easy_acl_check(room_uuid, &client1, "call-dudo", player1_payload, MOSQ_ACL_WRITE);
		easy_acl_check(room_uuid, &client1, "call-dudo", player1_payload, MOSQ_ACL_WRITE);
		easy_acl_check(room_uuid, &client1, "i-lost",    player1_payload, MOSQ_ACL_WRITE);
	}

	/* Now log out and back in again */
	easy_acl_check(room_uuid, &client1, "logout", player1_payload, MOSQ_ACL_WRITE);
	easy_acl_check(room_uuid, &client1, "login",  player1_payload, MOSQ_ACL_WRITE);

	for(i=1; i<5; i++){
		easy_acl_check(room_uuid, &client1, "roll-dice", player1_payload, MOSQ_ACL_WRITE);
		easy_acl_check(room_uuid, &client2, "roll-dice", player2_payload, MOSQ_ACL_WRITE);
		easy_acl_check(room_uuid, &client3, "roll-dice", player3_payload, MOSQ_ACL_WRITE);

		easy_acl_check(room_uuid, &client1, "call-dudo", player1_payload, MOSQ_ACL_WRITE);
		easy_acl_check(room_uuid, &client1, "i-lost",    player1_payload, MOSQ_ACL_WRITE);
	}

	for(i=0; i<5; i++){
		easy_acl_check(room_uuid, &client2, "roll-dice", player2_payload, MOSQ_ACL_WRITE);
		easy_acl_check(room_uuid, &client3, "roll-dice", player3_payload, MOSQ_ACL_WRITE);

		easy_acl_check(room_uuid, &client2, "call-dudo", player2_payload, MOSQ_ACL_WRITE);
		easy_acl_check(room_uuid, &client2, "i-lost",    player2_payload, MOSQ_ACL_WRITE);
	}

	easy_acl_check(room_uuid, &client1, "logout", player1_payload, MOSQ_ACL_WRITE);
	easy_acl_check(room_uuid, &client2, "logout", player2_payload, MOSQ_ACL_WRITE);
	easy_acl_check(room_uuid, &client3, "logout", player3_payload, MOSQ_ACL_WRITE);

	mosquitto_auth_plugin_cleanup(NULL, NULL, 0);
}


void TEST_three_player_game_undo_loser(void)
{
	char payload[1000];
	int i;

	unlink("tfdg-state.json");
	mosquitto_auth_plugin_init(NULL, NULL, 0);

	easy_acl_check(room_uuid, &client1, "login", player1_payload, MOSQ_ACL_WRITE);
	easy_acl_check(room_uuid, &client2, "login", player2_payload, MOSQ_ACL_WRITE);
	easy_acl_check(room_uuid, &client3, "login", player3_payload, MOSQ_ACL_WRITE);

	snprintf(payload, sizeof(payload), "{\"name\":\"%s\", \"uuid\":\"%s\", \"option\":\"roll-dice-at-start\", \"value\":false}", player1_name, player1_uuid);
	easy_acl_check(room_uuid, &client1, "set-option", payload, MOSQ_ACL_WRITE);

	easy_acl_check(room_uuid, &client1, "start-game", player1_payload, MOSQ_ACL_WRITE);

	{
		easy_acl_check(room_uuid, &client1, "roll-dice", player1_payload, MOSQ_ACL_WRITE);
		easy_acl_check(room_uuid, &client2, "roll-dice", player2_payload, MOSQ_ACL_WRITE);
		easy_acl_check(room_uuid, &client3, "roll-dice", player3_payload, MOSQ_ACL_WRITE);

		easy_acl_check(room_uuid, &client1, "call-dudo", player1_payload, MOSQ_ACL_WRITE);
		easy_acl_check(room_uuid, &client1, "i-lost",    player1_payload, MOSQ_ACL_WRITE);
	}

	/* Player 1 decides not to lose */
	easy_acl_check(room_uuid, &client1, "undo-loser", player1_payload, MOSQ_ACL_WRITE);

	/* Player 2 loses */
	easy_acl_check(room_uuid, &client2, "i-lost",     player2_payload, MOSQ_ACL_WRITE);

	/* Player 2 decides not to lose */
	easy_acl_check(room_uuid, &client2, "undo-loser", player2_payload, MOSQ_ACL_WRITE);

	/* Player 1 loses */
	easy_acl_check(room_uuid, &client1, "i-lost",     player1_payload, MOSQ_ACL_WRITE);

	for(i=1; i<5; i++){
		easy_acl_check(room_uuid, &client1, "roll-dice", player1_payload, MOSQ_ACL_WRITE);
		easy_acl_check(room_uuid, &client2, "roll-dice", player2_payload, MOSQ_ACL_WRITE);
		easy_acl_check(room_uuid, &client3, "roll-dice", player3_payload, MOSQ_ACL_WRITE);

		easy_acl_check(room_uuid, &client1, "call-dudo", player1_payload, MOSQ_ACL_WRITE);
		easy_acl_check(room_uuid, &client1, "i-lost",    player1_payload, MOSQ_ACL_WRITE);
	}

	for(i=0; i<5; i++){
		easy_acl_check(room_uuid, &client2, "roll-dice", player2_payload, MOSQ_ACL_WRITE);
		easy_acl_check(room_uuid, &client3, "roll-dice", player3_payload, MOSQ_ACL_WRITE);

		easy_acl_check(room_uuid, &client2, "call-dudo", player2_payload, MOSQ_ACL_WRITE);
		easy_acl_check(room_uuid, &client2, "i-lost",    player2_payload, MOSQ_ACL_WRITE);
	}

	easy_acl_check(room_uuid, &client1, "logout", player1_payload, MOSQ_ACL_WRITE);
	easy_acl_check(room_uuid, &client2, "logout", player2_payload, MOSQ_ACL_WRITE);
	easy_acl_check(room_uuid, &client3, "logout", player3_payload, MOSQ_ACL_WRITE);

	mosquitto_auth_plugin_cleanup(NULL, NULL, 0);
}

void TEST_three_player_game_with_calza(void)
{
	char payload[1000];
	int i;

	unlink("tfdg-state.json");
	mosquitto_auth_plugin_init(NULL, NULL, 0);

	easy_acl_check(room_uuid, &client1, "login", player1_payload, MOSQ_ACL_WRITE);
	easy_acl_check(room_uuid, &client2, "login", player2_payload, MOSQ_ACL_WRITE);
	easy_acl_check(room_uuid, &client3, "login", player3_payload, MOSQ_ACL_WRITE);

	snprintf(payload, sizeof(payload), "{\"name\":\"%s\", \"uuid\":\"%s\", \"option\":\"roll-dice-at-start\", \"value\":false}", player1_name, player1_uuid);
	easy_acl_check(room_uuid, &client1, "set-option", payload, MOSQ_ACL_WRITE);

	easy_acl_check(room_uuid, &client1, "start-game", player1_payload, MOSQ_ACL_WRITE);

	{
		easy_acl_check(room_uuid, &client1, "roll-dice", player1_payload, MOSQ_ACL_WRITE);
		easy_acl_check(room_uuid, &client2, "roll-dice", player2_payload, MOSQ_ACL_WRITE);
		easy_acl_check(room_uuid, &client3, "roll-dice", player3_payload, MOSQ_ACL_WRITE);

		easy_acl_check(room_uuid, &client1, "call-dudo", player1_payload, MOSQ_ACL_WRITE);
		easy_acl_check(room_uuid, &client1, "i-lost",    player1_payload, MOSQ_ACL_WRITE);
	}
	{
		easy_acl_check(room_uuid, &client1, "roll-dice", player1_payload, MOSQ_ACL_WRITE);
		easy_acl_check(room_uuid, &client2, "roll-dice", player2_payload, MOSQ_ACL_WRITE);
		easy_acl_check(room_uuid, &client3, "roll-dice", player3_payload, MOSQ_ACL_WRITE);

		easy_acl_check(room_uuid, &client1, "call-calza", player1_payload, MOSQ_ACL_WRITE);
		easy_acl_check(room_uuid, &client1, "i-won",    player1_payload, MOSQ_ACL_WRITE);
	}
	{
		easy_acl_check(room_uuid, &client1, "roll-dice", player1_payload, MOSQ_ACL_WRITE);
		easy_acl_check(room_uuid, &client2, "roll-dice", player2_payload, MOSQ_ACL_WRITE);
		easy_acl_check(room_uuid, &client3, "roll-dice", player3_payload, MOSQ_ACL_WRITE);

		easy_acl_check(room_uuid, &client1, "call-dudo", player1_payload, MOSQ_ACL_WRITE);
		easy_acl_check(room_uuid, &client1, "i-lost",    player1_payload, MOSQ_ACL_WRITE);
	}
	{
		easy_acl_check(room_uuid, &client1, "roll-dice", player1_payload, MOSQ_ACL_WRITE);
		easy_acl_check(room_uuid, &client2, "roll-dice", player2_payload, MOSQ_ACL_WRITE);
		easy_acl_check(room_uuid, &client3, "roll-dice", player3_payload, MOSQ_ACL_WRITE);

		easy_acl_check(room_uuid, &client1, "call-calza", player1_payload, MOSQ_ACL_WRITE);
		easy_acl_check(room_uuid, &client1, "i-lost",    player1_payload, MOSQ_ACL_WRITE);
	}
	for(i=1; i<5; i++){
		easy_acl_check(room_uuid, &client1, "roll-dice", player1_payload, MOSQ_ACL_WRITE);
		easy_acl_check(room_uuid, &client2, "roll-dice", player2_payload, MOSQ_ACL_WRITE);
		easy_acl_check(room_uuid, &client3, "roll-dice", player3_payload, MOSQ_ACL_WRITE);

		easy_acl_check(room_uuid, &client1, "call-dudo", player1_payload, MOSQ_ACL_WRITE);
		easy_acl_check(room_uuid, &client1, "i-lost",    player1_payload, MOSQ_ACL_WRITE);
	}

	for(i=0; i<5; i++){
		easy_acl_check(room_uuid, &client2, "roll-dice", player2_payload, MOSQ_ACL_WRITE);
		easy_acl_check(room_uuid, &client3, "roll-dice", player3_payload, MOSQ_ACL_WRITE);

		easy_acl_check(room_uuid, &client2, "call-dudo", player2_payload, MOSQ_ACL_WRITE);
		easy_acl_check(room_uuid, &client2, "i-lost",    player2_payload, MOSQ_ACL_WRITE);
	}

	mosquitto_auth_plugin_cleanup(NULL, NULL, 0);
}


void TEST_set_option_max_dice(void)
{
	int i;
	char payload[200];

	unlink("tfdg-state.json");
	mosquitto_auth_plugin_init(NULL, NULL, 0);

	add_expected_publish("lobby-players",
			"{\"players\":[{\"name\":\"Player 1\",\"uuid\":\"00000000-0000-0000-0000-000000000001\"}],"
			"\"options\":{\"losers-see-dice\":true,\"allow-calza\":true,\"max-dice\":5,\"max-dice-value\":6,\"show-results-table\":true}}",
			false);
	add_expected_publish("host", player1_payload, false);
	for(i=3; i<21; i++){
		snprintf(payload, sizeof(payload), "{\"max-dice\":%d}", i);
		add_expected_publish("set-option", payload, false);
	}

	easy_acl_check(room_uuid, &client1, "login", player1_payload, MOSQ_ACL_WRITE);
	for(i=-1; i<25; i++){
		snprintf(payload, sizeof(payload), "{\"name\":\"%s\",\"uuid\":\"%s\",\"option\":\"max-dice\",\"value\":%d}", player1_name, player1_uuid, i);
		easy_acl_check(room_uuid, &client1, "set-option", payload, MOSQ_ACL_WRITE);
	}

	CU_ASSERT_PTR_NULL(expected_publishes);
	mosquitto_auth_plugin_cleanup(NULL, NULL, 0);
}

void TEST_set_option_max_dice_value(void)
{
	int i;
	char payload[200];

	unlink("tfdg-state.json");
	mosquitto_auth_plugin_init(NULL, NULL, 0);

	add_expected_publish("lobby-players",
			"{\"players\":[{\"name\":\"Player 1\",\"uuid\":\"00000000-0000-0000-0000-000000000001\"}],"
			"\"options\":{\"losers-see-dice\":true,\"allow-calza\":true,\"max-dice\":5,\"max-dice-value\":6,\"show-results-table\":true}}",
			false);
	add_expected_publish("host", player1_payload, false);
	for(i=3; i<10; i++){
		snprintf(payload, sizeof(payload), "{\"max-dice-value\":%d}", i);
		add_expected_publish("set-option", payload, false);
	}

	easy_acl_check(room_uuid, &client1, "login", player1_payload, MOSQ_ACL_WRITE);
	for(i=-1; i<25; i++){
		snprintf(payload, sizeof(payload), "{\"name\":\"%s\",\"uuid\":\"%s\",\"option\":\"max-dice-value\",\"value\":%d}", player1_name, player1_uuid, i);
		easy_acl_check(room_uuid, &client1, "set-option", payload, MOSQ_ACL_WRITE);
	}

	CU_ASSERT_PTR_NULL(expected_publishes);
	mosquitto_auth_plugin_cleanup(NULL, NULL, 0);
}

void TEST_sound_effects(void)
{
	char payload[1000];
	int i;

	add_expected_publish("lobby-players",
			"{\"players\":[{\"name\":\"Player 1\",\"uuid\":\"00000000-0000-0000-0000-000000000001\"}],"
			"\"options\":{\"losers-see-dice\":true,\"allow-calza\":true,\"max-dice\":5,\"max-dice-value\":6,\"show-results-table\":true}}",
			false);

	add_expected_publish("host", player1_payload, false);

	add_expected_publish("lobby-players",
			"{\"players\":["
			"{\"name\":\"Player 1\",\"uuid\":\"00000000-0000-0000-0000-000000000001\"},"
			"{\"name\":\"Player 2\",\"uuid\":\"00000000-0000-0000-0000-000000000002\"}"
			"],"
			"\"options\":{\"losers-see-dice\":true,\"allow-calza\":true,\"max-dice\":5,\"max-dice-value\":6,\"show-results-table\":true}}",
			false);

	add_expected_publish("host", player1_payload, false);

	add_expected_publish("lobby-players",
			"{\"players\":["
			"{\"name\":\"Player 1\",\"uuid\":\"00000000-0000-0000-0000-000000000001\"},"
			"{\"name\":\"Player 2\",\"uuid\":\"00000000-0000-0000-0000-000000000002\"},"
			"{\"name\":\"Player 3\",\"uuid\":\"00000000-0000-0000-0000-000000000003\"}"
			"],"
			"\"options\":{\"losers-see-dice\":true,\"allow-calza\":true,\"max-dice\":5,\"max-dice-value\":6,\"show-results-table\":true}}",
			false);

	add_expected_publish("host", player1_payload, false);

	add_expected_publish("set-option", "{\"roll-dice-at-start\":false}", false);

	add_expected_publish("lobby-players",
			"{\"players\":["
			"{\"name\":\"Player 1\",\"uuid\":\"00000000-0000-0000-0000-000000000001\"},"
			"{\"name\":\"Player 2\",\"uuid\":\"00000000-0000-0000-0000-000000000002\"},"
			"{\"name\":\"Player 3\",\"uuid\":\"00000000-0000-0000-0000-000000000003\"}"
			"],"
			"\"options\":{\"losers-see-dice\":true,\"allow-calza\":true,\"max-dice\":5,\"max-dice-value\":6,\"show-results-table\":true}}",
			true);

	for(i=0; i<5; i++){
		add_expected_publish("new-round", "", true);
		add_expected_publish("loser-results", "", true);
		add_expected_publish("dice/00000000-0000-0000-0000-000000000001", "", true);
		add_expected_publish("dice/00000000-0000-0000-0000-000000000002", "", true);
		add_expected_publish("dice/00000000-0000-0000-0000-000000000003", "", true);
		add_expected_publish("snd-higher", "", true);
		add_expected_publish("snd-exact", "", true);
		add_expected_publish("snd-higher", "", true);
		add_expected_publish("snd-exact", "", true);
		add_expected_publish("snd-higher", "", true);
		add_expected_publish("snd-exact", "", true);
		add_expected_publish("dudo-candidates", "[{\"name\":\"Player 1\",\"uuid\":\"00000000-0000-0000-0000-000000000001\"},{\"name\":\"Player 3\",\"uuid\":\"00000000-0000-0000-0000-000000000003\"}]", true);
		add_expected_publish("player-results", "", true);
		add_expected_publish("summary-results", "", true);
		add_expected_publish("round-loser", player1_payload, false);
	}
	
	add_expected_publish("game-loser", player1_payload, false);
	add_expected_publish("host", player1_payload, true);
	add_expected_publish("player-lost", player1_payload, false);

	for(i=0; i<4; i++){
		add_expected_publish("new-round", "", true);
		add_expected_publish("loser-results", "", true);
		add_expected_publish("dice/00000000-0000-0000-0000-000000000002", "", true);
		add_expected_publish("dice/00000000-0000-0000-0000-000000000003", "", true);
		add_expected_publish("snd-higher", "", true);
		add_expected_publish("snd-exact", "", true);
		add_expected_publish("snd-higher", "", true);
		add_expected_publish("snd-exact", "", true);
		add_expected_publish("snd-higher", "", true);
		add_expected_publish("snd-exact", "", true);
		add_expected_publish("dudo-candidates", "[{\"name\":\"Player 1\",\"uuid\":\"00000000-0000-0000-0000-000000000001\"},{\"name\":\"Player 3\",\"uuid\":\"00000000-0000-0000-0000-000000000003\"}]", true);
		add_expected_publish("player-results", "", true);
		add_expected_publish("summary-results", "", true);
		add_expected_publish("round-loser", player2_payload, false);
	}
	{
		add_expected_publish("new-round", "", true);
		add_expected_publish("loser-results", "", true);
		add_expected_publish("dice/00000000-0000-0000-0000-000000000002", "", true);
		add_expected_publish("dice/00000000-0000-0000-0000-000000000003", "", true);
		add_expected_publish("snd-higher", "", true);
		add_expected_publish("snd-exact", "", true);
		add_expected_publish("snd-higher", "", true);
		add_expected_publish("snd-exact", "", true);
		add_expected_publish("snd-higher", "", true);
		add_expected_publish("snd-exact", "", true);
		add_expected_publish("dudo-candidates", "[{\"name\":\"Player 1\",\"uuid\":\"00000000-0000-0000-0000-000000000001\"},{\"name\":\"Player 3\",\"uuid\":\"00000000-0000-0000-0000-000000000003\"}]", true);
		add_expected_publish("player-results", "", true);
		add_expected_publish("summary-results", "", true);
	}
	add_expected_publish("player-lost", player2_payload, false);
	add_expected_publish("winner", player3_payload, true);
	add_expected_publish("room-closing", player3_payload, false);
	//add_expected_publish("game-loser", player1_payload, false);


	unlink("tfdg-state.json");
	mosquitto_auth_plugin_init(NULL, NULL, 0);

	easy_acl_check(room_uuid, &client1, "login", player1_payload, MOSQ_ACL_WRITE);
	easy_acl_check(room_uuid, &client2, "login", player2_payload, MOSQ_ACL_WRITE);
	easy_acl_check(room_uuid, &client3, "login", player3_payload, MOSQ_ACL_WRITE);

	snprintf(payload, sizeof(payload), "{\"name\":\"%s\", \"uuid\":\"%s\", \"option\":\"roll-dice-at-start\", \"value\":false}", player1_name, player1_uuid);
	easy_acl_check(room_uuid, &client1, "set-option", payload, MOSQ_ACL_WRITE);

	easy_acl_check(room_uuid, &client1, "start-game", player1_payload, MOSQ_ACL_WRITE);

	for(i=0; i<5; i++){
		easy_acl_check(room_uuid, &client1, "roll-dice", player1_payload, MOSQ_ACL_WRITE);
		easy_acl_check(room_uuid, &client2, "roll-dice", player2_payload, MOSQ_ACL_WRITE);
		easy_acl_check(room_uuid, &client3, "roll-dice", player3_payload, MOSQ_ACL_WRITE);

		easy_acl_check(room_uuid, &client1, "snd-higher", player1_payload, MOSQ_ACL_WRITE);
		easy_acl_check(room_uuid, &client1, "snd-exact", player1_payload, MOSQ_ACL_WRITE);
		easy_acl_check(room_uuid, &client2, "snd-higher", player2_payload, MOSQ_ACL_WRITE);
		easy_acl_check(room_uuid, &client2, "snd-exact", player2_payload, MOSQ_ACL_WRITE);
		easy_acl_check(room_uuid, &client3, "snd-higher", player3_payload, MOSQ_ACL_WRITE);
		easy_acl_check(room_uuid, &client3, "snd-exact", player3_payload, MOSQ_ACL_WRITE);

		easy_acl_check(room_uuid, &client1, "call-dudo", player1_payload, MOSQ_ACL_WRITE);
		easy_acl_check(room_uuid, &client1, "i-lost",    player1_payload, MOSQ_ACL_WRITE);
	}

	for(i=0; i<5; i++){
		easy_acl_check(room_uuid, &client2, "roll-dice", player2_payload, MOSQ_ACL_WRITE);
		easy_acl_check(room_uuid, &client3, "roll-dice", player3_payload, MOSQ_ACL_WRITE);

		easy_acl_check(room_uuid, &client1, "snd-higher", player1_payload, MOSQ_ACL_WRITE);
		easy_acl_check(room_uuid, &client1, "snd-exact", player1_payload, MOSQ_ACL_WRITE);
		easy_acl_check(room_uuid, &client2, "snd-higher", player2_payload, MOSQ_ACL_WRITE);
		easy_acl_check(room_uuid, &client2, "snd-exact", player2_payload, MOSQ_ACL_WRITE);
		easy_acl_check(room_uuid, &client3, "snd-higher", player3_payload, MOSQ_ACL_WRITE);
		easy_acl_check(room_uuid, &client3, "snd-exact", player3_payload, MOSQ_ACL_WRITE);

		easy_acl_check(room_uuid, &client2, "call-dudo", player2_payload, MOSQ_ACL_WRITE);
		easy_acl_check(room_uuid, &client2, "i-lost",    player2_payload, MOSQ_ACL_WRITE);
	}

	mosquitto_auth_plugin_cleanup(NULL, NULL, 0);
}

void TEST_room_expiry(void)
{
	char payload[1000];
	struct mosquitto_opt opts[1];
	int i;

	opts[0].key = "room-expiry-time";
	opts[0].value = "1";

	unlink("tfdg-state.json");
	mosquitto_auth_plugin_init(NULL, opts, 1);

	add_expected_publish("lobby-players",
			"{\"players\":[{\"name\":\"Player 1\",\"uuid\":\"00000000-0000-0000-0000-000000000001\"}],"
			"\"options\":{\"losers-see-dice\":true,\"allow-calza\":true,\"max-dice\":5,\"max-dice-value\":6,\"show-results-table\":true}}",
			false);

	add_expected_publish("host", player1_payload, false);
	easy_acl_check(room_uuid, &client1, "login", player1_payload, MOSQ_ACL_WRITE);
	easy_acl_check(room_uuid, &client2, "login", player2_payload, MOSQ_ACL_WRITE);
	easy_acl_check(room_uuid, &client3, "login", player3_payload, MOSQ_ACL_WRITE);

	snprintf(payload, sizeof(payload), "{\"name\":\"%s\", \"uuid\":\"%s\", \"option\":\"roll-dice-at-start\", \"value\":false}", player1_name, player1_uuid);
	easy_acl_check(room_uuid, &client1, "set-option",     payload, MOSQ_ACL_WRITE);

	easy_acl_check(room_uuid, &client1, "start-game", player1_payload, MOSQ_ACL_WRITE);

	for(i=0; i<5; i++){
		easy_acl_check(room_uuid, &client1, "roll-dice", player1_payload, MOSQ_ACL_WRITE);
		easy_acl_check(room_uuid, &client2, "roll-dice", player2_payload, MOSQ_ACL_WRITE);
		easy_acl_check(room_uuid, &client3, "roll-dice", player3_payload, MOSQ_ACL_WRITE);

		easy_acl_check(room_uuid, &client1, "call-dudo", player1_payload, MOSQ_ACL_WRITE);
		easy_acl_check(room_uuid, &client1, "i-lost",    player1_payload, MOSQ_ACL_WRITE);
	}

	for(i=0; i<4; i++){
		easy_acl_check(room_uuid, &client2, "roll-dice", player2_payload, MOSQ_ACL_WRITE);
		easy_acl_check(room_uuid, &client3, "roll-dice", player3_payload, MOSQ_ACL_WRITE);

		easy_acl_check(room_uuid, &client2, "call-dudo", player2_payload, MOSQ_ACL_WRITE);
		easy_acl_check(room_uuid, &client2, "i-lost",    player2_payload, MOSQ_ACL_WRITE);
	}
	easy_acl_check(room_uuid, &client2, "roll-dice", player2_payload, MOSQ_ACL_WRITE);
	easy_acl_check(room_uuid, &client3, "roll-dice", player3_payload, MOSQ_ACL_WRITE);

	easy_acl_check(room_uuid, &client2, "call-dudo", player2_payload, MOSQ_ACL_WRITE);

	/* Now sleep for two seconds, the room should have expired due to inactivity */
	sleep(5);

	add_expected_publish("lobby-players",
			"{\"players\":[{\"name\":\"Player 1\",\"uuid\":\"00000000-0000-0000-0000-000000000001\"}],"
			"\"options\":{\"losers-see-dice\":true,\"allow-calza\":true,\"max-dice\":5,\"max-dice-value\":6,\"show-results-table\":true}}",
			false);

	add_expected_publish("host", player1_payload, false);
	easy_acl_check(room_uuid2, &client1, "login", player1_payload, MOSQ_ACL_WRITE);
	easy_acl_check(room_uuid2, &client2, "login", player2_payload, MOSQ_ACL_WRITE);
	easy_acl_check(room_uuid2, &client3, "login", player3_payload, MOSQ_ACL_WRITE);

	snprintf(payload, sizeof(payload), "{\"name\":\"%s\", \"uuid\":\"%s\", \"option\":\"roll-dice-at-start\", \"value\":false}", player1_name, player1_uuid);
	easy_acl_check(room_uuid2, &client1, "set-option",     payload, MOSQ_ACL_WRITE);

	easy_acl_check(room_uuid2, &client1, "start-game", player1_payload, MOSQ_ACL_WRITE);

	for(i=0; i<5; i++){
		easy_acl_check(room_uuid2, &client1, "roll-dice", player1_payload, MOSQ_ACL_WRITE);
		easy_acl_check(room_uuid2, &client2, "roll-dice", player2_payload, MOSQ_ACL_WRITE);
		easy_acl_check(room_uuid2, &client3, "roll-dice", player3_payload, MOSQ_ACL_WRITE);

		easy_acl_check(room_uuid2, &client1, "call-dudo", player1_payload, MOSQ_ACL_WRITE);
		easy_acl_check(room_uuid2, &client1, "i-lost",    player1_payload, MOSQ_ACL_WRITE);
	}

	for(i=0; i<5; i++){
		easy_acl_check(room_uuid2, &client2, "roll-dice", player2_payload, MOSQ_ACL_WRITE);
		easy_acl_check(room_uuid2, &client3, "roll-dice", player3_payload, MOSQ_ACL_WRITE);

		easy_acl_check(room_uuid2, &client2, "call-dudo", player2_payload, MOSQ_ACL_WRITE);
		easy_acl_check(room_uuid2, &client2, "i-lost",    player2_payload, MOSQ_ACL_WRITE);
	}

	easy_acl_check(room_uuid2, &client1, "logout", player1_payload, MOSQ_ACL_WRITE);
	easy_acl_check(room_uuid2, &client2, "logout", player2_payload, MOSQ_ACL_WRITE);
	easy_acl_check(room_uuid2, &client3, "logout", player3_payload, MOSQ_ACL_WRITE);

	easy_acl_check(room_uuid, &client1, "login", player1_payload, MOSQ_ACL_WRITE);
	easy_acl_check(room_uuid, &client2, "login", player2_payload, MOSQ_ACL_WRITE);
	easy_acl_check(room_uuid, &client3, "login", player3_payload, MOSQ_ACL_WRITE);

	snprintf(payload, sizeof(payload), "{\"name\":\"%s\", \"uuid\":\"%s\", \"option\":\"roll-dice-at-start\", \"value\":false}", player1_name, player1_uuid);
	easy_acl_check(room_uuid, &client1, "set-option",     payload, MOSQ_ACL_WRITE);

	easy_acl_check(room_uuid, &client1, "start-game", player1_payload, MOSQ_ACL_WRITE);

	for(i=0; i<5; i++){
		easy_acl_check(room_uuid, &client1, "roll-dice", player1_payload, MOSQ_ACL_WRITE);
		easy_acl_check(room_uuid, &client2, "roll-dice", player2_payload, MOSQ_ACL_WRITE);
		easy_acl_check(room_uuid, &client3, "roll-dice", player3_payload, MOSQ_ACL_WRITE);

		easy_acl_check(room_uuid, &client1, "call-dudo", player1_payload, MOSQ_ACL_WRITE);
		easy_acl_check(room_uuid, &client1, "i-lost",    player1_payload, MOSQ_ACL_WRITE);
	}

	for(i=0; i<4; i++){
		easy_acl_check(room_uuid, &client2, "roll-dice", player2_payload, MOSQ_ACL_WRITE);
		easy_acl_check(room_uuid, &client3, "roll-dice", player3_payload, MOSQ_ACL_WRITE);

		easy_acl_check(room_uuid, &client2, "call-dudo", player2_payload, MOSQ_ACL_WRITE);
		easy_acl_check(room_uuid, &client2, "i-lost",    player2_payload, MOSQ_ACL_WRITE);
	}
	easy_acl_check(room_uuid, &client2, "roll-dice", player2_payload, MOSQ_ACL_WRITE);
	easy_acl_check(room_uuid, &client3, "roll-dice", player3_payload, MOSQ_ACL_WRITE);

	easy_acl_check(room_uuid, &client2, "call-dudo", player2_payload, MOSQ_ACL_WRITE);
	mosquitto_auth_plugin_cleanup(NULL, NULL, 0);
}


int main(int argc, char *argv[])
{
	CU_pSuite test_suite = NULL;

	snprintf(player1_payload, sizeof(player1_payload), "{\"name\":\"%s\",\"uuid\":\"%s\"}", player1_name, player1_uuid);
	snprintf(player2_payload, sizeof(player2_payload), "{\"name\":\"%s\",\"uuid\":\"%s\"}", player2_name, player2_uuid);
	snprintf(player3_payload, sizeof(player3_payload), "{\"name\":\"%s\",\"uuid\":\"%s\"}", player3_name, player3_uuid);

	memset(&client1, 0, sizeof(struct mosquitto));
	memset(&client2, 0, sizeof(struct mosquitto));
	memset(&client3, 0, sizeof(struct mosquitto));


    if(CU_initialize_registry() != CUE_SUCCESS){
        printf("Error initializing CUnit registry.\n");
        return 1;
    }

	test_suite = CU_add_suite("TFDG", NULL, NULL);
	if(!test_suite){
		printf("Error adding CUnit test suite.\n");
		return 1;
	}

	if(0
#if 0
			|| !CU_add_test(test_suite, "Non TFDG topic", TEST_non_tfdg_topic)
			|| !CU_add_test(test_suite, "Subscribe success", TEST_subscribe_success)
			|| !CU_add_test(test_suite, "Subscribe fail", TEST_subscribe_fail)
			|| !CU_add_test(test_suite, "Topic tokenise", TEST_topic_tokenise)
			|| !CU_add_test(test_suite, "Single login login logout logout", TEST_single_login_login_logout_logout)
			|| !CU_add_test(test_suite, "Single login bad payload", TEST_single_login_bad_payload)
			|| !CU_add_test(test_suite, "Single login logout", TEST_single_login_logout)
			|| !CU_add_test(test_suite, "Single login logout logout", TEST_single_login_logout_logout)
			|| !CU_add_test(test_suite, "Single login leave game logout", TEST_single_login_leave_game_logout)
			|| !CU_add_test(test_suite, "Set option non-matching player", TEST_set_option_non_matching_player)
			|| !CU_add_test(test_suite, "Two player game", TEST_two_player_game)
			|| !CU_add_test(test_suite, "Three player game", TEST_three_player_game)
			|| !CU_add_test(test_suite, "Three player game multiple", TEST_three_player_game_multiple)
#endif
#if 0
			|| !CU_add_test(test_suite, "Three player game rejoin", TEST_three_player_game_rejoin)
			|| !CU_add_test(test_suite, "Three player game undo loser", TEST_three_player_game_undo_loser)
			|| !CU_add_test(test_suite, "Three player game with calza", TEST_three_player_game_with_calza)
			|| !CU_add_test(test_suite, "Room expiry", TEST_room_expiry)
			|| !CU_add_test(test_suite, "Option: max dice", TEST_set_option_max_dice)
			|| !CU_add_test(test_suite, "Option: max dice value", TEST_set_option_max_dice_value)
#endif
			|| !CU_add_test(test_suite, "Sound effects", TEST_sound_effects)
			){

		printf("Error adding CUnit tests.\n");
		return 1;
	}

    CU_basic_set_mode(CU_BRM_VERBOSE);
    CU_basic_run_tests();
    CU_cleanup_registry();

	printf("pub count: %d\n", publ);
	printf("random bytes: %d\n", random_count);
	return 0;
}
