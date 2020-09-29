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

#include <ctype.h>
#include <stdio.h>
#include <cJSON.h>
#include <uthash.h>
#include <utlist.h>
#include <time.h>
#include <openssl/rand.h>

#include "mosquitto_broker.h"
#include "mosquitto_plugin.h"
#include "mosquitto.h"

#define ANSI_RED "\e[0;31m"
#define ANSI_GREEN "\e[0;32m"
#define ANSI_YELLOW "\e[0;33m"
#define ANSI_BLUE "\e[0;34m"
#define ANSI_MAGENTA "\e[0;35m"
#define ANSI_CYAN "\e[0;36m"
#define ANSI_WHITE "\e[0;37m"
#define ANSI_RESET "\e[0m"

#define MAX_DICE 20
#define MAX_DICE_VALUE 9
#define MAX_LOG_LEN 15
#define MAX_NAME_LEN 30
#define UUIDLEN 36
/* 00000000-0000-0000-0000-000000000000 */

struct tfdg_room;

enum tfdg_game_state{
	tgs_none = -1,
	tgs_lobby = 0,
	tgs_playing_round = 1,
	tgs_sending_results = 4,
	tgs_awaiting_loser = 5,
	tgs_round_over = 6,
	tgs_game_over = 7,
	tgs_pre_roll = 8,
	tgs_pre_roll_over = 9,
};

enum tfdg_player_state{
	tps_none = -1,
	tps_lobby = 0,
	tps_awaiting_dice = 1,
	tps_have_dice = 2,
	tps_dudo_candidate = 3,
	tps_calza_candidate = 4,
	tps_awaiting_loser = 5,
	tps_awaiting_new_round = 6,
	tps_spectator = 7,
	tps_pre_roll = 8,
	tps_pre_roll_sent = 9,
	tps_pre_roll_lost = 10,
};

struct tfdg_player{
	UT_hash_handle hh_uuid;
	UT_hash_handle hh_client_id;
	struct tfdg_player *next, *prev;
	char *uuid;
	char *name;
	char *client_id;
	cJSON *json;
	int dice_count;
	int dice_values[MAX_DICE];
	int login_count;
	enum tfdg_player_state state;
	char pre_roll;
	bool ex_palifico;
};


struct tfdg_room_options{
	int max_dice;
	int max_dice_value;
	bool allow_calza;
	bool losers_see_dice;
	bool random_max_dice_value;
	bool roll_dice_at_start;
	bool show_results_table;
};


struct tfdg_room{
	UT_hash_handle hh;
	struct tfdg_player *player_by_uuid;
	struct tfdg_player *player_by_client_id;
	char uuid[UUIDLEN+1];
	struct tfdg_player *players;
	struct tfdg_player *lost_players;
	int player_count;
	int current_count;
	enum tfdg_game_state state;
	time_t start_time;
	time_t last_event;
	struct tfdg_player *host;
	struct tfdg_player *starter;
	struct tfdg_player *dudo_caller;
	struct tfdg_player *calza_caller;
	struct tfdg_player *round_loser;
	struct tfdg_player *round_winner;
	int round;
	int dudo_success;
	int dudo_fail;
	int calza_success;
	int calza_fail;
	bool palifico_round;
	cJSON *json;
	struct tfdg_room_options options;
	int pre_roll_count;
	int totals[20];
};


struct tfdg_stats{
	int calza_success;
	int calza_fail;
	int dudo_success;
	int dudo_fail;
	int dice_count[21];
	int thrown_dice_values[10];
	int dice_values[10];
	int players[101];
	int durations[2001];
	int duration_counts[2001];
	int game_count;
	int max_players;
	int max_duration;
};


static cJSON *j_full_state = NULL;
static cJSON *j_stats_games = NULL;
static cJSON *j_all_games = NULL;
static char *state_file = NULL;

static struct tfdg_room *room_by_uuid = NULL;
static int room_expiry_time = 7200;

static cJSON *json_create_results_array(struct tfdg_room *room_s);
static cJSON *json_create_dudo_candidates_object(struct tfdg_room *room_s);
static cJSON *json_create_my_dice_array(struct tfdg_player *player_s);
static void save_full_state(void);
void room_append_player(struct tfdg_room *room_s, struct tfdg_player *player_s, bool onload);
static cJSON *room_dice_totals(struct tfdg_room *room_s);
void room_set_current_count(struct tfdg_room *room_s, int count);
void room_set_host(struct tfdg_room *room_s, struct tfdg_player *host);
static void report_results_to_losers(struct tfdg_room *room_s);
static void report_summary_results(struct tfdg_room *room_s, const char *topic_suffix);
static cJSON *json_delete_game(cJSON *j_game);
static void tfdg_handle_player_lost(struct tfdg_room *room_s, struct tfdg_player *player_s);
void player_set_state(struct tfdg_player *player_s, enum tfdg_player_state state);
void room_pre_roll_init(struct tfdg_room *room_s);
void publish_int_option(struct tfdg_room *room_s, const char *option, int value);
cJSON *room_pre_roll_to_cjson(struct tfdg_room *room_s);
void load_stats(void);

static struct tfdg_stats stats;

static int json_get_long(cJSON *json, const char *name, long *value)
{
	cJSON *jtmp;

	jtmp = cJSON_GetObjectItemCaseSensitive(json, name);
	if(jtmp){
		if(cJSON_IsNumber(jtmp) == false){
			return MOSQ_ERR_INVAL;
		}else{
			*value  = jtmp->valueint;
			return MOSQ_ERR_SUCCESS;
		}
	}else{
			return MOSQ_ERR_INVAL;
	}
}


static int json_get_int(cJSON *json, const char *name, int *value)
{
	cJSON *jtmp;

	jtmp = cJSON_GetObjectItemCaseSensitive(json, name);
	if(jtmp){
		if(cJSON_IsNumber(jtmp) == false){
			return MOSQ_ERR_INVAL;
		}else{
			*value  = jtmp->valueint;
			return MOSQ_ERR_SUCCESS;
		}
	}else{
			return MOSQ_ERR_INVAL;
	}
}


static int json_get_bool(cJSON *json, const char *name, bool *value)
{
	cJSON *jtmp;

	jtmp = cJSON_GetObjectItemCaseSensitive(json, name);
	if(jtmp){
		if(cJSON_IsBool(jtmp) == false){
			return MOSQ_ERR_INVAL;
		}else{
			*value  = cJSON_IsTrue(jtmp);
			return MOSQ_ERR_SUCCESS;
		}
	}else{
			return MOSQ_ERR_INVAL;
	}
}


static int json_get_string(cJSON *json, const char *name, char **value)
{
	cJSON *jtmp;

	*value = NULL;

	jtmp = cJSON_GetObjectItemCaseSensitive(json, name);
	if(jtmp){
		if(cJSON_IsString(jtmp) == false){
			return MOSQ_ERR_INVAL;
		}else{
			*value  = jtmp->valuestring;
			return MOSQ_ERR_SUCCESS;
		}
	}else{
		return MOSQ_ERR_INVAL;
	}
}


static void cleanup_player(struct tfdg_player *player_s)
{
	if(player_s){
		free(player_s->name);
		free(player_s->uuid);
		free(player_s->client_id);
		free(player_s);
	}
}


static bool is_hex(char c)
{
	if(isdigit(c)
			|| (c >= 'A' && c <= 'F')
			|| (c >= 'a' && c <= 'f')){

		return true;
	}else{
		return false;
	}
}


static bool validate_uuid(const char *uuid)
{
	size_t len, i;

	len = strlen(uuid);
	if(len != UUIDLEN){
		return false;
	}
	if(uuid[8] != '-' || uuid[13] != '-' || uuid[18] != '-' || uuid[23] != '-'){
		return false;
	}
	for(i=0; i<8; i++){
		if(is_hex(uuid[i] == false)) return false;
	}
	for(i=9; i<13; i++){
		if(is_hex(uuid[i] == false)) return false;
	}
	for(i=14; i<18; i++){
		if(is_hex(uuid[i] == false)) return false;
	}
	for(i=19; i<len; i++){
		if(is_hex(uuid[i] == false)) return false;
	}
	return true;
}


static void add_room_to_stats(struct tfdg_room *room_s, const char *reason)
{
	cJSON *game, *jtmp;
	time_t now;
	struct tm *lt;
	char timestr[100];

	if(room_s->player_count == 0){
		return;
	}

	now = time(NULL);
	game = cJSON_CreateObject();

	jtmp = cJSON_CreateNumber(room_s->player_count);
	cJSON_AddItemToObject(game, "players", jtmp);

	if(room_s->options.allow_calza != true){
		jtmp = cJSON_CreateBool(room_s->options.allow_calza);
		cJSON_AddItemToObject(game, "allow-calza", jtmp);
	}

	if(room_s->options.losers_see_dice != true){
		jtmp = cJSON_CreateBool(room_s->options.losers_see_dice);
		cJSON_AddItemToObject(game, "losers-see-dice", jtmp);
	}

	if(room_s->options.show_results_table != true){
		jtmp = cJSON_CreateBool(room_s->options.show_results_table);
		cJSON_AddItemToObject(game, "show-results-table", jtmp);
	}

	if(room_s->options.max_dice != 5){
		jtmp = cJSON_CreateNumber(room_s->options.max_dice);
		cJSON_AddItemToObject(game, "max-dice", jtmp);
	}

	if(room_s->options.max_dice_value != 6){
		jtmp = cJSON_CreateNumber(room_s->options.max_dice_value);
		cJSON_AddItemToObject(game, "max-dice-value", jtmp);
	}

	if(room_s->options.random_max_dice_value != false){
		jtmp = cJSON_CreateBool(room_s->options.random_max_dice_value);
		cJSON_AddItemToObject(game, "random-max-dice-value", jtmp);
	}

	jtmp = cJSON_CreateString(reason);
	cJSON_AddItemToObject(game, "result", jtmp);

	jtmp = cJSON_CreateNumber(room_s->dudo_success);
	cJSON_AddItemToObject(game, "dudo-success", jtmp);

	jtmp = cJSON_CreateNumber(room_s->dudo_fail);
	cJSON_AddItemToObject(game, "dudo-fail", jtmp);

	if(room_s->calza_success > 0){
		jtmp = cJSON_CreateNumber(room_s->calza_success);
		cJSON_AddItemToObject(game, "calza-success", jtmp);
	}

	if(room_s->calza_fail > 0){
		jtmp = cJSON_CreateNumber(room_s->calza_fail);
		cJSON_AddItemToObject(game, "calza-fail", jtmp);
	}

	jtmp = cJSON_CreateNumber(room_s->round);
	cJSON_AddItemToObject(game, "round", jtmp);

	lt = localtime(&now);
	strftime(timestr, sizeof(timestr), "%FT%T", lt);
	jtmp = cJSON_CreateString(timestr);
	cJSON_AddItemToObject(game, "start-time", jtmp);

	jtmp = cJSON_CreateNumber(now - room_s->start_time);
	cJSON_AddItemToObject(game, "duration", jtmp);

	jtmp = room_dice_totals(room_s);
	cJSON_AddItemToObject(game, "dice-totals", jtmp);

	cJSON_AddItemToArray(j_stats_games, game);

	save_full_state();
}


static void cleanup_room(struct tfdg_room *room_s, const char *reason)
{
	struct tfdg_player *p, *tmp1, *tmp2, *tmp3;

	add_room_to_stats(room_s, reason);
	printf(ANSI_RED "%s" ANSI_RESET " : " ANSI_GREEN "%-*s" ANSI_RESET " : " ANSI_YELLOW "%s" ANSI_RESET "\n", room_s->uuid, MAX_LOG_LEN, "cleanup", reason);
	CDL_FOREACH_SAFE(room_s->players, p, tmp1, tmp2){
		CDL_DELETE(room_s->players, p);
		HASH_DELETE(hh_uuid, room_s->player_by_uuid, p);
		if(p->client_id){
			HASH_FIND(hh_client_id, room_s->player_by_client_id, p->client_id, strlen(p->client_id), tmp3);
			if(tmp3){
				HASH_DELETE(hh_client_id, room_s->player_by_client_id, p);
			}
		}
		cleanup_player(p);
	}
	DL_FOREACH_SAFE(room_s->lost_players, p, tmp1){
		DL_DELETE(room_s->lost_players, p);
		HASH_DELETE(hh_uuid, room_s->player_by_uuid, p);
		if(p->client_id){
			HASH_FIND(hh_client_id, room_s->player_by_client_id, p->client_id, strlen(p->client_id), tmp3);
			if(tmp3){
				HASH_DELETE(hh_client_id, room_s->player_by_client_id, p);
			}
		}
		cleanup_player(p);
	}
	if(room_s->json){
		json_delete_game(room_s->json);
	}
	HASH_DELETE(hh, room_by_uuid, room_s);
	free(room_s);
}

static void cleanup_all(void)
{
	struct tfdg_room *room_s, *room_tmp;

	HASH_ITER(hh, room_by_uuid, room_s, room_tmp){
		cleanup_room(room_s, "closing down");
	}
}

int json_parse_name_uuid(const char *json_str, size_t json_str_len, char **name, char **uuid)
{
	cJSON *tree, *jtmp;

	*name = NULL;
	*uuid = NULL;

	tree = cJSON_ParseWithLength(json_str, json_str_len);
	if(tree){
		jtmp = cJSON_GetObjectItemCaseSensitive(tree, "name");
		if(jtmp && cJSON_IsString(jtmp)){
			*name = strdup(jtmp->valuestring);
		}
		jtmp = cJSON_GetObjectItemCaseSensitive(tree, "uuid");
		if(jtmp && cJSON_IsString(jtmp)){
			*uuid = strdup(jtmp->valuestring);
		}
	}

	/* If only one is present, free both */
	if(*name == NULL || *uuid == NULL
			|| ((*name) && strlen(*name) > MAX_NAME_LEN)
			|| ((*uuid) && validate_uuid(*uuid) == false)){

		free(*name);
		free(*uuid);
		*name = NULL;
		*uuid = NULL;
		cJSON_Delete(tree);
		return 1;
	}else{
		cJSON_Delete(tree);
		return 0;
	}
}


int find_player_from_json(const char *json_str, size_t json_str_len, struct tfdg_room *room_s, struct tfdg_player **player_s)
{
	char *name, *uuid;
	struct tfdg_player *p;

	*player_s = NULL;
	if(json_parse_name_uuid(json_str, json_str_len, &name, &uuid) == 0){
		CDL_FOREACH(room_s->players, p){
			if(!strcmp(p->uuid, uuid)){
				*player_s = p;
				break;
			}
		}
	}
	free(name);
	free(uuid);
	if(*player_s){
		return 0;
	}else{
		return 1;
	}
}

static struct tfdg_player *find_player_check_id(struct mosquitto *client, struct tfdg_room *room_s, const struct mosquitto_acl_msg *msg)
{
	struct tfdg_player *player_s = NULL;

	/* Find the player structure described by '{"uuid":""}' if it is in this room */
	if(room_s == NULL || find_player_from_json(msg->payload, msg->payloadlen, room_s, &player_s)){
		return NULL;
	}
	/* Check that the client sending this message matches the client that is
	 * attached to this player. */
	if(strcmp(mosquitto_client_id(client), player_s->client_id)){
		return NULL;
	}
	return player_s;
}


int tfdg_topic_tokenise(const char *topic, char **room, char **cmd, char **player)
{
	int len;
	int start, stop;
	int tlen;
	int i, j;

	len = strlen(topic);

	*room = NULL;
	*cmd = NULL;
	*player = NULL;

	start = 0;
	for(i=0; i<len+1; i++){
		if(topic[i] == '/' || topic[i] == '\0'){
			stop = i;
			if(start != stop){
				tlen = stop-start + 1;
				(*room) = calloc(tlen, sizeof(char));
				if((*room) == NULL){
					return MOSQ_ERR_NOMEM;
				}
				for(j=start; j<stop; j++){
					(*room)[j-start] = topic[j];
				}
				break;
			}
			start = i+1;
		}
	}

	i++;
	start = i;
	for(; i<len+1; i++){
		if(topic[i] == '/' || topic[i] == '\0'){
			stop = i;
			if(start != stop){
				tlen = stop-start + 1;
				(*cmd) = calloc(tlen, sizeof(char));
				if((*cmd) == NULL){
					free(*room);
					*room = NULL;
					return MOSQ_ERR_NOMEM;
				}
				for(j=start; j<stop; j++){
					(*cmd)[j-start] = topic[j];
				}
				break;
			}
			start = i+1;
		}
	}
	i++;
	start = i;
	for(; i<len+1; i++){
		if(topic[i] == '/' || topic[i] == '\0'){
			stop = i;
			if(start != stop){
				tlen = stop-start + 1;
				(*player) = calloc(tlen, sizeof(char));
				if((*player) == NULL){
					free(*room);
					*room = NULL;
					return MOSQ_ERR_NOMEM;
				}
				for(j=start; j<stop; j++){
					(*player)[j-start] = topic[j];
				}
				break;
			}
			start = i+1;
		}
	}
	if(i != len && i != len+1){
		printf("overlong %d %d\n", i, len+1);
	}
	return MOSQ_ERR_SUCCESS;
}


static cJSON *player_to_cjson(struct tfdg_player *player_s)
{
	cJSON *tree, *jtmp;

	tree = cJSON_CreateObject();
	jtmp = cJSON_CreateString(player_s->name);
	cJSON_AddItemToObject(tree, "name", jtmp);
	jtmp = cJSON_CreateString(player_s->uuid);
	cJSON_AddItemToObject(tree, "uuid", jtmp);

	return tree;
}


static cJSON *json_create_lobby_players_obj(struct tfdg_room *room_s)
{
	struct tfdg_player *p;
	cJSON *tree, *j_player;

	tree = cJSON_CreateArray();
	CDL_FOREACH(room_s->players, p){
		j_player = player_to_cjson(p);
		cJSON_AddItemToArray(tree, j_player);
	}

	return tree;
}


static cJSON *json_create_options_obj(struct tfdg_room *room_s)
{
	cJSON *j_options, *jtmp;

	j_options = cJSON_CreateObject();

	jtmp = cJSON_CreateBool(room_s->options.losers_see_dice);
	cJSON_AddItemToObject(j_options, "losers-see-dice", jtmp);

	jtmp = cJSON_CreateBool(room_s->options.allow_calza);
	cJSON_AddItemToObject(j_options, "allow-calza", jtmp);

	jtmp = cJSON_CreateNumber(room_s->options.max_dice);
	cJSON_AddItemToObject(j_options, "max-dice", jtmp);

	jtmp = cJSON_CreateNumber(room_s->options.max_dice_value);
	cJSON_AddItemToObject(j_options, "max-dice-value", jtmp);

	jtmp = cJSON_CreateBool(room_s->options.show_results_table);
	cJSON_AddItemToObject(j_options, "show-results-table", jtmp);

	return j_options;
}


static void easy_publish(struct tfdg_room *room_s, const char *topic_suffix, cJSON *tree)
{
	char *json_str;
	int json_str_len;
	char topic[200];

	if(tree){
		json_str = cJSON_PrintUnformatted(tree);
		json_str_len = strlen(json_str);
	}else{
		json_str = NULL;
		json_str_len = 0;
	}

	snprintf(topic, sizeof(topic), "tfdg/%s/%s", room_s->uuid, topic_suffix);
	mosquitto_broker_publish(NULL, topic, json_str_len, json_str, 1, 0, NULL);
}


static void easy_publish_player(struct tfdg_room *room_s, const char *topic_suffix, struct tfdg_player *player_s)
{
	cJSON *tree;

	tree = player_to_cjson(player_s);
	easy_publish(room_s, topic_suffix, tree);
	cJSON_Delete(tree);
}


void tfdg_send_host(struct tfdg_room *room_s)
{
	if(room_s->host){
		easy_publish_player(room_s, "host", room_s->host);
	}
}

void tfdg_send_lobby_players(struct tfdg_room *room_s)
{
	cJSON *tree, *j_options, *j_players;

	tree = cJSON_CreateObject();
	if(tree){
		j_players = json_create_lobby_players_obj(room_s);
		cJSON_AddItemToObject(tree, "players", j_players);

		j_options = json_create_options_obj(room_s);
		cJSON_AddItemToObject(tree, "options", j_options);

		easy_publish(room_s, "lobby-players", tree);
		cJSON_Delete(tree);
	}
}


void tfdg_send_current_state(struct tfdg_room *room_s, struct tfdg_player *player_s)
{
	cJSON *tree, *players, *round_loser, *jtmp, *results, *dudo_candidates, *dice;
	cJSON *calza_caller, *j_options, *round_winner, *j_host;

	tree = cJSON_CreateObject();
	if(tree == NULL) return;

	players = json_create_lobby_players_obj(room_s);
	if(players == NULL){
		cJSON_Delete(tree);
		return;
	}
	cJSON_AddItemToObject(tree, "players", players);

	printf(ANSI_BLUE "%s" ANSI_RESET " : " ANSI_GREEN "%-*s" ANSI_RESET " : "
			ANSI_MAGENTA "%s" ANSI_RESET " : " ANSI_CYAN "%s" ANSI_RESET "\n",
			room_s->uuid, MAX_LOG_LEN, "sending-state", player_s->uuid, player_s->name);


	switch(room_s->state){
		case tgs_playing_round:
			jtmp = cJSON_CreateString("playing-round");
			cJSON_AddItemToObject(tree, "state", jtmp);
			break;
		case tgs_sending_results:
			jtmp = cJSON_CreateString("sending-results");
			cJSON_AddItemToObject(tree, "state", jtmp);
			break;
		case tgs_awaiting_loser:
			jtmp = cJSON_CreateString("awaiting-loser");
			cJSON_AddItemToObject(tree, "state", jtmp);
			break;
		case tgs_round_over:
			jtmp = cJSON_CreateString("round-over");
			cJSON_AddItemToObject(tree, "state", jtmp);
			break;
		case tgs_game_over:
			jtmp = cJSON_CreateString("game-over");
			cJSON_AddItemToObject(tree, "state", jtmp);
			break;
		case tgs_pre_roll:
			jtmp = cJSON_CreateString("pre-roll");
			cJSON_AddItemToObject(tree, "state", jtmp);
			break;
		case tgs_pre_roll_over:
			jtmp = cJSON_CreateString("pre-roll-over");
			cJSON_AddItemToObject(tree, "state", jtmp);
			break;
		default:
			break;
	}
	/* Results */
	if(room_s->state == tgs_sending_results
			|| room_s->state == tgs_awaiting_loser
			|| room_s->state == tgs_round_over){

		results = json_create_results_array(room_s);
		if(results){
			cJSON_AddItemToObject(tree, "results", results);
		}
	}

	/* Dudo/calza candidates */
	if(room_s->state == tgs_awaiting_loser
			|| room_s->state == tgs_round_over){

		if(room_s->dudo_caller){
			dudo_candidates = json_create_dudo_candidates_object(room_s);
			if(dudo_candidates){
				cJSON_AddItemToObject(tree, "dudo-candidates", dudo_candidates);
			}
		}else if(room_s->calza_caller){
			calza_caller = player_to_cjson(room_s->calza_caller);
			if(calza_caller){
				cJSON_AddItemToObject(tree, "calza-candidate", calza_caller);
			}
		}
	}

	if(room_s->host){
		j_host = player_to_cjson(room_s->host);
		if(j_host){
			cJSON_AddItemToObject(tree, "host", j_host);
		}
	}

	/* Pre roll */
	if(room_s->state == tgs_pre_roll){
		jtmp = room_pre_roll_to_cjson(room_s);
		cJSON_AddItemToObject(tree, "pre-roll", jtmp);
	}else if(room_s->state == tgs_pre_roll_over){
		jtmp = player_to_cjson(room_s->starter);
		cJSON_AddItemToObject(tree, "starter", jtmp);
	}

	/* Starter, my dice */
	if(room_s->state == tgs_playing_round){
		jtmp = player_to_cjson(room_s->starter);
		cJSON_AddItemToObject(tree, "starter", jtmp);
		if(player_s->state == tps_have_dice){
			dice = json_create_my_dice_array(player_s);
			cJSON_AddItemToObject(tree, "dice", dice);
		}
	}

	if(room_s->state == tgs_round_over){
		/* Send loser */

		if(room_s->round_loser){
			round_loser = player_to_cjson(room_s->round_loser);
			cJSON_AddItemToObject(tree, "round-loser", round_loser);
		}else if(room_s->round_winner){
			round_winner = player_to_cjson(room_s->round_winner);
			cJSON_AddItemToObject(tree, "round-winner", round_winner);
		}else{
			/* Player must have lost all of their dice */
			round_loser = cJSON_CreateObject();
			cJSON_AddItemToObject(tree, "round-loser", round_loser);
		}
	}

	/* Palifico */
	jtmp = cJSON_CreateBool(room_s->palifico_round);
	cJSON_AddItemToObject(tree, "palifico-round", jtmp);

	/* Options */
	j_options = json_create_options_obj(room_s);
	cJSON_AddItemToObject(tree, "options", j_options);

	easy_publish(room_s, "state", tree);
	cJSON_Delete(tree);
}


int mosquitto_auth_plugin_version(void)
{
	return MOSQ_AUTH_PLUGIN_VERSION;
}


static void save_full_state(void)
{
	char *json_str;
	FILE *fptr;

	if(j_full_state){
		json_str = cJSON_Print(j_full_state);
		if(json_str){
			fptr = fopen(state_file, "wt");
			if(fptr){
				fprintf(fptr, "%s", json_str);
				fclose(fptr);
			}

			free(json_str);
		}
	}
}


/* Returns next game in array */
static cJSON *json_delete_game(cJSON *j_game)
{
	cJSON *jtmp;
	jtmp = j_game->next;
	cJSON_DetachItemViaPointer(j_all_games, j_game);
	cJSON_Delete(j_game);
	return jtmp;
}


static struct tfdg_player *load_lost_player_state(struct tfdg_room *room_s, cJSON *j_player)
{
	struct tfdg_player *player_s;
	char *uuid;
	char *name;

	player_s = calloc(1, sizeof(struct tfdg_player));

	if(json_get_string(j_player, "uuid", &uuid) != 0
			|| json_get_string(j_player, "name", &name) != 0){

		free(player_s);
		return NULL;
	}
	if(validate_uuid(uuid) == false){
		free(player_s);
		return NULL;
	}
	player_s->uuid = strdup(uuid);
	player_s->name = strdup(name);
	if(player_s->uuid == NULL || player_s->name == NULL){
		free(player_s->uuid);
		free(player_s->name);
		free(player_s);
		return NULL;
	}

	DL_APPEND(room_s->lost_players, player_s);
	HASH_ADD_KEYPTR(hh_uuid, room_s->player_by_uuid, player_s->uuid,  strlen(player_s->uuid), player_s);

	return player_s;
}


static struct tfdg_player *load_player_state(struct tfdg_room *room_s, cJSON *j_player, bool onload)
{
	cJSON *j_dice, *j_die;
	struct tfdg_player *player_s;
	char *uuid;
	char *name;
	int i;

	player_s = calloc(1, sizeof(struct tfdg_player));
	player_s->json = j_player;

	if(json_get_int(j_player, "state", &player_s->state) != 0
			|| json_get_int(j_player, "dice-count", &player_s->dice_count) != 0
			|| json_get_string(j_player, "uuid", &uuid) != 0
			|| json_get_string(j_player, "name", &name) != 0
			|| json_get_bool(j_player, "ex-palifico", &player_s->ex_palifico) != 0){

		goto cleanup;
	}
	if(validate_uuid(uuid) == false){
		goto cleanup;
	}
	player_s->uuid = strdup(uuid);
	player_s->name = strdup(name);
	if(player_s->uuid == NULL || player_s->name == NULL){
		goto cleanup;
	}

	j_dice = cJSON_GetObjectItemCaseSensitive(j_player, "dice");
	if(cJSON_IsArray(j_dice) == false){
		goto cleanup;
	}
	if(player_s->dice_count > MAX_DICE
			|| player_s->dice_count > room_s->options.max_dice){

		goto cleanup;
	}
	i = 0;
	cJSON_ArrayForEach(j_die, j_dice){
		if(cJSON_IsNumber(j_die) == false){
			goto cleanup;
		}
		player_s->dice_values[i] = j_die->valuedouble;
		if(player_s->dice_values[i] > MAX_DICE_VALUE
				|| player_s->dice_values[i] < 0
				|| player_s->dice_values[i] > room_s->options.max_dice_value){

			goto cleanup;
		}
		i++;
	}
	room_append_player(room_s, player_s, onload);
	HASH_ADD_KEYPTR(hh_uuid, room_s->player_by_uuid, player_s->uuid,  strlen(player_s->uuid), player_s);

	return player_s;
cleanup:
	if(player_s){
		free(player_s->uuid);
		free(player_s->name);
		free(player_s);
	}
	return NULL;
}


static void load_game_state(void)
{
	struct tfdg_room *room_s;
	struct tfdg_player *player_s;
	cJSON *jtmp, *j_game, *j_players, *j_player, *j_options;
	time_t now;
	char *uuid;
	char *host;
	char *starter;
	char *dudo_caller;
	char *calza_caller;
	char *round_loser, *round_winner;

	j_all_games = cJSON_GetObjectItemCaseSensitive(j_full_state, "games");
	if(j_all_games == NULL){
		j_all_games = cJSON_CreateArray();
		cJSON_AddItemToObject(j_full_state, "games", j_all_games);
		return;
	}

	now = time(NULL);

	j_game = j_all_games->child;
	while(j_game != NULL){
		jtmp = cJSON_GetObjectItemCaseSensitive(j_game, "last-event");
		if(jtmp == NULL || cJSON_IsNumber(jtmp) == false || now > jtmp->valuedouble + 7200){
			/* Expired or invalid */
			j_game = json_delete_game(j_game);
			continue;
		}

		room_s = calloc(1, sizeof(struct tfdg_room));
		if(room_s == NULL) return;
		room_s->json = j_game;

		if(json_get_int(j_game, "player-count", &room_s->player_count) != 0
				|| json_get_int(j_game, "state", &room_s->state) != 0
				|| json_get_long(j_game, "start-time", &room_s->start_time) != 0
				|| json_get_long(j_game, "last-event", &room_s->last_event) != 0
				|| json_get_int(j_game, "round", &room_s->round) != 0
				|| json_get_int(j_game, "dudo-success", &room_s->dudo_success) != 0
				|| json_get_int(j_game, "dudo-fail", &room_s->dudo_fail) != 0
				|| json_get_int(j_game, "calza-success", &room_s->calza_success) != 0
				|| json_get_int(j_game, "calza-fail", &room_s->calza_fail) != 0
				|| json_get_string(j_game, "uuid", &uuid) != 0
				|| json_get_string(j_game, "host", &host) != 0
				|| json_get_string(j_game, "starter", &starter) != 0
				|| json_get_string(j_game, "dudo-caller", &dudo_caller) != 0
				|| json_get_string(j_game, "calza-caller", &calza_caller) != 0
				|| json_get_string(j_game, "round-loser", &round_loser) != 0
				|| json_get_string(j_game, "round-winner", &round_winner) != 0
				|| json_get_bool(j_game, "palifico-round", &room_s->palifico_round) != 0){

			/* Invalid */
			free(room_s);
			j_game = json_delete_game(j_game);
			continue;
		}
		if(validate_uuid(uuid) == false){
			j_game = j_game->next;
			cleanup_room(room_s, "config-load 1");
			continue;
		}
		strncpy(room_s->uuid, uuid, sizeof(room_s->uuid));
		HASH_ADD_KEYPTR(hh, room_by_uuid, room_s->uuid, strlen(room_s->uuid), room_s);

		j_options = cJSON_GetObjectItemCaseSensitive(j_game, "options");
		if(j_options == NULL){
			j_game = j_game->next;
			cleanup_room(room_s, "config-load 0");
			continue;
		}
		if(json_get_int(j_options, "max-dice", &room_s->options.max_dice) != 0
				|| json_get_int(j_options, "max-dice-value", &room_s->options.max_dice_value) != 0
				|| json_get_bool(j_options, "allow-calza", &room_s->options.allow_calza) != 0
				|| json_get_bool(j_options, "losers-see-dice", &room_s->options.losers_see_dice) != 0
				|| json_get_bool(j_options, "show-results-table", &room_s->options.show_results_table) != 0){

			j_game = j_game->next;
			cleanup_room(room_s, "config-load -1");
			continue;
		}
		if(room_s->options.max_dice > MAX_DICE){
			room_s->options.max_dice = MAX_DICE;
		}
		if(room_s->options.max_dice_value > MAX_DICE_VALUE){
			room_s->options.max_dice_value = MAX_DICE_VALUE;
		}

		j_players = cJSON_GetObjectItemCaseSensitive(j_game, "players");
		if(cJSON_IsArray(j_players) == false){
			j_game = j_game->next;
			cleanup_room(room_s, "config-load 2");
			continue;
		}

		room_set_current_count(room_s, cJSON_GetArraySize(j_players));
		cJSON_ArrayForEach(j_player, j_players){
			player_s = load_player_state(room_s, j_player, true);
			if(player_s == NULL || json_get_string(j_player, "uuid", &uuid) != 0){
				j_game = j_game->next;
				cleanup_room(room_s, "config-load 4");
				player_s = NULL;
				break;
			}
			if(!strcmp(uuid, host)){
				room_s->host = player_s;
			}
			if(!strcmp(uuid, starter)){
				room_s->starter = player_s;
			}
			if(!strcmp(uuid, dudo_caller)){
				room_s->dudo_caller = player_s;
			}
			if(!strcmp(uuid, calza_caller)){
				room_s->calza_caller = player_s;
			}
			if(!strcmp(uuid, round_loser)){
				room_s->round_loser = player_s;
			}
			if(!strcmp(uuid, round_winner)){
				room_s->round_winner = player_s;
			}
		}
		if(player_s == NULL){
			continue;
		}

		j_players = cJSON_GetObjectItemCaseSensitive(j_game, "lost-players");
		if(cJSON_IsArray(j_players) == false){
			j_game = j_game->next;
			cleanup_room(room_s, "config-load 5");
			continue;
		}
		cJSON_ArrayForEach(j_player, j_players){
			player_s = load_lost_player_state(room_s, j_player);
			if(player_s == NULL){
				j_game = j_game->next;
				cleanup_room(room_s, "config-load 6");
				continue;
			}
		}
		j_game = j_game->next;
	}
}


static void load_full_state(void)
{
	FILE *fptr;
	long len;
	char *json_str;
	cJSON *statistics = NULL;

	fptr = fopen("tfdg-state.json", "rt");
	if(fptr){
		fseek(fptr, 0, SEEK_END);
		len = ftell(fptr);

		json_str = calloc(1, len+1);

		if(json_str){
			fseek(fptr, 0, SEEK_SET);
			fread(json_str, 1, len, fptr);
			fclose(fptr);

			j_full_state = cJSON_Parse(json_str);
			free(json_str);
			if(j_full_state){
				statistics = cJSON_GetObjectItemCaseSensitive(j_full_state, "statistics");
				if(statistics){
					j_stats_games = cJSON_GetObjectItemCaseSensitive(statistics, "games");
					load_stats();
				}

				load_game_state();
			}
		}else{
			fclose(fptr);
		}
	}

	if(j_full_state == NULL){
		j_full_state = cJSON_CreateObject();
	}
	if(statistics == NULL){
		statistics = cJSON_CreateObject();
		cJSON_AddItemToObject(j_full_state, "statistics", statistics);
	}
	if(j_stats_games == NULL){
		j_stats_games = cJSON_CreateArray();
		cJSON_AddItemToObject(statistics, "games", j_stats_games);
	}
}


int mosquitto_auth_plugin_init(void **user_data, struct mosquitto_opt *auth_opts, int auth_opt_count)
{
	int i;

	j_full_state = NULL;
	j_stats_games = NULL;
	j_all_games = NULL;

	room_by_uuid = NULL;
	room_expiry_time = 7200;
	state_file = NULL;

	memset(&stats, 0, sizeof(stats));

	for(i=0; i<auth_opt_count; i++){
		if(!strcmp(auth_opts[i].key, "room-expiry-time")){
			room_expiry_time = atoi(auth_opts[i].value);
		}else if(!strcmp(auth_opts[i].key, "state-file")){
			state_file = strdup(auth_opts[i].value);
		}
	}
	if(state_file == NULL){
		state_file = strdup("tfdg-state.json");
	}
	load_full_state();
	return MOSQ_ERR_SUCCESS;
}

int mosquitto_auth_plugin_cleanup(void *user_data, struct mosquitto_opt *auth_opts, int auth_opt_count)
{
	save_full_state();
	//cleanup_all();
	cJSON_Delete(j_full_state);
	j_full_state = NULL;
	free(state_file);
	return MOSQ_ERR_SUCCESS;
}


void load_stats(void)
{
	cJSON *jtmp, *j_result, *j_array;
	int players;
	int max_dice_count;
	int i;

	cJSON_ArrayForEach(j_result, j_stats_games){
		jtmp = cJSON_GetObjectItem(j_result, "duration");
		if(jtmp == NULL) continue;
		if(jtmp->valuedouble < 100) continue;

		jtmp = cJSON_GetObjectItem(j_result, "result");
		if(jtmp == NULL) continue;
		if(cJSON_IsString(jtmp) == false || strcmp(jtmp->valuestring, "game-over")) continue;

		jtmp = cJSON_GetObjectItem(j_result, "calza-success");
		if(jtmp && cJSON_IsNumber(jtmp)){
			stats.calza_success += jtmp->valuedouble;
		}

		jtmp = cJSON_GetObjectItem(j_result, "calza-fail");
		if(jtmp && cJSON_IsNumber(jtmp)){
			stats.calza_fail += jtmp->valuedouble;
		}

		jtmp = cJSON_GetObjectItem(j_result, "dudo-success");
		if(jtmp && cJSON_IsNumber(jtmp)){
			stats.dudo_success += jtmp->valuedouble;
		}

		jtmp = cJSON_GetObjectItem(j_result, "dudo-fail");
		if(jtmp && cJSON_IsNumber(jtmp)){
			stats.dudo_fail += jtmp->valuedouble;
		}

		jtmp = cJSON_GetObjectItem(j_result, "max-dice");
		if(jtmp && cJSON_IsNumber(jtmp)
				&& jtmp->valuedouble >= 3 && jtmp->valuedouble <= 20){

			stats.dice_count[(int)jtmp->valuedouble]++;
			max_dice_count = jtmp->valuedouble;
		}else{
			stats.dice_count[5]++;
			max_dice_count = 5;
		}

		jtmp = cJSON_GetObjectItem(j_result, "max-dice-value");
		if(jtmp && cJSON_IsNumber(jtmp)
				&& jtmp->valuedouble >= 3 && jtmp->valuedouble <= 9){

			stats.dice_values[(int)jtmp->valuedouble]++;
		}else{
			stats.dice_values[6]++;
		}

		jtmp = cJSON_GetObjectItem(j_result, "players");
		if(jtmp && cJSON_IsNumber(jtmp)
				&& jtmp->valuedouble > 1 && jtmp->valuedouble < 100){

			if(jtmp->valuedouble > stats.max_players){
				stats.max_players = jtmp->valuedouble;
			}
			stats.players[(int)jtmp->valuedouble]++;
			players = jtmp->valuedouble;

			jtmp = cJSON_GetObjectItem(j_result, "duration");
			stats.durations[players*max_dice_count] += jtmp->valuedouble;
			stats.duration_counts[players*max_dice_count]++;
			if(players*max_dice_count > stats.max_duration){
				stats.max_duration = players*max_dice_count;
			}
		}

		j_array = cJSON_GetObjectItem(j_result, "dice-totals");
		if(j_array && cJSON_IsArray(j_array)){

			i = 0;
			cJSON_ArrayForEach(jtmp, j_array){
				stats.thrown_dice_values[i] += jtmp->valuedouble;
				i++;
			}
		}
	}
}


void publish_stats(void)
{
	cJSON *tree, *jtmp, *j_array;
	char *json_str;
	int i;
	double success, fail, total, count;

	tree = cJSON_CreateObject();
	if(tree == NULL) return;

	/* Calza */
	total = stats.calza_success + stats.calza_fail;
	success = 100.0*(double)stats.calza_success / total;
	fail = 100.0*(double)stats.calza_fail / total;

	jtmp = cJSON_CreateNumber(success);
	if(jtmp == NULL){
		cJSON_Delete(tree);
		return;
	}
	cJSON_AddItemToObject(tree, "calza-success", jtmp);

	jtmp = cJSON_CreateNumber(fail);
	if(jtmp == NULL){
		cJSON_Delete(tree);
		return;
	}
	cJSON_AddItemToObject(tree, "calza-fail", jtmp);

	/* Dudo */
	total = stats.dudo_success + stats.dudo_fail;
	success = 100.0*(double)stats.dudo_success / total;
	fail = 100.0*(double)stats.dudo_fail / total;

	jtmp = cJSON_CreateNumber(success);
	if(jtmp == NULL){
		cJSON_Delete(tree);
		return;
	}
	cJSON_AddItemToObject(tree, "dudo-success", jtmp);

	jtmp = cJSON_CreateNumber(fail);
	if(jtmp == NULL){
		cJSON_Delete(tree);
		return;
	}
	cJSON_AddItemToObject(tree, "dudo-fail", jtmp);

	/* Player count */
	j_array = cJSON_CreateArray();
	if(j_array == NULL){
		cJSON_Delete(tree);
		return;
	}
	cJSON_AddItemToObject(tree, "players", j_array);

	total = 0.0;
	for(i=2; i<=stats.max_players; i++){
		total += stats.players[i];
	}

	for(i=2; i<=stats.max_players; i++){
		count = 100.0*(double)stats.players[i] / total;
		jtmp = cJSON_CreateNumber(count);
		if(jtmp == NULL){
			cJSON_Delete(tree);
			return;
		}
		cJSON_AddItemToArray(j_array, jtmp);
	}

	/* Durations */
	j_array = cJSON_CreateArray();
	if(j_array == NULL){
		cJSON_Delete(tree);
		return;
	}
	cJSON_AddItemToObject(tree, "durations", j_array);
	for(i=0; i<=stats.max_duration; i++){
		if(stats.duration_counts[i] > 0){
			jtmp = cJSON_CreateNumber((double)stats.durations[i] / (double)stats.duration_counts[i]);
		}else{
			jtmp = cJSON_CreateNumber(0);
		}
		if(jtmp == NULL){
			cJSON_Delete(tree);
			return;
		}
		cJSON_AddItemToArray(j_array, jtmp);
	}

	/* Dice count */
	j_array = cJSON_CreateArray();
	if(j_array == NULL){
		cJSON_Delete(tree);
		return;
	}
	cJSON_AddItemToObject(tree, "dice-count", j_array);

	total = 0.0;
	for(i=0; i<=20; i++){
		total += stats.dice_count[i];
	}
	for(i=0; i<=20; i++){
		count = 100.0 * (double)stats.dice_count[i] / total;
		jtmp = cJSON_CreateNumber(count);
		if(jtmp == NULL){
			cJSON_Delete(tree);
			return;
		}
		cJSON_AddItemToArray(j_array, jtmp);
	}

	/* Dice values */
	j_array = cJSON_CreateArray();
	if(j_array == NULL){
		cJSON_Delete(tree);
		return;
	}
	cJSON_AddItemToObject(tree, "dice-values", j_array);

	total = 0.0;
	for(i=0; i<=9; i++){
		total += stats.dice_values[i];
	}
	for(i=0; i<=9; i++){
		count = 100.0 * (double)stats.dice_values[i] / total;
		jtmp = cJSON_CreateNumber(count);
		if(jtmp == NULL){
			cJSON_Delete(tree);
			return;
		}
		cJSON_AddItemToArray(j_array, jtmp);
	}

	/* Thrown dice values */
	j_array = cJSON_CreateArray();
	if(j_array == NULL){
		cJSON_Delete(tree);
		return;
	}
	cJSON_AddItemToObject(tree, "thrown-dice-values", j_array);

	total = 0.0;
	for(i=0; i<=9; i++){
		total += stats.thrown_dice_values[i];
	}
	for(i=0; i<=9; i++){
		count = 100.0 * (double)stats.thrown_dice_values[i] / total;
		jtmp = cJSON_CreateNumber(count);
		if(jtmp == NULL){
			cJSON_Delete(tree);
			return;
		}
		cJSON_AddItemToArray(j_array, jtmp);
	}

	tree->precision = 1;
	json_str = cJSON_PrintUnformatted(tree);
	cJSON_Delete(tree);
	if(json_str == NULL) return;

	mosquitto_broker_publish(NULL, "tfdg/stats", strlen(json_str), json_str, 1, 1, NULL);
}


int mosquitto_auth_security_init(void *user_data, struct mosquitto_opt *auth_opts, int auth_opt_count, bool reload)
{
	publish_stats();
	return MOSQ_ERR_SUCCESS;
}

int mosquitto_auth_security_cleanup(void *user_data, struct mosquitto_opt *auth_opts, int auth_opt_count, bool reload)
{
	return MOSQ_ERR_SUCCESS;
}


static cJSON *room_create_json(const char *uuid)
{
	const char strings[6][30] = {
		"host", "starter", "dudo-caller", "calza-caller",
		"round-loser", "round-winner"
	};

	const char ints[10][30] = {
		"player-count", "current-count", "state", "start-time",
		"last-event", "round", "dudo-success", "dudo-fail",
		"calza-success", "calza-fail",
	};
	int i;
	cJSON *j_room, *j_options, *jtmp;

	j_room = cJSON_CreateObject();

	for(i=0; i<10; i++){
		jtmp = cJSON_CreateNumber(0);
		cJSON_AddItemToObject(j_room, ints[i], jtmp);
	}
	for(i=0; i<6; i++){
		jtmp = cJSON_CreateString("");
		cJSON_AddItemToObject(j_room, strings[i], jtmp);
	}

	jtmp = cJSON_CreateBool(false);
	cJSON_AddItemToObject(j_room, "palifico-round", jtmp);

	jtmp = cJSON_CreateString(uuid);
	cJSON_AddItemToObject(j_room, "uuid", jtmp);

	jtmp = cJSON_CreateArray();
	cJSON_AddItemToObject(j_room, "players", jtmp);

	jtmp = cJSON_CreateArray();
	cJSON_AddItemToObject(j_room, "lost-players", jtmp);

	j_options = cJSON_CreateObject();
	cJSON_AddItemToObject(j_room, "options", j_options);

	jtmp = cJSON_CreateNumber(5);
	cJSON_AddItemToObject(j_options, "max-dice", jtmp);

	jtmp = cJSON_CreateNumber(6);
	cJSON_AddItemToObject(j_options, "max-dice-value", jtmp);

	jtmp = cJSON_CreateNumber(4);
	cJSON_AddItemToObject(j_options, "results-timeout", jtmp);

	jtmp = cJSON_CreateBool(true);
	cJSON_AddItemToObject(j_options, "allow-calza", jtmp);

	jtmp = cJSON_CreateBool(true);
	cJSON_AddItemToObject(j_options, "roll-dice-at-start", jtmp);

	jtmp = cJSON_CreateBool(false);
	cJSON_AddItemToObject(j_options, "losers-see-dice", jtmp);

	jtmp = cJSON_CreateBool(false);
	cJSON_AddItemToObject(j_options, "show-results-table", jtmp);

	return j_room;
}


void room_append_player(struct tfdg_room *room_s, struct tfdg_player *player_s, bool onload)
{
	cJSON *j_players;

	CDL_APPEND(room_s->players, player_s);
	if(onload == false){
		j_players = cJSON_GetObjectItemCaseSensitive(room_s->json, "players");
		cJSON_AddItemToArray(j_players, player_s->json);
	}
}

void room_append_lost_player(struct tfdg_room *room_s, struct tfdg_player *player_s)
{
	DL_APPEND(room_s->lost_players, player_s);
}

void room_delete_player(struct tfdg_room *room_s, struct tfdg_player *player_s)
{
	cJSON *j_players;

	CDL_DELETE(room_s->players, player_s);
	j_players = cJSON_GetObjectItemCaseSensitive(room_s->json, "players");
	cJSON_DetachItemViaPointer(j_players, player_s->json);
	cJSON_Delete(player_s->json);
	player_s->json = NULL;

	if(player_s == room_s->host){
		room_set_host(room_s, room_s->players);
		tfdg_send_host(room_s);
	}

}

void room_set_calza_caller(struct tfdg_room *room_s, struct tfdg_player *caller)
{
	cJSON *jtmp;

	jtmp = cJSON_GetObjectItemCaseSensitive(room_s->json, "calza-caller");
	if(caller){
		cJSON_SetValuestring(jtmp, caller->uuid);
	}else{
		cJSON_SetValuestring(jtmp, "");
	}
	room_s->calza_caller = caller;
}


void room_set_calza_success(struct tfdg_room *room_s, int success)
{
	cJSON *jtmp;

	jtmp = cJSON_GetObjectItemCaseSensitive(room_s->json, "calza-success");
	cJSON_SetNumberValue(jtmp, success);
	room_s->calza_success = success;
}

void room_set_calza_fail(struct tfdg_room *room_s, int fail)
{
	cJSON *jtmp;

	jtmp = cJSON_GetObjectItemCaseSensitive(room_s->json, "calza-fail");
	cJSON_SetNumberValue(jtmp, fail);
	room_s->calza_fail = fail;
}

void room_set_current_count(struct tfdg_room *room_s, int count)
{
	cJSON *jtmp;

	jtmp = cJSON_GetObjectItemCaseSensitive(room_s->json, "current-count");
	cJSON_SetNumberValue(jtmp, count);
	room_s->current_count = count;
}


void room_set_dudo_caller(struct tfdg_room *room_s, struct tfdg_player *caller)
{
	cJSON *jtmp;

	jtmp = cJSON_GetObjectItemCaseSensitive(room_s->json, "dudo-caller");
	if(caller){
		cJSON_SetValuestring(jtmp, caller->uuid);
	}else{
		cJSON_SetValuestring(jtmp, "");
	}
	room_s->dudo_caller = caller;
}


void room_set_dudo_success(struct tfdg_room *room_s, int success)
{
	cJSON *jtmp;

	jtmp = cJSON_GetObjectItemCaseSensitive(room_s->json, "dudo-success");
	cJSON_SetNumberValue(jtmp, success);
	room_s->dudo_success = success;
}


void room_set_dudo_fail(struct tfdg_room *room_s, int fail)
{
	cJSON *jtmp;

	jtmp = cJSON_GetObjectItemCaseSensitive(room_s->json, "dudo-fail");
	cJSON_SetNumberValue(jtmp, fail);
	room_s->dudo_fail = fail;
}


void room_set_last_event(struct tfdg_room *room_s, time_t last_event)
{
	cJSON *jtmp;

	jtmp = cJSON_GetObjectItemCaseSensitive(room_s->json, "last-event");
	cJSON_SetNumberValue(jtmp, last_event);
	room_s->last_event = last_event;
}


void room_set_palifico_round(struct tfdg_room *room_s, bool value)
{
	cJSON *jtmp;

	jtmp = cJSON_GetObjectItemCaseSensitive(room_s->json, "palifico-round");
	if(value){
		jtmp->type = cJSON_True;
	}else{
		jtmp->type = cJSON_False;
	}
	room_s->palifico_round = value;
}

void room_set_player_count(struct tfdg_room *room_s, int count)
{
	cJSON *jtmp;

	jtmp = cJSON_GetObjectItemCaseSensitive(room_s->json, "player-count");
	cJSON_SetNumberValue(jtmp, count);
	room_s->player_count = count;
}


void room_set_start_time(struct tfdg_room *room_s, time_t start_time)
{
	cJSON *jtmp;

	jtmp = cJSON_GetObjectItemCaseSensitive(room_s->json, "start-time");
	cJSON_SetNumberValue(jtmp, start_time);
	room_s->start_time = start_time;
}


void room_set_state(struct tfdg_room *room_s, enum tfdg_game_state state)
{
	cJSON *jtmp;

	jtmp = cJSON_GetObjectItemCaseSensitive(room_s->json, "state");
	cJSON_SetNumberValue(jtmp, state);
	room_s->state = state;
}


void room_set_host(struct tfdg_room *room_s, struct tfdg_player *host)
{
	cJSON *jtmp;

	jtmp = cJSON_GetObjectItemCaseSensitive(room_s->json, "host");
	if(host){
		cJSON_SetValuestring(jtmp, host->uuid);
	}else{
		cJSON_SetValuestring(jtmp, "");
	}
	room_s->host = host;

	if(host){
		printf(ANSI_BLUE "%s" ANSI_RESET " : " ANSI_GREEN "%-*s" ANSI_RESET " : "
				ANSI_MAGENTA "%s" ANSI_RESET " : " ANSI_CYAN "%s" ANSI_RESET "\n",
				room_s->uuid, MAX_LOG_LEN, "new-host", host->uuid, host->name);
	}
}


void room_set_option_int(struct tfdg_room *room_s, int *option, const char *option_name, int value)
{
	cJSON *j_options, *jtmp;

	j_options = cJSON_GetObjectItemCaseSensitive(room_s->json, "options");
	if(j_options){
		jtmp = cJSON_GetObjectItemCaseSensitive(j_options, option_name);
		if(jtmp){
			cJSON_SetNumberValue(jtmp, value);
		}
	}
	*option = value;
}

void room_set_option_bool(struct tfdg_room *room_s, bool *option, const char *option_name, bool value)
{
	cJSON *j_options, *jtmp;

	j_options = cJSON_GetObjectItemCaseSensitive(room_s->json, "options");
	if(j_options){
		jtmp = cJSON_GetObjectItemCaseSensitive(j_options, option_name);
		if(jtmp){
			jtmp->type = value == true?cJSON_True:cJSON_False;
		}
	}
	*option = value;
}

void room_set_round(struct tfdg_room *room_s, int round)
{
	cJSON *jtmp;

	jtmp = cJSON_GetObjectItemCaseSensitive(room_s->json, "round");
	cJSON_SetNumberValue(jtmp, round);
	room_s->round = round;
}


void room_set_round_loser(struct tfdg_room *room_s, struct tfdg_player *round_loser)
{
	cJSON *jtmp;

	jtmp = cJSON_GetObjectItemCaseSensitive(room_s->json, "round-loser");
	if(round_loser){
		cJSON_SetValuestring(jtmp, round_loser->uuid);
	}else{
		cJSON_SetValuestring(jtmp, "");
	}
	room_s->round_loser = round_loser;
}


void room_set_round_winner(struct tfdg_room *room_s, struct tfdg_player *round_winner)
{
	cJSON *jtmp;

	jtmp = cJSON_GetObjectItemCaseSensitive(room_s->json, "round-winner");
	if(round_winner){
		cJSON_SetValuestring(jtmp, round_winner->uuid);
	}else{
		cJSON_SetValuestring(jtmp, "");
	}
	room_s->round_winner = round_winner;
}


void room_set_starter(struct tfdg_room *room_s, struct tfdg_player *starter)
{
	cJSON *jtmp;

	jtmp = cJSON_GetObjectItemCaseSensitive(room_s->json, "starter");
	if(starter){
		cJSON_SetValuestring(jtmp, starter->uuid);
	}else{
		cJSON_SetValuestring(jtmp, "");
	}
	room_s->starter = starter;
}


void room_shuffle_players(struct tfdg_room *room_s)
{
	int count, cur_count;
	struct tfdg_player *list, *p;
	unsigned char bytes[1000];
	int i, j;

	CDL_COUNT(room_s->players, p, count);

	list = NULL;
	if(RAND_bytes(bytes, count) == 1){
		cur_count = count;
		for(i=0; i<count; i++){
			p = room_s->players;
			for(j=0; j<bytes[i]%cur_count; j++){
				p = p->next;
			}
			CDL_DELETE(room_s->players, p);
			CDL_APPEND(list, p);
			cur_count--;
		}
		room_s->players = list;
	}
}


static struct tfdg_room *room_create(const char *room)
{
	struct tfdg_room *room_s;

	room_s = calloc(1, sizeof(struct tfdg_room));
	if(room_s == NULL) return NULL;
	room_s->json = room_create_json(room);
	if(room_s->json == NULL){
		free(room_s);
		return NULL;
	}

	room_set_option_int(room_s, &room_s->options.max_dice, "max-dice", 5);
	room_set_option_int(room_s, &room_s->options.max_dice_value, "max-dice-value", 6);
	room_set_option_bool(room_s, &room_s->options.allow_calza, "allow-calza", true);
	room_set_option_bool(room_s, &room_s->options.roll_dice_at_start, "roll-dice-at-start", true);
	room_set_option_bool(room_s, &room_s->options.losers_see_dice, "losers-see-dice", true);
	room_set_option_bool(room_s, &room_s->options.show_results_table, "show-results-table", true);

	cJSON_AddItemToArray(j_all_games, room_s->json);

	room_set_state(room_s, tgs_lobby);
	strncpy(room_s->uuid, room, sizeof(room_s->uuid));
	HASH_ADD_KEYPTR(hh, room_by_uuid, room_s->uuid, strlen(room_s->uuid), room_s);
	return room_s;
}

static cJSON *player_create_json(void)
{
	cJSON *j_player, *jtmp;

	j_player = cJSON_CreateObject();

	jtmp = cJSON_CreateString("");
	cJSON_AddItemToObject(j_player, "uuid", jtmp);

	jtmp = cJSON_CreateString("");
	cJSON_AddItemToObject(j_player, "name", jtmp);

	jtmp = cJSON_CreateNumber(tps_none);
	cJSON_AddItemToObject(j_player, "state", jtmp);

	jtmp = cJSON_CreateNumber(5);
	cJSON_AddItemToObject(j_player, "dice-count", jtmp);

	jtmp = cJSON_CreateArray();
	cJSON_AddItemToObject(j_player, "dice", jtmp);

	jtmp = cJSON_CreateBool(false);
	cJSON_AddItemToObject(j_player, "ex-palifico", jtmp);

	return j_player;
}


void player_set_dice_values(struct tfdg_room *room_s, struct tfdg_player *player_s, unsigned char *bytes, int max_dice_value)
{
	int i;
	cJSON *j_array;
	cJSON *jtmp;

	j_array = cJSON_CreateArray();

	for(i=0; i<player_s->dice_count; i++){
		player_s->dice_values[i] = (bytes[i]%max_dice_value)+1;
		jtmp = cJSON_CreateNumber(player_s->dice_values[i]);
		cJSON_AddItemToArray(j_array, jtmp);
		room_s->totals[player_s->dice_values[i]-1]++;
	}
	cJSON_ReplaceItemInObject(player_s->json, "dice", j_array);
}


void player_set_state(struct tfdg_player *player_s, enum tfdg_player_state state)
{
	cJSON *jtmp;

	jtmp = cJSON_GetObjectItemCaseSensitive(player_s->json, "state");
	cJSON_SetNumberValue(jtmp, state);
	player_s->state = state;
}


void player_set_ex_palifico(struct tfdg_player *player_s)
{
	cJSON *jtmp;

	jtmp = cJSON_GetObjectItemCaseSensitive(player_s->json, "ex-palifico");
	jtmp->type = cJSON_True;
	player_s->ex_palifico = true;
}


void player_set_name(struct tfdg_player *player_s, char *name)
{
	cJSON *jtmp;

	jtmp = cJSON_GetObjectItemCaseSensitive(player_s->json, "name");
	cJSON_SetValuestring(jtmp, name);
	player_s->name = name;
}


void player_set_dice_count(struct tfdg_player *player_s, int dice_count)
{
	cJSON *jtmp;

	jtmp = cJSON_GetObjectItemCaseSensitive(player_s->json, "dice-count");
	cJSON_SetNumberValue(jtmp, dice_count);
	player_s->dice_count = dice_count;
}


void player_set_uuid(struct tfdg_player *player_s, char *uuid)
{
	cJSON *jtmp;

	jtmp = cJSON_GetObjectItemCaseSensitive(player_s->json, "uuid");
	cJSON_SetValuestring(jtmp, uuid);
	player_s->uuid = uuid;
}


void tfdg_handle_login(struct mosquitto *client, const char *room, struct tfdg_room *room_s, const struct mosquitto_acl_msg *msg)
{
	char *uuid = NULL;
	char *name = NULL;
	struct tfdg_player *player_s = NULL, *p;
	const char *client_id;

	if(json_parse_name_uuid(msg->payload, msg->payloadlen, &name, &uuid)){
		return;
	}

	if(room_s == NULL){
		printf(ANSI_RED "%s" ANSI_RESET " : " ANSI_GREEN "%-*s" ANSI_RESET "\n",
				room, MAX_LOG_LEN, "new-room");

		room_s = room_create(room);
		if(room_s == NULL) return;
	}

	room_set_last_event(room_s, time(NULL));

	find_player_from_json(msg->payload, msg->payloadlen, room_s, &player_s);

	if(room_s->state == tgs_lobby){
		if(player_s == NULL){
			player_s = calloc(1, sizeof(struct tfdg_player));
			if(player_s == NULL){
				free(name);
				free(uuid);
				return;
			}
			player_s->json = player_create_json();
			player_set_uuid(player_s, uuid);
			uuid = NULL;
			player_set_name(player_s, name);
			name = NULL;
			player_s->client_id = strdup(mosquitto_client_id(client));
			player_set_dice_count(player_s, room_s->options.max_dice);
			room_append_player(room_s, player_s, false);
			room_set_player_count(room_s, room_s->player_count+1);
			HASH_ADD_KEYPTR(hh_uuid, room_s->player_by_uuid, player_s->uuid,  strlen(player_s->uuid), player_s);
			HASH_ADD_KEYPTR(hh_client_id, room_s->player_by_client_id, player_s->client_id,  strlen(player_s->client_id), player_s);
		}else{
			HASH_FIND(hh_client_id, room_s->player_by_client_id, player_s->client_id, strlen(player_s->client_id), p);
			if(p){
				HASH_DELETE(hh_client_id, room_s->player_by_client_id, player_s);
			}
			free(player_s->client_id);
			player_s->client_id = strdup(mosquitto_client_id(client));
			HASH_ADD_KEYPTR(hh_client_id, room_s->player_by_client_id, player_s->client_id,  strlen(player_s->client_id), player_s);
		}
		printf(ANSI_BLUE "%s" ANSI_RESET " : " ANSI_GREEN "%-*s" ANSI_RESET " : "
				ANSI_MAGENTA "%s" ANSI_RESET " : " ANSI_CYAN "%s" ANSI_RESET "\n",
				room_s->uuid, MAX_LOG_LEN, "login", player_s->uuid, player_s->name);

		tfdg_send_lobby_players(room_s);
	}else{
		if(player_s != NULL){
			printf(ANSI_BLUE "%s" ANSI_RESET " : " ANSI_GREEN "%-*s" ANSI_RESET " : "
					ANSI_MAGENTA "%s" ANSI_RESET " : " ANSI_CYAN "%s" ANSI_RESET "\n",
					room_s->uuid, MAX_LOG_LEN, "re-login", player_s->uuid, player_s->name);

			client_id = mosquitto_client_id(client);
			HASH_FIND(hh_client_id, room_s->player_by_client_id, client_id, strlen(client_id), p);
			if(p){
				HASH_DELETE(hh_client_id, room_s->player_by_client_id, p);
			}

			free(player_s->client_id);
			player_s->client_id = strdup(client_id);

			HASH_ADD_KEYPTR(hh_client_id, room_s->player_by_client_id, player_s->client_id,  strlen(player_s->client_id), player_s);
			tfdg_send_current_state(room_s, player_s);
		}else{
			/* Spectator */
			player_s = calloc(1, sizeof(struct tfdg_player));
			if(player_s == NULL){
				free(name);
				free(uuid);
				return;
			}
			player_s->json = player_create_json();

			player_set_uuid(player_s, uuid);
			uuid = NULL;
			player_set_name(player_s, name);
			name = NULL;
			player_s->client_id = strdup(mosquitto_client_id(client));
			player_set_dice_count(player_s, 0);
			player_set_state(player_s, tps_spectator);

			HASH_FIND(hh_client_id, room_s->player_by_client_id, player_s->client_id, strlen(player_s->client_id), p);
			if(p){
				HASH_DELETE(hh_client_id, room_s->player_by_client_id, p);
			}

			HASH_ADD_KEYPTR(hh_client_id, room_s->player_by_client_id, player_s->client_id,  strlen(player_s->client_id), player_s);
			tfdg_send_current_state(room_s, player_s);

			printf(ANSI_BLUE "%s" ANSI_RESET " : " ANSI_GREEN "%-*s" ANSI_RESET " : "
					ANSI_MAGENTA "%s" ANSI_RESET " : " ANSI_CYAN "%s" ANSI_RESET "\n",
					room_s->uuid, MAX_LOG_LEN, "spectator", player_s->uuid, player_s->name);
		}
	}
	if(room_s->host == NULL){
		room_set_host(room_s, player_s);
	}
	player_s->login_count++;
	tfdg_send_host(room_s);
	free(name);
	free(uuid);
}


static cJSON *json_create_results_array(struct tfdg_room *room_s)
{
	struct tfdg_player *p, *start = NULL;
	cJSON *tree, *player, *array, *jtmp;
	int i;

	if(room_s->dudo_caller){
		CDL_FOREACH(room_s->players, p){
			if(p == room_s->dudo_caller){
				start = room_s->dudo_caller;
				break;
			}
		}
	}else if(room_s->calza_caller){
		CDL_FOREACH(room_s->players, p){
			if(p == room_s->calza_caller){
				start = room_s->calza_caller;
				break;
			}
		}
	}
	if(start == NULL){
		/* Starter was a player that has lost or left */
		start = room_s->players;
	}
	tree = cJSON_CreateArray();
	CDL_FOREACH(start, p){
		player = player_to_cjson(p);
		array = cJSON_CreateArray();
		for(i=0; i<p->dice_count; i++){
			if(p->dice_values[i] != 0){
				jtmp = cJSON_CreateNumber(p->dice_values[i]);
				cJSON_AddItemToArray(array, jtmp);
			}
		}
		cJSON_AddItemToObject(player, "dice", array);
		cJSON_AddItemToArray(tree, player);
	}
	return tree;
}


static void send_results(struct tfdg_room *room_s, const char *topic_suffix)
{
	cJSON *tree;

	printf(ANSI_BLUE "%s" ANSI_RESET " : " ANSI_GREEN "%-*s" ANSI_RESET " : " ANSI_MAGENTA "round %d" ANSI_RESET "\n",
			room_s->uuid, MAX_LOG_LEN, topic_suffix, room_s->round);

	tree = json_create_results_array(room_s);
	easy_publish(room_s, topic_suffix, tree);
	cJSON_Delete(tree);
}


static void report_results_to_losers(struct tfdg_room *room_s)
{
	send_results(room_s, "loser-results");
	report_summary_results(room_s, "loser-summary-results");
}


void report_player_results(struct tfdg_room *room_s)
{
	room_set_state(room_s, tgs_sending_results);

	send_results(room_s, "player-results");
}



void tfdg_handle_logout(struct mosquitto *client, struct tfdg_room *room_s, const struct mosquitto_acl_msg *msg)
{
	char *uuid = NULL;
	char *name = NULL;
	struct tfdg_player *player_s = NULL, *p;

	if(room_s == NULL) return;

	if(json_parse_name_uuid(msg->payload, msg->payloadlen, &name, &uuid)){
		return;
	}

	HASH_FIND(hh_uuid, room_s->player_by_uuid, uuid, strlen(uuid), player_s);
	if(player_s == NULL){
		free(name);
		free(uuid);
		return;
	}

	player_s->login_count--;
	if(player_s->login_count > 0){
		return;
	}

	HASH_FIND(hh_client_id, room_s->player_by_client_id, player_s->client_id, strlen(player_s->client_id), p);
	if(p){
		HASH_DELETE(hh_client_id, room_s->player_by_client_id, p);
	}
	if(room_s->state == tgs_lobby){
		HASH_DELETE(hh_uuid, room_s->player_by_uuid, player_s);
		room_delete_player(room_s, player_s);
		room_set_player_count(room_s, room_s->player_count-1);

		printf(ANSI_BLUE "%s" ANSI_RESET " : " ANSI_GREEN "%-*s" ANSI_RESET " : "
				ANSI_MAGENTA "%s" ANSI_RESET " : " ANSI_CYAN "%s" ANSI_RESET "\n",
				room_s->uuid, MAX_LOG_LEN, "logout", player_s->uuid, player_s->name);

		cleanup_player(player_s);
	}
	free(name);
	free(uuid);

	if(room_s->players == NULL){
		cleanup_room(room_s, "lobby");
	}else{
		if(player_s == room_s->host){
			room_set_host(room_s, room_s->players);
			tfdg_send_host(room_s);
		}

		if(room_s->state == tgs_lobby){
			tfdg_send_lobby_players(room_s);
		}else{
			// FIXME - send warning
		}
	}
}


void tfdg_new_round(struct tfdg_room *room_s)
{
	struct tfdg_player *p;
	int i;
	int count;
	unsigned char bytes[1000];
	cJSON *tree, *jtmp;
	int max_dice_value;

	// FIXME - checks on current state
	if(room_s->player_count > 199){
		/* buffer size protection */
		return;
	}
	count = room_s->player_count*room_s->options.max_dice + 1;
	/* +1 is for random_max_dice_value */

	CDL_FOREACH(room_s->players, p){
		if(p->dice_count == 0){
			tfdg_handle_player_lost(room_s, p);
		}
	}

	room_set_round(room_s, room_s->round+1);
	room_set_calza_caller(room_s, NULL);
	room_set_dudo_caller(room_s, NULL);
	room_set_round_loser(room_s, NULL);
	room_set_round_winner(room_s, NULL);

	if(RAND_bytes(bytes, count) == 1){
		if(room_s->round == 1 || room_s->options.random_max_dice_value == false){
			max_dice_value = room_s->options.max_dice_value;
		}else{
			max_dice_value = 3 + (bytes[count-1] % (room_s->options.max_dice_value - 3 + 1));
			publish_int_option(room_s, "max-dice-value", max_dice_value);
		}
		room_set_state(room_s, tgs_playing_round);
		i = 0;
		CDL_FOREACH(room_s->players, p){
			player_set_dice_values(room_s, p, &bytes[room_s->options.max_dice*i], max_dice_value);
			player_set_state(p, tps_awaiting_dice);
			i++;
		}

		printf(ANSI_BLUE "%s" ANSI_RESET " : " ANSI_GREEN "%-*s" ANSI_RESET " : "
				ANSI_MAGENTA "%d (%d players)" ANSI_RESET "\n",
				room_s->uuid, MAX_LOG_LEN, "new-round", room_s->round, room_s->current_count);

		tree = cJSON_CreateObject();
		jtmp = player_to_cjson(room_s->starter);
		cJSON_AddItemToObject(tree, "starter", jtmp);

		jtmp = cJSON_CreateBool(room_s->palifico_round);
		cJSON_AddItemToObject(tree, "palifico-round", jtmp);

		easy_publish(room_s, "new-round", tree);
		cJSON_Delete(tree);

		report_results_to_losers(room_s);
	}
}


/* Remove rooms that haven't seen any changes in two hours */
void tfdg_expire_rooms(void)
{
	struct tfdg_room *room_s, *room_tmp;
	time_t now;

	now = time(NULL);
	HASH_ITER(hh, room_by_uuid, room_s, room_tmp){
		if(now > room_s->last_event + room_expiry_time){
			printf(ANSI_BLUE "%s" ANSI_RESET " : " ANSI_GREEN "%-*s" ANSI_RESET " : "
					ANSI_MAGENTA "%d players" ANSI_RESET "\n",
					room_s->uuid, MAX_LOG_LEN, "room-expiring", room_s->current_count);

			cleanup_room(room_s, "expire");
		}
	}
}


void tfdg_handle_start_game(struct mosquitto *client, struct tfdg_room *room_s, const struct mosquitto_acl_msg *msg)
{
	unsigned char bytes[1];
	struct tfdg_player *player_s, *p;
	int i;

	tfdg_expire_rooms();

	if(room_s == NULL || room_s->state != tgs_lobby || room_s->player_count < 2){
		return;
	}

	player_s = find_player_check_id(client, room_s, msg);
	if(player_s == NULL || player_s != room_s->host) return;

	room_s->current_count = room_s->player_count;
	printf(ANSI_BLUE "%s" ANSI_RESET " : " ANSI_GREEN "%-*s" ANSI_RESET " : "
			ANSI_MAGENTA "%d players" ANSI_RESET "(%d)\n",
			room_s->uuid, MAX_LOG_LEN, "start-game", room_s->current_count, HASH_COUNT(room_by_uuid));

	room_shuffle_players(room_s);
	tfdg_send_lobby_players(room_s);

	CDL_FOREACH(room_s->players, p){
		player_set_dice_count(p, room_s->options.max_dice);
	}

	if(RAND_bytes(bytes, 1) == 1){
		room_s->starter = room_s->players;
		for(i=0; i<bytes[0]%room_s->player_count; i++){
			room_set_starter(room_s, room_s->starter->next);
		}
	}else{
		room_set_starter(room_s, room_s->players);
	}
	room_set_start_time(room_s, time(NULL));
	room_set_last_event(room_s, room_s->start_time);

	if(room_s->options.roll_dice_at_start){
		room_pre_roll_init(room_s);
	}else{
		tfdg_new_round(room_s);
	}
}


static cJSON *json_create_my_dice_array(struct tfdg_player *player_s)
{
	cJSON *tree, *jtmp;
	int i;

	tree = cJSON_CreateArray();
	for(i=0; i<player_s->dice_count; i++){
		jtmp = cJSON_CreateNumber(player_s->dice_values[i]);
		if(jtmp == NULL){
			return NULL;
		}
		cJSON_AddItemToArray(tree, jtmp);
	}
	return tree;
}


void send_dice(struct tfdg_room *room_s, struct tfdg_player *player_s)
{
	cJSON *tree;
	char *json_str;
	char topic[200];

	tree = json_create_my_dice_array(player_s);
	json_str = cJSON_PrintUnformatted(tree);
	cJSON_Delete(tree);
	if(json_str){
		snprintf(topic, sizeof(topic), "tfdg/%s/dice/%s", room_s->uuid, player_s->uuid);
		mosquitto_broker_publish(NULL, topic, strlen(json_str), json_str, 1, 0, NULL);
		printf(ANSI_BLUE "%s" ANSI_RESET " : " ANSI_GREEN "%-*s" ANSI_RESET " : "
			ANSI_MAGENTA "%s" ANSI_RESET " : " ANSI_CYAN "%s" ANSI_RESET "\n",
			room_s->uuid, MAX_LOG_LEN, "send-dice", player_s->uuid, player_s->name);
	}
}


cJSON *room_pre_roll_to_cjson(struct tfdg_room *room_s)
{
	cJSON *tree, *j_player, *jtmp;
	struct tfdg_player *p;

	tree = cJSON_CreateArray();
	CDL_FOREACH(room_s->players, p){
		if(p->state == tps_pre_roll){
			j_player = player_to_cjson(p);
			jtmp = cJSON_CreateNumber(p->pre_roll);
			cJSON_AddItemToObject(j_player, "value", jtmp);
			cJSON_AddItemToArray(tree, j_player);
		}
	}
	return tree;
}


void room_pre_roll_init(struct tfdg_room *room_s)
{
	struct tfdg_player *p;
	unsigned char bytes[1000];
	int i;
	cJSON *tree = NULL, *j_player;

	room_set_state(room_s, tgs_pre_roll);
	RAND_bytes(bytes, room_s->player_count);
	i = 0;
	CDL_FOREACH(room_s->players, p){
		if(p->state != tps_pre_roll_lost){
			p->pre_roll = (bytes[i] % room_s->options.max_dice_value) + 1;
			player_set_state(p, tps_pre_roll);
			i++;

			j_player = player_to_cjson(p);
			if(tree == NULL){
				tree = cJSON_CreateArray();
			}
			cJSON_AddItemToArray(tree, j_player);
		}
	}
	room_s->pre_roll_count = i;
	easy_publish(room_s, "pre-roll-init", tree);
	cJSON_Delete(tree);
}


void tfdg_handle_pre_roll_result(struct tfdg_room *room_s)
{
	int i;
	cJSON *tree = NULL, *j_player;
	int max_rolled = 0, max_rolled_count = 0;
	struct tfdg_player *p, *starter = NULL;

	for(i=room_s->options.max_dice_value+1; i>=0; i--){
		CDL_FOREACH(room_s->players, p){
			if(p->state == tps_pre_roll_sent){
				if(p->pre_roll == i){
					max_rolled_count++;
				}
			}
		}
		if(max_rolled_count > 0){
			max_rolled = i;
			break;
		}
	}
	CDL_FOREACH(room_s->players, p){
		if(p->state == tps_pre_roll_sent && p->pre_roll == max_rolled){
			player_set_state(p, tps_pre_roll);
			j_player = player_to_cjson(p);
			if(tree == NULL){
				tree = cJSON_CreateArray();
			}
			cJSON_AddItemToArray(tree, j_player);
			starter = p;
		}else{
			player_set_state(p, tps_pre_roll_lost);
		}
	}
	easy_publish(room_s, "pre-roll-results", tree);
	cJSON_Delete(tree);
	
	if(max_rolled_count == 1){
		room_set_state(room_s, tgs_pre_roll_over);
		room_set_starter(room_s, starter);
	}else{
		room_pre_roll_init(room_s);
	}
}


void tfdg_handle_pre_roll_dice(struct tfdg_room *room_s, struct tfdg_player *player_s)
{
	cJSON *tree, *jtmp;

	if(player_s->state == tps_pre_roll){
		player_set_state(player_s, tps_pre_roll_sent);

		tree = player_to_cjson(player_s);
		if(tree){
			jtmp = cJSON_CreateNumber(player_s->pre_roll);
			cJSON_AddItemToObject(tree, "value", jtmp);

			easy_publish(room_s, "pre-roll", tree);
			cJSON_Delete(tree);
		}
		room_s->pre_roll_count--;
		if(room_s->pre_roll_count == 0){
			tfdg_handle_pre_roll_result(room_s);
		}
	}
}


void tfdg_handle_roll_dice(struct mosquitto *client, struct tfdg_room *room_s, const struct mosquitto_acl_msg *msg)
{
	struct tfdg_player *player_s = NULL;

	player_s = find_player_check_id(client, room_s, msg);
	if(player_s == NULL) return;

	if(room_s->state == tgs_pre_roll){
		tfdg_handle_pre_roll_dice(room_s, player_s);
		return;
	}else if(room_s->state == tgs_pre_roll_over){
		tfdg_new_round(room_s);
	}

	if(room_s->state == tgs_round_over){
		tfdg_new_round(room_s);
	}

	if(player_s->state != tps_awaiting_dice){
		return;
	}

	send_dice(room_s, player_s);
	player_set_state(player_s, tps_have_dice);
}


static cJSON *json_create_dudo_candidates_object(struct tfdg_room *room_s)
{
	cJSON *tree = NULL, *j_player;

	if(room_s->dudo_caller){
		tree = cJSON_CreateArray();

		j_player = player_to_cjson(room_s->dudo_caller);
		cJSON_AddItemToArray(tree, j_player);

		j_player = player_to_cjson(room_s->dudo_caller->prev);
		cJSON_AddItemToArray(tree, j_player);
	}
	return tree;
}


static void report_summary_results(struct tfdg_room *room_s, const char *topic_suffix)
{
	cJSON *tree, *array, *jtmp;
	struct tfdg_player *p;
	int totals[MAX_DICE_VALUE], totals_wild[MAX_DICE_VALUE];
	int i;

	memset(totals, 0, sizeof(int)*MAX_DICE_VALUE);
	memset(totals_wild, 0, sizeof(int)*MAX_DICE_VALUE);

	CDL_FOREACH(room_s->players, p){
		for(i=0; i<p->dice_count; i++){
			totals[p->dice_values[i]-1]++;
		}
	}
	totals_wild[0] = totals[0];
	for(i=1; i<room_s->options.max_dice_value; i++){
		totals_wild[i] = totals[0] + totals[i];
	}

	tree = cJSON_CreateObject();

	array = cJSON_CreateArray();
	cJSON_AddItemToObject(tree, "totals", array);
	for(i=0; i<room_s->options.max_dice_value; i++){
		if(room_s->palifico_round){
			jtmp = cJSON_CreateNumber(totals[i]);
		}else{
			jtmp = cJSON_CreateNumber(totals_wild[i]);
		}
		cJSON_AddItemToArray(array, jtmp);
	}

	easy_publish(room_s, topic_suffix, tree);
	cJSON_Delete(tree);
}


void tfdg_handle_call_dudo(struct mosquitto *client, struct tfdg_room *room_s, const struct mosquitto_acl_msg *msg)
{
	struct tfdg_player *player_s = NULL, *p;
	cJSON *tree;

	player_s = find_player_check_id(client, room_s, msg);
	if(player_s == NULL) return;

	if(player_s->state != tps_have_dice
			|| room_s->state != tgs_playing_round){

		return;
	}

	CDL_FOREACH(room_s->players, p){
		player_set_state(player_s, tps_awaiting_loser);
	}
	player_set_state(player_s, tps_dudo_candidate);
	player_set_state(player_s->prev, tps_dudo_candidate);
	room_set_dudo_caller(room_s, player_s);

	tree = json_create_dudo_candidates_object(room_s);
	if(tree){
		printf(ANSI_BLUE "%s" ANSI_RESET " : " ANSI_GREEN "%-*s" ANSI_RESET " : "
			ANSI_MAGENTA "%s" ANSI_RESET " : " ANSI_CYAN "%s" ANSI_RESET "\n",
			room_s->uuid, MAX_LOG_LEN, "call-dudo", player_s->uuid, player_s->name);

		easy_publish(room_s, "dudo-candidates", tree);
		cJSON_Delete(tree);
	}

	report_player_results(room_s);
	report_summary_results(room_s, "summary-results");
	room_set_state(room_s, tgs_awaiting_loser);
}


void tfdg_handle_call_calza(struct mosquitto *client, struct tfdg_room *room_s, const struct mosquitto_acl_msg *msg)
{
	struct tfdg_player *player_s = NULL, *p;

	if(room_s->options.allow_calza == false){
		return;
	}

	player_s = find_player_check_id(client, room_s, msg);
	if(player_s == NULL) return;


	if(player_s->state != tps_have_dice){
		return;
	}
	if(player_s->dice_count == room_s->options.max_dice){
		return;
	}
	printf(ANSI_BLUE "%s" ANSI_RESET " : " ANSI_GREEN "%-*s" ANSI_RESET " : "
			ANSI_MAGENTA "%s" ANSI_RESET " : " ANSI_CYAN "%s" ANSI_RESET "\n",
			room_s->uuid, MAX_LOG_LEN, "call-calza", player_s->uuid, player_s->name);

	CDL_FOREACH(room_s->players, p){
		player_set_state(p, tps_awaiting_loser);
	}
	player_set_state(player_s, tps_calza_candidate);
	room_set_calza_caller(room_s, player_s);

	easy_publish_player(room_s, "calza-candidate", player_s);

	report_player_results(room_s);
	report_summary_results(room_s, "summary-results");
	room_set_state(room_s, tgs_awaiting_loser);
}


static cJSON *room_dice_totals(struct tfdg_room *room_s)
{
	cJSON *array, *jtmp;
	int i;

	array = cJSON_CreateArray();
	for(i=0; i<room_s->options.max_dice_value; i++){
		jtmp = cJSON_CreateNumber(room_s->totals[i]);
		cJSON_AddItemToArray(array, jtmp);
	}
	return array;
}


static void room_add_to_stats(struct tfdg_room *room_s)
{
	int i;

	if(room_s->player_count > 1 && room_s->player_count < 100){
		stats.players[room_s->player_count]++;
		if(room_s->player_count > stats.max_players){
			stats.max_players = room_s->player_count;
		}
		stats.duration_counts[room_s->player_count]++;
		stats.durations[room_s->player_count] += time(NULL)-room_s->start_time;
	}
	stats.calza_success += room_s->calza_success;
	stats.calza_fail += room_s->calza_fail;
	stats.dudo_success += room_s->dudo_success;
	stats.dudo_fail += room_s->dudo_fail;

	if(room_s->options.max_dice >= 3 && room_s->options.max_dice <= 20){
		stats.dice_count[room_s->options.max_dice]++;
	}
	if(room_s->options.max_dice_value >= 3 && room_s->options.max_dice_value <= 9){
		stats.dice_values[room_s->options.max_dice_value]++;
	}
	for(i=0; i<10; i++){
		stats.thrown_dice_values[i] += room_s->totals[i];
	}
	publish_stats();
}


static void tfdg_handle_winner(struct tfdg_room *room_s)
{
	cJSON *tree, *array, *j_player;

	tree = cJSON_CreateObject();
	array = room_dice_totals(room_s);
	cJSON_AddItemToObject(tree, "totals", array);

	j_player = player_to_cjson(room_s->players);
	cJSON_AddItemToObject(tree, "winner", j_player);
	easy_publish(room_s, "winner", tree);
	cJSON_Delete(tree);

	room_add_to_stats(room_s);
	room_set_state(room_s, tgs_game_over);
	easy_publish(room_s, "room-closing", NULL);
}


void tfdg_handle_kick_player(struct mosquitto *client, struct tfdg_room *room_s, const struct mosquitto_acl_msg *msg)
{
	struct tfdg_player *kicker_s, *player_s = NULL;
	const char *client_id;

	/* Find the player structure described by '{"uuid":""}' if it is in this room */
	if(room_s == NULL || find_player_from_json(msg->payload, msg->payloadlen, room_s, &player_s)){
		return;
	}

	client_id = mosquitto_client_id(client);
	HASH_FIND(hh_client_id, room_s->player_by_client_id, client_id, strlen(client_id), kicker_s);

	if(kicker_s && room_s->host == kicker_s &&
			(room_s->state == tgs_lobby || room_s->state == tgs_playing_round || room_s->state == tgs_round_over || room_s->state == tgs_game_over)){

		printf(ANSI_BLUE "%s" ANSI_RESET " : " ANSI_GREEN "%-*s" ANSI_RESET " : "
				ANSI_MAGENTA "%s" ANSI_RESET " : " ANSI_CYAN "%s" ANSI_RESET "\n",
				room_s->uuid, MAX_LOG_LEN, "kick-player", player_s->uuid, player_s->name);

		easy_publish_player(room_s, "player-left", player_s);

		room_delete_player(room_s, player_s);
		/* Don't add to lost players, they were kicked for a reason */
		room_set_current_count(room_s, room_s->current_count-1);

		if(room_s->current_count == 1){
			tfdg_handle_winner(room_s);
			return;
		}
	}
}


void tfdg_handle_leave_game(struct mosquitto *client, struct tfdg_room *room_s, const struct mosquitto_acl_msg *msg)
{
	struct tfdg_player *player_s = NULL;

	player_s = find_player_check_id(client, room_s, msg);
	if(player_s == NULL) return;

	if(room_s->state == tgs_playing_round || room_s->state == tgs_round_over || room_s->state == tgs_game_over){

		printf(ANSI_BLUE "%s" ANSI_RESET " : " ANSI_GREEN "%-*s" ANSI_RESET " : "
				ANSI_MAGENTA "%s" ANSI_RESET " : " ANSI_CYAN "%s" ANSI_RESET "\n",
				room_s->uuid, MAX_LOG_LEN, "leave-game", player_s->uuid, player_s->name);

		easy_publish_player(room_s, "player-left", player_s);

		room_delete_player(room_s, player_s);
		room_append_lost_player(room_s, player_s);
		room_set_current_count(room_s, room_s->current_count-1);

		if(room_s->current_count == 1){
			tfdg_handle_winner(room_s);
			return;
		}
	}else if(room_s->state == tgs_lobby){
		tfdg_handle_logout(client, room_s, msg);
	}
}


void tfdg_handle_undo_loser(struct mosquitto *client, struct tfdg_room *room_s, const struct mosquitto_acl_msg *msg)
{
	struct tfdg_player *player_s = NULL;
	cJSON *tree;

	player_s = find_player_check_id(client, room_s, msg);
	if(player_s == NULL) return;

	/* Check that the client is in the correct state. */
	if(player_s->state != tps_dudo_candidate && player_s->state != tps_calza_candidate){
		return;
	}
	/* Check that the room is in the correct state. */
	if(room_s->state != tgs_round_over){
		return;
	}
	/* Check we have the round loser */
	if(room_s->round_loser != player_s){
		return;
	}
	printf(ANSI_BLUE "%s" ANSI_RESET " : " ANSI_GREEN "%-*s" ANSI_RESET " : "
			ANSI_MAGENTA "%s" ANSI_RESET " : " ANSI_CYAN "%s" ANSI_RESET "\n",
			room_s->uuid, MAX_LOG_LEN, "undo-loser", player_s->uuid, player_s->name);

	player_set_dice_count(player_s, player_s->dice_count+1);
	room_set_round_loser(room_s, NULL);

	if(player_s->state == tps_dudo_candidate){
		tree = json_create_dudo_candidates_object(room_s);
	}else{
		tree = player_to_cjson(player_s);
	}
	if(tree){
		easy_publish(room_s, "undo-loser", tree);
		cJSON_Delete(tree);
	}
}


void tfdg_handle_undo_winner(struct mosquitto *client, struct tfdg_room *room_s, const struct mosquitto_acl_msg *msg)
{
	struct tfdg_player *player_s = NULL;

	player_s = find_player_check_id(client, room_s, msg);
	if(player_s == NULL) return;

	/* Check that the client is in the correct state. */
	if(player_s->state != tps_calza_candidate){
		return;
	}
	/* Check that the room is in the correct state. */
	if(room_s->state != tgs_round_over){
		return;
	}
	/* Check we have the round winner */
	if(room_s->round_winner != player_s){
		return;
	}
	printf(ANSI_BLUE "%s" ANSI_RESET " : " ANSI_GREEN "%-*s" ANSI_RESET " : "
			ANSI_MAGENTA "%s" ANSI_RESET " : " ANSI_CYAN "%s" ANSI_RESET "\n",
			room_s->uuid, MAX_LOG_LEN, "undo-winner", player_s->uuid, player_s->name);

	player_set_dice_count(player_s, player_s->dice_count-1);

	easy_publish_player(room_s, "undo-winner", player_s);
}


static void tfdg_handle_player_lost(struct tfdg_room *room_s, struct tfdg_player *player_s)
{
	printf(ANSI_BLUE "%s" ANSI_RESET " : " ANSI_GREEN "%-*s" ANSI_RESET " : "
			ANSI_MAGENTA "%s" ANSI_RESET " : " ANSI_CYAN "%s" ANSI_RESET "\n",
			room_s->uuid, MAX_LOG_LEN, "game-lost", player_s->uuid, player_s->name);

	room_set_starter(room_s, player_s->next);

	room_delete_player(room_s, player_s);
	CDL_DELETE(room_s->players, player_s);
	DL_APPEND(room_s->lost_players, player_s);
	room_s->current_count--;

	easy_publish_player(room_s, "player-lost", player_s);
}


void tfdg_handle_i_lost(struct mosquitto *client, struct tfdg_room *room_s, const struct mosquitto_acl_msg *msg)
{
	struct tfdg_player *player_s = NULL;

	player_s = find_player_check_id(client, room_s, msg);
	if(player_s == NULL) return;
	if(room_s->round_loser) return; /* Another player has already pressed Loser, prevent a race condition */

	/* Check that the client is in the correct state. */
	if(player_s->state == tps_dudo_candidate){
		if(room_s->dudo_caller == player_s){
			room_set_dudo_fail(room_s, room_s->dudo_fail+1);
		}else{
			room_set_dudo_success(room_s, room_s->dudo_success+1);
		}
	}else if(player_s->state == tps_calza_candidate){
		room_set_calza_fail(room_s, room_s->calza_fail+1);
	}else{
		return;
	}
	room_set_state(room_s, tgs_round_over);
	room_set_round_loser(room_s, player_s);
	player_set_dice_count(player_s, player_s->dice_count-1);

	if(player_s->dice_count == 0 && room_s->current_count == 2){
		/* Game is over, no chance of undo */
		tfdg_handle_player_lost(room_s, player_s);
		tfdg_handle_winner(room_s);
	}else{
		printf(ANSI_BLUE "%s" ANSI_RESET " : " ANSI_GREEN "%-*s" ANSI_RESET " : "
				ANSI_MAGENTA "%s" ANSI_RESET " : " ANSI_CYAN "%s" ANSI_RESET "\n",
				room_s->uuid, MAX_LOG_LEN, "round-lost", player_s->uuid, player_s->name);

		easy_publish_player(room_s, "round-loser", player_s);
		if(player_s->dice_count == 0){
			easy_publish_player(room_s, "game-loser", player_s);
		}
		room_set_starter(room_s, player_s);

		room_set_palifico_round(room_s, false);
		if(player_s->dice_count == 1 && room_s->current_count > 2 && player_s->ex_palifico == false){
			player_set_ex_palifico(player_s);
			room_set_palifico_round(room_s, true);
		}
	}
}


void tfdg_handle_i_won(struct mosquitto *client, struct tfdg_room *room_s, const struct mosquitto_acl_msg *msg)
{
	struct tfdg_player *player_s = NULL;

	/* Find the player structure described by '{"uuid":""}' if it is in this room */
	if(room_s == NULL || find_player_from_json(msg->payload, msg->payloadlen, room_s, &player_s)){
		return;
	}
	/* Check that the client sending this message matches the client that is
	 * attached to this player. */
	if(strcmp(mosquitto_client_id(client), player_s->client_id)){
		return;
	}
	/* Check that the client is in the correct state. */
	if(player_s->state != tps_calza_candidate){
		return;
	}
	room_set_calza_success(room_s, room_s->calza_success+1);
	room_set_state(room_s, tgs_round_over);
	room_set_round_winner(room_s, player_s);
	player_set_dice_count(player_s, player_s->dice_count+1);

	printf(ANSI_BLUE "%s" ANSI_RESET " : " ANSI_GREEN "%-*s" ANSI_RESET " : "
			ANSI_MAGENTA "%s" ANSI_RESET " : " ANSI_CYAN "%s" ANSI_RESET "\n",
			room_s->uuid, MAX_LOG_LEN, "calza-won", player_s->uuid, player_s->name);

	easy_publish_player(room_s, "round-winner", player_s);
	room_set_starter(room_s, player_s);
}

void publish_bool_option(struct tfdg_room *room_s, const char *option, bool value)
{
	cJSON *tree, *jtmp;

	tree = cJSON_CreateObject();
	jtmp = cJSON_CreateBool(value);
	cJSON_AddItemToObject(tree, option, jtmp);

	easy_publish(room_s, "set-option", tree);
	cJSON_Delete(tree);
}

void publish_int_option(struct tfdg_room *room_s, const char *option, int value)
{
	cJSON *tree, *jtmp;

	tree = cJSON_CreateObject();
	jtmp = cJSON_CreateNumber(value);
	cJSON_AddItemToObject(tree, option, jtmp);

	easy_publish(room_s, "set-option", tree);
	cJSON_Delete(tree);
}


void tfdg_handle_sound(struct mosquitto *client, struct tfdg_room *room_s, const struct mosquitto_acl_msg *msg, const char *type)
{
	char topic[200];
	uint8_t value;
	cJSON *tree, *jtmp;
	char *json_str;

	if(room_s->state != tgs_playing_round){
		return;
	}
	RAND_bytes(&value, 1);

	tree = cJSON_CreateObject();
	jtmp = cJSON_CreateNumber(value);
	cJSON_AddItemToObject(tree, "sound", jtmp);
	json_str = cJSON_PrintUnformatted(tree);
	cJSON_Delete(tree);

	snprintf(topic, sizeof(topic), "tfdg/%s/snd-%s", room_s->uuid, type);
	mosquitto_broker_publish(NULL, topic, strlen(json_str), json_str, 1, 0, NULL);
}


void tfdg_handle_set_option(struct mosquitto *client, struct tfdg_room *room_s, const struct mosquitto_acl_msg *msg)
{
	struct tfdg_player *player_s = NULL;
	cJSON *tree, *j_option, *j_value;
	int ival;

	player_s = find_player_check_id(client, room_s, msg);
	if(player_s == NULL) return;

	if(room_s->state != tgs_lobby
			|| player_s != room_s->host){

		return;
	}

	tree = cJSON_ParseWithLength(msg->payload, msg->payloadlen);
	if(tree){
		j_option = cJSON_GetObjectItemCaseSensitive(tree, "option");
		if(j_option == NULL || cJSON_IsString(j_option) == false){
			cJSON_Delete(tree);
			return;
		}
		j_value = cJSON_GetObjectItemCaseSensitive(tree, "value");
		if(j_value == NULL){
			cJSON_Delete(tree);
			return;
		}

		if(strcmp(j_option->valuestring, "max-dice") == 0){
			if(cJSON_IsNumber(j_value)){
				ival = j_value->valueint;
				if(ival >= 3 && ival <= MAX_DICE){
					room_set_option_int(room_s, &room_s->options.max_dice, "max-dice", ival);

					printf(ANSI_BLUE "%s" ANSI_RESET " : " ANSI_GREEN "%-*s" ANSI_RESET " : "
							ANSI_MAGENTA "%s" ANSI_RESET " : " ANSI_CYAN "%s" ANSI_RESET " %s = %d\n",
							room_s->uuid, MAX_LOG_LEN, "setting-option", player_s->uuid, player_s->name,
							"max-dice", ival);

					publish_int_option(room_s, "max-dice", ival);
				}
			}
		}else if(strcmp(j_option->valuestring, "max-dice-value") == 0){
			if(cJSON_IsNumber(j_value)){
				ival = j_value->valueint;
				if(ival >= 3 && ival <= MAX_DICE_VALUE){
					room_set_option_int(room_s, &room_s->options.max_dice_value, "max-dice-value", ival);

					printf(ANSI_BLUE "%s" ANSI_RESET " : " ANSI_GREEN "%-*s" ANSI_RESET " : "
							ANSI_MAGENTA "%s" ANSI_RESET " : " ANSI_CYAN "%s" ANSI_RESET " %s = %d\n",
							room_s->uuid, MAX_LOG_LEN, "setting-option", player_s->uuid, player_s->name,
							"max-dice-value", ival);

					publish_int_option(room_s, "max-dice-value", ival);
				}
			}
		}else if(strcmp(j_option->valuestring, "random-max-dice-value") == 0){
			if(cJSON_IsBool(j_value)){
				room_set_option_bool(room_s, &room_s->options.random_max_dice_value, "random-max-dice-value", cJSON_IsTrue(j_value));

				printf(ANSI_BLUE "%s" ANSI_RESET " : " ANSI_GREEN "%-*s" ANSI_RESET " : "
						ANSI_MAGENTA "%s" ANSI_RESET " : " ANSI_CYAN "%s" ANSI_RESET " %s = %d\n",
						room_s->uuid, MAX_LOG_LEN, "setting-option", player_s->uuid, player_s->name,
						"random-max-dice-value", cJSON_IsTrue(j_value));

				publish_bool_option(room_s, "random-max-dice-value", cJSON_IsTrue(j_value));
			}
		}else if(strcmp(j_option->valuestring, "allow-calza") == 0){
			if(cJSON_IsBool(j_value)){
				room_set_option_bool(room_s, &room_s->options.allow_calza, "allow-calza", cJSON_IsTrue(j_value));

				printf(ANSI_BLUE "%s" ANSI_RESET " : " ANSI_GREEN "%-*s" ANSI_RESET " : "
						ANSI_MAGENTA "%s" ANSI_RESET " : " ANSI_CYAN "%s" ANSI_RESET " %s = %d\n",
						room_s->uuid, MAX_LOG_LEN, "setting-option", player_s->uuid, player_s->name,
						"allow-calza", cJSON_IsTrue(j_value));

				publish_bool_option(room_s, "allow-calza", cJSON_IsTrue(j_value));
			}
		}else if(strcmp(j_option->valuestring, "roll-dice-at-start") == 0){
			if(cJSON_IsBool(j_value)){
				room_set_option_bool(room_s, &room_s->options.roll_dice_at_start, "roll-dice-at-start", cJSON_IsTrue(j_value));

				printf(ANSI_BLUE "%s" ANSI_RESET " : " ANSI_GREEN "%-*s" ANSI_RESET " : "
						ANSI_MAGENTA "%s" ANSI_RESET " : " ANSI_CYAN "%s" ANSI_RESET " %s = %d\n",
						room_s->uuid, MAX_LOG_LEN, "setting-option", player_s->uuid, player_s->name,
						"roll-dice-at-start", cJSON_IsTrue(j_value));

				publish_bool_option(room_s, "roll-dice-at-start", cJSON_IsTrue(j_value));
			}
		}else if(strcmp(j_option->valuestring, "show-results-table") == 0){
			if(cJSON_IsBool(j_value)){
				room_set_option_bool(room_s, &room_s->options.show_results_table, "show-results-table", cJSON_IsTrue(j_value));

				printf(ANSI_BLUE "%s" ANSI_RESET " : " ANSI_GREEN "%-*s" ANSI_RESET " : "
						ANSI_MAGENTA "%s" ANSI_RESET " : " ANSI_CYAN "%s" ANSI_RESET " %s = %d\n",
						room_s->uuid, MAX_LOG_LEN, "setting-option", player_s->uuid, player_s->name,
						"show-results-table", cJSON_IsTrue(j_value));

				publish_bool_option(room_s, "show-results-table", cJSON_IsTrue(j_value));
			}
		}else if(strcmp(j_option->valuestring, "losers-see-dice") == 0){
			if(cJSON_IsBool(j_value)){
				room_set_option_bool(room_s, &room_s->options.losers_see_dice, "losers-see-dice", cJSON_IsTrue(j_value));

				printf(ANSI_BLUE "%s" ANSI_RESET " : " ANSI_GREEN "%-*s" ANSI_RESET " : "
						ANSI_MAGENTA "%s" ANSI_RESET " : " ANSI_CYAN "%s" ANSI_RESET " %s = %d\n",
						room_s->uuid, MAX_LOG_LEN, "setting-option", player_s->uuid, player_s->name,
						"losers-see-dice", cJSON_IsTrue(j_value));

				publish_bool_option(room_s, "losers-see-dice", cJSON_IsTrue(j_value));
			}
		}
		cJSON_Delete(tree);
	}
}



int mosquitto_auth_acl_check(void *user_data, int access, struct mosquitto *client, const struct mosquitto_acl_msg *msg)
{
	struct tfdg_room *room_s = NULL;
	struct tfdg_player *player_s = NULL;
	char *room;
	char *cmd;
	char *player;
	const char *client_id;

	if(strncmp(msg->topic, "tfdg/", 5) != 0){
		/* We only want messages in the 'tfdg/' tree. */
		return MOSQ_ERR_PLUGIN_DEFER;
	}

	/* Subscription access check */
	if(access == MOSQ_ACL_SUBSCRIBE){
		if(strcmp(msg->topic, "tfdg/#") == 0
				|| strcmp(msg->topic, "tfdg/stats") == 0){

			return MOSQ_ERR_SUCCESS;
		}else{
			return MOSQ_ERR_ACL_DENIED;
		}
	}else if(access == MOSQ_ACL_READ){
		if(strcmp(msg->topic, "tfdg/stats") == 0){
			return MOSQ_ERR_SUCCESS;
		}
	}

	tfdg_topic_tokenise(msg->topic+5, &room, &cmd, &player);

	if(room == NULL || cmd == NULL){
		free(room);
		free(cmd);
		free(player);
		return MOSQ_ERR_ACL_DENIED;
	}

	if(validate_uuid(room) == false){
		free(room);
		free(cmd);
		free(player);
		return MOSQ_ERR_ACL_DENIED;
	}
	if(player != NULL && validate_uuid(player) == false){
		free(room);
		free(cmd);
		free(player);
		return MOSQ_ERR_ACL_DENIED;
	}

	HASH_FIND(hh, room_by_uuid, room, strlen(room), room_s);

	if(access == MOSQ_ACL_READ){
		free(room);

		if(strcmp(cmd, "room-closing") == 0 && room_s && room_s->state == tgs_game_over){
			free(cmd);
			free(player);
			cleanup_room(room_s, "game-over");
			return MOSQ_ERR_ACL_DENIED;
		}

		/* Players can only read messages from:
		 * tfdg/<room>/dice/<player>
		 * tfdg/<room>/<cmds>
		 */
		if((strcmp(cmd, "dice") == 0 || strcmp(cmd, "msg") == 0)){
			free(cmd);
			if(player == NULL || room_s == NULL){
				free(player);
				return MOSQ_ERR_ACL_DENIED;
			}
			client_id = mosquitto_client_id(client);
			HASH_FIND(hh_client_id, room_s->player_by_client_id, client_id, strlen(client_id), player_s);


			if(player_s == NULL ||
					strcmp(player_s->uuid, player) != 0){

				free(player);
				return MOSQ_ERR_ACL_DENIED;
			}else{
				free(player);
				return MOSQ_ERR_SUCCESS;
			}
		}else if(strcmp(cmd, "loser-results") == 0 || strcmp(cmd, "loser-summary-results") == 0){
			client_id = mosquitto_client_id(client);
			DL_FOREACH(room_s->lost_players, player_s){
				if(strcmp(player_s->client_id, client_id) == 0){
					free(cmd);
					free(player);
					return MOSQ_ERR_SUCCESS;
				}
			}
			free(cmd);
			free(player);
			return MOSQ_ERR_ACL_DENIED;
		}else{
			free(cmd);
			free(player);
			if(room_s == NULL){
				return MOSQ_ERR_ACL_DENIED;
			}
			client_id = mosquitto_client_id(client);
			if(client_id == NULL){
				return MOSQ_ERR_ACL_DENIED;
			}
			HASH_FIND(hh_client_id, room_s->player_by_client_id, client_id, strlen(client_id), player_s);
			if(player_s == NULL){
				return MOSQ_ERR_ACL_DENIED;
			}else{
				return MOSQ_ERR_SUCCESS;
			}
		}
	}else if(access == MOSQ_ACL_WRITE){
		if(room_s){
			room_set_last_event(room_s, time(NULL));
		}
		if(!strcmp(cmd, "login")){
			tfdg_handle_login(client, room, room_s, msg);
		}else if(!strcmp(cmd, "logout")){
			tfdg_handle_logout(client, room_s, msg);
		}else if(!strcmp(cmd, "start-game")){
			tfdg_handle_start_game(client, room_s, msg);
		}else if(!strcmp(cmd, "roll-dice")){
			tfdg_handle_roll_dice(client, room_s, msg);
		}else if(!strcmp(cmd, "call-dudo")){
			tfdg_handle_call_dudo(client, room_s, msg);
		}else if(!strcmp(cmd, "call-calza")){
			tfdg_handle_call_calza(client, room_s, msg);
		}else if(!strcmp(cmd, "i-lost")){
			tfdg_handle_i_lost(client, room_s, msg);
		}else if(!strcmp(cmd, "i-won")){
			tfdg_handle_i_won(client, room_s, msg);
		}else if(!strcmp(cmd, "undo-loser")){
			tfdg_handle_undo_loser(client, room_s, msg);
		}else if(!strcmp(cmd, "undo-winner")){
			tfdg_handle_undo_winner(client, room_s, msg);
		}else if(!strcmp(cmd, "leave-game")){
			tfdg_handle_leave_game(client, room_s, msg);
		}else if(!strcmp(cmd, "kick-player")){
			tfdg_handle_kick_player(client, room_s, msg);
		}else if(!strcmp(cmd, "set-option")){
			tfdg_handle_set_option(client, room_s, msg);
       }else if(!strcmp(cmd, "snd-higher")){
			tfdg_handle_sound(client, room_s, msg, "higher");
       }else if(!strcmp(cmd, "snd-exact")){
			tfdg_handle_sound(client, room_s, msg, "exact");
		}
		free(room);
		free(cmd);
		free(player);
		/* All messages are denied, because they are only client->plugin */
		return MOSQ_ERR_ACL_DENIED;
	}else{
		free(room);
		free(cmd);
		free(player);
		return MOSQ_ERR_INVAL;
	}

	return MOSQ_ERR_SUCCESS;
}
